#include "mainwindow.h"
#include "binarydialog.h"

#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QIcon>
#include <QMetaObject>
#include <QStyleFactory>
#include <QTimer>

namespace {

QString appStyleSheet()
{
    return QStringLiteral(R"(
QMainWindow { background: #EEF2F7; }
QWidget#rootWidget { background: #EEF2F7; }

QMenuBar { background: #FFFFFF; border-bottom: 1px solid #E2E8F0; padding: 2px 8px; }
QMenuBar::item { padding: 5px 12px; border-radius: 5px; color: #334155; background: transparent; }
QMenuBar::item:selected { background: #EFF6FF; color: #1D4ED8; }
QMenu { background: #FFFFFF; border: 1px solid #E2E8F0; padding: 5px; }
QMenu::item { padding: 6px 26px 6px 18px; border-radius: 4px; color: #334155; }
QMenu::item:selected { background: #EFF6FF; color: #1D4ED8; }
QMenu::separator { height: 1px; background: #EDF1F7; margin: 4px 8px; }

QStatusBar { background: #FFFFFF; border-top: 1px solid #E2E8F0; color: #64748B; }
QStatusBar::item { border: none; }

QFrame#cardPanel { background: #FFFFFF; border: 1px solid #E2E8F0; border-radius: 10px; }

QLabel { color: #475569; }
QLabel#sectionTitle { color: #94A3B8; font-size: 8pt; font-weight: 700; }
QLabel#formLabel { color: #64748B; font-size: 9pt; }
QLabel#fileLabel { color: #475569; font-size: 9pt; }
QLabel#metricName { color: #8A99AD; font-size: 8.5pt; }
QLabel#metricValue { color: #1E293B; font-size: 11.5pt; font-weight: 700; }
QLabel#hrValue { color: #1D4ED8; font-size: 27pt; font-weight: 700; }
QLabel#hrUnit { color: #94A3B8; font-size: 8.5pt; padding-bottom: 6px; }
QLabel#hintLabel { color: #A9B5C6; font-size: 8pt; }

QPushButton { background: #FFFFFF; border: 1px solid #D7E0EA; border-radius: 7px;
              padding: 7px 12px; color: #334155; font-size: 9pt; }
QPushButton:hover { background: #F6FAFF; border-color: #B9CDE3; }
QPushButton:pressed { background: #EAF2FC; }
QPushButton:disabled { color: #AEBACB; background: #F7F9FC; border-color: #E6ECF3; }
QPushButton#primaryBtn { background: #2563EB; border: 1px solid #2563EB; color: #FFFFFF;
                         font-weight: 600; font-size: 10pt; }
QPushButton#primaryBtn:hover { background: #1D4ED8; border-color: #1D4ED8; }
QPushButton#primaryBtn:pressed { background: #1E40AF; border-color: #1E40AF; }
QPushButton#primaryBtn:disabled { background: #B9CDEF; border-color: #B9CDEF; color: #FFFFFF; }

QDoubleSpinBox, QSpinBox, QComboBox { background: #FFFFFF; border: 1px solid #D7E0EA; border-radius: 6px;
                            padding: 5px 7px; min-height: 20px; color: #1E293B; font-size: 9pt;
                            selection-background-color: #DBEAFE; selection-color: #1E293B; }
QDoubleSpinBox:hover, QSpinBox:hover, QComboBox:hover { border-color: #B9CDE3; }
QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color: #2563EB; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #FFFFFF; border: 1px solid #E2E8F0; outline: none;
                              selection-background-color: #EFF6FF; selection-color: #1D4ED8; }

QScrollArea#sideScroll { background: transparent; border: none; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: #CBD5E1; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: #A9B8CA; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

QToolTip { background: #1E293B; color: #F8FAFC; border: none; padding: 5px 7px; }

QDialog { background: #F2F6FB; }
QLabel#dialogTitle { color: #1E293B; font-size: 13pt; font-weight: 700; }
QLabel#dialogInfo { color: #64748B; font-size: 9pt; }
QLabel#dialogNote { color: #B45309; font-size: 8.5pt; }
QFrame#paramCard { background: #FFFFFF; border: 1px solid #E2E8F0; border-radius: 10px; }
)");
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ECG R Peak Detector"));
    QApplication::setOrganizationName(QStringLiteral("EcgRpeak"));
    QApplication::setApplicationVersion(QStringLiteral("1.4.1"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    QFont font(QStringLiteral("Microsoft YaHei UI"), 10);
    app.setFont(font);
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion")))
        app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setStyleSheet(appStyleSheet());

    MainWindow window;
    window.show();

    // 便于自动化检查：--open <文件> / --demo 载入数据，--shot <png> 截图后退出
    // --bin-dialog <文件> 单独打开二进制导入设置对话框（配合 --shot 截图）
    const QStringList args = QCoreApplication::arguments();
    const auto valueOf = [&args](const QString& key) {
        const int i = args.indexOf(key);
        return (i > 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
    };

    const QString shotPath = valueOf(QStringLiteral("--shot"));
    const QString sizeArg = valueOf(QStringLiteral("--size"));
    const QString openPath = valueOf(QStringLiteral("--open"));
    const QString dialogPath = valueOf(QStringLiteral("--bin-dialog"));
    const QString binOpts = valueOf(QStringLiteral("--bin-opts"));
    const QString exportPath = valueOf(QStringLiteral("--export"));

    if (sizeArg.contains(QLatin1Char('x'))) {
        const QStringList wh = sizeArg.split(QLatin1Char('x'));
        if (wh.size() == 2) window.resize(wh[0].toInt(), wh[1].toInt());
    }

    if (args.contains(QStringLiteral("--demo"))) window.loadDemoData();

    if (!openPath.isEmpty()) {
        if (!ecg::isAnnotatedFile(openPath) && ecg::isBinaryPath(openPath)) {
            ecg::BinaryProbe probe;
            QString probeError;
            ecg::probeBinary(openPath, probe, probeError);
            ecg::BinaryOptions opt = probe.ok ? probe.options : ecg::BinaryOptions();
            ecg::BinaryOptions fromArgs;
            if (!binOpts.isEmpty() && ecg::binaryOptionsFromString(binOpts, fromArgs)) opt = fromArgs;
            const QString fsArg = valueOf(QStringLiteral("--fs"));
            if (fsArg.toDouble() > 0.0) opt.sampleRate = fsArg.toDouble();
            window.loadBinaryFromPath(openPath, false, &opt);
        } else {
            window.loadFromPath(openPath); // 文本 / 标注 bin
        }

        if (args.contains(QStringLiteral("--fit"))) window.fitViewNow();

        // 自动化脚本：cursor=<秒>;add;del;left;right（用于回归测试界面编辑流程）
        const QString editScript = valueOf(QStringLiteral("--edit-script"));
        if (!editScript.isEmpty()) {
            const QStringList cmds = editScript.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString& rawCmd : cmds) {
                const QString cmd = rawCmd.trimmed();
                if (cmd.startsWith(QLatin1String("cursor="))) {
                    window.setEditCursorTime(cmd.mid(7).toDouble());
                } else if (cmd == QLatin1String("add")) {
                    window.addPeakNow();
                } else if (cmd == QLatin1String("del")) {
                    window.deletePeakNow();
                } else if (cmd == QLatin1String("left")) {
                    window.panLeft();
                } else if (cmd == QLatin1String("right")) {
                    window.panRight();
                }
            }
        }

        if (!exportPath.isEmpty()) {
            QString exportError;
            if (!window.exportResultsTo(exportPath, exportError))
                qWarning("导出失败: %s", exportError.toUtf8().constData());
            else
                qInfo("已导出 R 峰结果: %s", exportPath.toUtf8().constData());
        }

        const QString pwavePath = valueOf(QStringLiteral("--p-export"));
        if (!pwavePath.isEmpty()) {
            QString pError;
            if (!window.exportPWaveTo(pwavePath, pError))
                qWarning("导出 P 波失败: %s", pError.toUtf8().constData());
            else
                qInfo("已导出 P 波结果: %s", pwavePath.toUtf8().constData());
        }

        const QString saveAnnotPath = valueOf(QStringLiteral("--save-annot"));
        if (!saveAnnotPath.isEmpty()) {
            QString saveError;
            if (!window.saveAnnotatedTo(saveAnnotPath, saveError))
                qWarning("导出标注失败: %s", saveError.toUtf8().constData());
            else
                qInfo("已导出标注 bin: %s", saveAnnotPath.toUtf8().constData());
        }
    }

    if (!dialogPath.isEmpty()) {
        ecg::BinaryProbe probe;
        QString probeError;
        ecg::probeBinary(dialogPath, probe, probeError);
        auto* dlg = new BinaryDialog(dialogPath, probe.ok ? probe.options : ecg::BinaryOptions(), &window);
        dlg->setModal(false);
        dlg->show();
        if (!shotPath.isEmpty()) {
            QTimer::singleShot(1400, dlg, [dlg, shotPath]() {
                dlg->grab().save(shotPath);
                QCoreApplication::quit();
            });
        } else {
            QObject::connect(dlg, &QDialog::finished, &app, &QCoreApplication::quit);
        }
    } else if (!shotPath.isEmpty()) {
        QTimer::singleShot(1500, &window, [&window, shotPath]() {
            window.grab().save(shotPath);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
