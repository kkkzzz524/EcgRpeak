// 轻量波形绘制控件：多曲线 + 网格刻度 + 事件标记 + 滚轮缩放/拖拽平移
#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

class PlotWidget : public QWidget
{
    Q_OBJECT
public:
    enum class Style { Line, Dashed, Dots };

    explicit PlotWidget(QWidget* parent = nullptr);

    int  addSeries(const QString& name, const QColor& color,
                   qreal width = 1.4, Style style = Style::Line);
    void setSeries(int index, const QVector<double>& xs, const QVector<double>& ys);
    void clearSeriesData();
    void setMarkers(const QVector<double>& xs, const QColor& color, qreal width = 1.0);
    void setSearchbackMarkers(const QVector<double>& xs, const QColor& color);
    void setManualMarkers(const QVector<double>& xs, const QColor& color);
    void setTitle(const QString& title);
    void setAxisLabels(const QString& xLabel, const QString& yLabel);
    void setEmptyText(const QString& text);
    void setXRange(double x0, double x1, bool notify = true);
    void setXRangeSilent(double x0, double x1) { setXRange(x0, x1, false); }
    void fitView();
    void panByFraction(double frac);          // 按可视跨度的比例平移（正数向右）

    void setCursorTime(double t);             // 编辑光标（竖线 + 时间）
    void setCursorVisible(bool visible);
    double cursorTime() const { return m_cursorX; }
    bool   hasCursor() const { return m_hasCursor; }

    double xStart() const { return m_x0; }
    double xEnd() const { return m_x1; }

signals:
    void xRangeChanged(double x0, double x1);
    void cursorMoved(double t);

protected:
    void paintEvent(QPaintEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    struct Series {
        QString name;
        QColor  color;
        qreal   width = 1.4;
        Style   style = Style::Line;
        QVector<double> xs;
        QVector<double> ys;
    };

    QRectF plotRect() const;
    void   recomputeY();
    void   paintGrid(QPainter& p, const QRectF& r);
    void   paintSeries(QPainter& p, const Series& s, const QRectF& r);
    void   paintMarkers(QPainter& p, const QRectF& r);
    void   paintCursor(QPainter& p, const QRectF& r);
    void   paintLegend(QPainter& p, const QRectF& r);
    void   paintEmpty(QPainter& p, const QRectF& r);
    void   updateDataBounds();
    QPointF toPixel(double x, double y, const QRectF& r) const;

    QVector<Series> m_series;
    QVector<double> m_markerX;
    QVector<double> m_sbMarkerX;
    QVector<double> m_manualX;
    QColor          m_markerColor{0xE1, 0x1D, 0x48};
    QColor          m_sbMarkerColor{0xF7, 0x8C, 0x1E};
    QColor          m_manualColor{0x0E, 0xA5, 0xE9};

    double m_cursorX = 0.0;
    bool   m_hasCursor = false;

    QString m_title;
    QString m_xLabel;
    QString m_yLabel;
    QString m_emptyText;

    double m_x0 = 0.0, m_x1 = 1.0;
    double m_y0 = -1.0, m_y1 = 1.0;
    double m_dataX0 = 0.0, m_dataX1 = 1.0;

    bool m_autoY = true;
    bool m_dragging = false;
    bool m_hasData = false;

    QPointF m_dragPos;
    double  m_dragX0 = 0.0, m_dragX1 = 0.0;
    double  m_dragY0 = 0.0, m_dragY1 = 0.0;
};
