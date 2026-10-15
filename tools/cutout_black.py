#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cutout_black.py - 黑底图软抠 v2（针对白发+黑底出图的发丝黑边/黑口袋）

模型：出图 = 前景在纯黑上预乘合成，pixel = color * alpha。
  1) bg_region：边缘洪水填充，连通近黑背景 → alpha=0
  2) dark_flood：从 bg_region 向暗像素（maxc<95）继续扩散
     → 其中「离亮部（maxc>120）近」或「贴着 bg_region」的像素判定为
       发丝黑口袋 / 黑晕边 → alpha=maxc 软 matte + 去黑预乘还原本色
     深处暗区（耳机内部、围巾主体、裙体）保持 alpha=255 不受影响
  3) 其余不透明像素中贴边的做颜色去黑预乘（去 1px 暗描边），alpha 不变
  4) 0.8 高斯羽化 + 裁透明框
"""
import os
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import binary_propagation, binary_dilation, distance_transform_edt, gaussian_filter, label

TOL = 40      # 连通背景判定：与纯黑的最大色距
DARK = 95     # 暗区扩散上限（低于此亮度视为"可能是黑底/黑晕"）
BRIGHT = 120  # 亮部阈值（白发/白衬衫）
NEAR_BRIGHT = 10   # 暗区像素离亮部多近算"黑口袋/黑晕"
NEAR_BG = 3        # 贴背景多近直接软 matte
NEAR_DARK = 12     # 暗区扩散离 bg_region 的最大深度（防误侵入大块暗色物体）
POCKET_MAXC = 45     # 封闭黑口袋判定：组成像素亮度上限
POCKET_P90 = 55      # 封闭黑口袋判定：组件 90 分位亮度上限（有质感的物体不会这么黑）
POCKET_MIN_AREA = 1500  # 封闭黑口袋最小面积（防误伤褶皱/睫毛等暗色纹理）


def cutout_black(path, dry=False):
    im = Image.open(path).convert('RGB')
    a = np.asarray(im).astype(np.float32)
    h, w, _ = a.shape

    border = np.concatenate([a[0, :, :], a[-1, :, :], a[:, 0, :], a[:, -1, :]], axis=0)
    bg = np.median(border, axis=0)

    dist = np.sqrt(((a - bg) ** 2).sum(axis=2))
    cand = dist < TOL
    seeds = np.zeros_like(cand)
    seeds[0, :] = cand[0, :]
    seeds[-1, :] = cand[-1, :]
    seeds[:, 0] = cand[:, 0]
    seeds[:, -1] = cand[:, -1]
    bg_region = binary_propagation(seeds, mask=cand)

    maxc = a.max(axis=2)
    bright = maxc > BRIGHT
    dist_bright = distance_transform_edt(~bright)

    # 从 bg_region 向暗区扩散，限制深度 NEAR_DARK
    dark_mask = (maxc < DARK) & ~bg_region
    grow = bg_region.copy()
    depth = np.zeros_like(cand, dtype=np.int32)
    cur = bg_region
    for i in range(1, NEAR_DARK + 1):
        nxt = binary_dilation(cur) & dark_mask & ~cur
        depth[nxt] = i
        cur = cur | nxt
    dark_flood = cur & ~bg_region   # 从背景可达的暗区（限深）

    # 封闭黑口袋：与边缘不连通、整体接近纯黑、面积够大的区域（发丝间包住的背景）
    pocket_cand = (maxc < POCKET_MAXC) & ~bg_region
    lab, n = label(pocket_cand)
    pocket = np.zeros_like(cand)
    if n:
        vals = np.where(pocket_cand, maxc, 0)
        p90 = nd_percentile(lab, n, vals)
        sizes = np.bincount(lab.ravel(), minlength=n + 1)
        for i in range(1, n + 1):
            if sizes[i] >= POCKET_MIN_AREA and p90.get(i, 255) < POCKET_P90:
                pocket |= lab == i

    # 要软 matte 的像素：贴背景的 + 可达暗区里离亮部近的 + 封闭黑口袋
    near_bg = distance_transform_edt(~bg_region) <= NEAR_BG
    soft = (near_bg | ((dist_bright <= NEAR_BRIGHT) & dark_flood) | pocket) & ~bg_region

    alpha = np.where(bg_region, 0.0, 255.0)
    safe = np.maximum(maxc, 1.0)
    scale = np.clip(255.0 / safe, 1.0, 255.0 / 40.0)  # 太暗不再过度提亮，防噪声
    unpre = np.clip(a * scale[..., None], 0, 255)
    alpha = np.where(soft, np.minimum(maxc, 255.0), alpha)
    out_rgb = np.where(soft[..., None], unpre, a)

    # 不透明区贴边像素：只做颜色去黑预乘（保持 alpha=255），去暗描边
    opaque_edge = (distance_transform_edt(~(bg_region | soft)) <= 2) & ~bg_region & ~soft & (maxc > 40)
    out_rgb = np.where(opaque_edge[..., None],
                       np.clip(a * np.clip(255.0 / np.maximum(maxc, 1.0), 1, 255.0 / 40.0)[..., None], 0, 255),
                       out_rgb)

    alpha = gaussian_filter(alpha, 0.8)
    alpha = np.clip(alpha, 0, 255).astype(np.uint8)
    rgba = np.dstack([np.clip(out_rgb, 0, 255).astype(np.uint8), alpha])
    out = Image.fromarray(rgba, 'RGBA')

    bbox = out.getchannel('A').getbbox()
    if bbox:
        out = out.crop(bbox)

    cleared = float((alpha < 12).mean()) * 100
    print(f'{os.path.basename(path):<28} {w}x{h} -> {out.size[0]}x{out.size[1]}  '
          f'去背 {cleared:5.1f}%  bg=#{int(bg[0]):02x}{int(bg[1]):02x}{int(bg[2]):02x}  [black-soft-v2]')
    if not dry:
        rawp = os.path.join(os.path.dirname(path), '_raw', os.path.basename(path))
        os.makedirs(os.path.dirname(rawp), exist_ok=True)
        if not os.path.exists(rawp):
            import shutil
            shutil.copy2(path, rawp)
        out.save(path, 'PNG', optimize=True)


def nd_percentile(lab, n, vals):
    """每个连通组件的 90 分位亮度"""
    out = {}
    flat_lab = lab.ravel()
    flat_val = vals.ravel()
    idx = np.argsort(flat_lab, kind='stable')
    labs = flat_lab[idx]
    vs = flat_val[idx]
    bounds = np.searchsorted(labs, np.arange(1, n + 2))
    for i in range(1, n + 1):
        seg = vs[bounds[i - 1]:bounds[i]]
        seg = seg[seg > 0]
        if seg.size:
            out[i] = float(np.percentile(seg, 90))
    return out


if __name__ == '__main__':
    for p in sys.argv[1:]:
        cutout_black(p)
