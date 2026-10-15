#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""alpha_cutout.py - 把白底/纯色底的图抠成透明 PNG

背景：出图器给的都是 JPEG（无 alpha），直接放到暗色页面上会是一个方块。
本脚本用「从四边洪水填充 + 1px 膨胀 + 羽化」把与边缘连通的背景色变透明，
保留被主体包围的浅色区域（如白衬衫、图标内的浅色描边）。

原图会先备份到 site/assets/_raw/（该目录不入库）。

用法：
  python alpha_cutout.py            # 处理全部
  python alpha_cutout.py --dry      # 只报告，不写文件
"""
import os
import sys
import shutil

import numpy as np
from PIL import Image
from scipy.ndimage import binary_propagation, binary_dilation, gaussian_filter

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.abspath(os.path.join(HERE, '..', 'assets'))
RAW = os.path.join(ASSETS, '_raw')

# 每项：(容差, 模式)
#   mode 'flood' = 只去掉与边缘连通的背景（人物/图标/字标，安全）
#   mode 'all'   = 去掉全部近背景色像素（空心边框类，如 HUD 框）
TOL = 38   # 人物：较大，连脚底柔和阴影一起吃掉
TOL_T = 20 # 图形：较小，保住米白描边
JOBS = {
    # ---- 人物（白底 + 地面阴影）----
    'character_full.png':       (TOL, 'flood'),
    'character_half.png':       (TOL, 'flood'),
    'character_alt_1.png':      (TOL, 'flood'),
    'character_alt_2.png':      (TOL, 'flood'),
    'character_alt_3.png':      (TOL, 'flood'),
    'character_bust.png':       (TOL, 'flood'),
    'character_chibi.png':      (TOL, 'flood'),
    'character_silhouette.png': (TOL, 'flood'),
    # ---- 徽记 / 字标 / 图标（白底产品图）----
    'logo_ra3copilot.png':      (TOL_T, 'flood'),
    'emblem.png':               (TOL_T, 'flood'),
    'favicon.png':              (TOL_T, 'flood'),
    'reticle.png':              (TOL_T, 'flood'),
    'icon_resource.png':        (TOL_T, 'flood'),
    'icon_build.png':           (TOL_T, 'flood'),
    'icon_combat.png':          (TOL_T, 'flood'),
    'icon_unit.png':            (TOL_T, 'flood'),
    'icon_summon.png':          (TOL_T, 'flood'),
    'icon_crate.png':           (TOL_T, 'flood'),
    'icon_mcp.png':             (TOL_T, 'flood'),
    'icon_http.png':            (TOL_T, 'flood'),
    # ---- 空心框（内部也要透）----
    'hud_frame.png':            (TOL_T, 'all'),
    'hud_corner.png':           (TOL_T, 'all'),
    'tag_frame.png':            (TOL_T, 'all'),
}


def cutout(path, tol, mode, dry=False):
    im = Image.open(path).convert('RGB')
    a = np.asarray(im).astype(np.int16)
    h, w, _ = a.shape

    # 背景色 = 四边像素的中位数
    border = np.concatenate([a[0, :, :], a[-1, :, :], a[:, 0, :], a[:, -1, :]], axis=0)
    bg = np.median(border, axis=0)

    dist = np.sqrt(((a - bg) ** 2).sum(axis=2))
    cand = dist < tol

    if mode == 'all':
        region = cand
    else:
        seeds = np.zeros_like(cand)
        seeds[0, :] = cand[0, :]
        seeds[-1, :] = cand[-1, :]
        seeds[:, 0] = cand[:, 0]
        seeds[:, -1] = cand[:, -1]
        region = binary_propagation(seeds, mask=cand)

    region = binary_dilation(region, iterations=1)         # 吃掉抗锯齿光晕
    alpha = np.where(region, 0.0, 255.0).astype(np.float32)
    alpha = gaussian_filter(alpha, 0.8)                    # 羽化边缘
    alpha = np.clip(alpha, 0, 255).astype(np.uint8)

    rgba = np.dstack([np.asarray(im).astype(np.uint8), alpha])
    out = Image.fromarray(rgba, 'RGBA')

    # 裁掉全透明外框（顺带减体积）
    bbox = out.getchannel('A').getbbox()
    if bbox:
        out = out.crop(bbox)

    cleared = float((alpha < 12).mean()) * 100
    print(f'{os.path.basename(path):<28} {w}x{h} -> {out.size[0]}x{out.size[1]}  '
          f'去背 {cleared:5.1f}%  bg=#{int(bg[0]):02x}{int(bg[1]):02x}{int(bg[2]):02x}  [{mode}]')
    if not dry:
        os.makedirs(RAW, exist_ok=True)
        rawp = os.path.join(RAW, os.path.basename(path))
        if not os.path.exists(rawp):
            shutil.copy2(path, rawp)
        out.save(path, 'PNG', optimize=True)


def main():
    dry = '--dry' in sys.argv
    n = 0
    for name, (tol, mode) in JOBS.items():
        p = os.path.join(ASSETS, name)
        if not os.path.exists(p):
            print(f'{name:<28} MISSING')
            continue
        with open(p, 'rb') as f:
            if f.read(8) == b'\x89PNG\r\n\x1a\n':
                f.seek(25)
                if f.read(1)[0] in (4, 6):
                    print(f'{name:<28} already has alpha, skip')
                    continue
        cutout(p, tol, mode, dry)
        n += 1
    print(f'--- {"(dry) " if dry else ""}done: {n} files ---')


if __name__ == '__main__':
    main()
