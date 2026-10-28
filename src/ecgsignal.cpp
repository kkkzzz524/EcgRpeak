#include "ecgsignal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace ecg {
namespace {

constexpr double kPi = 3.14159265358979323846;

QStringList tokenize(const QString& line)
{
    static const QRegularExpression re(QStringLiteral("[,;\\t|\\s]+"));
    return line.split(re, Qt::SkipEmptyParts);
}

bool isNumeric(const QString& s)
{
    bool ok = false;
    s.trimmed().toDouble(&ok);
    return ok;
}

// 从注释行里找 fs = 360 / 采样率 360Hz 之类的提示
double parseFsHint(const QString& line)
{
    static const QRegularExpression reFs(
        QStringLiteral("(?:fs|FS|采样率|频率)\\s*[=:：]?\\s*([0-9]+(?:\\.[0-9]+)?)"));
    static const QRegularExpression reHz(
        QStringLiteral("([0-9]+(?:\\.[0-9]+)?)\\s*(?:Hz|hz|HZ)"));

    QRegularExpressionMatch m = reFs.match(line);
    if (!m.hasMatch()) m = reHz.match(line);
    if (!m.hasMatch()) return 0.0;
    const double v = m.captured(1).toDouble();
    return (v > 20.0 && v < 20000.0) ? v : 0.0;
}

double fsFromTimeColumn(const std::vector<double>& t)
{
    if (t.size() < 10) return 0.0;
    double sum = 0.0, mn = 1e300, mx = -1e300;
    int cnt = 0;
    for (size_t i = 1; i < t.size(); ++i) {
        const double d = t[i] - t[i - 1];
        if (!(d > 0.0)) return 0.0;
        sum += d;
        mn = std::min(mn, d);
        mx = std::max(mx, d);
        ++cnt;
    }
    if (cnt == 0) return 0.0;
    const double mean = sum / cnt;
    if (mean <= 0.0) return 0.0;
    if ((mx - mn) > 0.25 * mean) return 0.0; // 间隔不均匀，不像时间轴
    const double fs = 1.0 / mean;
    return (fs > 20.0 && fs < 5000.0) ? fs : 0.0;
}

bool looksLikeTimeHeader(const QString& h)
{
    const QString s = h.trimmed().toLower();
    return s.contains(QStringLiteral("time")) || s.contains(QStringLiteral("时间"))
        || s == QStringLiteral("t") || s.contains(QStringLiteral("sec"))
        || s.contains(QStringLiteral("stamp")) || s.contains(QStringLiteral("t_s"));
}

double stdDev(const std::vector<double>& v)
{
    if (v.size() < 2) return 0.0;
    double mean = 0.0;
    for (double x : v) mean += x;
    mean /= static_cast<double>(v.size());
    double s = 0.0;
    for (double x : v) s += (x - mean) * (x - mean);
    return std::sqrt(s / static_cast<double>(v.size() - 1));
}

void addGaussian(std::vector<double>& v, double fs, double center, double sigma, double amp)
{
    int lo = static_cast<int>(std::floor((center - 4.0 * sigma) * fs));
    int hi = static_cast<int>(std::ceil((center + 4.0 * sigma) * fs));
    lo = std::max(0, lo);
    hi = std::min(static_cast<int>(v.size()) - 1, hi);
    for (int i = lo; i <= hi; ++i) {
        const double dt = static_cast<double>(i) / fs - center;
        v[static_cast<size_t>(i)] += amp * std::exp(-0.5 * dt * dt / (sigma * sigma));
    }
}

} // namespace

bool loadFile(const QString& path, Data& out, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("无法打开文件：%1").arg(file.errorString());
        return false;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    out = Data();
    out.filePath = path;

    bool headerSet = false;
    double fsHint = 0.0;
    int expectedCols = -1;

    while (!in.atEnd()) {
        const QString raw = in.readLine();
        const QString line = raw.trimmed();
        if (line.isEmpty()) continue;

        if (line.startsWith(QLatin1Char('#')) || line.startsWith(QStringLiteral("//"))) {
            if (fsHint <= 0.0) fsHint = parseFsHint(line);
            continue;
        }

        const QStringList tokens = tokenize(line);
        if (tokens.isEmpty()) continue;

        bool allNumeric = true;
        for (const QString& t : tokens) {
            if (!isNumeric(t)) { allNumeric = false; break; }
        }

        // 首个非数值行视作表头
        if (!headerSet && !allNumeric) {
            out.headers = tokens;
            headerSet = true;
            continue;
        }
        if (!allNumeric) continue;

        if (expectedCols < 0) {
            expectedCols = static_cast<int>(tokens.size());
            out.columns.assign(static_cast<size_t>(expectedCols), {});
            for (auto& c : out.columns) c.reserve(4096);
        }
        if (static_cast<int>(tokens.size()) != expectedCols) continue;

        for (int c = 0; c < expectedCols; ++c)
            out.columns[static_cast<size_t>(c)].push_back(tokens[c].trimmed().toDouble());
    }

    if (out.columns.empty() || out.rows() < 8) {
        error = QStringLiteral("文件中没有找到足够的数值数据（至少需要 8 个采样点）。");
        return false;
    }

    // ---- 采样率推断：注释提示 > 时间列 ----
    double fs = fsHint;
    out.timeColumn = -1;
    for (int c = 0; c < out.cols(); ++c) {
        if (c >= out.headers.size()) continue;
        if (!looksLikeTimeHeader(out.headers.at(c))) continue;
        const double guess = fsFromTimeColumn(out.columns[static_cast<size_t>(c)]);
        if (guess > 0.0) {
            out.timeColumn = c;
            if (fs <= 0.0) fs = guess;
            break;
        }
    }

    // 无表头文件：若首列严格等间隔递增，按时间列处理
    if (out.timeColumn < 0 && out.headers.isEmpty() && out.cols() >= 2) {
        const double guess = fsFromTimeColumn(out.columns.front());
        if (guess > 0.0) {
            out.timeColumn = 0;
            if (fs <= 0.0) fs = guess;
        }
    }
    if (fs > 0.0) out.fsGuess = fs;

    // ---- 建议信号列：非时间列且方差最大 ----
    int best = -1;
    double bestStd = -1.0;
    for (int c = 0; c < out.cols(); ++c) {
        if (c == out.timeColumn) continue;
        const double sd = stdDev(out.columns[static_cast<size_t>(c)]);
        if (sd > bestStd) { bestStd = sd; best = c; }
    }
    out.signalColumn = (best >= 0) ? best : 0;
    out.loaded = true;
    return true;
}

// =====================================================================
//                        二进制原始数据
// =====================================================================

namespace {

constexpr qint64 kMaxFrames = 30000000; // 单文件最多 3 千万帧，避免一次吃光内存

QString normalizeKey(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        if (c.isLetterOrNumber()) out.append(c.toLower());
    }
    return out;
}

bool parseEndianValue(const QString& text, bool& little)
{
    const QString t = normalizeKey(text);
    if (t.isEmpty()) return false;
    if (t == QLatin1String("little") || t == QLatin1String("le") || t == QLatin1String("lsb")
        || t == QLatin1String("lsbfirst") || t == QLatin1String("intel")
        || t == QStringLiteral("小端") || t == QStringLiteral("低字节在前")) {
        little = true;
        return true;
    }
    if (t == QLatin1String("big") || t == QLatin1String("be") || t == QLatin1String("msb")
        || t == QLatin1String("msbfirst") || t == QLatin1String("motorola")
        || t == QStringLiteral("大端") || t == QStringLiteral("高字节在前")) {
        little = false;
        return true;
    }
    return false;
}

// 解析 "fs=360" / "采样率: 360 Hz" / "# format = int16" 之类的键值行
bool parseKeyValue(const QString& line, QString& key, QString& value)
{
    QString s = line.trimmed();
    while (!s.isEmpty() && (s.startsWith(QLatin1Char('#')) || s.startsWith(QLatin1Char(';'))
                           || s.startsWith(QLatin1String("//")))) {
        s = s.mid(s.startsWith(QLatin1String("//")) ? 2 : 1).trimmed();
    }
    int sep = -1;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('=') || c == QLatin1Char(':') || c == QChar(0xFF1A)) {
            sep = i;
            break;
        }
    }
    if (sep <= 0) return false;
    key = s.left(sep).trimmed();
    value = s.mid(sep + 1).trimmed();
    const int cut = value.indexOf(QLatin1Char('#'));
    if (cut > 0) value = value.left(cut).trimmed();
    // 去掉常见单位后缀，仅保留数值部分交给数字解析
    return !key.isEmpty() && !value.isEmpty();
}

bool applyBinaryKeyValue(const QString& rawKey, const QString& rawValue, BinaryOptions& opt)
{
    const QString k = normalizeKey(rawKey);
    const QString v = rawValue.trimmed();
    if (k.isEmpty() || v.isEmpty()) return false;

    bool ok = false;
    if (k == QLatin1String("fs") || k == QLatin1String("samplerate")
        || k == QLatin1String("samplingrate") || k == QLatin1String("rate")
        || k == QLatin1String("frequency") || k == QStringLiteral("采样率")
        || k == QStringLiteral("采样频率") || k == QStringLiteral("频率")) {
        const double d = QString(v).remove(QRegularExpression(QStringLiteral("[^0-9.]+"))).toDouble(&ok);
        if (ok && d > 1.0 && d < 200000.0) {
            opt.sampleRate = d;
            return true;
        }
        return false;
    }
    if (k == QLatin1String("format") || k == QLatin1String("dtype") || k == QLatin1String("datatype")
        || k == QLatin1String("sampleformat") || k == QLatin1String("type")
        || k == QStringLiteral("数据类型") || k == QStringLiteral("格式")) {
        SampleFormat f;
        if (sampleFormatFromString(v, f)) {
            opt.format = f;
            return true;
        }
        return false;
    }
    if (k == QLatin1String("layout") || k == QLatin1String("packing") || k == QLatin1String("packed")
        || k == QLatin1String("dataformat") || k == QStringLiteral("布局") || k == QStringLiteral("打包")) {
        BinaryLayout l;
        if (binaryLayoutFromString(v, l)) {
            opt.layout = l;
            return true;
        }
        return false;
    }
    if (k == QLatin1String("endian") || k == QLatin1String("endianness")
        || k == QLatin1String("byteorder") || k == QStringLiteral("字节序")) {
        bool little = true;
        if (parseEndianValue(v, little)) {
            opt.littleEndian = little;
            return true;
        }
        return false;
    }
    if (k == QLatin1String("channels") || k == QLatin1String("channel")
        || k == QLatin1String("channelcount") || k == QLatin1String("nchannels")
        || k == QLatin1String("nch") || k == QLatin1String("leads") || k == QLatin1String("leadcount")
        || k == QStringLiteral("通道数") || k == QStringLiteral("导联数")) {
        const double d = QString(v).remove(QRegularExpression(QStringLiteral("[^0-9]+"))).toDouble(&ok);
        if (ok && d >= 1.0 && d <= 64.0) {
            opt.channels = static_cast<int>(d);
            return true;
        }
        return false;
    }
    if (k == QLatin1String("offset") || k == QLatin1String("headerbytes")
        || k == QLatin1String("headersize") || k == QLatin1String("header")
        || k == QLatin1String("startbyte") || k == QLatin1String("dataoffset")
        || k == QStringLiteral("偏移") || k == QStringLiteral("文件头")) {
        const double d = QString(v).remove(QRegularExpression(QStringLiteral("[^0-9.]+"))).toDouble(&ok);
        if (ok && d >= 0.0 && d < 1e9) {
            opt.headerBytes = static_cast<qint64>(d);
            return true;
        }
        return false;
    }
    if (k == QLatin1String("scale") || k == QLatin1String("gain") || k == QLatin1String("lsb")
        || k == QLatin1String("resolution") || k == QLatin1String("lsbmv")
        || k == QStringLiteral("缩放") || k == QStringLiteral("增益") || k == QStringLiteral("分辨率")) {
        const double d = v.toDouble(&ok);
        if (ok && d > 0.0 && d < 1e9) {
            opt.scale = d;
            return true;
        }
        return false;
    }
    if (k == QLatin1String("bias") || k == QLatin1String("baseline")
        || k == QLatin1String("zero") || k == QStringLiteral("偏置") || k == QStringLiteral("零点")) {
        const double d = v.toDouble(&ok);
        if (ok && std::fabs(d) < 1e9) {
            opt.bias = d;
            return true;
        }
        return false;
    }
    return false;
}

bool readSidecarJson(const QString& file, BinaryOptions& opt, int& recognized)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) return false;

    QJsonObject root = doc.object();
    QJsonObject obj = root;
    if (root.value(QStringLiteral("ecg")).isObject()) obj = root.value(QStringLiteral("ecg")).toObject();
    else if (root.value(QStringLiteral("data")).isObject()) obj = root.value(QStringLiteral("data")).toObject();

    recognized = 0;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QJsonValue v = it.value();
        QString text;
        if (v.isString()) text = v.toString();
        else if (v.isDouble()) text = QString::number(v.toDouble(), 'g', 15);
        else if (v.isBool()) text = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else continue;
        if (applyBinaryKeyValue(it.key(), text, opt)) ++recognized;
    }
    return recognized > 0;
}

bool readSidecarMeta(const QString& file, BinaryOptions& opt, int& recognized)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    recognized = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        QString key, value;
        if (parseKeyValue(line, key, value) && applyBinaryKeyValue(key, value, opt)) ++recognized;
    }
    return recognized > 0;
}

// 文件开头的纯文本说明头，返回头部字节数
bool parseEmbeddedHeader(const QByteArray& head, BinaryOptions& opt, qint64& headerBytes, int& recognized)
{
    headerBytes = 0;
    recognized = 0;
    if (head.size() < 2) return false;
    const uchar first = static_cast<uchar>(head.at(0));
    const bool printableStart = (first >= 0x20 && first < 0x7F) || first == '\t' || first == '\r';
    if (!printableStart) return false;

    qint64 consumed = 0;
    int lines = 0;
    while (consumed < head.size() && lines < 48) {
        const int nl = head.indexOf('\n', static_cast<int>(consumed));
        if (nl < 0) break;
        const QByteArray raw = head.mid(static_cast<int>(consumed), nl - static_cast<int>(consumed));
        if (raw.size() > 400) break;
        bool text = true;
        for (const char c : raw) {
            const uchar u = static_cast<uchar>(c);
            if (u == '\t' || u == '\r') continue;
            if (u < 0x20 || u > 0x7E) { text = false; break; }
        }
        if (!text) break;

        const QString line = QString::fromLatin1(raw).trimmed();
        if (!line.isEmpty()) {
            QString key, value;
            if (parseKeyValue(line, key, value) && applyBinaryKeyValue(key, value, opt)) ++recognized;
        }
        consumed = nl + 1;
        ++lines;
    }
    if (recognized == 0) return false;
    headerBytes = consumed;
    return true;
}

inline quint32 readU32(const uchar* p, bool little)
{
    return little ? (quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24))
                  : ((quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]));
}

inline double decodeSample(const uchar* p, SampleFormat f, bool little)
{
    switch (f) {
    case SampleFormat::Int8:
        return static_cast<double>(static_cast<qint8>(p[0]));
    case SampleFormat::UInt8:
        return static_cast<double>(p[0]);
    case SampleFormat::Int16: {
        const quint32 u = little ? (quint32(p[0]) | (quint32(p[1]) << 8))
                                 : ((quint32(p[0]) << 8) | quint32(p[1]));
        return static_cast<double>(static_cast<qint16>(u & 0xFFFFu));
    }
    case SampleFormat::UInt16: {
        const quint32 u = little ? (quint32(p[0]) | (quint32(p[1]) << 8))
                                 : ((quint32(p[0]) << 8) | quint32(p[1]));
        return static_cast<double>(u & 0xFFFFu);
    }
    case SampleFormat::Int32: {
        const quint32 u = readU32(p, little);
        qint32 v = 0;
        std::memcpy(&v, &u, sizeof(v));
        return static_cast<double>(v);
    }
    case SampleFormat::Float32: {
        const quint32 u = readU32(p, little);
        float v = 0.0f;
        std::memcpy(&v, &u, sizeof(v));
        return static_cast<double>(v);
    }
    case SampleFormat::Float64: {
        quint64 u = 0;
        if (little) {
            for (int i = 7; i >= 0; --i) u = (u << 8) | quint64(p[i]);
        } else {
            for (int i = 0; i < 8; ++i) u = (u << 8) | quint64(p[i]);
        }
        double v = 0.0;
        std::memcpy(&v, &u, sizeof(v));
        return v;
    }
    }
    return 0.0;
}

struct QuickStats {
    int n = 0;
    int nonFinite = 0;
    double sd = 0.0;
    double maxAbs = 0.0;
    double fractional = 0.0;
};

// 判断是否像「两个 12 位通道打包进 int32」：高/低 16 位都落在 0..4095，
// 且两条序列都足够平滑（相邻帧变化远小于整体起伏），这是监护仪常见的存储方式。
bool looksPacked12(const QByteArray& head, bool little)
{
    const int n = static_cast<int>(head.size() / 4);
    if (n < 64) return false;

    const uchar* p = reinterpret_cast<const uchar*>(head.constData());
    int bad = 0;
    int used = 0;
    double s1 = 0.0, q1 = 0.0, s2 = 0.0, q2 = 0.0, d1 = 0.0, d2 = 0.0;
    int prev1 = -1, prev2 = -1;

    for (int i = 0; i < n; ++i, p += 4) {
        const quint32 u = readU32(p, little);
        const int hi = static_cast<int>(u >> 16);
        const int lo = static_cast<int>(u & 0xFFFFu);
        if (hi > 4095 || lo > 4095) {
            if (++bad * 200 > n) return false; // 超过 0.5% 越界即否决
            continue;
        }
        ++used;
        s1 += hi; q1 += double(hi) * hi;
        s2 += lo; q2 += double(lo) * lo;
        if (prev1 >= 0) {
            d1 += std::abs(hi - prev1);
            d2 += std::abs(lo - prev2);
        }
        prev1 = hi;
        prev2 = lo;
    }
    if (used * 2 < n) return false;

    const auto sd = [used](double s, double q) {
        const double m = s / used;
        return std::sqrt(std::max(0.0, q / used - m * m));
    };
    const double sd1 = sd(s1, q1);
    const double sd2 = sd(s2, q2);
    if (sd1 < 8.0 || sd2 < 8.0) return false;
    const double denom = std::max(1, used - 1);
    return (d1 / denom) < 0.5 * sd1 && (d2 / denom) < 0.5 * sd2;
}

QuickStats quickStats(const QByteArray& buf, SampleFormat fmt, bool little, int maxFrames)
{
    QuickStats s;
    const int bps = sampleFormatBytes(fmt);
    if (bps <= 0) return s;
    const int n = std::min(maxFrames, static_cast<int>(buf.size() / bps));
    if (n <= 0) return s;

    const uchar* base = reinterpret_cast<const uchar*>(buf.constData());
    double sum = 0.0, sum2 = 0.0;
    int frac = 0;
    for (int i = 0; i < n; ++i) {
        const double v = decodeSample(base + static_cast<qint64>(i) * bps, fmt, little);
        if (!std::isfinite(v)) { ++s.nonFinite; continue; }
        sum += v;
        sum2 += v * v;
        s.maxAbs = std::max(s.maxAbs, std::fabs(v));
        if (v != std::floor(v)) ++frac;
    }
    s.n = n;
    const double mean = sum / n;
    s.sd = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
    s.fractional = static_cast<double>(frac) / n;
    return s;
}

} // namespace

QString sampleFormatShortName(SampleFormat f)
{
    switch (f) {
    case SampleFormat::Int8: return QStringLiteral("int8");
    case SampleFormat::UInt8: return QStringLiteral("uint8");
    case SampleFormat::Int16: return QStringLiteral("int16");
    case SampleFormat::UInt16: return QStringLiteral("uint16");
    case SampleFormat::Int32: return QStringLiteral("int32");
    case SampleFormat::Float32: return QStringLiteral("float32");
    case SampleFormat::Float64: return QStringLiteral("float64");
    }
    return QStringLiteral("int16");
}

QString sampleFormatLongName(SampleFormat f)
{
    switch (f) {
    case SampleFormat::Int8: return QStringLiteral("int8 · 8 位有符号");
    case SampleFormat::UInt8: return QStringLiteral("uint8 · 8 位无符号");
    case SampleFormat::Int16: return QStringLiteral("int16 · 16 位有符号（常用）");
    case SampleFormat::UInt16: return QStringLiteral("uint16 · 16 位无符号");
    case SampleFormat::Int32: return QStringLiteral("int32 · 32 位有符号");
    case SampleFormat::Float32: return QStringLiteral("float32 · 32 位浮点");
    case SampleFormat::Float64: return QStringLiteral("float64 · 64 位浮点");
    }
    return QString();
}

int sampleFormatBytes(SampleFormat f)
{
    switch (f) {
    case SampleFormat::Int8:
    case SampleFormat::UInt8: return 1;
    case SampleFormat::Int16:
    case SampleFormat::UInt16: return 2;
    case SampleFormat::Int32:
    case SampleFormat::Float32: return 4;
    case SampleFormat::Float64: return 8;
    }
    return 2;
}

bool sampleFormatFromString(const QString& text, SampleFormat& out)
{
    const QString t = normalizeKey(text);
    if (t.isEmpty()) return false;
    if (t == QLatin1String("int8") || t == QLatin1String("i8") || t == QLatin1String("s8")
        || t == QLatin1String("char") || t == QLatin1String("signed8")) {
        out = SampleFormat::Int8;
        return true;
    }
    if (t == QLatin1String("uint8") || t == QLatin1String("u8") || t == QLatin1String("uchar")
        || t == QLatin1String("unsigned8") || t == QLatin1String("byte")) {
        out = SampleFormat::UInt8;
        return true;
    }
    if (t == QLatin1String("int16") || t == QLatin1String("i16") || t == QLatin1String("s16")
        || t == QLatin1String("short") || t == QLatin1String("int16le") || t == QLatin1String("int16be")) {
        out = SampleFormat::Int16;
        return true;
    }
    if (t == QLatin1String("uint16") || t == QLatin1String("u16") || t == QLatin1String("ushort")
        || t == QLatin1String("word")) {
        out = SampleFormat::UInt16;
        return true;
    }
    if (t == QLatin1String("int32") || t == QLatin1String("i32") || t == QLatin1String("s32")
        || t == QLatin1String("int") || t == QLatin1String("long")) {
        out = SampleFormat::Int32;
        return true;
    }
    if (t == QLatin1String("float32") || t == QLatin1String("f32") || t == QLatin1String("float")
        || t == QLatin1String("single") || t == QLatin1String("real32")) {
        out = SampleFormat::Float32;
        return true;
    }
    if (t == QLatin1String("float64") || t == QLatin1String("f64") || t == QLatin1String("double")
        || t == QLatin1String("real64")) {
        out = SampleFormat::Float64;
        return true;
    }
    return false;
}

QString binaryLayoutName(BinaryLayout l)
{
    switch (l) {
    case BinaryLayout::Interleaved: return QStringLiteral("紧密排列（逐样本）");
    case BinaryLayout::Packed12: return QStringLiteral("12 位 ×2 打包进 int32（高=CH1，低=CH2）");
    }
    return QString();
}

bool binaryLayoutFromString(const QString& text, BinaryLayout& out)
{
    const QString t = normalizeKey(text);
    if (t.isEmpty()) return false;
    if (t == QLatin1String("interleaved") || t == QLatin1String("plain") || t == QLatin1String("linear")
        || t == QLatin1String("interleave") || t == QStringLiteral("紧密") || t == QStringLiteral("交织")) {
        out = BinaryLayout::Interleaved;
        return true;
    }
    if (t == QLatin1String("packed12") || t == QLatin1String("packed") || t == QLatin1String("pack12")
        || t == QLatin1String("int32packed") || t == QLatin1String("12bitpacked")
        || t == QStringLiteral("打包12") || t == QStringLiteral("打包")) {
        out = BinaryLayout::Packed12;
        return true;
    }
    return false;
}

int BinaryOptions::bytesPerFrame() const
{
    if (layout == BinaryLayout::Packed12) return 4; // 一个 int32 = 2 个 12 位通道
    return sampleFormatBytes(format) * (channels > 0 ? channels : 1);
}

QString BinaryOptions::describe() const
{
    QStringList parts;
    if (layout == BinaryLayout::Packed12) {
        parts << QStringLiteral("12bit×2 打包")
              << (littleEndian ? QStringLiteral("小端") : QStringLiteral("大端"))
              << QStringLiteral("2 通道");
    } else {
        parts << sampleFormatShortName(format)
              << (littleEndian ? QStringLiteral("小端") : QStringLiteral("大端"))
              << QStringLiteral("%1 通道").arg(channels > 0 ? channels : 1);
    }
    if (headerBytes > 0) parts << QStringLiteral("偏移 %1 B").arg(headerBytes);
    if (sampleRate > 0.0) parts << QStringLiteral("%1 Hz").arg(QString::number(sampleRate, 'g', 6));
    if (std::fabs(scale - 1.0) > 1e-12)
        parts << QStringLiteral("×%1").arg(QString::number(scale, 'g', 6));
    if (std::fabs(bias) > 1e-12)
        parts << QStringLiteral("%1%2").arg(bias > 0 ? QStringLiteral("+") : QString())
                     .arg(QString::number(bias, 'g', 6));
    return parts.join(QStringLiteral(" · "));
}

QString binaryOptionsToString(const BinaryOptions& o)
{
    const QStringList parts{
        sampleFormatShortName(o.format),
        o.littleEndian ? QStringLiteral("le") : QStringLiteral("be"),
        QString::number(o.channels),
        QString::number(o.headerBytes),
        QString::number(o.sampleRate, 'g', 10),
        QString::number(o.scale, 'g', 12),
        QString::number(o.bias, 'g', 10),
        o.layout == BinaryLayout::Packed12 ? QStringLiteral("packed12") : QStringLiteral("interleaved")
    };
    return parts.join(QLatin1Char('|'));
}

bool binaryOptionsFromString(const QString& text, BinaryOptions& o)
{
    const QStringList parts = text.split(QLatin1Char('|'));
    if (parts.size() < 4) return false;
    SampleFormat fmt;
    if (!sampleFormatFromString(parts.at(0), fmt)) return false;
    o.format = fmt;
    o.littleEndian = (parts.at(1) != QLatin1String("be"));
    o.channels = std::clamp(parts.at(2).toInt(), 1, 16);
    o.headerBytes = std::max<qint64>(0, parts.at(3).toLongLong());
    if (parts.size() > 4) o.sampleRate = parts.at(4).toDouble();
    if (parts.size() > 5) o.scale = parts.at(5).toDouble();
    if (parts.size() > 6) o.bias = parts.at(6).toDouble();
    if (parts.size() > 7) {
        BinaryLayout l;
        if (binaryLayoutFromString(parts.at(7), l)) o.layout = l;
    }
    if (o.layout == BinaryLayout::Packed12) o.channels = 2;
    return true;
}

bool isBinaryPath(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    static const QStringList explicitBin = {
        QStringLiteral("bin"),  QStringLiteral("raw"), QStringLiteral("i16"), QStringLiteral("i32"),
        QStringLiteral("f32"),  QStringLiteral("f64"), QStringLiteral("ecgb"), QStringLiteral("adc")
    };
    if (explicitBin.contains(suffix)) return true;

    // 其余后缀按内容嗅探：出现 NUL 或不可打印字节比例高即视为二进制
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(2048);
    if (head.isEmpty()) return false;
    int bad = 0;
    for (const char c : head) {
        const uchar u = static_cast<uchar>(c);
        if (u == 0) return true;
        if (u < 0x09 || (u > 0x0D && u < 0x20)) ++bad;
    }
    return bad * 100 > head.size() * 15;
}

bool probeBinary(const QString& path, BinaryProbe& out, QString& error)
{
    out = BinaryProbe();
    error.clear();

    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        error = QStringLiteral("文件不存在：%1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    const qint64 size = fi.size();
    if (size < 8) {
        error = QStringLiteral("文件太小（%1 字节），不足以构成心电数据。").arg(size);
        return false;
    }

    BinaryOptions opt;
    bool confident = false;
    QString source;
    QStringList notes;

    // ① 同名说明文件：xxx.bin.json / xxx.json / xxx.meta
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();
    QStringList candidates;
    candidates << path + QStringLiteral(".json")
               << QDir(dir).filePath(base + QStringLiteral(".json"))
               << path + QStringLiteral(".meta")
               << QDir(dir).filePath(base + QStringLiteral(".meta"));
    candidates.removeDuplicates();
    for (const QString& sc : candidates) {
        if (!QFileInfo::exists(sc)) continue;
        BinaryOptions o;
        int recognized = 0;
        const bool ok = sc.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)
                            ? readSidecarJson(sc, o, recognized)
                            : readSidecarMeta(sc, o, recognized);
        if (ok && recognized > 0) {
            opt = o;
            confident = true;
            source = QStringLiteral("说明文件 %1").arg(QFileInfo(sc).fileName());
            break;
        }
    }

    // ② 文件内文本头
    if (!confident) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray head = f.read(4096);
            BinaryOptions o;
            qint64 headerBytes = 0;
            int recognized = 0;
            if (parseEmbeddedHeader(head, o, headerBytes, recognized)) {
                opt = o;
                if (opt.headerBytes <= 0) opt.headerBytes = headerBytes;
                else notes << QStringLiteral("文件头中显式指定了偏移 %1 字节，已优先采用。").arg(opt.headerBytes);
                confident = true;
                source = QStringLiteral("文件内文本头（%1 字节）").arg(headerBytes);
            }
        }
    }

    // ③ 按文件大小与数值特征推测
    if (!confident) {
        QFile f(path);
        QByteArray head;
        if (f.open(QIODevice::ReadOnly)) head = f.read(8192);

        BinaryOptions o;
        bool found = false;

        // 原始浮点记录若被当作整数解读会呈满量程噪声，先排除这种情况
        if (head.size() >= 4 * 64) {
            const QuickStats st = quickStats(head, SampleFormat::Float32, true, 2048);
            if (st.nonFinite == 0 && st.sd > 1e-9 && st.maxAbs > 1e-6 && st.maxAbs < 1e4
                && st.fractional > 0.5) {
                o.format = SampleFormat::Float32;
                found = true;
            }
        }
        if (!found && size % 4 == 0 && looksPacked12(head, true)) {
            o.layout = BinaryLayout::Packed12;
            o.format = SampleFormat::Int32;
            o.channels = 2;
            found = true;
            notes << QStringLiteral("检测到 12 位双通道打包格式（int32 高/低 16 位各存一个导联）。");
        }
        if (!found) {
            const SampleFormat order[] = {
                SampleFormat::Int16, SampleFormat::Int32, SampleFormat::Int8,
                SampleFormat::UInt8, SampleFormat::Float64
            };
            for (SampleFormat f : order) {
                const qint64 bps = sampleFormatBytes(f);
                if (bps <= 0 || size % bps != 0 || size / bps < 8) continue;
                o.format = f;
                found = true;
                break;
            }
        }
        if (!found) {
            error = QStringLiteral("无法按常见位宽整除文件大小（%1 字节），请在导入设置中手动指定格式。").arg(size);
            return false;
        }
        if (o.layout == BinaryLayout::Packed12) {
            source = QStringLiteral("按数据特征推测（12 位打包）");
        } else {
            o.channels = 1;
            source = QStringLiteral("按文件大小推测");
        }
        o.headerBytes = 0;
        opt = o;
        notes << QStringLiteral("未找到说明文件或文本头，已按 %1\u3001小端推测，波形异常时请在导入设置中调整。")
                     .arg(opt.layout == BinaryLayout::Packed12
                              ? QStringLiteral("12 位双通道打包")
                              : sampleFormatShortName(o.format) + QStringLiteral(" / 单通道"));
    }

    if (opt.channels < 1) opt.channels = 1;
    if (opt.layout == BinaryLayout::Packed12) opt.channels = 2;
    const qint64 frameBytes = opt.bytesPerFrame();
    if (frameBytes <= 0 || size <= opt.headerBytes) {
        error = QStringLiteral("偏移量（%1 字节）已超过文件大小。").arg(opt.headerBytes);
        return false;
    }
    const qint64 frames = (size - opt.headerBytes) / frameBytes;
    if (frames < 8) {
        error = QStringLiteral("有效数据不足 8 帧，请检查偏移量与通道数。");
        return false;
    }
    if (opt.sampleRate <= 0.0)
        notes << QStringLiteral("说明中没有采样率，导入时需要手动填写。");

    out.ok = true;
    out.confident = confident;
    out.options = opt;
    out.frames = frames;
    out.source = source;
    out.notes = notes;
    return true;
}

bool loadBinary(const QString& path, const BinaryOptions& opt, Data& out, QString& error,
                int maxFrames)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法打开文件：%1").arg(f.errorString());
        return false;
    }

    const qint64 size = f.size();
    const bool packed = (opt.layout == BinaryLayout::Packed12);
    const int bps = packed ? 4 : sampleFormatBytes(opt.format);
    const int ch = packed ? 2 : std::max(1, opt.channels);
    const qint64 frameBytes = static_cast<qint64>(bps) * (packed ? 1 : ch);
    const qint64 start = std::max<qint64>(0, opt.headerBytes);

    if (size - start < frameBytes * 8) {
        error = QStringLiteral("偏移量过大或数据量不足（至少需要 8 帧，每帧 %1 字节）。").arg(frameBytes);
        return false;
    }

    qint64 frames = (size - start) / frameBytes;
    if (frames > kMaxFrames) {
        error = QStringLiteral("文件包含约 %1 帧，超出单次可处理上限（%2 帧）。")
                    .arg(frames).arg(kMaxFrames);
        return false;
    }
    if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;

    if (!f.seek(start)) {
        error = QStringLiteral("无法定位到数据起始位置：%1").arg(f.errorString());
        return false;
    }
    const QByteArray buf = f.read(frames * frameBytes);
    frames = buf.size() / frameBytes;
    if (frames < 8) {
        error = QStringLiteral("读取到的数据不足 8 帧，请检查偏移量设置。");
        return false;
    }

    out = Data();
    out.filePath = path;
    out.headers.clear();
    for (int i = 0; i < ch; ++i) out.headers << QStringLiteral("CH%1").arg(i + 1);
    out.columns.assign(static_cast<size_t>(ch), {});
    for (auto& c : out.columns) c.resize(static_cast<size_t>(frames));

    const double scale = (opt.scale > 0.0) ? opt.scale : 1.0;
    const uchar* p = reinterpret_cast<const uchar*>(buf.constData());
    for (qint64 i = 0; i < frames; ++i) {
        if (packed) {
            // 一个 int32 = 高 16 位的 12 位 CH1 + 低 16 位的 12 位 CH2（零点 2048，CH2 取反）
            const quint32 u = readU32(p, opt.littleEndian);
            const int hi = static_cast<int>((u >> 16) & 0x0FFFu);
            const int lo = static_cast<int>(u & 0x0FFFu);
            p += 4;
            out.columns[0][static_cast<size_t>(i)] = (hi - 2048) * scale + opt.bias;
            out.columns[1][static_cast<size_t>(i)] = (2048 - lo) * scale + opt.bias;
            continue;
        }
        for (int c = 0; c < ch; ++c) {
            double v = decodeSample(p, opt.format, opt.littleEndian);
            p += bps;
            if (!std::isfinite(v)) v = 0.0;
            out.columns[static_cast<size_t>(c)][static_cast<size_t>(i)] = v * scale + opt.bias;
        }
    }

    out.timeColumn = -1;
    out.signalColumn = 0;
    out.fsGuess = (opt.sampleRate > 0.0) ? opt.sampleRate : 0.0;
    out.loaded = true;
    return true;
}

bool writeBinarySidecar(const QString& path, const BinaryOptions& opt, QString& error)
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), sampleFormatShortName(opt.format));
    if (opt.layout == BinaryLayout::Packed12)
        o.insert(QStringLiteral("layout"), QStringLiteral("packed12"));
    o.insert(QStringLiteral("endian"), opt.littleEndian ? QStringLiteral("little") : QStringLiteral("big"));
    o.insert(QStringLiteral("channels"), opt.layout == BinaryLayout::Packed12 ? 2 : opt.channels);
    o.insert(QStringLiteral("offset"), static_cast<double>(opt.headerBytes));
    if (opt.sampleRate > 0.0) o.insert(QStringLiteral("fs"), opt.sampleRate);
    o.insert(QStringLiteral("scale"), opt.scale);
    o.insert(QStringLiteral("bias"), opt.bias);

    const QString sidecar = path + QStringLiteral(".json");
    QFile f(sidecar);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("无法写入 %1：%2").arg(QDir::toNativeSeparators(sidecar), f.errorString());
        return false;
    }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

Data demoData(double fs, double seconds)
{
    Data d;
    d.loaded = true;
    d.fsGuess = fs;
    d.timeColumn = 0;
    d.signalColumn = 1;
    d.headers << QStringLiteral("time_s") << QStringLiteral("ecg_mV");

    const int n = std::max(16, static_cast<int>(std::lround(fs * seconds)));
    std::vector<double> t(static_cast<size_t>(n));
    std::vector<double> v(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) t[static_cast<size_t>(i)] = static_cast<double>(i) / fs;

    // 合成 P-QRS-T：每搏由 5 个高斯波叠加，RR 带轻微呼吸性变异
    double beat = 0.55;
    int guard = 0;
    while (beat < seconds + 1.0 && guard++ < 4000) {
        addGaussian(v, fs, beat - 0.160, 0.030, 0.14);   // P
        addGaussian(v, fs, beat - 0.030, 0.011, -0.12);  // Q
        addGaussian(v, fs, beat,         0.0125, 1.05);  // R
        addGaussian(v, fs, beat + 0.034, 0.014, -0.26);  // S
        addGaussian(v, fs, beat + 0.300, 0.062, 0.30);   // T
        beat += 0.780 + 0.060 * std::sin(0.55 * beat) + 0.018 * std::sin(1.7 * beat);
    }

    std::mt19937 rng(20260527u);
    std::normal_distribution<double> white(0.0, 0.010);

    for (int i = 0; i < n; ++i) {
        const double ti = t[static_cast<size_t>(i)];
        v[static_cast<size_t>(i)] += 0.095 * std::sin(2.0 * kPi * 0.30 * ti); // 基线漂移
        v[static_cast<size_t>(i)] += 0.045 * std::sin(2.0 * kPi * 0.06 * ti);
        v[static_cast<size_t>(i)] += 0.015 * std::sin(2.0 * kPi * 50.0 * ti); // 50 Hz 工频
        v[static_cast<size_t>(i)] += white(rng);                              // 肌电/白噪声
    }

    d.columns.push_back(t);
    d.columns.push_back(v);
    return d;
}

// =====================================================================
//                人工标注文件（ECGRPK1：头 + float32 数据 + 标注）
// =====================================================================

namespace {

const char kAnnotMagic[] = "ECGRPK1\n";
constexpr int kAnnotMagicLen = 8;
constexpr int kAnnotBlock = 8192; // 写数据区时的分块帧数

} // namespace

bool isAnnotatedFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return f.read(kAnnotMagicLen) == QByteArray(kAnnotMagic, kAnnotMagicLen);
}

bool writeAnnotatedFile(const QString& path, const Data& data, double fs,
                        const Annotations& ann, QString& error)
{
    if (!data.loaded || data.cols() < 1 || data.rows() < 1) {
        error = QStringLiteral("当前没有可写出的心电数据。");
        return false;
    }
    if (!(fs > 0.0)) fs = (data.fsGuess > 0.0) ? data.fsGuess : 1.0;

    const int ch = data.cols();
    const int n = data.rows();

    QString header = QStringLiteral("ECGRPK1\n");
    header += QStringLiteral("version=1\n");
    header += QStringLiteral("format=float32\n");
    header += QStringLiteral("channels=%1\n").arg(ch);
    header += QStringLiteral("samples=%1\n").arg(n);
    header += QStringLiteral("fs=%1\n").arg(QString::number(fs, 'g', 10));
    header += QStringLiteral("signal=%1\n").arg(std::max(0, data.signalColumn));
    header += QStringLiteral("annotations=%1\n").arg(ann.peaks.size());
    header += QStringLiteral("logs=%1\n").arg(ann.log.size());
    QString src = ann.source;
    src.replace(QLatin1Char('\n'), QLatin1Char(' '));
    src.replace(QLatin1Char('\r'), QLatin1Char(' '));
    header += QStringLiteral("source=%1\n").arg(src);
    header += QStringLiteral("--\n");

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("无法写入 %1：%2").arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    f.write(header.toUtf8());

    // 数据区：float32 小端，按帧交错
    QByteArray buf(kAnnotBlock * 4, '\0');
    uchar* base = reinterpret_cast<uchar*>(buf.data());
    int idx = 0;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < ch; ++c) {
            const float v = static_cast<float>(data.columns[static_cast<size_t>(c)][static_cast<size_t>(i)]);
            quint32 u = 0;
            std::memcpy(&u, &v, sizeof(u));
            base[idx * 4 + 0] = static_cast<uchar>(u & 0xFFu);
            base[idx * 4 + 1] = static_cast<uchar>((u >> 8) & 0xFFu);
            base[idx * 4 + 2] = static_cast<uchar>((u >> 16) & 0xFFu);
            base[idx * 4 + 3] = static_cast<uchar>((u >> 24) & 0xFFu);
            if (++idx == kAnnotBlock) {
                f.write(buf.constData(), idx * 4);
                idx = 0;
            }
        }
    }
    if (idx > 0) f.write(buf.constData(), idx * 4);

    QString annText;
    for (size_t i = 0; i < ann.peaks.size(); ++i) {
        const unsigned char t = (i < ann.types.size() && ann.types[i] != 0) ? ann.types[i] : 1;
        annText += QStringLiteral("%1,%2\n").arg(ann.peaks[i]).arg(static_cast<int>(t));
    }
    QString logText;
    for (const QString& line : ann.log) {
        QString s = line;
        s.replace(QLatin1Char('\n'), QLatin1Char(' '));
        logText += s + QLatin1Char('\n');
    }

    f.write("@ANN\n");
    f.write(annText.toUtf8());
    f.write("@LOG\n");
    f.write(logText.toUtf8());

    if (f.error() != QFileDevice::NoError) {
        error = QStringLiteral("写入过程中出错：%1").arg(f.errorString());
        return false;
    }
    f.close();
    return true;
}

bool loadAnnotatedFile(const QString& path, Data& out, Annotations& ann, QString& error)
{
    out = Data();
    ann = Annotations();
    error.clear();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("无法打开文件：%1").arg(f.errorString());
        return false;
    }
    if (f.read(kAnnotMagicLen) != QByteArray(kAnnotMagic, kAnnotMagicLen)) {
        error = QStringLiteral("不是本程序导出的标注文件（缺少 ECGRPK1 标识）。");
        return false;
    }

    QMap<QString, QString> kv;
    bool headerEnd = false;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine(4096);
        if (line.isEmpty()) break;
        const QString text = QString::fromUtf8(line).trimmed();
        if (text == QLatin1String("--")) {
            headerEnd = true;
            break;
        }
        const int eq = text.indexOf(QLatin1Char('='));
        if (eq > 0) kv.insert(text.left(eq).trimmed().toLower(), text.mid(eq + 1).trimmed());
    }
    if (!headerEnd) {
        error = QStringLiteral("标注文件头不完整（缺少 -- 结束标记）。");
        return false;
    }

    bool ok = false;
    const int ch = kv.value(QStringLiteral("channels")).toInt(&ok);
    if (!ok || ch < 1 || ch > 64) {
        error = QStringLiteral("标注文件头中的通道数无效。");
        return false;
    }
    const int n = kv.value(QStringLiteral("samples")).toInt(&ok);
    if (!ok || n < 8) {
        error = QStringLiteral("标注文件头中的采样点数无效。");
        return false;
    }
    const QString fmt = kv.value(QStringLiteral("format"), QStringLiteral("float32")).toLower();
    if (fmt != QLatin1String("float32")) {
        error = QStringLiteral("暂不支持该标注文件的数据编码：%1").arg(fmt);
        return false;
    }

    const qint64 need = static_cast<qint64>(n) * ch * 4;
    const qint64 start = f.pos();
    const QByteArray payload = f.read(need);
    if (payload.size() < need) {
        error = QStringLiteral("数据区不完整（应 %1 字节，实际 %2 字节），文件可能被截断。")
                    .arg(need).arg(payload.size());
        return false;
    }

    out.filePath = path;
    out.headers.clear();
    for (int c = 0; c < ch; ++c) out.headers << QStringLiteral("CH%1").arg(c + 1);
    out.columns.assign(static_cast<size_t>(ch), {});
    for (auto& col : out.columns) col.resize(static_cast<size_t>(n));

    const uchar* p = reinterpret_cast<const uchar*>(payload.constData());
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < ch; ++c) {
            const quint32 u = static_cast<quint32>(p[0]) | (static_cast<quint32>(p[1]) << 8)
                              | (static_cast<quint32>(p[2]) << 16) | (static_cast<quint32>(p[3]) << 24);
            p += 4;
            float v = 0.0f;
            std::memcpy(&v, &u, sizeof(v));
            out.columns[static_cast<size_t>(c)][static_cast<size_t>(i)] = std::isfinite(v) ? v : 0.0;
        }
    }
    out.timeColumn = -1;
    out.signalColumn = std::clamp(kv.value(QStringLiteral("signal")).toInt(), 0, ch - 1);
    out.fsGuess = kv.value(QStringLiteral("fs")).toDouble();
    out.loaded = true;

    const int annCount = kv.value(QStringLiteral("annotations")).toInt();
    const int logCount = kv.value(QStringLiteral("logs")).toInt();

    const QByteArray tail = f.readAll();
    int mode = 0;
    const QStringList lines = QString::fromUtf8(tail).split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line == QLatin1String("@ANN")) { mode = 1; continue; }
        if (line == QLatin1String("@LOG")) { mode = 2; continue; }
        if (line.isEmpty()) continue;
        if (mode == 1 && static_cast<int>(ann.peaks.size()) < annCount) {
            const QStringList parts = line.split(QLatin1Char(','));
            bool okIdx = false;
            const int idx = parts.value(0).toInt(&okIdx);
            if (!okIdx || idx < 0 || idx >= n) continue;
            int type = parts.value(1).toInt();
            if (type != 2) type = 1;
            ann.peaks.push_back(idx);
            ann.types.push_back(static_cast<unsigned char>(type));
        } else if (mode == 2 && ann.log.size() < logCount) {
            ann.log << line;
        }
    }
    ann.source = kv.value(QStringLiteral("source"));

    // 保证升序
    if (!std::is_sorted(ann.peaks.begin(), ann.peaks.end())) {
        std::vector<size_t> order(ann.peaks.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&ann](size_t a, size_t b) { return ann.peaks[a] < ann.peaks[b]; });
        std::vector<int> sp(ann.peaks.size());
        std::vector<unsigned char> st(ann.types.size());
        for (size_t i = 0; i < order.size(); ++i) {
            sp[i] = ann.peaks[order[i]];
            st[i] = ann.types[order[i]];
        }
        ann.peaks.swap(sp);
        ann.types.swap(st);
    }
    if (ann.types.size() < ann.peaks.size()) ann.types.resize(ann.peaks.size(), 1);
    return true;
}

} // namespace ecg
