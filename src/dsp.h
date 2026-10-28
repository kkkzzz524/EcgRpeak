// 通用二阶 IIR（Butterworth biquad）+ 零相位正反向滤波
// 供 Pan-Tompkins（5–15 Hz）与 P 波检测（0.5–12 Hz）共用，避免重复实现。
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace dsp {

constexpr double kPi = 3.14159265358979323846;
constexpr double kButterQ = 0.7071067811865476; // 1/sqrt(2)，二阶 Butterworth

struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    double step(double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// RBJ Audio EQ Cookbook 双线性变换，采样率无关
inline Biquad makeLowPass(double fc, double fs)
{
    Biquad q;
    const double w0 = 2.0 * kPi * fc / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kButterQ);
    const double a0 = 1.0 + alpha;
    q.b0 = (1.0 - cw) * 0.5 / a0;
    q.b1 = (1.0 - cw) / a0;
    q.b2 = q.b0;
    q.a1 = -2.0 * cw / a0;
    q.a2 = (1.0 - alpha) / a0;
    return q;
}

inline Biquad makeHighPass(double fc, double fs)
{
    Biquad q;
    const double w0 = 2.0 * kPi * fc / fs;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * kButterQ);
    const double a0 = 1.0 + alpha;
    q.b0 = (1.0 + cw) * 0.5 / a0;
    q.b1 = -(1.0 + cw) / a0;
    q.b2 = q.b0;
    q.a1 = -2.0 * cw / a0;
    q.a2 = (1.0 - alpha) / a0;
    return q;
}

// 正反向滤波（零相位），两端镜像延拓抑制边界瞬态
inline void filtfilt(std::vector<double>& x, const Biquad& proto, double fs)
{
    const int n = static_cast<int>(x.size());
    if (n < 8) {
        Biquad f = proto;
        for (double& v : x) v = f.step(v);
        return;
    }

    const int pad = std::min(n / 2, std::max(1, static_cast<int>(std::lround(0.30 * fs))));
    std::vector<double> buf;
    buf.reserve(static_cast<size_t>(n + 2 * pad));
    for (int i = pad; i >= 1; --i) buf.push_back(x[i]);              // 左镜像
    buf.insert(buf.end(), x.begin(), x.end());
    for (int i = n - 2; i >= n - 1 - pad; --i) buf.push_back(x[i]);  // 右镜像

    Biquad fwd = proto;
    for (double& v : buf) v = fwd.step(v);
    Biquad bwd = proto;
    for (int i = static_cast<int>(buf.size()) - 1; i >= 0; --i) buf[i] = bwd.step(buf[i]);

    std::copy(buf.begin() + pad, buf.begin() + pad + n, x.begin());
}

} // namespace dsp
