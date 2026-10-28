// P 波检测命令行工具：对一段信号逐搏输出 P 波主峰与正/负瓣峰，便于回归与参数调试。
// 用法: pwave_test <信号文件> [采样率] [R峰文件(每行一个秒数)] [输出CSV] [低切] [高切] [噪声倍数] [最小幅值]
#include "ecgsignal.h"
#include "pantompkins.h"
#include "pwave.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QStringConverter>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

std::vector<double> readTimes(const QString& path)
{
    std::vector<double> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("//")) continue;
        if (line.startsWith("r_time", Qt::CaseInsensitive)) continue;
        const QStringList parts = line.split(QRegularExpression("[,;\\t ]+"), Qt::SkipEmptyParts);
        bool ok = false;
        const double t = parts.value(0).toDouble(&ok);
        if (ok && t >= 0.0) out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    return out;
}

const char* morphName(pw::Morph m)
{
    switch (m) {
    case pw::Morph::Positive: return "正相";
    case pw::Morph::Negative: return "负相";
    case pw::Morph::Biphasic: return "双相";
    case pw::Morph::Flat: return "低平";
    default: return "无效";
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 2) {
        std::printf("用法: pwave_test <信号文件> [采样率] [R峰文件] [输出CSV] [低切] [高切] [噪声倍数] [最小幅值]\n");
        return 2;
    }

    const QString path = args.at(1);
    double fsArg = (args.size() > 2) ? args.at(2).toDouble() : 0.0;

    ecg::Data data;
    QString error;
    if (ecg::isBinaryPath(path) && !ecg::isAnnotatedFile(path)) {
        ecg::BinaryProbe probe;
        QString perr;
        if (!ecg::probeBinary(path, probe, perr)) {
            std::printf("二进制探测失败: %s\n", perr.toUtf8().constData());
            return 1;
        }
        ecg::BinaryOptions opt = probe.options;
        if (fsArg > 0.0) opt.sampleRate = fsArg;
        if (!ecg::loadBinary(path, opt, data, error)) {
            std::printf("读取失败: %s\n", error.toUtf8().constData());
            return 1;
        }
    } else if (!ecg::loadFile(path, data, error)) {
        std::printf("读取失败: %s\n", error.toUtf8().constData());
        return 1;
    }

    const double fs = fsArg > 0.0 ? fsArg : (data.fsGuess > 0.0 ? data.fsGuess : 500.0);
    const int col = data.signalColumn;
    const std::vector<double>& raw = data.columns[static_cast<size_t>(col)];

    // R 峰：优先用给定的标注文件，否则跑一次 Pan-Tompkins
    std::vector<int> rPeaks;
    if (args.size() > 3 && !args.at(3).isEmpty()) {
        const std::vector<double> times = readTimes(args.at(3));
        for (double t : times) {
            const int idx = static_cast<int>(std::lround(t * fs));
            if (idx >= 0 && idx < static_cast<int>(raw.size())) rPeaks.push_back(idx);
        }
    }
    bool autoDetected = false;
    if (rPeaks.empty()) {
        pt::Params pp;
        pp.fs = fs;
        const pt::Result r = pt::detect(raw, pp);
        rPeaks = r.rPeaks;
        autoDetected = true;
    }

    pw::Params p;
    p.fs = fs;
    if (args.size() > 5) p.lowCut = args.at(5).toDouble();
    if (args.size() > 6) p.highCut = args.at(6).toDouble();
    if (args.size() > 7) p.noiseFactor = args.at(7).toDouble();
    if (args.size() > 8) p.minAmp = args.at(8).toDouble();

    const pw::Result res = pw::detect(raw, rPeaks, p);

    std::printf("文件      : %s\n", path.toUtf8().constData());
    std::printf("采样率    : %.2f Hz, 通道 %d, 信号列 %d\n", fs, data.cols(), col + 1);
    std::printf("R 峰      : %zu (%s)\n", rPeaks.size(), autoDetected ? "算法检出" : "由标注文件给出");
    std::printf("P 波      : 正相 %d / 负相 %d / 双相 %d / 低平 %d / 无效 %d（模板补位 %d）\n",
                res.nPositive, res.nNegative, res.nBiphasic, res.nFlat, res.nInvalid, res.nTemplate);
    std::printf("平均 PR   : %.1f ms, 平均宽度 %.1f ms\n", res.meanPrMs, res.meanWidthMs);
    if (res.flipBeat >= 0)
        std::printf("极性翻转  : 第 %d 搏, R@%.3f s\n", res.flipBeat + 1, res.flipTime);
    else
        std::printf("极性翻转  : 未检出\n");

    if (args.size() > 4 && !args.at(4).isEmpty()) {
        QFile f(args.at(4));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            std::printf("无法写出 %s\n", args.at(4).toUtf8().constData());
            return 1;
        }
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts.setGenerateByteOrderMark(true);
        ts << "beat,r_idx,r_time,p_idx,p_time,p_amp,pos_idx,pos_time,pos_amp,neg_idx,neg_time,neg_amp,"
              "signed_amp,morph,polarity,width_ms,pr_ms,snr,quality,source\n";
        for (size_t i = 0; i < res.beats.size(); ++i) {
            const pw::Beat& b = res.beats[i];
            ts << static_cast<int>(i + 1) << ',' << b.rIndex << ','
               << QString::number(b.rTime, 'f', 4) << ','
               << b.p.index << ',' << QString::number(b.p.time, 'f', 4) << ','
               << QString::number(b.p.amp, 'f', 4) << ','
               << b.pos.index << ',' << QString::number(b.pos.time, 'f', 4) << ','
               << QString::number(b.pos.amp, 'f', 4) << ','
               << b.neg.index << ',' << QString::number(b.neg.time, 'f', 4) << ','
               << QString::number(b.neg.amp, 'f', 4) << ','
               << QString::number(b.signedAmp, 'f', 4) << ','
               << QString::fromUtf8(morphName(b.morph)) << ','
               << (b.signedAmp >= 0.0 ? "+" : "-") << ','
               << QString::number(b.widthMs, 'f', 1) << ','
               << QString::number(b.prMs, 'f', 1) << ','
               << QString::number(b.snr, 'f', 2) << ','
               << b.quality << ','
               << (b.source == pw::Source::Template ? "模板" : "实测") << '\n';
        }
        f.close();
        std::printf("逐搏结果  : %s\n", args.at(4).toUtf8().constData());
    }
    return 0;
}
