#include "mainwindow.h"
#include "binarydialog.h"
#include "plotwidget.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringConverter>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace {

QVector<double> toVector(const std::vector<double>& v)
{
    QVector<double> out;
    out.reserve(static_cast<int>(v.size()));
    for (double x : v) out.append(x);
    return out;
}

QLabel* makeSectionLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("sectionTitle"));
    return l;
}

QDoubleSpinBox* makeSpin(double minV, double maxV, double value, int decimals, const QString& suffix)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(minV, maxV);
    s->setDecimals(decimals);
    s->setValue(value);
    s->setSingleStep(std::max(0.1, (maxV - minV) / 100.0));
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    return s;
}

QLabel* makeFormLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("formLabel"));
    return l;
}

QFrame* makeSeparator()
{
    auto* line = new QFrame;
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:#EDF1F7;border:none;"));
    return line;
}

// 二进制解析参数以 "int16|le|1|0|360|1|0|interleaved" 形式存进 QSettings
QString binOptionsToString(const ecg::BinaryOptions& o)
{
    return ecg::binaryOptionsToString(o);
}

bool binOptionsFromString(const QString& text, ecg::BinaryOptions& o)
{
    return ecg::binaryOptionsFromString(text, o);
}

QString defaultOpenDir(const QString& lastDir)
{
    if (!lastDir.isEmpty() && QFileInfo::exists(lastDir)) return lastDir;
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ECG R 峰检测 · Pan-Tompkins 算法"));
    resize(1440, 920);
    setMinimumSize(1024, 640);
    setAcceptDrops(true);

    if (QScreen* scr = QGuiApplication::primaryScreen()) {
        const QRect avail = scr->availableGeometry();
        const int w = std::max(1024, std::min(1440, avail.width() - 40));
        const int h = std::max(640, std::min(920, avail.height() - 50));
        resize(w, h);
        move(avail.x() + (avail.width() - w) / 2, avail.y() + (avail.height() - h) / 2);
    }

    auto* central = new QWidget;
    central->setObjectName(QStringLiteral("rootWidget"));
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    QWidget* side = buildSidePanel();
    QWidget* plots = buildPlotArea();

    // 左右可拖动：把波形区拉大 / 把参数面板收窄
    auto* mainSplit = new QSplitter(Qt::Horizontal);
    mainSplit->setObjectName(QStringLiteral("mainSplit"));
    mainSplit->setHandleWidth(8);
    mainSplit->setChildrenCollapsible(false);
    mainSplit->addWidget(side);
    mainSplit->addWidget(plots);
    mainSplit->setStretchFactor(0, 0);
    mainSplit->setStretchFactor(1, 1);
    mainSplit->setSizes({320, 1080});
    root->addWidget(mainSplit, 1);
    setCentralWidget(central);

    setupPlots();
    qApp->installEventFilter(this);

    // 采样率改动会改变时间轴，需要重画原始波形；其余参数只需重跑检测
    connect(m_fsSpin, &QDoubleSpinBox::valueChanged, this, [this]() {
        if (!m_data.loaded || m_data.rows() > 2000000) return;
        refreshRawPlot();
        if (m_annotatedMode) {
            m_usedFs = std::max(1.0, m_fsSpin->value());
            pt::recomputeStats(m_result, m_usedFs);
            m_result.ok = !m_result.rPeaks.empty();
            m_hasResult = m_result.ok;
            refreshAnnotatedView();
            setStatus(QStringLiteral("已按 %1 Hz 刷新时间轴（仍使用文件内的标注，未运行检测）")
                          .arg(QString::number(m_usedFs, 'g', 6)));
            return;
        }
        runDetection();
    });
    for (QDoubleSpinBox* spin : {m_lowCutSpin, m_highCutSpin, m_winSpin, m_refrSpin, m_weightSpin}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() {
            if (!m_data.loaded || m_data.rows() > 2000000) return;
            if (m_annotatedMode) {
                setStatus(QStringLiteral("当前为人工标注结果；改动算法参数后点「开始检测 R 峰」才会重新识别"));
                return;
            }
            runDetection();
        });
    }

    // ---- 菜单 ----
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    QAction* actOpen = fileMenu->addAction(QStringLiteral("打开数据文件…"));
    actOpen->setShortcut(QKeySequence::Open);
    QAction* actOpenBin = fileMenu->addAction(QStringLiteral("打开二进制原始数据（.bin）…"));
    actOpenBin->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    QAction* actDemo = fileMenu->addAction(QStringLiteral("载入示例数据"));
    actDemo->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    QAction* actExport = fileMenu->addAction(QStringLiteral("导出检测结果…"));
    actExport->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    QAction* actSaveAnn = fileMenu->addAction(QStringLiteral("导出标注结果（.bin）…"));
    actSaveAnn->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    fileMenu->addSeparator();
    QAction* actQuit = fileMenu->addAction(QStringLiteral("退出"));
    actQuit->setShortcut(QKeySequence::Quit);

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    QAction* actReset = viewMenu->addAction(QStringLiteral("缩放复位（回到起始窗口）"));
    actReset->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    QAction* actFitAll = viewMenu->addAction(QStringLiteral("全览整段记录"));
    actFitAll->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+0")));

    installActions();

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    QAction* actAbout = helpMenu->addAction(QStringLiteral("关于 Pan-Tompkins"));

    connect(actOpen, &QAction::triggered, this, &MainWindow::openFile);
    connect(actOpenBin, &QAction::triggered, this, &MainWindow::openBinaryFile);
    connect(actDemo, &QAction::triggered, this, &MainWindow::loadDemo);
    connect(actExport, &QAction::triggered, this, &MainWindow::exportResults);
    connect(actSaveAnn, &QAction::triggered, this, &MainWindow::exportAnnotated);
    connect(actQuit, &QAction::triggered, this, &QWidget::close);
    connect(actReset, &QAction::triggered, this, &MainWindow::resetView);
    connect(actFitAll, &QAction::triggered, this, [this]() {
        m_rawPlot->fitView();
        setStatus(QStringLiteral("已全览整段记录"));
    });
    connect(actAbout, &QAction::triggered, this, &MainWindow::showAbout);

    setStatus(QStringLiteral("就绪 · 请打开心电数据文件，或点「载入示例数据」体验"));
}

void MainWindow::installActions()
{
    QMenu* markMenu = menuBar()->addMenu(QStringLiteral("标注(&M)"));

    // 快捷键统一由 MainWindow::eventFilter 处理（避免与 QAction 重复触发），
    // 菜单项只显示按键提示（\t 之后的部分仅作显示）。
    QAction* actAdd = markMenu->addAction(QStringLiteral("在光标处标注 R 峰\tA"));
    QAction* actDel = markMenu->addAction(QStringLiteral("删除光标处的 R 峰\tD"));

    markMenu->addSeparator();
    QAction* actLeft = markMenu->addAction(QStringLiteral("波形向左移动\t←"));
    QAction* actRight = markMenu->addAction(QStringLiteral("波形向右移动\t→"));

    connect(actAdd, &QAction::triggered, this, &MainWindow::addPeakNow);
    connect(actDel, &QAction::triggered, this, &MainWindow::deletePeakNow);
    connect(actLeft, &QAction::triggered, this, &MainWindow::panLeft);
    connect(actRight, &QAction::triggered, this, &MainWindow::panRight);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e)
{
    // A/D 标注、←/→ 平移：只要本窗口是活动窗口就生效，
    // 不依赖焦点落在哪个控件上（数值框编辑时方向键仍归控件）。
    if (e->type() != QEvent::KeyPress || !isActiveWindow()
        || QApplication::activePopupWidget() != nullptr)
        return QMainWindow::eventFilter(obj, e);

    auto* ke = static_cast<QKeyEvent*>(e);
    if (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))
        return QMainWindow::eventFilter(obj, e);

    QWidget* fw = QApplication::focusWidget();
    const bool textInput = qobject_cast<QLineEdit*>(fw) != nullptr;
    if (textInput) return QMainWindow::eventFilter(obj, e);

    switch (ke->key()) {
    case Qt::Key_A:
        addPeakNow();
        return true;
    case Qt::Key_D:
        deletePeakNow();
        return true;
    case Qt::Key_Left:
    case Qt::Key_Right:
        if (qobject_cast<QAbstractSpinBox*>(fw)) return QMainWindow::eventFilter(obj, e);
        if (ke->key() == Qt::Key_Left) panLeft();
        else panRight();
        return true;
    default:
        break;
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::fitViewNow()
{
    if (m_rawPlot && m_data.loaded) {
        m_rawPlot->fitView();
        setStatus(QStringLiteral("已全览整段记录"));
    }
}

void MainWindow::setEditCursorTime(double t)
{
    if (!m_rawPlot) return;
    m_rawPlot->setCursorTime(t);
    updateCursorLabel(t);
}

void MainWindow::updateCursorLabel(double t)
{
    if (!m_cursorLabel) return;
    if (!std::isfinite(t)) {
        m_cursorLabel->setText(QStringLiteral("光标：—"));
        return;
    }
    const double fs = m_data.loaded ? std::max(1.0, m_usedFs) : 360.0;
    m_cursorLabel->setText(QStringLiteral("光标：%1 s · 采样点 %2")
                               .arg(QString::number(t, 'f', 3))
                               .arg(static_cast<int>(std::lround(t * fs))));
}

int MainWindow::nearestPeakPosition(int sample, double maxDistanceSec) const
{
    if (m_result.rPeaks.empty()) return -1;
    const double fs = std::max(1.0, m_usedFs);
    const int maxDist = static_cast<int>(std::lround(maxDistanceSec * fs));
    int best = -1;
    int bestD = INT_MAX;
    for (size_t i = 0; i < m_result.rPeaks.size(); ++i) {
        const int d = std::abs(m_result.rPeaks[i] - sample);
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return (bestD <= maxDist) ? best : -1;
}

int MainWindow::snapToExtremum(int sample) const
{
    if (!m_data.loaded || m_usedColumn < 0 || m_usedColumn >= m_data.cols()) return sample;
    const std::vector<double>& col = m_data.columns[static_cast<size_t>(m_usedColumn)];
    const int n = static_cast<int>(col.size());
    if (n <= 2) return sample;

    const double fs = std::max(1.0, m_usedFs);
    const int half = std::max(2, static_cast<int>(std::lround(0.06 * fs))); // ±60 ms

    // 极性投票：已有 R 峰处是正峰还是负峰
    int votes = 0;
    for (int idx : m_result.rPeaks) {
        if (idx - 2 < 0 || idx + 2 >= n) continue;
        const double v = col[static_cast<size_t>(idx)];
        if (v >= col[static_cast<size_t>(idx - 2)] && v >= col[static_cast<size_t>(idx + 2)]) ++votes;
        else if (v <= col[static_cast<size_t>(idx - 2)] && v <= col[static_cast<size_t>(idx + 2)]) --votes;
    }

    const int lo = std::max(1, sample - half);
    const int hi = std::min(n - 2, sample + half);
    if (hi <= lo) return std::clamp(sample, 0, n - 1);

    // 只在窗口内找“局部极值”作为落点；窗口内单调时保留光标原位置，
    // 避免一路滑到窗口边缘。
    int best = std::clamp(sample, 0, n - 1);
    double bestScore = -1e300;
    for (int i = lo; i <= hi; ++i) {
        const double v0 = col[static_cast<size_t>(i - 1)];
        const double v1 = col[static_cast<size_t>(i)];
        const double v2 = col[static_cast<size_t>(i + 1)];
        double s0 = v0, s1 = v1, s2 = v2;
        if (votes < 0) { s0 = -v0; s1 = -v1; s2 = -v2; }
        else if (votes == 0) { s0 = std::fabs(v0); s1 = std::fabs(v1); s2 = std::fabs(v2); }
        if (s1 < s0 || s1 < s2) continue;      // 不是局部极值
        if (s1 > bestScore) {
            bestScore = s1;
            best = i;
        }
    }
    return best;
}

void MainWindow::addPeakNow()
{
    if (!m_data.loaded) {
        setStatus(QStringLiteral("请先载入心电数据"));
        return;
    }
    if (m_usedColumn < 0 || m_usedColumn >= m_data.cols()) return;

    const double fs = std::max(1.0, m_usedFs > 0.0 ? m_usedFs : m_fsSpin->value());
    double t = m_rawPlot->hasCursor() ? m_rawPlot->cursorTime()
                                      : 0.5 * (m_rawPlot->xStart() + m_rawPlot->xEnd());
    int sample = static_cast<int>(std::lround(t * fs));
    sample = std::clamp(sample, 0, std::max(0, m_data.rows() - 1));
    sample = snapToExtremum(sample);

    if (nearestPeakPosition(sample, 0.20) >= 0) {
        setStatus(QStringLiteral("光标 %1 s 附近 200 ms 内已有 R 峰，未重复添加")
                      .arg(QString::number(sample / fs, 'f', 3)));
        return;
    }

    const auto it = std::lower_bound(m_result.rPeaks.begin(), m_result.rPeaks.end(), sample);
    const size_t pos = static_cast<size_t>(it - m_result.rPeaks.begin());
    m_result.rPeaks.insert(it, sample);
    m_result.fromSearchBack.insert(m_result.fromSearchBack.begin() + static_cast<long>(pos), false);
    if (m_result.rAmp.size() == m_result.rPeaks.size() - 1)
        m_result.rAmp.insert(m_result.rAmp.begin() + static_cast<long>(pos), 0.0);
    m_peakType.insert(m_peakType.begin() + static_cast<long>(pos), 2);

    appendEditLog(QStringLiteral("新增"), sample,
                  QStringLiteral("在 %1 s 标注 R 峰").arg(QString::number(sample / fs, 'f', 3)));
    afterPeaksChanged();
    setStatus(QStringLiteral("已标注 R 峰 @ %1 s（采样点 %2），共 %3 个")
                  .arg(QString::number(sample / fs, 'f', 3))
                  .arg(sample)
                  .arg(m_result.rPeaks.size()));
}

void MainWindow::deletePeakNow()
{
    if (!m_data.loaded) {
        setStatus(QStringLiteral("请先载入心电数据"));
        return;
    }
    const double fs = std::max(1.0, m_usedFs > 0.0 ? m_usedFs : m_fsSpin->value());
    const double t = m_rawPlot->hasCursor() ? m_rawPlot->cursorTime()
                                            : 0.5 * (m_rawPlot->xStart() + m_rawPlot->xEnd());
    const int sample = static_cast<int>(std::lround(t * fs));

    const int pos = nearestPeakPosition(sample, 0.30);
    if (pos < 0) {
        setStatus(QStringLiteral("光标 %1 s 附近 300 ms 内没有可删除的 R 峰")
                      .arg(QString::number(t, 'f', 3)));
        return;
    }

    const int removed = m_result.rPeaks[static_cast<size_t>(pos)];
    m_result.rPeaks.erase(m_result.rPeaks.begin() + pos);
    if (pos < static_cast<int>(m_result.fromSearchBack.size()))
        m_result.fromSearchBack.erase(m_result.fromSearchBack.begin() + pos);
    if (pos < static_cast<int>(m_result.rAmp.size()))
        m_result.rAmp.erase(m_result.rAmp.begin() + pos);
    if (pos < static_cast<int>(m_peakType.size()))
        m_peakType.erase(m_peakType.begin() + pos);

    appendEditLog(QStringLiteral("删除"), removed,
                  QStringLiteral("删除 %1 s 处的 R 峰").arg(QString::number(removed / fs, 'f', 3)));
    afterPeaksChanged();
    setStatus(QStringLiteral("已删除 R 峰 @ %1 s，剩余 %2 个")
                  .arg(QString::number(removed / fs, 'f', 3))
                  .arg(m_result.rPeaks.size()));
}

void MainWindow::panLeft()
{
    if (!m_data.loaded) return;
    m_rawPlot->panByFraction(-0.12);
    setStatus(QStringLiteral("波形已左移（←/→ 可连续移动，Shift 不再加速）"));
}

void MainWindow::panRight()
{
    if (!m_data.loaded) return;
    m_rawPlot->panByFraction(0.12);
    setStatus(QStringLiteral("波形已右移（←/→ 可连续移动）"));
}

void MainWindow::resetEditState()
{
    m_editCount = 0;
    m_ann = ecg::Annotations();
    m_peakType.assign(m_result.rPeaks.size(), 1);
    updateEditLogUi();
}

void MainWindow::appendEditLog(const QString& action, int sample, const QString& note)
{
    const double fs = std::max(1.0, m_usedFs);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString line = QStringLiteral("%1|%2|%3|%4|%5")
                             .arg(stamp, action)
                             .arg(sample)
                             .arg(QString::number(sample / fs, 'f', 3), note);
    m_ann.log << line;
    ++m_editCount;
    updateEditLogUi();
}

void MainWindow::updateEditLogUi()
{
    if (!m_logList || !m_logLabel) return;
    m_logList->clear();
    const int show = std::min<int>(static_cast<int>(m_ann.log.size()), 200);
    for (int i = static_cast<int>(m_ann.log.size()) - 1; i >= static_cast<int>(m_ann.log.size()) - show; --i) {
        const QString line = m_ann.log.at(i);
        const QStringList parts = line.split(QLatin1Char('|'));
        if (parts.size() >= 4) {
            m_logList->addItem(QStringLiteral("%1  %2  @%3 s")
                                   .arg(parts.at(0), parts.at(1), parts.at(3)));
        } else {
            m_logList->addItem(line);
        }
    }
    if (m_editCount <= 0) {
        m_logLabel->setText(QStringLiteral("尚未修改"));
    } else {
        QString last;
        if (!m_ann.log.isEmpty()) {
            const QStringList parts = m_ann.log.last().split(QLatin1Char('|'));
            if (parts.size() >= 4)
                last = QStringLiteral("%1 @%2 s").arg(parts.at(1), parts.at(3));
        }
        m_logLabel->setText(QStringLiteral("已修改 %1 次 · 最近：%2").arg(m_editCount).arg(last));
    }
}

void MainWindow::afterPeaksChanged()
{
    m_result.ok = !m_result.rPeaks.empty();
    m_hasResult = m_result.ok;
    m_result.rr.clear();
    m_result.instHr.clear();
    pt::recomputeStats(m_result, std::max(1.0, m_usedFs));

    refreshPeakMarkers();
    refreshRrPlot();
    refreshPWave();
    updateStats();
    if (m_exportBtn) m_exportBtn->setEnabled(m_hasResult);
}

void MainWindow::refreshPeakMarkers()
{
    if (!m_data.loaded || m_usedColumn < 0 || m_usedColumn >= m_data.cols()) return;
    const double fs = std::max(1.0, m_usedFs);
    const std::vector<double>& raw = m_data.columns[static_cast<size_t>(m_usedColumn)];

    QVector<double> beats, sb, manual;
    QVector<double> px, py, pyFilt;
    for (size_t i = 0; i < m_result.rPeaks.size(); ++i) {
        const int idx = m_result.rPeaks[i];
        const double t = static_cast<double>(idx) / fs;
        const unsigned char ty = (i < m_peakType.size()) ? m_peakType[i] : 1;
        if (ty == 2) manual.append(t);
        else if (i < m_result.fromSearchBack.size() && m_result.fromSearchBack[i]) sb.append(t);
        else beats.append(t);

        if (idx >= 0 && idx < static_cast<int>(raw.size())) {
            px.append(t);
            py.append(raw[static_cast<size_t>(idx)]);
            if (idx < static_cast<int>(m_result.filtered.size()))
                pyFilt.append(m_result.filtered[static_cast<size_t>(idx)]);
        }
    }

    for (PlotWidget* p : {m_rawPlot, m_filtPlot}) {
        p->setMarkers(beats, QColor(0xE1, 0x1D, 0x48));
        p->setSearchbackMarkers(sb, QColor(0xF7, 0x8C, 0x1E));
        p->setManualMarkers(manual, QColor(0x0E, 0xA5, 0xE9));
    }
    m_rawPlot->setSeries(1, px, py);
    if (pyFilt.size() == px.size() && !m_result.filtered.empty()) m_filtPlot->setSeries(1, px, pyFilt);
}

void MainWindow::refreshRrPlot()
{
    const double fs = std::max(1.0, m_usedFs);
    QVector<double> rrX, rrY;
    const int rrN = static_cast<int>(m_result.rr.size());
    rrX.reserve(rrN);
    rrY.reserve(rrN);
    for (int i = 0; i < rrN; ++i) {
        rrX.append(static_cast<double>(m_result.rPeaks[static_cast<size_t>(i + 1)]) / fs);
        rrY.append(m_result.rr[static_cast<size_t>(i)] * 1000.0);
    }
    m_rrPlot->setSeries(0, rrX, rrY);

    if (rrN > 0) {
        QVector<double> mx, my;
        const int n = m_data.rows();
        mx << (n > 0 ? static_cast<double>(m_result.rPeaks.front()) / fs : 0.0)
           << (n > 0 ? static_cast<double>(m_result.rPeaks.back()) / fs : 1.0);
        my << m_result.meanRr * 1000.0 << m_result.meanRr * 1000.0;
        m_rrPlot->setSeries(1, mx, my);
        m_rrPlot->setEmptyText(QStringLiteral("等待检测"));
    } else {
        const QVector<double> empty;
        m_rrPlot->setSeries(1, empty, empty);
        m_rrPlot->setEmptyText(QStringLiteral("R 峰不足 2 个，无法计算 RR"));
    }
}

void MainWindow::refreshPWave()
{
    if (!m_data.loaded || m_usedColumn < 0 || m_usedColumn >= m_data.cols()
        || m_result.rPeaks.empty()) {
        m_pw = pw::Result();
        refreshPWaveMarkers();
        updatePWaveUi();
        return;
    }

    pw::Params p;
    p.fs = std::max(1.0, m_usedFs);
    m_pw = pw::detect(m_data.columns[static_cast<size_t>(m_usedColumn)], m_result.rPeaks, p);

    refreshPWaveMarkers();
    updatePWaveUi();
}

void MainWindow::refreshPWaveMarkers()
{
    const QVector<double> empty;
    if (!m_showP || !m_pw.ok || m_usedColumn < 0 || m_usedColumn >= m_data.cols()) {
        m_rawPlot->setSeries(2, empty, empty);
        m_rawPlot->setSeries(3, empty, empty);
        return;
    }

    const std::vector<double>& raw = m_data.columns[static_cast<size_t>(m_usedColumn)];
    const double fs = std::max(1.0, m_usedFs);
    const int n = static_cast<int>(raw.size());

    QVector<double> px, py, lx, ly;
    px.reserve(static_cast<int>(m_pw.beats.size()));
    py.reserve(static_cast<int>(m_pw.beats.size()));
    for (const pw::Beat& b : m_pw.beats) {
        if (b.morph == pw::Morph::Invalid || b.p.index < 0 || b.p.index >= n) continue;
        if (b.quality <= 0) continue;
        px.append(b.p.index / fs);
        py.append(raw[static_cast<size_t>(b.p.index)]);
        if (b.pos.significant && b.pos.index >= 0 && b.pos.index < n) {
            lx.append(b.pos.index / fs);
            ly.append(raw[static_cast<size_t>(b.pos.index)]);
        }
        if (b.neg.significant && b.neg.index >= 0 && b.neg.index < n) {
            lx.append(b.neg.index / fs);
            ly.append(raw[static_cast<size_t>(b.neg.index)]);
        }
    }
    m_rawPlot->setSeries(2, lx, ly);   // 两侧瓣峰
    m_rawPlot->setSeries(3, px, py);   // P 波（画在最上层）
}

void MainWindow::updatePWaveUi()
{
    if (!m_pwLabel || !m_flipLabel) return;
    if (!m_pw.ok || m_pw.beats.empty()) {
        m_pwLabel->setText(QStringLiteral("尚未计算（需要先有 R 峰）"));
        m_flipLabel->setText(QString());
        return;
    }
    const int total = static_cast<int>(m_pw.beats.size());
    const int usable = total - m_pw.nFlat - m_pw.nInvalid;
    m_pwLabel->setText(QStringLiteral("可判读 %1/%2 · 正相 %3 · 负相 %4 · 双相 %5\n低平 %6 · 无效 %7 · 平均 PR %8 ms")
                           .arg(usable).arg(total)
                           .arg(m_pw.nPositive).arg(m_pw.nNegative).arg(m_pw.nBiphasic)
                           .arg(m_pw.nFlat).arg(m_pw.nInvalid)
                           .arg(QString::number(m_pw.meanPrMs, 'f', 0)));
    if (m_pw.flipBeat >= 0) {
        m_flipLabel->setText(QStringLiteral("极性翻转：第 %1 搏 @ %2 s（模板补位 %3 搏）")
                                 .arg(m_pw.flipBeat + 1)
                                 .arg(QString::number(m_pw.flipTime, 'f', 2))
                                 .arg(m_pw.nTemplate));
    } else {
        m_flipLabel->setText(QStringLiteral("极性翻转：未检出（模板补位 %1 搏）").arg(m_pw.nTemplate));
    }
}

bool MainWindow::exportPWaveTo(const QString& path, QString& error)
{
    if (!m_pw.ok || m_pw.beats.empty()) {
        error = QStringLiteral("当前没有 P 波检测结果。");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = QStringLiteral("无法写入 %1：%2").arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts.setGenerateByteOrderMark(true);

    ts << "周期,R峰索引,R峰时间(s),P波索引,P波时间(s),P波幅值,正向峰索引,正向峰时间(s),正向峰幅值,"
          "负向峰索引,负向峰时间(s),负向峰幅值,净幅值,形态,极性,宽度(ms),PR(ms),SNR,质量,来源,距R偏移(ms)\n";
    for (size_t i = 0; i < m_pw.beats.size(); ++i) {
        const pw::Beat& b = m_pw.beats[i];
        QString morph;
        switch (b.morph) {
        case pw::Morph::Positive: morph = QStringLiteral("正相"); break;
        case pw::Morph::Negative: morph = QStringLiteral("负相"); break;
        case pw::Morph::Biphasic: morph = QStringLiteral("双相"); break;
        case pw::Morph::Flat: morph = QStringLiteral("低平"); break;
        default: morph = QStringLiteral("无效"); break;
        }
        ts << static_cast<int>(i + 1) << ',' << b.rIndex << ','
           << QString::number(b.rTime, 'f', 4) << ','
           << b.p.index << ',' << QString::number(b.p.time, 'f', 4) << ','
           << QString::number(b.p.amp, 'f', 4) << ','
           << b.pos.index << ',' << QString::number(b.pos.time, 'f', 4) << ','
           << QString::number(b.pos.amp, 'f', 4) << ','
           << b.neg.index << ',' << QString::number(b.neg.time, 'f', 4) << ','
           << QString::number(b.neg.amp, 'f', 4) << ','
           << QString::number(b.signedAmp, 'f', 4) << ','
           << morph << ','
           << (b.signedAmp >= 0.0 ? QStringLiteral("+") : QStringLiteral("-")) << ','
           << QString::number(b.widthMs, 'f', 1) << ','
           << QString::number(b.prMs, 'f', 1) << ','
           << QString::number(b.snr, 'f', 2) << ','
           << b.quality << ','
           << (b.source == pw::Source::Template ? QStringLiteral("模板") : QStringLiteral("实测")) << ','
           << QString::number((b.p.time - b.rTime) * 1000.0, 'f', 1) << '\n';
    }

    ts << "\n# 汇总\n";
    ts << "# 数据文件," << (m_data.filePath.isEmpty() ? QStringLiteral("(内置示例)") : m_data.filePath) << '\n';
    ts << "# 采样率(Hz)," << QString::number(m_usedFs, 'f', 2) << '\n';
    ts << "# R峰数," << m_result.rPeaks.size() << '\n';
    ts << "# 正相/负相/双相/低平/无效,"
       << m_pw.nPositive << '/' << m_pw.nNegative << '/' << m_pw.nBiphasic << '/'
       << m_pw.nFlat << '/' << m_pw.nInvalid << '\n';
    ts << "# 平均PR(ms)," << QString::number(m_pw.meanPrMs, 'f', 1) << '\n';
    ts << "# 平均P宽度(ms)," << QString::number(m_pw.meanWidthMs, 'f', 1) << '\n';
    ts << "# 模板补位(搏)," << m_pw.nTemplate << '\n';
    if (m_pw.flipBeat >= 0)
        ts << "# 极性翻转,第" << (m_pw.flipBeat + 1) << "搏 @" << QString::number(m_pw.flipTime, 'f', 2) << "s\n";
    else
        ts << "# 极性翻转,未检出\n";
    f.close();

    setStatus(QStringLiteral("已导出 P 波结果：%1 搏 → %2")
                  .arg(m_pw.beats.size())
                  .arg(QDir::toNativeSeparators(path)));
    return true;
}

void MainWindow::exportPWave()
{
    if (!m_pw.ok || m_pw.beats.empty()) {
        QMessageBox::information(this, QStringLiteral("导出 P 波"),
                                 QStringLiteral("当前没有 P 波检测结果。"));
        return;
    }
    QString base = QStringLiteral("pwave.csv");
    if (!m_data.filePath.isEmpty())
        base = QFileInfo(m_data.filePath).completeBaseName() + QStringLiteral("_pwave.csv");
    const QString startDir = m_lastDir.isEmpty()
                                 ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                                 : m_lastDir;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 P 波结果"), QDir(startDir).filePath(base),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;

    QString error;
    if (!exportPWaveTo(path, error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
}

void MainWindow::refreshAnnotatedView()
{
    if (!m_data.loaded || m_usedColumn < 0 || m_usedColumn >= m_data.cols()) return;
    const double fs = std::max(1.0, m_usedFs);
    const std::vector<double>& col = m_data.columns[static_cast<size_t>(m_usedColumn)];
    const int n = static_cast<int>(col.size());

    QVector<double> tx(n);
    for (int i = 0; i < n; ++i) tx[i] = static_cast<double>(i) / fs;
    m_rawPlot->setSeries(0, tx, toVector(col));

    m_filtPlot->clearSeriesData();
    m_filtPlot->setEmptyText(QStringLiteral("已导入人工标注 · 未运行检测算法"));
    m_envPlot->clearSeriesData();
    m_envPlot->setEmptyText(QStringLiteral("已导入人工标注 · 未运行检测算法"));
    m_rrPlot->setEmptyText(QStringLiteral("等待检测"));

    refreshPeakMarkers();
    refreshRrPlot();
    refreshPWave();
    updateStats();
    if (m_exportBtn) m_exportBtn->setEnabled(m_hasResult);
}

QWidget* MainWindow::buildSidePanel()
{
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("cardPanel"));

    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(16, 14, 16, 12);
    v->setSpacing(7);

    // ---------------- 数据源 ----------------
    v->addWidget(makeSectionLabel(QStringLiteral("数 据 源")));

    auto* openBtn = new QPushButton(QStringLiteral("打开数据文件…"));
    openBtn->setObjectName(QStringLiteral("primaryBtn"));
    openBtn->setCursor(Qt::PointingHandCursor);
    openBtn->setToolTip(QStringLiteral("支持文本 CSV / TXT / TSV，以及二进制 .bin / .raw / .dat"));
    connect(openBtn, &QPushButton::clicked, this, &MainWindow::openFile);
    v->addWidget(openBtn);

    auto* srcRow = new QHBoxLayout;
    srcRow->setContentsMargins(0, 0, 0, 0);
    srcRow->setSpacing(8);

    auto* demoBtn = new QPushButton(QStringLiteral("载入示例数据"));
    demoBtn->setCursor(Qt::PointingHandCursor);
    demoBtn->setToolTip(QStringLiteral("生成 20 秒、含基线漂移与工频干扰的合成心电"));
    connect(demoBtn, &QPushButton::clicked, this, &MainWindow::loadDemo);
    srcRow->addWidget(demoBtn, 1);

    auto* binBtn = new QPushButton(QStringLiteral("二进制 .bin…"));
    binBtn->setCursor(Qt::PointingHandCursor);
    binBtn->setToolTip(QStringLiteral("打开二进制原始数据，可设置位宽 / 字节序 / 通道数 / 采样率"));
    connect(binBtn, &QPushButton::clicked, this, &MainWindow::openBinaryFile);
    srcRow->addWidget(binBtn, 1);
    v->addLayout(srcRow);

    m_fileLabel = new QLabel(QStringLiteral("尚未选择文件"));
    m_fileLabel->setObjectName(QStringLiteral("fileLabel"));
    m_fileLabel->setWordWrap(false);
    m_fileLabel->setMinimumHeight(18);
    v->addWidget(m_fileLabel);

    auto* form1 = new QFormLayout;
    form1->setContentsMargins(0, 2, 0, 0);
    form1->setHorizontalSpacing(10);
    form1->setVerticalSpacing(6);
    form1->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form1->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_fsSpin = makeSpin(20.0, 5000.0, 360.0, 1, QStringLiteral(" Hz"));
    m_fsSpin->setToolTip(QStringLiteral("采样率，直接影响所有时间参数与心率计算"));
    form1->addRow(makeFormLabel(QStringLiteral("采样率")), m_fsSpin);

    m_colCombo = new QComboBox;
    m_colCombo->setToolTip(QStringLiteral("选择作为心电信号的列"));
    connect(m_colCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onColumnChanged);
    form1->addRow(makeFormLabel(QStringLiteral("信号通道")), m_colCombo);
    v->addLayout(form1);

    v->addWidget(makeSeparator());

    // ---------------- 算法参数 ----------------
    auto* paramHeader = new QHBoxLayout;
    paramHeader->setContentsMargins(0, 0, 0, 0);
    paramHeader->addWidget(makeSectionLabel(QStringLiteral("算 法 参 数")));
    paramHeader->addStretch(1);
    auto* resetBtn = new QPushButton(QStringLiteral("恢复默认"));
    resetBtn->setCursor(Qt::PointingHandCursor);
    resetBtn->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;border:none;color:#3B82F6;padding:0;font-size:8pt;}"));
    connect(resetBtn, &QPushButton::clicked, this, &MainWindow::resetParams);
    paramHeader->addWidget(resetBtn);
    v->addLayout(paramHeader);

    auto* form2 = new QFormLayout;
    form2->setContentsMargins(0, 2, 0, 0);
    form2->setHorizontalSpacing(10);
    form2->setVerticalSpacing(6);
    form2->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form2->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_lowCutSpin = makeSpin(0.5, 40.0, 5.0, 1, QStringLiteral(" Hz"));
    m_highCutSpin = makeSpin(2.0, 100.0, 15.0, 1, QStringLiteral(" Hz"));
    m_winSpin = makeSpin(40.0, 400.0, 150.0, 0, QStringLiteral(" ms"));
    m_refrSpin = makeSpin(100.0, 500.0, 200.0, 0, QStringLiteral(" ms"));
    m_weightSpin = makeSpin(0.05, 0.60, 0.25, 2, QString());

    m_lowCutSpin->setToolTip(QStringLiteral("带通下限，抑制基线漂移与 T 波"));
    m_highCutSpin->setToolTip(QStringLiteral("带通上限，抑制肌电与工频干扰"));
    m_winSpin->setToolTip(QStringLiteral("移动窗积分窗长，通常取 QRS 宽度"));
    m_refrSpin->setToolTip(QStringLiteral("不应期，既是最小 RR 间期也是回溯搜索步长"));
    m_weightSpin->setToolTip(QStringLiteral("自适应阈值系数：thr = NPKI + w·(SPKI−NPKI)"));

    form2->addRow(makeFormLabel(QStringLiteral("低截止频率")), m_lowCutSpin);
    form2->addRow(makeFormLabel(QStringLiteral("高截止频率")), m_highCutSpin);
    form2->addRow(makeFormLabel(QStringLiteral("积分窗长")), m_winSpin);
    form2->addRow(makeFormLabel(QStringLiteral("不应期")), m_refrSpin);
    form2->addRow(makeFormLabel(QStringLiteral("阈值系数")), m_weightSpin);
    v->addLayout(form2);

    m_detectBtn = new QPushButton(QStringLiteral("开始检测 R 峰"));
    m_detectBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_detectBtn->setMinimumHeight(40);
    m_detectBtn->setCursor(Qt::PointingHandCursor);
    m_detectBtn->setEnabled(false);
    connect(m_detectBtn, &QPushButton::clicked, this, &MainWindow::runDetection);
    v->addWidget(m_detectBtn);

    v->addWidget(makeSeparator());

    // ---------------- 结果 ----------------
    v->addWidget(makeSectionLabel(QStringLiteral("检 测 结 果")));

    auto* hrBox = new QWidget;
    auto* hrLayout = new QHBoxLayout(hrBox);
    hrLayout->setContentsMargins(0, 0, 0, 0);
    hrLayout->setSpacing(6);
    m_hrValue = new QLabel(QStringLiteral("—"));
    m_hrValue->setObjectName(QStringLiteral("hrValue"));
    auto* hrUnit = new QLabel(QStringLiteral("bpm 平均心率"));
    hrUnit->setObjectName(QStringLiteral("hrUnit"));
    hrLayout->addWidget(m_hrValue);
    hrLayout->addWidget(hrUnit, 0, Qt::AlignBottom);
    hrLayout->addStretch();
    v->addWidget(hrBox);

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 2, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(7);
    grid->addWidget(makeMetric(QStringLiteral("R 峰数量"), &m_beatValue), 0, 0);
    grid->addWidget(makeMetric(QStringLiteral("平均 RR (ms)"), &m_meanRrValue), 0, 1);
    grid->addWidget(makeMetric(QStringLiteral("SDNN (ms)"), &m_sdnnValue), 1, 0);
    grid->addWidget(makeMetric(QStringLiteral("RMSSD (ms)"), &m_rmssdValue), 1, 1);
    grid->addWidget(makeMetric(QStringLiteral("pNN50 (%)"), &m_pnnValue), 2, 0);
    v->addLayout(grid);

    m_exportBtn = new QPushButton(QStringLiteral("导出 R 峰结果 (CSV)"));
    m_exportBtn->setCursor(Qt::PointingHandCursor);
    m_exportBtn->setEnabled(false);
    connect(m_exportBtn, &QPushButton::clicked, this, &MainWindow::exportResults);
    v->addWidget(m_exportBtn);

    v->addWidget(makeSeparator());

    // ---------------- P 波 ----------------
    v->addWidget(makeSectionLabel(QStringLiteral("P 波 检 测")));

    m_pwLabel = new QLabel(QStringLiteral("尚未计算"));
    m_pwLabel->setObjectName(QStringLiteral("formLabel"));
    m_pwLabel->setWordWrap(true);
    v->addWidget(m_pwLabel);

    m_flipLabel = new QLabel(QString());
    m_flipLabel->setObjectName(QStringLiteral("hintLabel"));
    m_flipLabel->setWordWrap(true);
    v->addWidget(m_flipLabel);

    auto* pRow = new QHBoxLayout;
    pRow->setContentsMargins(0, 0, 0, 0);
    pRow->setSpacing(8);
    m_showPBox = new QCheckBox(QStringLiteral("显示标记"));
    m_showPBox->setChecked(true);
    m_showPBox->setCursor(Qt::PointingHandCursor);
    m_showPBox->setToolTip(QStringLiteral("在原始波形上叠加 P 波（绿色）与两侧瓣峰（橙色）"));
    connect(m_showPBox, &QCheckBox::toggled, this, [this](bool on) {
        m_showP = on;
        refreshPWaveMarkers();
    });
    auto* pExportBtn = new QPushButton(QStringLiteral("导出 P 波 (CSV)"));
    pExportBtn->setCursor(Qt::PointingHandCursor);
    pExportBtn->setToolTip(QStringLiteral("逐搏输出 P 波位置、正/负瓣峰、PR、形态与质量"));
    connect(pExportBtn, &QPushButton::clicked, this, &MainWindow::exportPWave);
    pRow->addWidget(m_showPBox);
    pRow->addWidget(pExportBtn, 1);
    v->addLayout(pRow);

    v->addWidget(makeSeparator());

    // ---------------- 人工标注 ----------------
    v->addWidget(makeSectionLabel(QStringLiteral("人 工 标 注")));

    m_cursorLabel = new QLabel(QStringLiteral("光标：—　（在波形上移动鼠标定位）"));
    m_cursorLabel->setObjectName(QStringLiteral("formLabel"));
    m_cursorLabel->setWordWrap(true);
    v->addWidget(m_cursorLabel);

    auto* editRow = new QHBoxLayout;
    editRow->setContentsMargins(0, 0, 0, 0);
    editRow->setSpacing(8);
    auto* addBtn = new QPushButton(QStringLiteral("标注 (A)"));
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setToolTip(QStringLiteral("在光标处新增 R 峰，自动吸附到附近最大的局部极值"));
    connect(addBtn, &QPushButton::clicked, this, &MainWindow::addPeakNow);
    auto* delBtn = new QPushButton(QStringLiteral("删除 (D)"));
    delBtn->setCursor(Qt::PointingHandCursor);
    delBtn->setToolTip(QStringLiteral("删除离光标最近的 R 峰（光标 300 ms 内）"));
    connect(delBtn, &QPushButton::clicked, this, &MainWindow::deletePeakNow);
    editRow->addWidget(addBtn, 1);
    editRow->addWidget(delBtn, 1);
    v->addLayout(editRow);

    m_logLabel = new QLabel(QStringLiteral("尚未修改"));
    m_logLabel->setObjectName(QStringLiteral("hintLabel"));
    v->addWidget(m_logLabel);

    m_logList = new QListWidget;
    m_logList->setObjectName(QStringLiteral("editLog"));
    m_logList->setMinimumHeight(58);
    m_logList->setMaximumHeight(88);
    m_logList->setFocusPolicy(Qt::NoFocus); // 不抢方向键
    m_logList->setSelectionMode(QAbstractItemView::NoSelection);
    m_logList->setToolTip(QStringLiteral("每一次人工增删都会记录在这里，并写进导出的标注 bin"));
    v->addWidget(m_logList);

    auto* saveAnnBtn = new QPushButton(QStringLiteral("导出标注结果 (.bin)"));
    saveAnnBtn->setCursor(Qt::PointingHandCursor);
    saveAnnBtn->setToolTip(QStringLiteral("把当前 R 峰与修改记录写成自描述 bin，再次导入时直接使用、不跑算法"));
    connect(saveAnnBtn, &QPushButton::clicked, this, &MainWindow::exportAnnotated);
    v->addWidget(saveAnnBtn);

    v->addStretch(1);

    auto* hint = new QLabel(QStringLiteral("滚轮缩放 · 拖拽平移 · ←/→ 移动波形 · A 标注 · D 删除"));
    hint->setObjectName(QStringLiteral("hintLabel"));
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("sideScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(296);
    scroll->setMaximumWidth(560);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#sideScroll{background:transparent;border:none;}"
        "QScrollArea#sideScroll > QWidget > QWidget{background:transparent;}"));
    scroll->setWidget(panel);
    return scroll;
}

QWidget* MainWindow::makeMetric(const QString& name, QLabel** valueOut)
{
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(1);

    auto* n = new QLabel(name);
    n->setObjectName(QStringLiteral("metricName"));
    auto* val = new QLabel(QStringLiteral("—"));
    val->setObjectName(QStringLiteral("metricValue"));

    v->addWidget(n);
    v->addWidget(val);
    *valueOut = val;
    return box;
}

QWidget* MainWindow::buildPlotArea()
{
    m_rawPlot = new PlotWidget;
    m_filtPlot = new PlotWidget;
    m_envPlot = new PlotWidget;
    m_rrPlot = new PlotWidget;

    m_rawPlot->setFocusPolicy(Qt::StrongFocus);
    m_rawPlot->setMinimumHeight(280);
    m_filtPlot->setMinimumHeight(150);
    m_envPlot->setMinimumHeight(150);
    m_rrPlot->setMinimumHeight(130);

    auto* midRow = new QWidget;
    auto* midLayout = new QHBoxLayout(midRow);
    midLayout->setContentsMargins(0, 0, 0, 0);
    midLayout->setSpacing(8);
    midLayout->addWidget(m_filtPlot, 1);
    midLayout->addWidget(m_envPlot, 1);

    // 竖向可拖动：原始波形默认占大头，中间的滤波/包络图可以压小
    auto* area = new QSplitter(Qt::Vertical);
    area->setObjectName(QStringLiteral("plotSplit"));
    area->setHandleWidth(8);
    area->setChildrenCollapsible(false);
    area->addWidget(m_rawPlot);
    area->addWidget(midRow);
    area->addWidget(m_rrPlot);
    area->setStretchFactor(0, 1);
    area->setStretchFactor(1, 0);
    area->setStretchFactor(2, 0);
    area->setSizes({600, 240, 170});
    return area;
}

void MainWindow::setupPlots()
{
    m_rawPlot->setTitle(QStringLiteral("① 原始心电信号与 R 峰标记"));
    m_rawPlot->setAxisLabels(QStringLiteral("时间 (s)"), QStringLiteral("幅值 (mV)"));
    m_rawPlot->setEmptyText(QStringLiteral("尚未载入心电数据"));
    m_rawPlot->addSeries(QStringLiteral("原始心电"), QColor(0x25, 0x63, 0xEB), 1.2);
    m_rawPlot->addSeries(QString(), QColor(0xE1, 0x1D, 0x48), 2.8, PlotWidget::Style::Dots);
    // 顺序有意义：瓣峰先画、P 波后画，双相时绿点才不会被橙点盖住
    m_rawPlot->addSeries(QStringLiteral("P 波瓣峰"), QColor(0xF5, 0x9E, 0x0B), 2.2, PlotWidget::Style::Dots);
    m_rawPlot->addSeries(QStringLiteral("P 波"), QColor(0x0E, 0x9F, 0x6E), 3.4, PlotWidget::Style::Dots);

    m_filtPlot->setTitle(QStringLiteral("② 带通滤波 5–15 Hz（零相位）"));
    m_filtPlot->setAxisLabels(QStringLiteral("时间 (s)"), QStringLiteral("幅值"));
    m_filtPlot->setEmptyText(QStringLiteral("等待检测"));
    m_filtPlot->addSeries(QStringLiteral("滤波信号"), QColor(0x7C, 0x3A, 0xED), 1.2);
    m_filtPlot->addSeries(QString(), QColor(0xF7, 0x8C, 0x1E), 2.4, PlotWidget::Style::Dots);

    m_envPlot->setTitle(QStringLiteral("③ 微分平方 / 移动窗积分 / 自适应阈值"));
    m_envPlot->setAxisLabels(QStringLiteral("时间 (s)"), QStringLiteral("能量"));
    m_envPlot->setEmptyText(QStringLiteral("等待检测"));
    m_envPlot->addSeries(QStringLiteral("微分平方"), QColor(0xF5, 0x9E, 0x0B), 1.0);
    m_envPlot->addSeries(QStringLiteral("积分包络"), QColor(0xDC, 0x26, 0x26), 1.3);
    m_envPlot->addSeries(QStringLiteral("自适应阈值"), QColor(0x64, 0x74, 0x8B), 1.1,
                         PlotWidget::Style::Dashed);
    m_envPlot->addSeries(QStringLiteral("检出峰"), QColor(0x99, 0x1B, 0x1B), 2.6,
                         PlotWidget::Style::Dots);

    m_rrPlot->setTitle(QStringLiteral("④ RR 间期趋势"));
    m_rrPlot->setAxisLabels(QStringLiteral("时间 (s)"), QStringLiteral("RR (ms)"));
    m_rrPlot->setEmptyText(QStringLiteral("等待检测"));
    m_rrPlot->addSeries(QStringLiteral("RR 间期"), QColor(0x0D, 0x94, 0x88), 1.4);
    m_rrPlot->addSeries(QStringLiteral("均值"), QColor(0x94, 0xA3, 0xB8), 1.0,
                        PlotWidget::Style::Dashed);

    for (PlotWidget* p : {m_rawPlot, m_filtPlot, m_envPlot, m_rrPlot})
        connect(p, &PlotWidget::xRangeChanged, this, &MainWindow::syncXRange);
    connect(m_rawPlot, &PlotWidget::cursorMoved, this, &MainWindow::updateCursorLabel);
}

QString MainWindow::columnLabel(int index) const
{
    if (index < 0 || index >= m_data.cols()) return QString();
    const QString name = (index < m_data.headers.size())
                             ? m_data.headers.at(index)
                             : QStringLiteral("未命名");
    return QStringLiteral("%1 · %2").arg(index + 1).arg(name);
}

void MainWindow::syncXRange(double x0, double x1)
{
    auto* src = qobject_cast<PlotWidget*>(sender());
    for (PlotWidget* p : {m_rawPlot, m_filtPlot, m_envPlot, m_rrPlot}) {
        if (p == src) continue;
        const QSignalBlocker blocker(p);
        p->setXRangeSilent(x0, x1);
    }
}

void MainWindow::openFile()
{
    QSettings settings(QStringLiteral("EcgRpeak"), QStringLiteral("RPeakDetector"));
    const QString dir = defaultOpenDir(settings.value(QStringLiteral("lastDir")).toString());

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择心电数据文件"), dir,
        QStringLiteral("心电数据 (*.csv *.txt *.tsv *.dat *.bin *.raw *.i16 *.f32);;"
                       "文本数据 (*.csv *.txt *.tsv);;"
                       "二进制原始数据 (*.bin *.raw *.dat *.i16 *.i32 *.f32 *.f64 *.ecgb *.adc);;"
                       "所有文件 (*)"));
    if (path.isEmpty()) return;

    loadFromPath(path);
}

void MainWindow::openBinaryFile()
{
    QSettings settings(QStringLiteral("EcgRpeak"), QStringLiteral("RPeakDetector"));
    const QString dir = defaultOpenDir(settings.value(QStringLiteral("lastDir")).toString());

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择二进制心电数据"), dir,
        QStringLiteral("二进制原始数据 (*.bin *.raw *.dat *.i16 *.i32 *.f32 *.f64 *.ecgb *.adc);;"
                       "所有文件 (*)"));
    if (path.isEmpty()) return;

    loadFromPath(path);
}

bool MainWindow::loadFromPath(const QString& path)
{
    if (path.isEmpty()) return false;
    if (ecg::isAnnotatedFile(path)) return loadAnnotatedFromPath(path);
    if (ecg::isBinaryPath(path)) return loadBinaryFromPath(path, true);

    ecg::Data data;
    QString error;
    if (!ecg::loadFile(path, data, error)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), error);
        setStatus(QStringLiteral("读取失败：%1").arg(error));
        return false;
    }

    QSettings settings(QStringLiteral("EcgRpeak"), QStringLiteral("RPeakDetector"));
    m_lastDir = QFileInfo(path).absolutePath();
    settings.setValue(QStringLiteral("lastDir"), m_lastDir);

    const QFileInfo fi(path);
    QString info = QStringLiteral("%1 · %2 采样点 · %3 列")
                       .arg(fi.fileName())
                       .arg(data.rows())
                       .arg(data.cols());
    if (data.fsGuess > 0.0)
        info += QStringLiteral(" · 识别到 %1 Hz").arg(QString::number(data.fsGuess, 'g', 4));

    setData(data, info);
    if (data.fsGuess > 0.0) {
        setStatus(QStringLiteral("已载入 %1").arg(QDir::toNativeSeparators(path)));
    } else {
        setStatus(QStringLiteral("已载入 %1（未识别采样率，请手动确认当前 %2 Hz）")
                      .arg(QDir::toNativeSeparators(path))
                      .arg(QString::number(m_fsSpin->value(), 'g', 5)));
    }
    return true;
}

bool MainWindow::loadBinaryFromPath(const QString& path, bool interactive,
                                    const ecg::BinaryOptions* preset)
{
    QSettings settings(QStringLiteral("EcgRpeak"), QStringLiteral("RPeakDetector"));

    ecg::BinaryProbe probe;
    QString probeError;
    const bool probed = ecg::probeBinary(path, probe, probeError);

    ecg::BinaryOptions opt;
    if (preset) {
        opt = *preset;
    } else if (probed && probe.confident) {
        opt = probe.options;                      // 说明文件 / 文本头最可靠
    } else {
        ecg::BinaryOptions saved;
        if (binOptionsFromString(settings.value(QStringLiteral("binOptions")).toString(), saved)) {
            saved.headerBytes = probed ? probe.options.headerBytes : saved.headerBytes;
            opt = saved;                          // 沿用上次成功解析的参数
        } else if (probed) {
            opt = probe.options;
        }
    }

    if (interactive) {
        BinaryDialog dlg(path, opt, this);
        if (dlg.exec() != QDialog::Accepted) {
            setStatus(QStringLiteral("已取消载入二进制数据"));
            return false;
        }
        opt = dlg.options();
        settings.setValue(QStringLiteral("binOptions"), binOptionsToString(opt));
    }

    bool fsFromUi = false;
    if (opt.sampleRate <= 0.0) {
        opt.sampleRate = m_fsSpin ? m_fsSpin->value() : 360.0;
        fsFromUi = true;
    }

    ecg::Data data;
    QString error;
    if (!ecg::loadBinary(path, opt, data, error)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), error);
        setStatus(QStringLiteral("二进制读取失败：%1").arg(error));
        return false;
    }

    const QFileInfo fi(path);
    m_lastDir = fi.absolutePath();
    settings.setValue(QStringLiteral("lastDir"), m_lastDir);

    const QString info = QStringLiteral("%1 · %2 采样点 · %3 通道 · %4")
                             .arg(fi.fileName())
                             .arg(data.rows())
                             .arg(data.cols())
                             .arg(opt.describe());
    setData(data, info + QLatin1Char('\n') + QDir::toNativeSeparators(path));

    if (fsFromUi) {
        setStatus(QStringLiteral("已载入二进制数据 %1（未指定采样率，暂按 %2 Hz 计算）")
                      .arg(fi.fileName())
                      .arg(QString::number(m_fsSpin->value(), 'g', 5)));
    } else {
        setStatus(QStringLiteral("已载入二进制数据 %1 · %2")
                      .arg(fi.fileName(), opt.describe()));
    }
    return true;
}

bool MainWindow::loadAnnotatedFromPath(const QString& path)
{
    ecg::Data data;
    ecg::Annotations ann;
    QString error;
    if (!ecg::loadAnnotatedFile(path, data, ann, error)) {
        QMessageBox::warning(this, QStringLiteral("读取标注文件失败"), error);
        setStatus(QStringLiteral("读取标注文件失败：%1").arg(error));
        return false;
    }

    const QFileInfo fi(path);
    const QString info = QStringLiteral("%1 · %2 采样点 · %3 通道 · 已标注 %4 个 R 峰 · %5 Hz")
                             .arg(fi.fileName())
                             .arg(data.rows())
                             .arg(data.cols())
                             .arg(ann.peaks.size())
                             .arg(QString::number(data.fsGuess, 'g', 6));

    setData(data, info + QLatin1Char('\n') + QDir::toNativeSeparators(path), false);

    QSettings settings(QStringLiteral("EcgRpeak"), QStringLiteral("RPeakDetector"));
    m_lastDir = fi.absolutePath();
    settings.setValue(QStringLiteral("lastDir"), m_lastDir);

    m_annotatedMode = true;
    m_ann = ann;
    m_result = pt::Result();
    m_result.rPeaks = ann.peaks;
    m_result.fromSearchBack.assign(ann.peaks.size(), false);
    m_result.rAmp.assign(ann.peaks.size(), 0.0);
    m_peakType = ann.types;
    m_peakType.resize(ann.peaks.size(), 1);
    m_editCount = ann.log.size();
    m_usedFs = data.fsGuess > 0.0 ? data.fsGuess : m_fsSpin->value();
    m_usedColumn = m_colCombo->currentIndex();
    m_result.ok = !ann.peaks.empty();
    m_hasResult = m_result.ok;
    pt::recomputeStats(m_result, std::max(1.0, m_usedFs));

    updateEditLogUi();
    refreshAnnotatedView();
    refreshPWave();

    setStatus(QStringLiteral("已导入标注文件 %1 · %2 个 R 峰 · 未运行检测算法")
                  .arg(fi.fileName())
                  .arg(ann.peaks.size()));
    return true;
}

bool MainWindow::saveAnnotatedTo(const QString& path, QString& error)
{
    if (!m_data.loaded) {
        error = QStringLiteral("当前没有可导出的数据。");
        return false;
    }
    ecg::Annotations ann;
    ann.peaks = m_result.rPeaks;
    ann.types.assign(m_result.rPeaks.size(), 1);
    for (size_t i = 0; i < ann.types.size() && i < m_peakType.size(); ++i) ann.types[i] = m_peakType[i];
    ann.log = m_ann.log;
    ann.source = m_data.filePath.isEmpty() ? QStringLiteral("(内置示例)")
                                           : QFileInfo(m_data.filePath).fileName();
    // 文本数据源可能包含时间列，时间轴由 fs 即可重建，导出时去掉，避免回读后把它当信号
    ecg::Data out = m_data;
    int usedCol = m_usedColumn;
    if (out.timeColumn >= 0 && out.cols() > 1) {
        const int tc = out.timeColumn;
        out.columns.erase(out.columns.begin() + tc);
        if (tc < out.headers.size()) out.headers.removeAt(tc);
        if (usedCol > tc) --usedCol;
        out.timeColumn = -1;
    }
    out.signalColumn = std::clamp(usedCol, 0, std::max(0, out.cols() - 1));

    if (!ecg::writeAnnotatedFile(path, out, m_usedFs, ann, error)) return false;

    setStatus(QStringLiteral("已导出标注结果：%1 个 R 峰、%2 条修改记录 → %3")
                  .arg(ann.peaks.size())
                  .arg(ann.log.size())
                  .arg(QDir::toNativeSeparators(path)));
    return true;
}

void MainWindow::exportAnnotated()
{
    if (!m_data.loaded || !m_hasResult) {
        QMessageBox::information(this, QStringLiteral("导出标注"),
                                 QStringLiteral("当前还没有 R 峰结果。"));
        return;
    }
    QString base = QStringLiteral("annotated.bin");
    if (!m_data.filePath.isEmpty()) {
        const QFileInfo fi(m_data.filePath);
        base = fi.completeBaseName() + QStringLiteral("_annotated.bin");
    }
    const QString startDir = m_lastDir.isEmpty()
                                 ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                                 : m_lastDir;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出标注结果"), QDir(startDir).filePath(base),
        QStringLiteral("标注 bin (*.bin)"));
    if (path.isEmpty()) return;

    QString error;
    if (!saveAnnotatedTo(path, error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
        return;
    }
    QMessageBox::information(
        this, QStringLiteral("导出完成"),
        QStringLiteral("已写出 %1\n\n下次用「打开数据文件」直接打开它即可，\n程序会读取文件内的标注、不再运行检测算法。")
            .arg(QDir::toNativeSeparators(path)));
}

void MainWindow::loadDemoData()
{
    setData(ecg::demoData(360.0, 20.0), QStringLiteral("内置示例信号 · 20 s @ 360 Hz · 含基线漂移与工频"));
}

void MainWindow::loadDemo()
{
    loadDemoData();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) continue;
        if (loadFromPath(url.toLocalFile())) {
            e->acceptProposedAction();
            return;
        }
    }
    e->ignore();
}

void MainWindow::setData(const ecg::Data& data, const QString& displayName, bool autoDetect)
{
    m_data = data;
    m_hasResult = false;
    m_result = pt::Result();
    m_annotatedMode = false;
    m_peakType.clear();
    m_editCount = 0;
    m_ann = ecg::Annotations();
    updateEditLogUi();
    if (m_rawPlot) m_rawPlot->setCursorVisible(false);

    {
        const QSignalBlocker blocker(m_colCombo);
        m_colCombo->clear();
        for (int i = 0; i < m_data.cols(); ++i) m_colCombo->addItem(columnLabel(i));
        if (!m_colCombo->count())
            m_colCombo->addItem(QStringLiteral("—"));
        const int sel = std::clamp(m_data.signalColumn, 0, std::max(0, m_colCombo->count() - 1));
        m_colCombo->setCurrentIndex(sel);
    }

    if (m_data.fsGuess > 0.0) {
        const QSignalBlocker blocker(m_fsSpin);
        m_fsSpin->setValue(m_data.fsGuess);
    }

    const QString name = m_data.filePath.isEmpty()
                             ? QStringLiteral("内置示例信号")
                             : QFileInfo(m_data.filePath).fileName();
    const QString shortText = QStringLiteral("%1 · %2 点 · %3 列")
                                  .arg(name)
                                  .arg(m_data.rows())
                                  .arg(m_data.cols());
    const QFontMetrics fm(m_fileLabel->font());
    m_fileLabel->setText(fm.elidedText(shortText, Qt::ElideMiddle, 288));
    m_fileLabel->setToolTip(displayName + (m_data.filePath.isEmpty()
                                               ? QString()
                                               : QStringLiteral("\n") + m_data.filePath));
    m_detectBtn->setEnabled(m_data.loaded);
    m_exportBtn->setEnabled(false);

    m_usedColumn = m_colCombo->currentIndex();
    m_usedFs = m_fsSpin->value();
    refreshRawPlot();
    if (autoDetect) runDetection();
    if (m_rawPlot) m_rawPlot->setFocus();
}

void MainWindow::onColumnChanged()
{
    if (!m_data.loaded) return;
    m_usedColumn = m_colCombo->currentIndex();
    m_usedFs = m_fsSpin->value();
    refreshRawPlot();
    if (m_annotatedMode) {
        // 标注按样点索引保存，换通道后仍沿用
        m_result.ok = !m_result.rPeaks.empty();
        m_hasResult = m_result.ok;
        refreshAnnotatedView();
        return;
    }
    runDetection();
}

void MainWindow::refreshRawPlot()
{
    if (!m_data.loaded || m_usedColumn < 0 || m_usedColumn >= m_data.cols()) return;

    const std::vector<double>& col = m_data.columns[static_cast<size_t>(m_usedColumn)];
    const double fs = std::max(1.0, m_fsSpin->value());
    const int n = static_cast<int>(col.size());

    QVector<double> tx(n);
    for (int i = 0; i < n; ++i) tx[i] = static_cast<double>(i) / fs;

    m_rawPlot->setSeries(0, tx, toVector(col));
    m_rawPlot->setMarkers({}, QColor(0xE1, 0x1D, 0x48));
    m_rawPlot->setSearchbackMarkers({}, QColor(0xF7, 0x8C, 0x1E));
    {
        const QVector<double> empty;
        m_rawPlot->setSeries(1, empty, empty);
        m_rawPlot->setSeries(2, empty, empty);
        m_rawPlot->setSeries(3, empty, empty);
    }
    m_pw = pw::Result();
    updatePWaveUi();

    m_filtPlot->clearSeriesData();
    m_envPlot->clearSeriesData();
    m_rrPlot->clearSeriesData();

    clearResults();
    showInitialView();
}

void MainWindow::showInitialView()
{
    if (!m_data.loaded) return;
    const double fs = std::max(1.0, m_fsSpin->value());
    const int n = m_data.rows();
    if (n < 2) return;

    const double total = static_cast<double>(n - 1) / fs;
    const double viewEnd = (total <= 15.0) ? total : 12.0;
    m_rawPlot->setXRange(0.0, std::max(0.05, viewEnd), true);
}

void MainWindow::runDetection()
{
    if (!m_data.loaded || m_usedColumn < 0 || m_usedColumn >= m_data.cols()) {
        setStatus(QStringLiteral("请先打开心电数据文件"));
        return;
    }

    if (m_annotatedMode && !m_ann.log.isEmpty()) {
        const auto ret = QMessageBox::question(
            this, QStringLiteral("重新运行检测"),
            QStringLiteral("当前显示的是导入/人工标注的结果，重新检测会用算法结果覆盖它。\n是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            setStatus(QStringLiteral("已取消重新检测"));
            return;
        }
    }

    const double fs = m_fsSpin->value();
    const double lo = m_lowCutSpin->value();
    const double hi = m_highCutSpin->value();
    if (hi <= lo) {
        QMessageBox::warning(this, QStringLiteral("参数不合法"),
                             QStringLiteral("带通高截止频率必须大于低截止频率。"));
        return;
    }
    if (hi > fs / 2.0) {
        setStatus(QStringLiteral("提示：高截止频率超过奈奎斯特频率，已自动限制到 fs/2 以内"));
    }

    pt::Params p;
    p.fs = fs;
    p.lowCut = lo;
    p.highCut = hi;
    p.integrationWindow = m_winSpin->value() / 1000.0;
    p.refractory = m_refrSpin->value() / 1000.0;
    p.tWaveWindow = 0.36;
    p.thresholdWeight = m_weightSpin->value();

    QElapsedTimer timer;
    timer.start();
    m_result = pt::detect(m_data.columns[static_cast<size_t>(m_usedColumn)], p);
    m_elapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;

    m_usedFs = fs;
    m_usedColumn = m_colCombo->currentIndex();
    m_usedParams = p;
    m_hasResult = m_result.ok;
    m_annotatedMode = false;
    resetEditState();

    refreshPlots();
    refreshPWave();
    updateStats();

    if (!m_hasResult) {
        setStatus(QStringLiteral("未能从当前通道检出 R 峰，请调整通道或算法参数"));
        return;
    }

    const int beats = static_cast<int>(m_result.rPeaks.size());
    if (beats == 0) {
        setStatus(QStringLiteral("未检出 R 峰，请检查采样率、通道选择或调低阈值系数"));
        return;
    }

    setStatus(QStringLiteral("检测完成 · %1 个 R 峰 · 平均心率 %2 bpm · 耗时 %3 ms")
                  .arg(beats)
                  .arg(QString::number(m_result.meanHr, 'f', 1))
                  .arg(QString::number(m_elapsedMs, 'f', 1)));
}

void MainWindow::refreshPlots()
{
    if (!m_hasResult) return;
    if (m_usedColumn < 0 || m_usedColumn >= m_data.cols()) return;

    const std::vector<double>& raw = m_data.columns[static_cast<size_t>(m_usedColumn)];
    const int n = static_cast<int>(raw.size());
    const double fs = std::max(1.0, m_usedFs);

    QVector<double> tx(n);
    for (int i = 0; i < n; ++i) tx[i] = static_cast<double>(i) / fs;

    m_rawPlot->setSeries(0, tx, toVector(raw));

    // 带通滤波（导入标注文件时没有中间结果）
    if (!m_result.filtered.empty()) {
        m_filtPlot->setEmptyText(QStringLiteral("等待检测"));
        m_filtPlot->setSeries(0, tx, toVector(m_result.filtered));
    } else {
        m_filtPlot->clearSeriesData();
        m_filtPlot->setEmptyText(QStringLiteral("已导入人工标注 · 未运行检测算法"));
    }

    // 微分平方 / 积分 / 阈值 / 检出点
    if (!m_result.squared.empty() && !m_result.integrated.empty() && !m_result.thresholdTrack.empty()) {
        m_envPlot->setSeries(0, tx, toVector(m_result.squared));
        m_envPlot->setSeries(1, tx, toVector(m_result.integrated));
        m_envPlot->setSeries(2, tx, toVector(m_result.thresholdTrack));

        QVector<double> dotX, dotY;
        dotX.reserve(static_cast<int>(m_result.rPeaks.size()));
        dotY.reserve(static_cast<int>(m_result.rPeaks.size()));
        for (int idx : m_result.rPeaks) {
            if (idx < 0 || idx >= n) continue;
            dotX.append(static_cast<double>(idx) / fs);
            dotY.append(m_result.integrated[static_cast<size_t>(idx)]);
        }
        m_envPlot->setSeries(3, dotX, dotY);
    } else {
        m_envPlot->clearSeriesData();
        m_envPlot->setEmptyText(QStringLiteral("已导入人工标注 · 未运行检测算法"));
    }

    refreshPeakMarkers();
    refreshRrPlot();
}

void MainWindow::updateStats()
{
    if (!m_hasResult || m_result.rPeaks.size() < 2) {
        m_hrValue->setText(QStringLiteral("—"));
        m_beatValue->setText(QString::number(m_hasResult ? static_cast<int>(m_result.rPeaks.size()) : 0));
        m_meanRrValue->setText(QStringLiteral("—"));
        m_sdnnValue->setText(QStringLiteral("—"));
        m_rmssdValue->setText(QStringLiteral("—"));
        m_pnnValue->setText(QStringLiteral("—"));
        m_exportBtn->setEnabled(m_hasResult && !m_result.rPeaks.empty());
        return;
    }

    m_hrValue->setText(QString::number(m_result.meanHr, 'f', 1));
    m_beatValue->setText(QString::number(m_result.rPeaks.size()));
    m_meanRrValue->setText(QString::number(m_result.meanRr * 1000.0, 'f', 0));
    m_sdnnValue->setText(QString::number(m_result.sdnn * 1000.0, 'f', 1));
    m_rmssdValue->setText(QString::number(m_result.rmssd * 1000.0, 'f', 1));
    m_pnnValue->setText(QString::number(m_result.pnn50, 'f', 1));
    m_exportBtn->setEnabled(true);
}

void MainWindow::clearResults()
{
    m_hasResult = false;
    m_hrValue->setText(QStringLiteral("—"));
    m_beatValue->setText(QStringLiteral("—"));
    m_meanRrValue->setText(QStringLiteral("—"));
    m_sdnnValue->setText(QStringLiteral("—"));
    m_rmssdValue->setText(QStringLiteral("—"));
    m_pnnValue->setText(QStringLiteral("—"));
    m_exportBtn->setEnabled(false);
}

void MainWindow::exportResults()
{
    if (!m_hasResult || m_result.rPeaks.empty()) {
        QMessageBox::information(this, QStringLiteral("导出"),
                                 QStringLiteral("当前没有可导出的检测结果。"));
        return;
    }

    QString base = QStringLiteral("rpeaks.csv");
    if (!m_data.filePath.isEmpty()) {
        const QFileInfo fi(m_data.filePath);
        base = fi.completeBaseName() + QStringLiteral("_rpeaks.csv");
    }
    const QString startDir = m_lastDir.isEmpty()
                                 ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                                 : m_lastDir;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 R 峰结果"), QDir(startDir).filePath(base),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) return;

    QString error;
    if (!exportResultsTo(path, error))
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
}

bool MainWindow::exportResultsTo(const QString& path, QString& error)
{
    if (!m_hasResult || m_result.rPeaks.empty()) {
        error = QStringLiteral("当前没有可导出的检测结果。");
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = QStringLiteral("无法写入 %1：%2").arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts.setGenerateByteOrderMark(true);

    ts << "序号,采样点,时间(s),RR间期(ms),瞬时心率(bpm),回溯补检\n";
    for (size_t i = 0; i < m_result.rPeaks.size(); ++i) {
        const double t = static_cast<double>(m_result.rPeaks[i]) / m_usedFs;
        const bool sb = (i < m_result.fromSearchBack.size()) && m_result.fromSearchBack[i];
        ts << static_cast<int>(i + 1) << ','
           << m_result.rPeaks[i] << ','
           << QString::number(t, 'f', 4) << ',';
        if (i == 0) ts << ',';
        else ts << QString::number(m_result.rr[i - 1] * 1000.0, 'f', 1) << ',';
        if (i == 0) ts << ',';
        else ts << QString::number(m_result.instHr[i - 1], 'f', 1) << ',';
        ts << (sb ? "是" : "") << '\n';
    }

    ts << "\n# 汇总\n";
    ts << "# 数据文件," << (m_data.filePath.isEmpty() ? QStringLiteral("(内置示例)") : m_data.filePath) << '\n';
    ts << "# 采样率(Hz)," << QString::number(m_usedFs, 'f', 2) << '\n';
    ts << "# 平均心率(bpm)," << QString::number(m_result.meanHr, 'f', 2) << '\n';
    ts << "# 平均RR(ms)," << QString::number(m_result.meanRr * 1000.0, 'f', 1) << '\n';
    ts << "# SDNN(ms)," << QString::number(m_result.sdnn * 1000.0, 'f', 1) << '\n';
    ts << "# RMSSD(ms)," << QString::number(m_result.rmssd * 1000.0, 'f', 1) << '\n';
    ts << "# pNN50(%)," << QString::number(m_result.pnn50, 'f', 2) << '\n';
    if (m_result.minHr > 0.0) {
        ts << "# 最低心率(bpm)," << QString::number(m_result.minHr, 'f', 1) << '\n';
        ts << "# 最高心率(bpm)," << QString::number(m_result.maxHr, 'f', 1) << '\n';
    }
    f.close();

    setStatus(QStringLiteral("已导出 %1 个 R 峰到 %2")
                  .arg(m_result.rPeaks.size()).arg(QDir::toNativeSeparators(path)));
    return true;
}

void MainWindow::resetView()
{
    showInitialView();
    setStatus(QStringLiteral("视图已复位"));
}

void MainWindow::resetParams()
{
    const QSignalBlocker b1(m_lowCutSpin);
    const QSignalBlocker b2(m_highCutSpin);
    const QSignalBlocker b3(m_winSpin);
    const QSignalBlocker b4(m_refrSpin);
    const QSignalBlocker b5(m_weightSpin);
    m_lowCutSpin->setValue(5.0);
    m_highCutSpin->setValue(15.0);
    m_winSpin->setValue(150.0);
    m_refrSpin->setValue(200.0);
    m_weightSpin->setValue(0.25);
    if (m_data.loaded) runDetection();
    setStatus(QStringLiteral("算法参数已恢复默认值"));
}

void MainWindow::setStatus(const QString& text)
{
    statusBar()->showMessage(text);
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, QStringLiteral("关于 Pan-Tompkins"),
        QStringLiteral(
            "<b>ECG R 峰检测 · Pan-Tompkins</b><br><br>"
            "处理流程：<br>"
            "① 5–15 Hz 零相位带通滤波（抑制基线漂移与工频）<br>"
            "② 五点差分求导（突出 QRS 斜率）<br>"
            "③ 逐点平方（全部转为正向能量）<br>"
            "④ 150 ms 移动窗积分（得到平滑包络）<br>"
            "⑤ 自适应双阈值 + 200 ms 不应期 + T 波判别<br>"
            "⑥ RR 间期异常时回溯搜索补检漏搏<br>"
            "⑦ 在滤波信号上精定位 R 峰，输出 HR / SDNN / RMSSD / pNN50<br><br>"
            "数据源：文本 CSV / TXT / TSV；二进制 .bin / .raw（含 12 位双通道打包），<br>"
            "支持说明文件 / 文本头自动识别，也可直接导入本程序导出的标注 bin。<br><br>"
            "<b>人工标注</b><br>"
            "← / → 左右移动波形，A 在光标处新增 R 峰，D 删除光标附近的 R 峰；<br>"
            "每次改动都会记录，并可导出为标注 bin 供下次直接载入（不再跑检测）。"));
}
