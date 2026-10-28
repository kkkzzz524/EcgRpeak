#include "pwave.h"

#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pw {
namespace {

double medianInPlace(std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        std::nth_element(v.begin(), v.begin() + static_cast<long>(mid - 1), v.begin() + static_cast<long>(mid));
        m = 0.5 * (m + v[mid - 1]);
    }
    return m;
}

double medianCopy(const std::vector<double>& v)
{
    std::vector<double> c = v;
    return medianInPlace(c);
}

double madSigma(const std::vector<double>& v, double med)
{
    if (v.size() < 4) return 0.0;
    std::vector<double> dev(v.size());
    for (size_t i = 0; i < v.size(); ++i) dev[i] = std::fabs(v[i] - med);
    return 1.4826 * medianInPlace(dev);
}

// 以 idx 为中心向两侧扩展，低于 thr 的样本允许 gap 个空档再继续（跨越双相 P 的过零点）
void expandLobe(const std::vector<double>& env, int idx, double thr, int lo, int hi, int gap,
                int& left, int& right)
{
    left = idx;
    int miss = 0;
    for (int i = idx - 1; i >= lo; --i) {
        if (env[static_cast<size_t>(i)] >= thr) {
            left = i;
            miss = 0;
        } else if (++miss > gap) {
            break;
        }
    }
    right = idx;
    miss = 0;
    for (int i = idx + 1; i <= hi; ++i) {
        if (env[static_cast<size_t>(i)] >= thr) {
            right = i;
            miss = 0;
        } else if (++miss > gap) {
            break;
        }
    }
}

double meanOf(const std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

} // namespace

Result detect(const std::vector<double>& raw, const std::vector<int>& rPeaks, const Params& pIn)
{
    Result res;
    const int n = static_cast<int>(raw.size());
    const int nb = static_cast<int>(rPeaks.size());
    if (n < 32 || nb < 3) return res;

    Params p = pIn;
    const double fs = (p.fs > 1.0) ? p.fs : 500.0;
    p.fs = fs;
    p.lowCut = std::clamp(p.lowCut, 0.1, 1.0);
    p.highCut = std::clamp(p.highCut, std::max(p.lowCut + 0.5, 3.0), 0.45 * fs);
    p.maxLookBack = std::clamp(p.maxLookBack, 0.10, 0.50);
    p.minLookBack = std::clamp(p.minLookBack, 0.06, p.maxLookBack);
    p.guardBeforeR = std::clamp(p.guardBeforeR, 0.02, 0.15);
    p.noiseFactor = std::clamp(p.noiseFactor, 1.0, 10.0);
    p.minAmp = std::max(0.0, p.minAmp);
    p.templateNeighbors = std::clamp(p.templateNeighbors, 3, 60);

    // ---- 0.5–12 Hz 零相位带通：保留 P 波低频成分，压掉基线漂移与肌电 ----
    std::vector<double> sig = raw;
    dsp::filtfilt(sig, dsp::makeHighPass(p.lowCut, fs), fs);
    dsp::filtfilt(sig, dsp::makeLowPass(p.highCut, fs), fs);
    res.filtered = sig;

    struct Work {
        int    r = 0;
        int    lo = 0, hi = 0;
        double med = 0.0, sigma = 0.0;
        double ampPos = 0.0, ampNeg = 0.0;
        int    iPos = -1, iNeg = -1;
        bool   ok = false;
    };
    std::vector<Work> work(static_cast<size_t>(nb));

    // ---- 1. 逐搏在 PR 段内找正/负两个瓣峰 ----
    for (int i = 0; i < nb; ++i) {
        Work& b = work[static_cast<size_t>(i)];
        b.r = rPeaks[static_cast<size_t>(i)];
        if (b.r <= 0 || b.r >= n) continue;

        const int prevR = (i > 0) ? rPeaks[static_cast<size_t>(i - 1)] : b.r;
        const int nextR = (i + 1 < nb) ? rPeaks[static_cast<size_t>(i + 1)] : b.r;
        double rr = 0.8;
        if (nextR > b.r) rr = static_cast<double>(nextR - b.r) / fs;
        else if (b.r > prevR) rr = static_cast<double>(b.r - prevR) / fs;
        rr = std::clamp(rr, 0.25, 2.0);

        const double look = std::clamp(0.55 * rr, p.minLookBack, p.maxLookBack);
        int lo = b.r - static_cast<int>(std::lround(look * fs));
        // 再留 12 ms 余量：极值落在窗口边缘多半是 QRS 起点而不是 P 波
        int hi = b.r - static_cast<int>(std::lround((p.guardBeforeR + 0.012) * fs));
        if (i > 0) lo = std::max(lo, prevR + static_cast<int>(std::lround(0.42 * rr * fs))); // 避开 T 波尾
        lo = std::max(lo, 1);
        hi = std::min(hi, n - 2);
        if (hi - lo < 8) continue;

        std::vector<double> vals(static_cast<size_t>(hi - lo + 1));
        for (int k = lo; k <= hi; ++k) vals[static_cast<size_t>(k - lo)] = sig[static_cast<size_t>(k)];
        b.med = medianCopy(vals);
        b.sigma = madSigma(vals, b.med);

        double vmax = -1e300, vmin = 1e300;
        int iMax = lo, iMin = lo;
        for (int k = lo; k <= hi; ++k) {
            if (sig[static_cast<size_t>(k)] > vmax) { vmax = sig[static_cast<size_t>(k)]; iMax = k; }
            if (sig[static_cast<size_t>(k)] < vmin) { vmin = sig[static_cast<size_t>(k)]; iMin = k; }
        }
        b.ampPos = vmax - b.med;
        b.ampNeg = vmin - b.med;
        b.iPos = iMax;
        b.iNeg = iMin;
        b.lo = lo;
        b.hi = hi;
        b.ok = true;
    }

    // ---- 2. 记录级参考：P 幅值中位数 / 噪声水平中位数 ----
    // 参考幅值只统计「确实像 P 波」的搏（SNR≥3），避免整段噪声把参考值抬高
    std::vector<double> domAmps, domClean, sigmas;
    for (const Work& b : work) {
        if (!b.ok) continue;
        sigmas.push_back(b.sigma);
        const double dom = std::max(std::fabs(b.ampPos), std::fabs(b.ampNeg));
        domAmps.push_back(dom);
        if (b.sigma > 0.0 && dom / b.sigma >= 3.0) domClean.push_back(dom);
    }
    const double ampRef = domClean.size() >= 3
                              ? medianInPlace(domClean)
                              : (domAmps.empty() ? 0.0 : medianInPlace(domAmps));
    const double sigmaRef = sigmas.empty() ? 0.0 : medianInPlace(sigmas);

    // ---- 3. 逐搏判定质量 / 形态，并把峰位对齐到原始信号的真实极值 ----
    res.beats.resize(static_cast<size_t>(nb));
    const int snapWin = std::max(1, static_cast<int>(std::lround(0.030 * fs))); // ±30 ms
    for (int i = 0; i < nb; ++i) {
        const Work& b = work[static_cast<size_t>(i)];
        Beat& out = res.beats[static_cast<size_t>(i)];
        out.rIndex = b.r;
        out.rTime = static_cast<double>(b.r) / fs;
        if (!b.ok) {
            out.morph = Morph::Invalid;
            out.quality = 0;
            continue;
        }

        const double ampAbs = std::max(std::fabs(b.ampPos), std::fabs(b.ampNeg));
        const double sigma = std::max(b.sigma, 1e-9);
        out.snr = ampAbs / sigma;
        const bool posDominant = std::fabs(b.ampPos) >= std::fabs(b.ampNeg);

        // 可见性 = 超过噪声阈值，或达到本记录典型 P 幅值的 1/4 且高于 1.8σ。
        // 后半段（极性翻转后）P 波变小，只靠绝对阈值会整段漏检。
        const double visibleThr = std::max(p.minAmp, p.noiseFactor * b.sigma);
        const double relative = (ampRef > 0.0) ? ampAbs / ampRef : 0.0;
        const bool visible = (ampAbs >= visibleThr) || (relative >= 0.25 && out.snr >= 1.8);
        if (visible) {
            if (out.snr >= 4.0 && relative >= 0.5) out.quality = 3;
            else if (out.snr >= 2.4 || (relative >= 0.3 && out.snr >= 1.8)) out.quality = 2;
            else out.quality = 1;
        } else {
            out.quality = 0;
        }

        const double lobeRatio = (ampAbs > 0.0)
                                     ? std::min(std::fabs(b.ampPos), std::fabs(b.ampNeg)) / ampAbs
                                     : 0.0;
        const bool posSig = b.ampPos >= std::max(0.5 * ampAbs, 2.0 * b.sigma);
        const bool negSig = -b.ampNeg >= std::max(0.5 * ampAbs, 2.0 * b.sigma);
        const bool biphasic = visible && posSig && negSig && lobeRatio >= 0.5;
        if (!visible) {
            out.morph = Morph::Flat;
        } else if (biphasic) {
            out.morph = Morph::Biphasic;
        } else {
            out.morph = (b.ampPos + b.ampNeg >= 0.0) ? Morph::Positive : Morph::Negative;
        }

        // 峰位取原始信号（绘图/导出都看这条曲线）：
        // 双相 P 固定标在正瓣峰；其余按主极性取正峰或负峰。
        std::vector<double> rawVals;
        rawVals.reserve(static_cast<size_t>(b.hi - b.lo + 1));
        for (int k = b.lo; k <= b.hi; ++k) rawVals.push_back(raw[static_cast<size_t>(k)]);
        const double rawBase = medianCopy(rawVals);

        const auto snap = [&](int center, int dir, double& ampOut) {
            const int lo = std::max(b.lo, center - snapWin);
            const int hi = std::min(b.hi, center + snapWin);
            int best = center;
            double bv = -1e300;
            for (int k = lo; k <= hi; ++k) {
                const double v = dir * (raw[static_cast<size_t>(k)] - rawBase);
                if (v > bv) {
                    bv = v;
                    best = k;
                }
            }
            ampOut = raw[static_cast<size_t>(best)] - rawBase;
            return best;
        };

        double ampPosRaw = 0.0, ampNegRaw = 0.0;
        const int iPosRaw = snap(b.iPos, +1, ampPosRaw);
        const int iNegRaw = snap(b.iNeg, -1, ampNegRaw);
        out.pos.index = iPosRaw;
        out.pos.time = static_cast<double>(iPosRaw) / fs;
        out.pos.amp = ampPosRaw;
        out.neg.index = iNegRaw;
        out.neg.time = static_cast<double>(iNegRaw) / fs;
        out.neg.amp = ampNegRaw;
        out.pos.significant = posSig;
        out.neg.significant = negSig;

        int seed = posDominant ? b.iPos : b.iNeg;
        int dir = posDominant ? +1 : -1;
        if (biphasic) {
            seed = b.iPos;   // 双相：标注点放在正峰值处
            dir = +1;
        }
        double ampP = 0.0;
        const int iP = snap(seed, dir, ampP);
        out.p.index = iP;
        out.p.time = static_cast<double>(iP) / fs;
        out.p.amp = ampP;
        out.p.significant = visible;

        // 净幅值用滤波域两瓣（抗噪），用于极性趋势与形态判断
        out.signedAmp = b.ampPos + b.ampNeg;

        // P 宽度与 PR 间期（以最终峰位为中心，25% 幅值处为界，允许 20 ms 过零空档）
        std::vector<double> env(static_cast<size_t>(b.hi - b.lo + 1));
        for (int k = b.lo; k <= b.hi; ++k)
            env[static_cast<size_t>(k - b.lo)] = std::fabs(sig[static_cast<size_t>(k)] - b.med);
        int left = out.p.index - b.lo, right = out.p.index - b.lo;
        const double widthThr = 0.25 * ampAbs;
        expandLobe(env, out.p.index - b.lo, widthThr, 0, b.hi - b.lo,
                   std::max(1, static_cast<int>(std::lround(0.020 * fs))), left, right);
        left += b.lo;
        right += b.lo;
        out.widthMs = 1000.0 * static_cast<double>(right - left) / fs;
        out.prMs = 1000.0 * static_cast<double>(b.r - left) / fs;
    }

    // ---- 4. 低质量搏：用邻搏模板补位（噪声大 / P 波被淹没时） ----
    const int half = std::max(1, p.templateNeighbors / 2);
    const int offLo = -static_cast<int>(std::lround(p.maxLookBack * fs));
    const int offHi = -static_cast<int>(std::lround((p.guardBeforeR + 0.012) * fs)); // 与实测窗一致
    const int len = offHi - offLo + 1;

    for (int i = 0; i < nb; ++i) {
        Beat& out = res.beats[static_cast<size_t>(i)];
        if (out.quality >= 2) continue;

        std::vector<double> tmpl(static_cast<size_t>(std::max(0, len)), 0.0);
        int used = 0;
        if (len > 8) {
            for (int j = std::max(0, i - half); j <= std::min(nb - 1, i + half); ++j) {
                if (j == i) continue;
                const Beat& ref = res.beats[static_cast<size_t>(j)];
                if (ref.quality < 2 || ref.source != Source::Measured) continue;
                const int rj = ref.rIndex;
                if (rj + offLo < 0 || rj + offHi >= n) continue;
                for (int k = 0; k < len; ++k) tmpl[static_cast<size_t>(k)] += sig[static_cast<size_t>(rj + offLo + k)];
                ++used;
            }
        }
        if (used >= 3) {
            for (double& v : tmpl) v /= used;
            const double tmed = medianCopy(tmpl);
            const double tSigma = madSigma(tmpl, tmed);
            int iMax = 0, iMin = 0;
            double vmax = -1e300, vmin = 1e300;
            for (int k = 0; k < len; ++k) {
                if (tmpl[static_cast<size_t>(k)] > vmax) { vmax = tmpl[static_cast<size_t>(k)]; iMax = k; }
                if (tmpl[static_cast<size_t>(k)] < vmin) { vmin = tmpl[static_cast<size_t>(k)]; iMin = k; }
            }
            const double aPos = vmax - tmed;
            const double aNeg = vmin - tmed;
            if (std::max(std::fabs(aPos), std::fabs(aNeg)) < std::max(p.minAmp, p.noiseFactor * tSigma)) {
                // 邻搏本身也没有明确 P 波（如房颤），不强行补位
                continue;
            }
            const bool posDominant = std::fabs(aPos) >= std::fabs(aNeg);
            out.pos.index = out.rIndex + offLo + iMax;
            out.pos.time = static_cast<double>(out.pos.index) / fs;
            out.pos.amp = aPos;
            out.neg.index = out.rIndex + offLo + iMin;
            out.neg.time = static_cast<double>(out.neg.index) / fs;
            out.neg.amp = aNeg;
            out.p = posDominant ? out.pos : out.neg;
            out.signedAmp = aPos + aNeg;
            out.snr = std::max(out.snr, 2.5);
            out.source = Source::Template;
            out.quality = 1;
            if (std::fabs(aPos) > 0.0 && std::fabs(aNeg) > 0.0
                && std::min(std::fabs(aPos), std::fabs(aNeg)) >= 0.5 * std::max(std::fabs(aPos), std::fabs(aNeg)))
                out.morph = Morph::Biphasic;
            else
                out.morph = (out.signedAmp >= 0.0) ? Morph::Positive : Morph::Negative;
            out.p.significant = true;
            out.widthMs = 0.0;
            out.prMs = 1000.0 * static_cast<double>(out.rIndex - out.p.index) / fs;
            ++res.nTemplate;
        } else if (out.morph == Morph::Invalid) {
            out.quality = 0;
        }
    }

    // ---- 5. 统计与极性翻转 ----
    std::vector<double> prs, widths;
    std::vector<int>    validIdx;
    std::vector<double> nets;
    for (int i = 0; i < nb; ++i) {
        const Beat& b = res.beats[static_cast<size_t>(i)];
        switch (b.morph) {
        case Morph::Positive: ++res.nPositive; break;
        case Morph::Negative: ++res.nNegative; break;
        case Morph::Biphasic: ++res.nBiphasic; break;
        case Morph::Flat: ++res.nFlat; break;
        default: ++res.nInvalid; break;
        }
        // 极性序列要包含低平搏：翻转恰恰发生在 P 波最不明显的位置，
        // 若把这些搏排除，过零点会被挤到后面去。
        if (b.morph != Morph::Invalid && b.p.index >= 0) {
            validIdx.push_back(i);
            nets.push_back(b.signedAmp);
        }
        if (b.quality >= 2 && b.morph != Morph::Flat && b.morph != Morph::Invalid) {
            if (b.prMs > 0.0) prs.push_back(b.prMs);
            if (b.widthMs > 0.0) widths.push_back(b.widthMs);
        }
    }
    res.meanPrMs = meanOf(prs);
    res.meanWidthMs = meanOf(widths);

    if (nets.size() >= 8) {
        // 9 点中值平滑，抑制单搏噪声
        std::vector<double> smooth(nets.size(), 0.0);
        for (size_t i = 0; i < nets.size(); ++i) {
            const size_t lo = (i >= 4) ? i - 4 : 0;
            const size_t hi = std::min(nets.size() - 1, i + 4);
            std::vector<double> wnd(nets.begin() + static_cast<long>(lo), nets.begin() + static_cast<long>(hi + 1));
            smooth[i] = medianInPlace(wnd);
        }
        std::vector<double> absv(smooth.size());
        for (size_t i = 0; i < smooth.size(); ++i) absv[i] = std::fabs(smooth[i]);
        const double thr = 0.25 * medianInPlace(absv);

        // 记录前后 1/4 的主极性必须相反，才认为发生了翻转
        const size_t q = std::max<size_t>(2, smooth.size() / 4);
        std::vector<double> head(smooth.begin(), smooth.begin() + static_cast<long>(q));
        std::vector<double> tail(smooth.end() - static_cast<long>(q), smooth.end());
        const double mHead = medianInPlace(head);
        const double mTail = medianInPlace(tail);

        // 前后段必须各自「极性明确」：中位幅值达到记录典型 P 幅值的 30%，
        // 且该段内至少 60% 的搏同号。否则只是噪声抖动，不算翻转。
        const auto posRatio = [](const std::vector<double>& v) {
            int pos = 0;
            for (double x : v)
                if (x > 0.0) ++pos;
            return v.empty() ? 0.5 : static_cast<double>(pos) / static_cast<double>(v.size());
        };
        // 阈值同时考虑记录典型幅值与噪声：翻转后 P 可能只剩 1/6，不能只按 ampRef 卡
        const double ampThr = std::max(1.5 * sigmaRef, 0.06 * ampRef);
        const bool headPos = mHead > 0.0;
        const bool headClear = std::fabs(mHead) >= ampThr
                               && (headPos ? posRatio(head) >= 0.6 : posRatio(head) <= 0.4);
        const bool tailClear = std::fabs(mTail) >= ampThr
                               && (headPos ? posRatio(tail) <= 0.4 : posRatio(tail) >= 0.6);

        if (thr > 0.0 && headClear && tailClear && ((mHead > 0.0) != (mTail > 0.0))) {
            const size_t begin = q;
            const size_t end = smooth.size() - q;
            for (size_t i = std::max<size_t>(1, begin); i < end; ++i) {
                if ((smooth[i - 1] > 0.0) != (smooth[i] > 0.0)) {
                    res.flipBeat = validIdx[i];
                    res.flipTime = res.beats[static_cast<size_t>(res.flipBeat)].rTime;
                    break;
                }
            }
        }
    }

    res.ok = true;
    return res;
}

} // namespace pw
