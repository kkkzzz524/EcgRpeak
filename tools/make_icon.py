#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用纯标准库绘制应用图标（PNG + ICO），避免引入 PIL 依赖。

图案：深蓝圆角底 + 白色心电波形（P-QRS-T）。
先用超采样绘制 1024px，再逐级盒式降采样，保证小尺寸也清晰。
"""
import math
import os
import struct
import zlib

SS = 4               # 超采样倍数
BASE = 256           # 目标最大尺寸


def rounded_rect_mask(x, y, w, h, r):
    """点(x,y)是否在圆角矩形内（坐标为 0..1 归一化）。"""
    cx = min(max(x, r), w - r)
    cy = min(max(y, r), h - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r or (r <= x <= w - r and 0 <= y <= h) \
        or (0 <= x <= w and r <= y <= h - r)


def dist_to_segment(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    if L2 <= 1e-12:
        return math.hypot(px - ax, py - ay)
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


# 归一化波形坐标（x, y），y 向下
WAVE = [
    (0.09, 0.56), (0.21, 0.56), (0.25, 0.50), (0.29, 0.56), (0.36, 0.56),
    (0.41, 0.62), (0.455, 0.30), (0.50, 0.76), (0.545, 0.56), (0.63, 0.56),
    (0.68, 0.46), (0.73, 0.56), (0.90, 0.56),
]


def render(size):
    """渲染 size x size 的 RGBA 图像，返回 bytes 行优先。"""
    n = size
    wave_w = 0.052                     # 线宽（归一化）
    px = bytearray(n * n * 4)
    for j in range(n):
        y = (j + 0.5) / n
        for i in range(n):
            x = (i + 0.5) / n
            a = 0.0
            # 背景圆角矩形
            if rounded_rect_mask(x, y, 1.0, 1.0, 0.235):
                # 垂直渐变 #3B82F6 -> #1D4ED8
                t = y
                r = int(0x3B + (0x1D - 0x3B) * t)
                g = int(0x82 + (0x4E - 0x82) * t)
                b = int(0xF6 + (0xD8 - 0xF6) * t)
                a = 1.0
            else:
                r = g = b = 0

            # 波形（白色）
            d = 1e9
            for k in range(len(WAVE) - 1):
                ax, ay = WAVE[k]
                bx, by = WAVE[k + 1]
                d = min(d, dist_to_segment(x, y, ax, ay, bx, by))
            if d <= wave_w * 0.5:
                # 轻微抗锯齿
                cov = min(1.0, (wave_w * 0.5 - d) / (wave_w * 0.18) + 1.0)
                cov = max(0.0, min(1.0, cov))
                r = int(r * (1 - cov) + 255 * cov)
                g = int(g * (1 - cov) + 255 * cov)
                b = int(b * (1 - cov) + 255 * cov)
                a = max(a, cov)

            o = (j * n + i) * 4
            px[o] = r
            px[o + 1] = g
            px[o + 2] = b
            px[o + 3] = int(a * 255)
    return px


def downsample(src, src_n, dst_n):
    """盒式降采样（src_n 必须是 dst_n 的整数倍）。"""
    factor = src_n // dst_n
    out = bytearray(dst_n * dst_n * 4)
    for j in range(dst_n):
        for i in range(dst_n):
            ar = ag = ab = aa = 0
            for dj in range(factor):
                for di in range(factor):
                    o = ((j * factor + dj) * src_n + (i * factor + di)) * 4
                    ar += src[o]
                    ag += src[o + 1]
                    ab += src[o + 2]
                    aa += src[o + 3]
            cnt = factor * factor
            o = (j * dst_n + i) * 4
            out[o] = ar // cnt
            out[o + 1] = ag // cnt
            out[o + 2] = ab // cnt
            out[o + 3] = aa // cnt
    return out


def png_bytes(rgba, n):
    raw = bytearray()
    stride = n * 4
    for j in range(n):
        raw.append(0)
        raw.extend(rgba[j * stride:(j + 1) * stride])

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(os.path.dirname(here), "src", "icons")
    os.makedirs(out_dir, exist_ok=True)

    big = render(BASE * SS)
    sizes = [256, 128, 64, 48, 32, 16]
    images = {}
    cur, cur_n = big, BASE * SS
    for s in sizes:
        while cur_n > s:
            cur_n //= 2
            cur = downsample(cur, cur_n * 2, cur_n)
        images[s] = png_bytes(cur, s)

    with open(os.path.join(out_dir, "app.png"), "wb") as f:
        f.write(images[256])

    # ICO 容器（每张图用 PNG 压缩）
    entries = []
    offset = 6 + 16 * len(sizes)
    for s in sizes:
        data = images[s]
        w = 0 if s >= 256 else s
        entries.append(struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    ico = struct.pack("<HHH", 0, 1, len(sizes)) + b"".join(entries) + b"".join(images[s] for s in sizes)

    ico_path = os.path.join(os.path.dirname(here), "src", "app.ico")
    with open(ico_path, "wb") as f:
        f.write(ico)
    print("生成:", os.path.join(out_dir, "app.png"), os.path.getsize(os.path.join(out_dir, "app.png")), "B")
    print("生成:", ico_path, os.path.getsize(ico_path), "B")


if __name__ == "__main__":
    main()
