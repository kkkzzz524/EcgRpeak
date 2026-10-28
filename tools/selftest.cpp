// 命令行精度自测：把检测结果与人工标注的真实 R 峰时间对比
// 用法: rpeak_selftest <信号文件> [采样率] [标注文件]
//   - 支持文本 CSV/TXT 与二进制 .bin/.raw（二进制按说明文件 / 文本头 / 默认 int16 小端单通道）
//   - 采样率省略或为 0 时使用文件推断值；二进制文件可用第 2 个参数强制指定
//   - 标注文件每行第一个数值为 R 峰时间（秒）
#include "ecgsignal.h"
#include "pantompkins.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

std::vector<double> readAnnotations(const QString& path)
{
    std::vector<double> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("//")) continue;
        const QStringList parts = line.split(QRegularExpression("[,;\\t ]+"), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        bool ok = false;
        const double t = parts.first().toDouble(&ok);
        if (ok) out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 2) {
        std::printf("用法: rpeak_selftest <信号文件> [采样率] [标注文件]\n");
        return 2;
    }

    const QString signalPath = args.at(1);
    double fsArg = 0.0;
    if (args.size() > 2) fsArg = args.at(2).toDouble();

    ecg::Data data;
    QString error;
    QString binaryInfo;
    if (ecg::isBinaryPath(signalPath)) {
        ecg::BinaryProbe probe;
        QString probeError;
        if (!ecg::probeBinary(signalPath, probe, probeError)) {
            std::printf("二进制探测失败: %s\n", probeError.toLocal8Bit().constData());
            return 1;
        }
        ecg::BinaryOptions opt = probe.options;
        if (fsArg > 0.0) opt.sampleRate = fsArg;
        if (!ecg::loadBinary(signalPath, opt, data, error)) {
            std::printf("读取失败: %s\n", error.toLocal8Bit().constData());
            return 1;
        }
        binaryInfo = QStringLiteral("%1（来源：%2）").arg(opt.describe(), probe.source);
    } else if (!ecg::loadFile(signalPath, data, error)) {
        std::printf("读取失败: %s\n", error.toLocal8Bit().constData());
        return 1;
    }

    const double fs = fsArg > 0.0 ? fsArg : (data.fsGuess > 0.0 ? data.fsGuess : 360.0);
    const int col = data.signalColumn;

    pt::Params p;
    p.fs = fs;

    const std::vector<double>& sig = data.columns[static_cast<size_t>(col)];
    const pt::Result r = pt::detect(sig, p);

    std::printf("文件      : %s\n", signalPath.toLocal8Bit().constData());
    if (!binaryInfo.isEmpty())
        std::printf("二进制参数: %s\n", binaryInfo.toUtf8().constData());
    std::printf("采样点/列 : %d / %d\n", data.rows(), data.cols());
    std::printf("采样率    : %.2f Hz\n", fs);
    std::printf("信号列    : %d (%s)\n", col + 1,
                (col < data.headers.size() ? data.headers.at(col).toLocal8Bit().constData() : "未命名"));
    std::printf("R 峰数量  : %zu\n", r.rPeaks.size());
    std::printf("平均心率  : %.1f bpm (RR 均值 %.1f ms)\n", r.meanHr, r.meanRr * 1000.0);
    std::printf("SDNN/RMSSD: %.1f / %.1f ms, pNN50 %.1f%%\n",
                r.sdnn * 1000.0, r.rmssd * 1000.0, r.pnn50);

    int sbCount = 0;
    for (bool b : r.fromSearchBack) if (b) ++sbCount;
    std::printf("回溯补检  : %d\n", sbCount);

    if (args.size() > 3) {
        const std::vector<double> gt = readAnnotations(args.at(3));
        if (gt.empty()) {
            std::printf("警告: 标注文件为空，跳过精度评估\n");
            return 0;
        }

        const double tol = 0.10; // 100 ms 匹配容差（AAMI 常用 150 ms，这里更严格）
        std::vector<bool> gtHit(gt.size(), false);
        int tp = 0;
        double errSum = 0.0, errMax = 0.0;

        for (int idx : r.rPeaks) {
            const double t = idx / fs;
            int best = -1;
            double bestD = 1e9;
            for (size_t k = 0; k < gt.size(); ++k) {
                const double d = std::fabs(gt[k] - t);
                if (d < bestD) { bestD = d; best = static_cast<int>(k); }
            }
            if (best >= 0 && bestD <= tol && !gtHit[static_cast<size_t>(best)]) {
                gtHit[static_cast<size_t>(best)] = true;
                ++tp;
                errSum += bestD;
                errMax = std::max(errMax, bestD);
            }
        }

        const int fn = static_cast<int>(gt.size()) - tp;
        const int fp = static_cast<int>(r.rPeaks.size()) - tp;
        const double se = 100.0 * tp / static_cast<double>(gt.size());
        const double ppv = r.rPeaks.empty() ? 0.0 : 100.0 * tp / static_cast<double>(r.rPeaks.size());
        const double f1 = (se + ppv > 0.0) ? 2.0 * se * ppv / (se + ppv) : 0.0;

        std::printf("\n真实 R 峰 : %zu\n", gt.size());
        std::printf("TP/FN/FP  : %d / %d / %d\n", tp, fn, fp);
        std::printf("灵敏度 Se : %.2f%%\n", se);
        std::printf("阳性预测+: %.2f%%\n", ppv);
        std::printf("F1        : %.2f%%\n", f1);
        std::printf("定位误差  : 平均 %.1f ms, 最大 %.1f ms\n",
                    tp > 0 ? errSum / tp * 1000.0 : 0.0, errMax * 1000.0);
    }
    return 0;
}
