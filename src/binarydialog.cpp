#include "binarydialog.h"
#include "plotwidget.h"

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QLabel* formLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("formLabel"));
    return l;
}

QString humanSize(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(QString::number(bytes / 1073741824.0, 'f', 2));
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MB").arg(QString::number(bytes / 1048576.0, 'f', 2));
    if (bytes >= 1024)
        return QStringLiteral("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

BinaryDialog::BinaryDialog(const QString& path, const ecg::BinaryOptions& initial, QWidget* parent)
    : QDialog(parent)
    , m_path(path)
{
    setWindowTitle(QStringLiteral("二进制原始数据导入 · %1").arg(QFileInfo(path).fileName()));
    setModal(true);
    resize(1000, 570);
    setMinimumSize(900, 500);

    QString probeError;
    if (!ecg::probeBinary(path, m_probe, probeError) && !probeError.isEmpty())
        m_probe.notes << probeError;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(11);

    auto* title = new QLabel(QStringLiteral("二进制原始数据导入"));
    title->setObjectName(QStringLiteral("dialogTitle"));
    root->addWidget(title);

    auto* sub = new QLabel(QStringLiteral("%1　·　%2")
                               .arg(QFileInfo(path).fileName(),
                                    QDir::toNativeSeparators(QFileInfo(path).absolutePath())));
    sub->setObjectName(QStringLiteral("dialogInfo"));
    sub->setToolTip(QDir::toNativeSeparators(path));
    root->addWidget(sub);

    auto* content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(14);
    content->addWidget(buildForm(), 0);
    content->addWidget(buildPreview(), 1);
    root->addLayout(content, 1);

    m_infoLabel = new QLabel;
    m_infoLabel->setObjectName(QStringLiteral("dialogInfo"));
    m_infoLabel->setWordWrap(true);
    root->addWidget(m_infoLabel);

    m_noteLabel = new QLabel;
    m_noteLabel->setObjectName(QStringLiteral("dialogNote"));
    m_noteLabel->setWordWrap(true);
    root->addWidget(m_noteLabel);

    auto* btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(8);
    auto* detectBtn = new QPushButton(QStringLiteral("自动识别"));
    detectBtn->setCursor(Qt::PointingHandCursor);
    detectBtn->setToolTip(QStringLiteral("重新读取说明文件 / 文件内文本头 / 文件大小"));
    m_saveBtn = new QPushButton(QStringLiteral("导出参数说明 (.json)"));
    m_saveBtn->setCursor(Qt::PointingHandCursor);
    m_saveBtn->setToolTip(QStringLiteral("把当前参数写成同名说明文件，下次打开即自动识别"));
    m_saveBtn->setFlat(true);
    m_saveBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;color:#3B82F6;padding:2px 4px;}"));
    btnRow->addWidget(detectBtn);
    btnRow->addWidget(m_saveBtn);
    btnRow->addStretch(1);

    auto* cancelBtn = new QPushButton(QStringLiteral("取消"));
    cancelBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setMinimumWidth(88);
    auto* okBtn = new QPushButton(QStringLiteral("载入数据"));
    okBtn->setObjectName(QStringLiteral("primaryBtn"));
    okBtn->setCursor(Qt::PointingHandCursor);
    okBtn->setMinimumWidth(120);
    okBtn->setMinimumHeight(34);
    okBtn->setDefault(true);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    root->addLayout(btnRow);

    connect(detectBtn, &QPushButton::clicked, this, &BinaryDialog::autoDetect);
    connect(m_saveBtn, &QPushButton::clicked, this, &BinaryDialog::saveSidecar);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(okBtn, &QPushButton::clicked, this, &BinaryDialog::accept);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(140);
    connect(m_debounce, &QTimer::timeout, this, &BinaryDialog::refreshPreview);

    connect(m_layoutCombo, &QComboBox::currentIndexChanged, this, [this]() {
        updateLayoutUi();
        schedulePreview();
    });
    connect(m_formatCombo, &QComboBox::currentIndexChanged, this, &BinaryDialog::schedulePreview);
    connect(m_endianCombo, &QComboBox::currentIndexChanged, this, &BinaryDialog::schedulePreview);
    connect(m_prevChCombo, &QComboBox::currentIndexChanged, this, &BinaryDialog::refreshPreview);
    connect(m_chSpin, &QSpinBox::valueChanged, this, [this](int n) {
        rebuildChannelCombo(n);
        schedulePreview();
    });
    connect(m_offSpin, &QSpinBox::valueChanged, this, &BinaryDialog::schedulePreview);
    connect(m_fsSpin, &QDoubleSpinBox::valueChanged, this, &BinaryDialog::schedulePreview);
    connect(m_scaleSpin, &QDoubleSpinBox::valueChanged, this, &BinaryDialog::schedulePreview);
    connect(m_biasSpin, &QDoubleSpinBox::valueChanged, this, &BinaryDialog::schedulePreview);

    applyOptions(initial);
    updateInfo();
}

QWidget* BinaryDialog::buildForm()
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("paramCard"));
    card->setFixedWidth(372);

    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(14, 12, 14, 12);
    v->setSpacing(9);

    auto* caption = new QLabel(QStringLiteral("解 析 参 数"));
    caption->setObjectName(QStringLiteral("sectionTitle"));
    v->addWidget(caption);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(8);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_layoutCombo = new QComboBox;
    m_layoutCombo->addItem(QStringLiteral("紧密排列（每个采样点依次存放）"),
                           static_cast<int>(ecg::BinaryLayout::Interleaved));
    m_layoutCombo->addItem(QStringLiteral("12 位 ×2 打包进 int32（高=CH1 低=CH2）"),
                           static_cast<int>(ecg::BinaryLayout::Packed12));
    m_layoutCombo->setToolTip(QStringLiteral("部分监护仪把两个 12 位导联打包进一个 32 位整数，\n"
                                             "CH1 = 高 16 位 − 2048，CH2 = 2048 − 低 12 位"));

    m_formatCombo = new QComboBox;
    for (ecg::SampleFormat f : {ecg::SampleFormat::Int16, ecg::SampleFormat::UInt16,
                                ecg::SampleFormat::Int8, ecg::SampleFormat::UInt8,
                                ecg::SampleFormat::Int32, ecg::SampleFormat::Float32,
                                ecg::SampleFormat::Float64}) {
        m_formatCombo->addItem(ecg::sampleFormatLongName(f), static_cast<int>(f));
    }
    m_formatCombo->setToolTip(QStringLiteral("每个采样点的存储类型，多数采集设备是 int16"));

    m_endianCombo = new QComboBox;
    m_endianCombo->addItem(QStringLiteral("小端 Little-Endian（常见）"), true);
    m_endianCombo->addItem(QStringLiteral("大端 Big-Endian"), false);
    m_endianCombo->setToolTip(QStringLiteral("x86/ARM 采集设备通常为小端；大端多来自网络字节序或部分 DSP"));

    m_chSpin = new QSpinBox;
    m_chSpin->setRange(1, 16);
    m_chSpin->setValue(1);
    m_chSpin->setSuffix(QStringLiteral(" 通道"));
    m_chSpin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_chSpin->setToolTip(QStringLiteral("多导联按帧交织存放，例如 CH1,CH2,CH1,CH2…"));

    m_offSpin = new QSpinBox;
    m_offSpin->setRange(0, 200000000);
    m_offSpin->setSingleStep(2);
    m_offSpin->setSuffix(QStringLiteral(" 字节"));
    m_offSpin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_offSpin->setToolTip(QStringLiteral("跳过采集设备自带的文件头；填错会让整段数据错位"));

    m_fsSpin = new QDoubleSpinBox;
    m_fsSpin->setRange(0.0, 5000.0);
    m_fsSpin->setDecimals(2);
    m_fsSpin->setValue(0.0);
    m_fsSpin->setSuffix(QStringLiteral(" Hz"));
    m_fsSpin->setSpecialValueText(QStringLiteral("未知（必须填写）"));
    m_fsSpin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_fsSpin->setToolTip(QStringLiteral("二进制文件不含时间信息，必须与采集设备一致（250 / 360 / 500 / 1000 Hz 等）"));

    m_scaleSpin = new QDoubleSpinBox;
    m_scaleSpin->setRange(0.000000001, 1000000.0);
    m_scaleSpin->setDecimals(9);
    m_scaleSpin->setValue(1.0);
    m_scaleSpin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_scaleSpin->setToolTip(QStringLiteral("物理量 = 原始值 × 系数 + 偏置。若 1 mV = 2048 LSB，请填 0.000488281"));

    m_biasSpin = new QDoubleSpinBox;
    m_biasSpin->setRange(-1000000.0, 1000000.0);
    m_biasSpin->setDecimals(3);
    m_biasSpin->setValue(0.0);
    m_biasSpin->setSuffix(QStringLiteral(" mV"));
    m_biasSpin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_biasSpin->setToolTip(QStringLiteral("无符号 ADC 通常需要减去零点，例如 32768 × 系数"));

    m_prevChCombo = new QComboBox;
    m_prevChCombo->setToolTip(QStringLiteral("预览与载入时默认使用的通道"));

    form->addRow(formLabel(QStringLiteral("数据布局")), m_layoutCombo);
    form->addRow(formLabel(QStringLiteral("采样类型")), m_formatCombo);
    form->addRow(formLabel(QStringLiteral("字节序")), m_endianCombo);
    form->addRow(formLabel(QStringLiteral("通道数")), m_chSpin);
    form->addRow(formLabel(QStringLiteral("起始偏移")), m_offSpin);
    form->addRow(formLabel(QStringLiteral("采样率")), m_fsSpin);
    form->addRow(formLabel(QStringLiteral("幅值缩放")), m_scaleSpin);
    form->addRow(formLabel(QStringLiteral("零点偏置")), m_biasSpin);
    form->addRow(formLabel(QStringLiteral("预览通道")), m_prevChCombo);
    v->addLayout(form);

    auto* hint = new QLabel(QStringLiteral("提示：确认波形是规则的心搏后\n再点「载入数据」。"));
    hint->setObjectName(QStringLiteral("hintLabel"));
    hint->setWordWrap(true);
    v->addWidget(hint);
    v->addStretch(1);
    return card;
}

QWidget* BinaryDialog::buildPreview()
{
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("previewCard"));
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(0);

    m_preview = new PlotWidget;
    m_preview->setTitle(QStringLiteral("波形预览（前 5 秒）"));
    m_preview->setAxisLabels(QStringLiteral("时间 (s)"), QStringLiteral("幅值"));
    m_preview->setEmptyText(QStringLiteral("等待解析…"));
    m_preview->addSeries(QStringLiteral("CH1"), QColor(0x25, 0x63, 0xEB), 1.2);
    m_preview->setMinimumHeight(240);
    v->addWidget(m_preview);
    return card;
}

void BinaryDialog::rebuildChannelCombo(int channels)
{
    const QSignalBlocker blocker(m_prevChCombo);
    const int keep = m_prevChCombo->currentData().toInt();
    m_prevChCombo->clear();
    const int n = std::max(1, channels);
    for (int i = 0; i < n; ++i)
        m_prevChCombo->addItem(QStringLiteral("通道 %1（CH%1）").arg(i + 1), i);
    const int idx = m_prevChCombo->findData(keep);
    m_prevChCombo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void BinaryDialog::updateLayoutUi()
{
    const bool packed = options().layout == ecg::BinaryLayout::Packed12;
    m_formatCombo->setEnabled(!packed);
    m_chSpin->setEnabled(!packed);
    if (packed) {
        {
            const QSignalBlocker blocker(m_chSpin);
            m_chSpin->setValue(2);
        }
        rebuildChannelCombo(2);
    }
}

void BinaryDialog::applyOptions(const ecg::BinaryOptions& opt)
{
    {
        const QSignalBlocker b1(m_layoutCombo);
        const QSignalBlocker b2(m_formatCombo);
        const QSignalBlocker b3(m_endianCombo);
        const QSignalBlocker b4(m_chSpin);
        const QSignalBlocker b5(m_offSpin);
        const QSignalBlocker b6(m_fsSpin);
        const QSignalBlocker b7(m_scaleSpin);
        const QSignalBlocker b8(m_biasSpin);

        const int layoutIdx = m_layoutCombo->findData(static_cast<int>(opt.layout));
        if (layoutIdx >= 0) m_layoutCombo->setCurrentIndex(layoutIdx);
        const int fmtIdx = m_formatCombo->findData(static_cast<int>(opt.format));
        if (fmtIdx >= 0) m_formatCombo->setCurrentIndex(fmtIdx);
        m_endianCombo->setCurrentIndex(opt.littleEndian ? 0 : 1);
        m_chSpin->setValue(std::clamp(opt.channels, 1, 16));
        m_offSpin->setValue(static_cast<int>(std::min<qint64>(std::max<qint64>(0, opt.headerBytes), 200000000)));
        m_fsSpin->setValue(opt.sampleRate > 0.0 ? opt.sampleRate : 0.0);
        m_scaleSpin->setValue(opt.scale > 0.0 ? opt.scale : 1.0);
        m_biasSpin->setValue(opt.bias);
    }
    updateLayoutUi();
    rebuildChannelCombo(m_chSpin->value());
    refreshPreview();
    updateInfo();
}

ecg::BinaryOptions BinaryDialog::options() const
{
    ecg::BinaryOptions o;
    o.layout = static_cast<ecg::BinaryLayout>(m_layoutCombo->currentData().toInt());
    o.format = static_cast<ecg::SampleFormat>(m_formatCombo->currentData().toInt());
    o.littleEndian = m_endianCombo->currentData().toBool();
    o.channels = (o.layout == ecg::BinaryLayout::Packed12) ? 2 : m_chSpin->value();
    o.headerBytes = m_offSpin->value();
    o.sampleRate = m_fsSpin->value();
    o.scale = m_scaleSpin->value();
    o.bias = m_biasSpin->value();
    return o;
}

void BinaryDialog::schedulePreview()
{
    if (m_debounce) m_debounce->start();
    updateInfo();
}

void BinaryDialog::refreshPreview()
{
    const ecg::BinaryOptions o = options();
    const bool fsKnown = o.sampleRate > 0.0;
    const double fs = fsKnown ? o.sampleRate : 360.0;

    m_preview->setTitle(fsKnown
                            ? QStringLiteral("波形预览（前 5 秒 · %1 Hz）").arg(QString::number(fs, 'g', 6))
                            : QStringLiteral("波形预览（采样率未知，暂按 360 Hz 显示）"));

    ecg::BinaryOptions previewOpt = o;
    previewOpt.sampleRate = fs;

    ecg::Data preview;
    QString error;
    const int maxFrames = std::max(600, static_cast<int>(std::lround(fs * 5.0)));
    if (!ecg::loadBinary(m_path, previewOpt, preview, error, maxFrames)) {
        m_preview->clearSeriesData();
        m_preview->setEmptyText(error);
        return;
    }

    const int ch = std::clamp(m_prevChCombo->currentData().toInt(), 0, preview.cols() - 1);
    const std::vector<double>& col = preview.columns[static_cast<size_t>(ch)];
    const int n = static_cast<int>(col.size());

    QVector<double> xs(n), ys(n);
    for (int i = 0; i < n; ++i) {
        xs[i] = static_cast<double>(i) / fs;
        ys[i] = col[static_cast<size_t>(i)];
    }
    m_preview->setSeries(0, xs, ys);
    const double span = (n > 1) ? static_cast<double>(n - 1) / fs : 1.0;
    m_preview->setXRangeSilent(0.0, std::max(0.05, span));
}

void BinaryDialog::updateInfo()
{
    const QFileInfo fi(m_path);
    const qint64 size = fi.size();
    const ecg::BinaryOptions o = options();
    const qint64 frameBytes = o.bytesPerFrame();
    const qint64 usable = std::max<qint64>(0, size - o.headerBytes);
    const qint64 frames = frameBytes > 0 ? usable / frameBytes : 0;

    QString text = QStringLiteral("文件 %1 · 每帧 %2 字节 · %3 帧 · %4")
                       .arg(humanSize(size))
                       .arg(frameBytes)
                       .arg(frames)
                       .arg(o.describe());
    if (o.sampleRate > 0.0 && frames > 0)
        text += QStringLiteral(" · 时长 %1 s").arg(QString::number(frames / o.sampleRate, 'f', 2));
    if (frameBytes > 0 && usable % frameBytes != 0)
        text += QStringLiteral(" · 末尾余 %1 字节（格式或通道数可能不符）").arg(usable % frameBytes);
    m_infoLabel->setText(text);

    QStringList notes;
    if (m_probe.ok) {
        notes << QStringLiteral("参数来源：%1").arg(m_probe.source);
        notes << m_probe.notes;
    } else {
        notes << m_probe.notes;
    }
    if (o.sampleRate <= 0.0)
        notes.prepend(QStringLiteral("请填写采样率，否则无法计算心率与时间轴。"));
    m_noteLabel->setText(notes.join(QStringLiteral("\n")));
}

void BinaryDialog::autoDetect()
{
    ecg::BinaryProbe probe;
    QString error;
    if (!ecg::probeBinary(m_path, probe, error)) {
        QMessageBox::warning(this, QStringLiteral("自动识别"),
                             error.isEmpty() ? QStringLiteral("无法识别该文件的二进制格式。") : error);
        return;
    }
    m_probe = probe;
    applyOptions(probe.options);
}

void BinaryDialog::saveSidecar()
{
    const ecg::BinaryOptions o = options();
    QString error;
    if (!ecg::writeBinarySidecar(m_path, o, error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("参数说明已保存"),
        QStringLiteral("已写出：\n%1\n\n下次打开该文件时会自动识别这些参数。")
            .arg(QDir::toNativeSeparators(m_path + QStringLiteral(".json"))));
}

void BinaryDialog::accept()
{
    const ecg::BinaryOptions o = options();
    if (o.sampleRate <= 0.0) {
        QMessageBox::warning(this, QStringLiteral("缺少采样率"),
                             QStringLiteral("请填写采集设备实际的采样率（Hz）。"));
        m_fsSpin->setFocus();
        return;
    }

    ecg::Data probe;
    QString error;
    if (!ecg::loadBinary(m_path, o, probe, error, 512)) {
        QMessageBox::warning(this, QStringLiteral("无法按当前设置解析"), error);
        return;
    }
    QDialog::accept();
}
