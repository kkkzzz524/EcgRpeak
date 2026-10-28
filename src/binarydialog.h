// 二进制原始数据导入设置：位宽/字节序/通道/偏移/采样率 + 实时波形预览
#pragma once

#include <QDialog>

#include "ecgsignal.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class PlotWidget;

class BinaryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BinaryDialog(const QString& path, const ecg::BinaryOptions& initial,
                          QWidget* parent = nullptr);

    ecg::BinaryOptions options() const;

private slots:
    void autoDetect();
    void schedulePreview();
    void refreshPreview();
    void saveSidecar();
    void accept() override;

private:
    QWidget* buildForm();
    QWidget* buildPreview();

    void applyOptions(const ecg::BinaryOptions& opt);
    void rebuildChannelCombo(int channels);
    void updateLayoutUi();
    void updateInfo();

    QString m_path;
    ecg::BinaryProbe m_probe;

    PlotWidget*     m_preview = nullptr;
    QComboBox*      m_layoutCombo = nullptr;
    QComboBox*      m_formatCombo = nullptr;
    QComboBox*      m_endianCombo = nullptr;
    QSpinBox*       m_chSpin = nullptr;
    QSpinBox*       m_offSpin = nullptr;
    QDoubleSpinBox* m_fsSpin = nullptr;
    QDoubleSpinBox* m_scaleSpin = nullptr;
    QDoubleSpinBox* m_biasSpin = nullptr;
    QComboBox*      m_prevChCombo = nullptr;
    QLabel*         m_infoLabel = nullptr;
    QLabel*         m_noteLabel = nullptr;
    QPushButton*    m_saveBtn = nullptr;
    QTimer*         m_debounce = nullptr;
};
