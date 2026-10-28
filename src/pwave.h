// P 波检测：以 R 峰为锚点，在每个心动周期的 PR 段内定位 P 波主峰及其正向/负向瓣峰。
// 纯 C++17，不依赖 Qt，可直接放进 ecgcore 复用。
//
// 适用场景：电极逐步探入时 P 波由正相 → 双相 → 负相翻转，
// 因此判定不依赖固定极性，而是逐搏给出带符号幅值并追踪极性转变。
#pragma once

#include <vector>

namespace pw {

enum class Morph : int {
    Invalid = 0,  // 无法判定（噪声过大 / 无邻近参考）
    Flat = 1,     // 低平，未见明确 P 波
    Positive = 2, // 单相正
    Negative = 3, // 单相负
    Biphasic = 4  // 双相（正负瓣同时显著）
};

enum class Source : int { Measured = 0, Template = 1 };

struct Peak {
    int    index = -1;   // 样点下标
    double time = 0.0;   // 秒
    double amp = 0.0;    // 相对局部等电位线的幅值（与原信号同单位，mV）
    bool   significant = false;
};

struct Beat {
    int    rIndex = 0;      // 该周期 R 峰样点下标
    double rTime = 0.0;     // 秒
    Peak   p;               // P 波主峰（|幅值|更大的那一瓣）
    Peak   pos;             // 正向瓣峰
    Peak   neg;             // 负向瓣峰
    double signedAmp = 0.0; // 正峰幅值 + 负峰幅值（带符号净幅值，用于极性追踪）
    double widthMs = 0.0;   // P 波宽度（25% 幅值处左右边界）
    double prMs = 0.0;      // PR 间期（P 起点 → R 峰）
    double snr = 0.0;       // 主峰幅值 / 局部噪声 σ
    Morph  morph = Morph::Invalid;
    Source source = Source::Measured;
    int    quality = 0;     // 0 无效 1 低 2 中 3 高
};

struct Params {
    double fs = 500.0;          // 采样率 Hz
    double lowCut = 0.5;        // P 波段带通下限
    double highCut = 12.0;      // 上限（保留 P，抑制肌电/工频）
    double maxLookBack = 0.34;  // 搜索窗最远 R-340 ms
    double minLookBack = 0.16;  // 最短回看（心动过速时）
    double guardBeforeR = 0.075;// 搜索窗止于 R-75 ms，避开 QRS 起点（PR 段末端）
    double noiseFactor = 2.2;   // 主峰需超过噪声的倍数才算可见（翻转后 P 变小，阈值不宜太高）
    double minAmp = 0.005;      // 绝对幅值下限（信号单位，仅防纯噪声被当成 P）
    int    templateNeighbors = 10; // 模板回退时参考的邻搏数量
};

struct Result {
    bool   ok = false;
    std::vector<Beat>   beats;     // 与输入 R 峰一一对应
    std::vector<double> filtered;  // 0.5–12 Hz 零相位滤波结果（可视化用）

    int    flipBeat = -1;      // 极性翻转过后的第一搏（-1 表示未检出翻转）
    double flipTime = 0.0;     // 对应 R 峰时间（秒）
    int    nPositive = 0;
    int    nNegative = 0;
    int    nBiphasic = 0;
    int    nFlat = 0;
    int    nInvalid = 0;
    int    nTemplate = 0;
    double meanPrMs = 0.0;
    double meanWidthMs = 0.0;
};

// raw 与 rPeaks 必须来自同一段信号；rPeaks 为升序样点下标。
Result detect(const std::vector<double>& raw, const std::vector<int>& rPeaks, const Params& p);

} // namespace pw
