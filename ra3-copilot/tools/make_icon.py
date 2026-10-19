#!/usr/bin/env python3
"""make_icon.py — 从角色立绘生成 exe 图标 res\\app.ico

res\\app.ico 是二进制，换构图只能重新从立绘裁切缩放；手改无法复现，也没法给每个
尺寸单独调裁切。本脚本是这块资源的唯一生成入口。

为什么每个尺寸单独渲染，而不是把 256 缩下去：
16/24 px 下整张半身像只剩一团白，必须把头放大到接近满幅才认得出；48 px 以上按
正常头肩构图。所以裁切边长跟尺寸挂钩，而不是一次 resize 到底。

为什么 256 用 PNG、其余用 DIB：rc.exe 只搬运字节，两种都能嵌；DIB 在旧 shell 和
第三方资源工具里兼容性最好，PNG 能把 256 条目从 260 KB 压到几 KB。

用法：
    python tools/make_icon.py            # 主方案（半身特写）
    python tools/make_icon.py --variant chibi
"""

import argparse
import io
import os
import struct

from PIL import Image, ImageDraw

RES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "res")
DEFAULT_SRC = os.path.join(RES_DIR, "..", "..", "site", "assets", "character_operator.png")

# 底色与描边沿用改造前的图标，任务栏里仍是同一个应用
BASE = (24, 33, 45, 255)
EDGE = (63, 169, 201, 255)

# 裁切锚点：源图像素坐标。cx 取双眼中点，cy 取头部竖直中点。
# operator 立绘 1587x2358，耳朵顶 y≈77，下颏 y≈874，双眼 x 中点≈880
# chibi 立绘 1411x1910，头顶 y≈100，头宽≈1028，双眼 x 中点≈700
VARIANTS = {
    "chibi": {
        "src": "character_chibi.png",
        "cx": 700,
        "cy": 640,
        "head": 1028,         # 头宽（Q 版头近方形，取宽度更稳）
        "occupy": 0.69,       # 头在图标里的尺寸占比
        "small_boost": False,
    },
    "bust": {
        "src": "character_operator.png",
        "cx": 880,
        "cy": 476,
        "head": 797,          # 头高（耳朵顶到下颏）
        "occupy": 0.70,
        "small_boost": True,
    },
}

SIZES = (256, 128, 64, 48, 32, 24, 16)
# 64 px 以上用 PNG（体积差一个数量级），更小的用 DIB（旧工具链只认 DIB）
PNG_SIZES = (256, 128, 64)


def size_occupy(base, size, boost):
    """小尺寸把头再放大一档：16 px 只有 256 像素格，正常构图认不出五官。

    Q 版头本来就占满画面，再放大只会把猫耳切掉，所以 chibi 关掉这档补偿。
    """
    if not boost:
        return base
    if size <= 24:
        return base * 1.18
    if size <= 32:
        return base * 1.08
    return base


def size_stroke(size):
    """16 px 上 2 px 描边会吃掉六分之一画面，细描边才看得出是头像。"""
    if size <= 24:
        return 1
    if size <= 96:
        return 2
    return max(2, size // 64)


def render(src, cx, cy, side, size):
    s = max(8, int(round(side)))
    x0 = int(round(cx - s / 2))
    y0 = int(round(cy - s / 2))
    canvas = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sx0, sy0 = max(0, x0), max(0, y0)
    sx1, sy1 = min(src.width, x0 + s), min(src.height, y0 + s)
    if sx1 > sx0 and sy1 > sy0:
        canvas.alpha_composite(src.crop((sx0, sy0, sx1, sy1)), (sx0 - x0, sy0 - y0))
    canvas = canvas.resize((size, size), Image.LANCZOS)

    radius = int(size * 0.22)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=radius, fill=255
    )

    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(Image.new("RGBA", (size, size), BASE), (0, 0), mask)
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    layer.paste(canvas, (0, 0), mask)
    out.alpha_composite(layer)

    stroke = size_stroke(size)
    ImageDraw.Draw(out).rounded_rectangle(
        [stroke // 2, stroke // 2, size - 1 - stroke // 2, size - 1 - stroke // 2],
        radius=radius,
        outline=EDGE,
        width=stroke,
    )
    return out


def dib_bytes(img):
    """32 位 BGRA DIB：图标格式要求高度写两倍，并跟一段全 0 的 AND 掩码。"""
    w, h = img.size
    px = img.load()
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            rows += bytes((b, g, r, a))
    and_row = (w + 31) // 32 * 4
    and_mask = bytes(and_row * h)
    header = struct.pack(
        "<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(rows) + len(and_mask), 0, 0, 0, 0
    )
    return header + bytes(rows) + and_mask


def write_ico(images, path):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for img in images:
        if img.size[0] in PNG_SIZES:
            buf = io.BytesIO()
            img.save(buf, format="PNG", optimize=True)
            data = buf.getvalue()
        else:
            data = dib_bytes(img)
        w, h = img.size
        entries.append(
            struct.pack(
                "<BBBBHHII",
                0 if w >= 256 else w,
                0 if h >= 256 else h,
                0,
                0,
                1,
                32,
                len(data),
                offset,
            )
        )
        blobs.append(data)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        for e in entries:
            f.write(e)
        for b in blobs:
            f.write(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="chibi", choices=sorted(VARIANTS))
    ap.add_argument("--out", default=os.path.join(RES_DIR, "app.ico"))
    args = ap.parse_args()

    cfg = VARIANTS[args.variant]
    src_path = os.path.join(RES_DIR, "..", "..", "site", "assets", cfg["src"])
    src = Image.open(src_path).convert("RGBA")

    images = []
    for size in SIZES:
        side = cfg["head"] / size_occupy(cfg["occupy"], size, cfg["small_boost"])
        images.append(render(src, cfg["cx"], cfg["cy"], side, size))

    write_ico(images, args.out)
    print(f"wrote {args.out}  variant={args.variant}  sizes={SIZES}")
    print(f"bytes={os.path.getsize(args.out)}")


if __name__ == "__main__":
    main()
