#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <vector>

#include "ecgsignal.h"
#include "pantompkins.h"
#include "pwave.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QWidget;
class QDragEnterEvent;
class QDropEvent;
class PlotWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool loadFromPath(const QString& path); // 载入指定数据文件（.bin 会弹出解析设置）
    bool loadBinaryFromPath(const QString& path, bool interactive,
                            const ecg::BinaryOptions* preset = nullptr);
    bool loadAnnotatedFromPath(const QString& path); // 导入已标注 bin，不跑检测
    void loadDemoData();                             // 载入内置示例信号

    bool exportResultsTo(const QString& path, QString& error);  // 无对话框导出 CSV
    bool exportPWaveTo(const QString& path, QString& error);    // 导出逐搏 P 波结果
    bool saveAnnotatedTo(const QString& path, QString& error);  // 无对话框导出标注 bin

    // 供命令行 / 自动化调用
    void setEditCursorTime(double t);
    void fitViewNow();
    void addPeakNow();
    void deletePeakNow();
    void panLeft();
    void panRight();

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private slots:
    void openFile();
    void openBinaryFile();
    void loadDemo();
    void runDetection();
    void exportResults();
    void exportPWave();
    void exportAnnotated();
    void resetView();
    void resetParams();
    void onColumnChanged();
    void syncXRange(double x0, double x1);
    void showAbout();

private:
    QWidget* buildSidePanel();
    QWidget* buildPlotArea();
    QWidget* makeMetric(const QString& name, QLabel** valueOut);
    void     setupPlots();
    void     installActions();

    void setData(const ecg::Data& data, const QString& displayName, bool autoDetect = true);
    void refreshRawPlot();
    void showInitialView();
    void refreshPlots();
    void refreshAnnotatedView();
    void refreshPeakMarkers();
    void refreshRrPlot();
    void refreshPWave();
    void refreshPWaveMarkers();
    void updatePWaveUi();
    void updateStats();
    void clearResults();
    void setStatus(const QString& text);
    QString columnLabel(int index) const;

    void resetEditState();
    void afterPeaksChanged();
    void appendEditLog(const QString& action, int sample, const QString& note);
    void updateEditLogUi();
    void updateCursorLabel(double t);
    int  snapToExtremum(int sample) const;
    int  nearestPeakPosition(int sample, double maxDistanceSec) const;

    ecg::Data   m_data;
    pt::Result  m_result;
    pt::Params  m_usedParams;
    double      m_usedFs = 360.0;
    int         m_usedColumn = 0;
    bool        m_hasResult = false;
    double      m_elapsedMs = 0.0;
    QString     m_lastDir;

    pw::Result                 m_pw;                    // P 波检测结果
    bool                       m_showP = true;

    bool                       m_annotatedMode = false; // 来自标注文件，未跑检测
    ecg::Annotations           m_ann;                   // 编辑记录与来源
    std::vector<unsigned char> m_peakType;              // 1=算法 2=人工
    int                        m_editCount = 0;

    QLabel*         m_fileLabel = nullptr;
    QComboBox*      m_colCombo = nullptr;
    QDoubleSpinBox* m_fsSpin = nullptr;
    QDoubleSpinBox* m_lowCutSpin = nullptr;
    QDoubleSpinBox* m_highCutSpin = nullptr;
    QDoubleSpinBox* m_winSpin = nullptr;
    QDoubleSpinBox* m_refrSpin = nullptr;
    QDoubleSpinBox* m_weightSpin = nullptr;
    QPushButton*    m_detectBtn = nullptr;
    QPushButton*    m_exportBtn = nullptr;

    QLabel*      m_pwLabel = nullptr;
    QLabel*      m_flipLabel = nullptr;
    QCheckBox*   m_showPBox = nullptr;

    QLabel*      m_cursorLabel = nullptr;
    QLabel*      m_logLabel = nullptr;
    QListWidget* m_logList = nullptr;

    QLabel* m_hrValue = nullptr;
    QLabel* m_beatValue = nullptr;
    QLabel* m_meanRrValue = nullptr;
    QLabel* m_sdnnValue = nullptr;
    QLabel* m_rmssdValue = nullptr;
    QLabel* m_pnnValue = nullptr;

    PlotWidget* m_rawPlot = nullptr;
    PlotWidget* m_filtPlot = nullptr;
    PlotWidget* m_envPlot = nullptr;
    PlotWidget* m_rrPlot = nullptr;
};
