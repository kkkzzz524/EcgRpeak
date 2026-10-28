#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成示例心电数据（CSV）与人工标注文件，用于验证 Pan-Tompkins 检测精度。

纯标准库实现，不依赖 numpy。每个心搏由 P/Q/R/S/T 五个高斯波叠加，
并叠加基线漂移、工频干扰与白噪声。
"""
import json
import math
import os
import random
import struct


def gauss_add(sig, fs, center, sigma, amp):
    lo = max(0, int(math.floor((center - 4 * sigma) * fs)))
    hi = min(len(sig) - 1, int(math.ceil((center + 4 * sigma) * fs)))
    for i in range(lo, hi + 1):
        dt = i / fs - center
        sig[i] += amp * math.exp(-0.5 * dt * dt / (sigma * sigma))


def synth(fs, seconds, rng, lead="II"):
    n = int(round(fs * seconds))
    sig = [0.0] * n
    peaks = []

    if lead == "II":
        p_amp, r_amp, s_amp, t_amp = 0.14, 1.05, -0.26, 0.30
    else:  # V1：R 波小、S 波深
        p_amp, r_amp, s_amp, t_amp = 0.10, 0.35, -0.85, 0.18

    t = 0.55
    while t < seconds - 0.4:
        gauss_add(sig, fs, t - 0.160, 0.030, p_amp)
        gauss_add(sig, fs, t - 0.030, 0.011, -0.12)
        gauss_add(sig, fs, t,          0.0125, r_amp)
        gauss_add(sig, fs, t + 0.034,  0.014, s_amp)
        gauss_add(sig, fs, t + 0.300,  0.062, t_amp)
        peaks.append(t)
        t += 0.780 + 0.060 * math.sin(0.55 * t) + 0.018 * math.sin(1.7 * t)

    for i in range(n):
        ti = i / fs
        sig[i] += 0.095 * math.sin(2 * math.pi * 0.30 * ti)
        sig[i] += 0.045 * math.sin(2 * math.pi * 0.06 * ti)
        sig[i] += 0.015 * math.sin(2 * math.pi * 50.0 * ti)
        sig[i] += rng.gauss(0.0, 0.010)

    return sig, peaks


def write_csv(path, fs, seconds, leads):
    header = ["time_s"] + ["%s_mV" % name for name, _ in leads]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 示例心电数据  采样率 fs=%d Hz  通道数=%d\n" % (fs, len(leads)))
        f.write(",".join(header) + "\n")
        n = len(leads[0][1])
        for i in range(n):
            row = ["%.6f" % (i / fs)] + ["%.5f" % col[i] for _, col in leads]
            f.write(",".join(row) + "\n")


def write_annotations(path, peaks):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 真实 R 峰时间(s)\n")
        f.write("r_time_s\n")
        for t in peaks:
            f.write("%.4f\n" % t)


def write_bin_int16(path, leads, scale=1.0 / 2048.0):
    """多通道交织写入 int16 小端原始 ADC 码（1 mV = 2048 LSB）。"""
    n = len(leads[0][1])
    with open(path, "wb") as f:
        for i in range(n):
            for _, col in leads:
                v = int(round(col[i] / scale))
                f.write(struct.pack("<h", max(-32768, min(32767, v))))


def write_bin_float32(path, col, header_lines=None):
    with open(path, "wb") as f:
        if header_lines:
            f.write(header_lines.encode("ascii"))
        f.write(struct.pack("<%df" % len(col), *col))


def write_bin_sidecar(path, **kw):
    with open(path + ".json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(kw, f, indent=2)
        f.write("\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(os.path.dirname(here), "samples")
    os.makedirs(out, exist_ok=True)

    # 1) 360 Hz, 20 s, 双通道
    rng = random.Random(20260527)
    fs1, sec1 = 360, 20.0
    ii, peaks1 = synth(fs1, sec1, rng, "II")
    v1, _ = synth(fs1, sec1, random.Random(7), "V1")
    write_csv(os.path.join(out, "sample_ecg_360hz.csv"), fs1, sec1,
              [("Lead_II", ii), ("Lead_V1", v1)])
    write_annotations(os.path.join(out, "sample_ecg_360hz_annotations.csv"), peaks1)

    # 2) 250 Hz, 15 s, 单通道（模拟 MIT-BIH 采样率）
    rng2 = random.Random(1234)
    fs2, sec2 = 250, 15.0
    ii2, peaks2 = synth(fs2, sec2, rng2, "II")
    write_csv(os.path.join(out, "sample_ecg_250hz.csv"), fs2, sec2, [("Lead_II", ii2)])
    write_annotations(os.path.join(out, "sample_ecg_250hz_annotations.csv"), peaks2)

    # 3) 纯数值无表头 + 空格分隔（验证格式兼容性）
    rng3 = random.Random(99)
    fs3, sec3 = 500, 10.0
    ii3, peaks3 = synth(fs3, sec3, rng3, "II")
    with open(os.path.join(out, "sample_ecg_500hz_noheader.txt"), "w",
              encoding="utf-8", newline="\n") as f:
        for i in range(len(ii3)):
            f.write("%.6f %.5f\n" % (i / fs3, ii3[i]))
    write_annotations(os.path.join(out, "sample_ecg_500hz_annotations.csv"), peaks3)

    # 4) 500 Hz / 12 s / int16 双通道交织 + 同名 .json 说明文件
    rng4 = random.Random(4242)
    fs4, sec4 = 500, 12.0
    ii4, peaks4 = synth(fs4, sec4, rng4, "II")
    v14, _ = synth(fs4, sec4, random.Random(4243), "V1")
    bin4 = os.path.join(out, "sample_ecg_500hz_2ch.bin")
    write_bin_int16(bin4, [("Lead_II", ii4), ("Lead_V1", v14)])
    write_bin_sidecar(bin4, format="int16", endian="little", channels=2,
                      offset=0, fs=fs4, scale=1.0 / 2048.0, bias=0.0)
    write_annotations(os.path.join(out, "sample_ecg_500hz_bin_annotations.csv"), peaks4)

    # 5) 360 Hz / 15 s / float32 小端，文件自带文本头（无说明文件）
    rng5 = random.Random(777)
    fs5, sec5 = 360, 15.0
    ii5, peaks5 = synth(fs5, sec5, rng5, "II")
    header5 = (
        "# ECG binary sample: float32 little-endian, 1 lead\n"
        "# the text header ends at this line, payload follows immediately\n"
        "fs = 360\n"
        "channels = 1\n"
        "format = float32\n"
        "endian = little\n"
        "scale = 1.0\n"
    )
    write_bin_float32(os.path.join(out, "sample_ecg_360hz_float32.bin"), ii5, header5)
    write_annotations(os.path.join(out, "sample_ecg_360hz_f32_annotations.csv"), peaks5)

    # 6) 250 Hz / 15 s / int16 单通道，完全裸数据（无说明文件、无文本头）
    bin6 = os.path.join(out, "sample_ecg_250hz_raw.bin")
    write_bin_int16(bin6, [("Lead_II", ii2)])

    print("已生成:")
    for name in sorted(os.listdir(out)):
        p = os.path.join(out, name)
        print("  %-40s %8.1f KB" % (name, os.path.getsize(p) / 1024.0))


if __name__ == "__main__":
    main()
