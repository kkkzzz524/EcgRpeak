// Pan-Tompkins QRS 检测算法（纯 C++17，无 Qt 依赖）
// 参考：Pan J., Tompkins W.J. "A Real-Time QRS Detection Algorithm",
//       IEEE Trans. Biomed. Eng., vol. BME-32, no. 3, pp. 230-236, 1985.
#pragma once

#include <vector>

namespace pt {

struct Params {
    double fs = 360.0;                 // 采样率 Hz
    double lowCut = 5.0;               // 带通低截止 Hz
    double highCut = 15.0;             // 带通高截止 Hz
    double integrationWindow = 0.150;  // 移动窗积分窗长 s
    double refractory = 0.200;         // 不应期(最小 RR) s
    double tWaveWindow = 0.360;        // 与前一 R 波距离小于该值时做 T 波判别 s
    double tWaveFactor = 0.50;         // 斜率小于前一搏动的该比例则判为 T 波
    double thresholdWeight = 0.25;     // threshold = NPKI + w * (SPKI - NPKI)
    double searchBackFactor = 1.66;    // RR 超过平均 RR 的该倍数时启动回溯搜索
    double refineWindow = 0.100;       // 在积分峰附近精定位 R 峰的半窗宽 s
    bool   zeroPhase = true;           // 正反向滤波(零相位)，避免 R 峰时移
};

struct Result {
    std::vector<double> filtered;      // 带通滤波后信号
    std::vector<double> derivative;    // 微分
    std::vector<double> squared;       // 平方
    std::vector<double> integrated;    // 移动窗积分(包络)
    std::vector<double> thresholdTrack;// 自适应阈值轨迹(与采样点等长)

    std::vector<int>    rPeaks;        // R 峰样点下标
    std::vector<bool>   fromSearchBack;// 该 R 峰是否由回溯搜索补检
    std::vector<double> rAmp;          // R 峰处积分幅值

    std::vector<double> rr;            // RR 间期，单位 s
    std::vector<double> instHr;        // 瞬时心率 bpm

    double meanRr = 0.0;               // s
    double meanHr = 0.0;               // bpm
    double minHr = 0.0;
    double maxHr = 0.0;
    double sdnn = 0.0;                 // s
    double rmssd = 0.0;                // s
    double pnn50 = 0.0;                // %

    int    skippedRows = 0;            // 预留：非法样点计数
    bool   ok = false;
};

Result detect(const std::vector<double>& raw, const Params& p);

// 按 Result::rPeaks 重新计算 RR / 心率 / HRV（人工增删 R 峰后调用）
void recomputeStats(Result& r, double fs);

} // namespace pt
