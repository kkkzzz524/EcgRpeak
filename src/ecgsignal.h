// 心电数据加载：文本（CSV / TSV / 空格分隔）与二进制原始数据（.bin / .raw）
#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <vector>

namespace ecg {

struct Data {
    bool loaded = false;
    QString filePath;
    QStringList headers;                       // 空表示文件无表头
    std::vector<std::vector<double>> columns;  // 逐列存储
    double fsGuess = 0.0;                      // <=0 表示未推断出采样率
    int timeColumn = -1;                       // 识别到的时间列
    int signalColumn = 0;                      // 建议使用的信号列

    int rows() const { return columns.empty() ? 0 : static_cast<int>(columns.front().size()); }
    int cols() const { return static_cast<int>(columns.size()); }
};

// ---------------- 文本数据 ----------------
// 读取文件；成功返回 true，失败时 error 含中文原因
bool loadFile(const QString& path, Data& out, QString& error);

// 生成一段带基线漂移/工频/肌电噪声的合成心电，用于无数据时演示
Data demoData(double fs, double seconds);

// ---------------- 二进制原始数据 ----------------
// 采样点以定点或浮点方式紧密排列，缺少表头，需要额外告知位宽/字节序/通道/采样率
enum class SampleFormat { Int8, UInt8, Int16, UInt16, Int32, Float32, Float64 };

// 数据布局：逐样本紧密排列；或把两个 12 位通道打包进一个 int32（高 16 位 CH1、低 16 位 CH2）
enum class BinaryLayout { Interleaved, Packed12 };

QString sampleFormatShortName(SampleFormat f);   // "int16"
QString sampleFormatLongName(SampleFormat f);    // "int16 · 16 位有符号整数"
int     sampleFormatBytes(SampleFormat f);
bool    sampleFormatFromString(const QString& text, SampleFormat& out);

QString binaryLayoutName(BinaryLayout l);
bool    binaryLayoutFromString(const QString& text, BinaryLayout& out);

struct BinaryOptions {
    SampleFormat format = SampleFormat::Int16;
    BinaryLayout layout = BinaryLayout::Interleaved;
    bool   littleEndian = true;
    int    channels = 1;         // 交织存放的通道数
    qint64 headerBytes = 0;      // 跳过的文件头/偏移字节
    double sampleRate = 0.0;     // 0 表示未知
    double scale = 1.0;          // 物理量 = 原始整数 × scale + bias（如 1 LSB = 1/2048 mV）
    double bias = 0.0;

    int bytesPerFrame() const;
    QString describe() const;    // "int16 · 小端 · 2 通道 · 500 Hz"
};

// "int16|le|2|0|500|1|0|packed12" 形式，用于记忆上次参数与命令行
QString binaryOptionsToString(const BinaryOptions& opt);
bool    binaryOptionsFromString(const QString& text, BinaryOptions& opt);

struct BinaryProbe {
    bool ok = false;
    bool confident = false;      // 来自同名说明文件或文件内文本头
    BinaryOptions options;
    qint64 frames = 0;           // 按 options 推算的帧数
    QString source;              // 参数来源描述
    QStringList notes;           // 需要提示用户的事项
};

// 后缀名 / 内容嗅探判断是否按二进制读取
bool isBinaryPath(const QString& path);

// 探测二进制参数（说明文件 > 文件内文本头 > 按文件大小推测），不会失败到无法使用
bool probeBinary(const QString& path, BinaryProbe& out, QString& error);

// 解码为多列信号；maxFrames > 0 时只读取前若干帧（用于预览）
bool loadBinary(const QString& path, const BinaryOptions& opt, Data& out, QString& error,
                int maxFrames = 0);

// 写出同名 .json 说明文件，便于下次自动识别
bool writeBinarySidecar(const QString& path, const BinaryOptions& opt, QString& error);

// ---------------- 人工标注文件（ECGRPK1） ----------------
// 自描述 bin：文本头 + float32 交错采样数据 + 标注表 + 编辑记录，
// 重新导入时直接使用文件内的 R 峰，不再跑检测算法。
struct Annotations {
    std::vector<int> peaks;            // R 峰样点索引（升序）
    std::vector<unsigned char> types;  // 1=算法检出 2=人工新增
    QStringList log;                   // 每次修改的记录
    QString source;                    // 原始数据来源
};

bool isAnnotatedFile(const QString& path);
bool loadAnnotatedFile(const QString& path, Data& data, Annotations& ann, QString& error);
bool writeAnnotatedFile(const QString& path, const Data& data, double fs,
                        const Annotations& ann, QString& error);

} // namespace ecg
