#include "pantompkins.h"
#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <climits>

namespace pt {
namespace {

// 二阶 IIR 与零相位滤波在 dsp.h 中，和 P 波检测共用同一套实现

double maxAbsSlope(const std::vector<double>& d, int center, int half)
{
    const int n = static_cast<int>(d.size());
    const int lo = std::max(0, center - half);
    const int hi = std::min(n - 1, center + half);
    double m = 0.0;
    for (int i = lo; i <= hi; ++i) m = std::max(m, std::fabs(d[i]));
    return m;
}

void computeStats(Result& r, double fs)
{
    r.rr.clear();
    r.instHr.clear();
    r.meanRr = 0.0;
    r.meanHr = 0.0;
    r.minHr = 0.0;
    r.maxHr = 0.0;
    r.sdnn = 0.0;
    r.rmssd = 0.0;
    r.pnn50 = 0.0;

    const auto& pk = r.rPeaks;
    if (pk.size() < 2) return;

    for (size_t i = 1; i < pk.size(); ++i)
        r.rr.push_back(static_cast<double>(pk[i] - pk[i - 1]) / fs);

    double sum = 0.0;
    for (double v : r.rr) sum += v;
    r.meanRr = sum / static_cast<double>(r.rr.size());
    if (r.meanRr > 0.0) r.meanHr = 60.0 / r.meanRr;

    if (r.rr.size() > 1) {
        double sq = 0.0;
        for (double v : r.rr) sq += (v - r.meanRr) * (v - r.meanRr);
        r.sdnn = std::sqrt(sq / static_cast<double>(r.rr.size() - 1));
    }

    double dsq = 0.0;
    int cnt = 0, nn50 = 0;
    for (size_t i = 1; i < r.rr.size(); ++i) {
        const double d = r.rr[i] - r.rr[i - 1];
        dsq += d * d;
        ++cnt;
        if (std::fabs(d) > 0.050) ++nn50;
    }
    if (cnt > 0) {
        r.rmssd = std::sqrt(dsq / static_cast<double>(cnt));
        r.pnn50 = 100.0 * nn50 / cnt;
    }

    for (double v : r.rr) r.instHr.push_back(v > 0.0 ? 60.0 / v : 0.0);

    r.minHr = 1e9;
    r.maxHr = 0.0;
    for (double v : r.instHr) {
        r.minHr = std::min(r.minHr, v);
        r.maxHr = std::max(r.maxHr, v);
    }
    if (r.minHr > 1e8) r.minHr = 0.0;
}

} // namespace

void recomputeStats(Result& r, double fs)
{
    computeStats(r, fs);
}

Result detect(const std::vector<double>& raw, const Params& pIn)
{
    Result res;
    const int n = static_cast<int>(raw.size());
    if (n < 16) return res;

    // ---- 参数合法化 ----
    Params p = pIn;
    const double fs = std::max(1.0, p.fs);
    p.fs = fs;
    p.lowCut = std::clamp(p.lowCut, 0.2, 0.45 * fs);
    p.highCut = std::clamp(p.highCut, p.lowCut + 0.5, 0.48 * fs);
    p.integrationWindow = std::clamp(p.integrationWindow, 0.020, 0.500);
    p.refractory = std::clamp(p.refractory, 0.050, 1.000);
    p.tWaveWindow = std::clamp(p.tWaveWindow, p.refractory, 1.000);
    p.thresholdWeight = std::clamp(p.thresholdWeight, 0.010, 0.950);
    p.refineWindow = std::clamp(p.refineWindow, 0.020, 0.250);

    // ---- 1. 带通滤波 5~15 Hz ----
    std::vector<double> sig = raw;
    const dsp::Biquad hp = dsp::makeHighPass(p.lowCut, fs);
    const dsp::Biquad lp = dsp::makeLowPass(p.highCut, fs);
    if (p.zeroPhase) {
        dsp::filtfilt(sig, hp, fs);
        dsp::filtfilt(sig, lp, fs);
    } else {
        dsp::Biquad a = hp, b = lp;
        for (double& v : sig) v = b.step(a.step(v));
    }
    res.filtered = sig;

    // ---- 2. 五点微分 ----
    std::vector<double> deriv(static_cast<size_t>(n), 0.0);
    for (int i = 4; i < n; ++i)
        deriv[static_cast<size_t>(i)] =
            (2.0 * sig[i] + sig[i - 1] - sig[i - 3] - 2.0 * sig[i - 4]) / 8.0;
    res.derivative = deriv;

    // ---- 3. 平方（全部转为正向能量）----
    std::vector<double> sq(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) sq[static_cast<size_t>(i)] = deriv[static_cast<size_t>(i)] * deriv[static_cast<size_t>(i)];
    res.squared = sq;

    // ---- 4. 移动窗积分（窗长 150 ms）----
    const int win = std::max(1, static_cast<int>(std::lround(p.integrationWindow * fs)));
    std::vector<double> integ(static_cast<size_t>(n), 0.0);
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        acc += sq[static_cast<size_t>(i)];
        if (i >= win) acc -= sq[static_cast<size_t>(i - win)];
        integ[static_cast<size_t>(i)] = acc / static_cast<double>(win);
    }
    res.integrated = integ;

    // ---- 5. 积分信号的局部极大值 ----
    std::vector<int> maxima;
    maxima.reserve(static_cast<size_t>(n / 8 + 8));
    for (int i = 1; i < n - 1; ++i) {
        if (integ[static_cast<size_t>(i)] > integ[static_cast<size_t>(i - 1)] &&
            integ[static_cast<size_t>(i)] >= integ[static_cast<size_t>(i + 1)])
            maxima.push_back(i);
    }
    if (maxima.empty()) return res;

    // ---- 6. 自适应双阈值检测 ----
    const int refrN = std::max(1, static_cast<int>(std::lround(p.refractory * fs)));
    const int tWinN = std::max(refrN, static_cast<int>(std::lround(p.tWaveWindow * fs)));
    const int slopeHalf = std::max(2, static_cast<int>(std::lround(0.050 * fs)));

    const int learnEnd = std::min(n - 1, static_cast<int>(std::lround(2.0 * fs)));
    double learnMax = 0.0;
    for (int i = 0; i <= learnEnd; ++i) learnMax = std::max(learnMax, integ[static_cast<size_t>(i)]);
    if (learnMax <= 0.0) return res;

    double spki = learnMax;             // 信号峰估计
    double npki = 0.5 * learnMax;       // 噪声峰估计

    std::vector<int>    cand;           // 候选（积分峰）
    std::vector<double> candAmp;
    std::vector<double> candSlope;
    std::vector<bool>   candSb;
    std::vector<std::pair<int, double>> thrSamples; // (样点, 阈值)

    for (int m : maxima) {
        const double amp = integ[static_cast<size_t>(m)];
        const double thr = npki + p.thresholdWeight * (spki - npki);
        thrSamples.emplace_back(m, thr);

        const bool hasPrev = !cand.empty();
        const int  rrN = hasPrev ? (m - cand.back()) : INT_MAX;
        const double slope = maxAbsSlope(deriv, m, slopeHalf);

        if (amp <= thr) {
            npki = 0.125 * amp + 0.875 * npki;   // 更新噪声电平
            continue;
        }

        // 不应期内的峰：不可能是有效 R 波，只更新噪声电平
        if (hasPrev && rrN < refrN) {
            npki = 0.125 * amp + 0.875 * npki;
            continue;
        }

        // T 波判别：靠得太近且上升斜率明显偏小 → 判为 T 波
        if (hasPrev && rrN < tWinN && slope < p.tWaveFactor * candSlope.back()) {
            npki = 0.125 * amp + 0.875 * npki;
            continue;
        }

        spki = 0.125 * amp + 0.875 * spki;
        cand.push_back(m);
        candAmp.push_back(amp);
        candSlope.push_back(slope);
        candSb.push_back(false);
    }

    if (cand.empty()) {
        res.thresholdTrack.assign(static_cast<size_t>(n), 0.0);
        return res;
    }

    // ---- 7. 回溯搜索：RR 异常增大说明漏检 ----
    if (cand.size() >= 2) {
        double rrSum = 0.0;
        int rrCnt = 0;
        for (size_t i = 1; i < cand.size(); ++i) {
            const double rr = static_cast<double>(cand[i] - cand[i - 1]) / fs;
            if (rr < 2.5) { rrSum += rr; ++rrCnt; }
        }
        const double rrMean = rrCnt > 0 ? rrSum / rrCnt : 0.0;

        if (rrMean > 0.0) {
            const double thrNow = npki + p.thresholdWeight * (spki - npki);
            const double thr2 = 0.5 * thrNow;
            std::vector<std::pair<int, bool>> extra;

            for (size_t i = 1; i < cand.size(); ++i) {
                const double gap = static_cast<double>(cand[i] - cand[i - 1]) / fs;
                if (gap <= p.searchBackFactor * rrMean) continue;

                const int lo = cand[i - 1] + refrN;
                const int hi = cand[i] - refrN;
                if (hi <= lo + 1) continue;

                int best = -1;
                double bestAmp = 0.0;
                for (int k = lo; k <= hi; ++k) {
                    const double v = integ[static_cast<size_t>(k)];
                    if (v > bestAmp) { bestAmp = v; best = k; }
                }
                if (best > 0 && bestAmp > thr2 && bestAmp > 0.0)
                    extra.emplace_back(best, true);
            }

            for (const auto& e : extra) {
                const int pos = static_cast<int>(
                    std::lower_bound(cand.begin(), cand.end(), e.first) - cand.begin());
                if (pos > 0 && pos < static_cast<int>(cand.size())) {
                    // 避免与已有峰重合
                    if (cand[static_cast<size_t>(pos)] - cand[static_cast<size_t>(pos - 1)] < refrN) continue;
                }
                cand.insert(cand.begin() + pos, e.first);
                candAmp.insert(candAmp.begin() + pos, integ[static_cast<size_t>(e.first)]);
                candSlope.insert(candSlope.begin() + pos, maxAbsSlope(deriv, e.first, slopeHalf));
                candSb.insert(candSb.begin() + pos, true);
            }
        }
    }

    // ---- 8. R 峰精定位：在滤波信号上取局部极值 ----
    // 极性用分位数判断，避免个别饱和伪迹样本把整段带偏；
    // 正负相当（≥75%）时优先取正向顶点，让标记落在 R 波顶而不是 S 波谷。
    double upExc = 0.0, downExc = 0.0;
    {
        std::vector<double> sorted = sig;
        const size_t hi = static_cast<size_t>(0.995 * static_cast<double>(sorted.size() - 1));
        const size_t lo = static_cast<size_t>(0.005 * static_cast<double>(sorted.size() - 1));
        std::nth_element(sorted.begin(), sorted.begin() + hi, sorted.end());
        upExc = sorted[hi];
        std::nth_element(sorted.begin(), sorted.begin() + lo, sorted.end());
        downExc = -sorted[lo];
    }
    const double polarity = (upExc >= 0.75 * downExc) ? 1.0 : -1.0;
    const int half = std::max(1, static_cast<int>(std::lround(p.refineWindow * fs)));

    std::vector<int>    beats;
    std::vector<bool>   beatSb;
    std::vector<double> beatAmp;
    beats.reserve(cand.size());

    for (size_t k = 0; k < cand.size(); ++k) {
        const int c = cand[k];
        int lo = std::max(0, c - half);
        int hi = std::min(n - 1, c + half);
        if (k > 0) lo = std::max(lo, (cand[k - 1] + c) / 2 + 1);
        if (k + 1 < cand.size()) hi = std::min(hi, (c + cand[k + 1]) / 2);

        int best = c;
        double bv = -1e300;
        for (int i = lo; i <= hi; ++i) {
            const double v = polarity * sig[static_cast<size_t>(i)];
            if (v > bv) { bv = v; best = i; }
        }
        if (!beats.empty() && best <= beats.back()) best = beats.back() + 1;
        if (best >= n) break;
        beats.push_back(best);
        beatSb.push_back(candSb[k]);
        beatAmp.push_back(candAmp[k]);
    }

    // 精定位的超前/滞后可能把相邻两峰拉到一起（饱和平台或强伪迹处），
    // 这里强制不应期约束：冲突时保留能量更强的那个，保证 RR 不会短于不应期。
    if (beats.size() > 1) {
        std::vector<int>    fb;
        std::vector<bool>   fsb;
        std::vector<double> famp;
        fb.reserve(beats.size());
        for (size_t i = 0; i < beats.size(); ++i) {
            if (!fb.empty() && beats[i] - fb.back() < refrN) {
                if (beatAmp[i] > famp.back()) {
                    fb.back() = beats[i];
                    fsb.back() = beatSb[i];
                    famp.back() = beatAmp[i];
                }
                continue;
            }
            fb.push_back(beats[i]);
            fsb.push_back(beatSb[i]);
            famp.push_back(beatAmp[i]);
        }
        beats.swap(fb);
        beatSb.swap(fsb);
        beatAmp.swap(famp);
    }

    res.rPeaks = beats;
    res.fromSearchBack = beatSb;
    res.rAmp = beatAmp;

    // ---- 9. 阈值轨迹（用于可视化）----
    res.thresholdTrack.assign(static_cast<size_t>(n), 0.0);
    {
        double cur = thrSamples.empty() ? 0.0 : thrSamples.front().second;
        size_t k = 0;
        for (int i = 0; i < n; ++i) {
            while (k < thrSamples.size() && thrSamples[k].first <= i) {
                cur = thrSamples[k].second;
                ++k;
            }
            res.thresholdTrack[static_cast<size_t>(i)] = cur;
        }
    }

    computeStats(res, fs);
    res.ok = true;
    return res;
}

} // namespace pt
