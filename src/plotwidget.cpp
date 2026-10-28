#include "plotwidget.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

const QColor kCardBg(255, 255, 255);
const QColor kCardBorder(0xDF, 0xE7, 0xF0);
const QColor kGrid(0x94, 0xA3, 0xB8, 62);
const QColor kAxisText(0x7B, 0x8B, 0xA1);
const QColor kTitleText(0x1E, 0x29, 0x3B);
const QColor kZeroLine(0x94, 0xA3, 0xB8, 120);

double niceStep(double range, int targetTicks)
{
    if (!(range > 0.0) || targetTicks < 1) return 1.0;
    const double raw = range / targetTicks;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    double mult = 10.0;
    if (norm <= 1.0) mult = 1.0;
    else if (norm <= 2.0) mult = 2.0;
    else if (norm <= 2.5) mult = 2.5;
    else if (norm <= 5.0) mult = 5.0;
    return mult * mag;
}

QString formatTick(double v, double step)
{
    Q_UNUSED(step);
    QString s;
    const double av = std::fabs(v);
    if (av < 1e-12) return QStringLiteral("0");
    if (av >= 1000.0) s = QString::number(v, 'f', 0);
    else s = QString::number(v, 'g', 3);
    return s;
}

int lowerIndex(const QVector<double>& xs, double v)
{
    return static_cast<int>(std::lower_bound(xs.begin(), xs.end(), v) - xs.begin());
}

} // namespace

PlotWidget::PlotWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(120);
    setMouseTracking(false);
    setEmptyText(QStringLiteral("暂无数据"));
    setCursor(Qt::ArrowCursor);
}

int PlotWidget::addSeries(const QString& name, const QColor& color, qreal width, Style style)
{
    Series s;
    s.name = name;
    s.color = color;
    s.width = width;
    s.style = style;
    m_series.push_back(s);
    return static_cast<int>(m_series.size()) - 1;
}

void PlotWidget::setSeries(int index, const QVector<double>& xs, const QVector<double>& ys)
{
    if (index < 0 || index >= m_series.size()) return;
    Series& s = m_series[index];
    s.xs = xs;
    s.ys = ys;
    if (s.xs.size() > s.ys.size()) s.xs.resize(s.ys.size());
    updateDataBounds();
    if (m_autoY) recomputeY();
    update();
}

void PlotWidget::clearSeriesData()
{
    for (Series& s : m_series) {
        s.xs.clear();
        s.ys.clear();
    }
    m_markerX.clear();
    m_sbMarkerX.clear();
    m_hasData = false;
    m_dataX0 = 0.0;
    m_dataX1 = 1.0;
    update();
}

void PlotWidget::setMarkers(const QVector<double>& xs, const QColor& color, qreal width)
{
    m_markerX = xs;
    m_markerColor = color;
    Q_UNUSED(width);
    update();
}

void PlotWidget::setSearchbackMarkers(const QVector<double>& xs, const QColor& color)
{
    m_sbMarkerX = xs;
    m_sbMarkerColor = color;
    update();
}

void PlotWidget::setManualMarkers(const QVector<double>& xs, const QColor& color)
{
    m_manualX = xs;
    m_manualColor = color;
    update();
}

void PlotWidget::setTitle(const QString& title)
{
    m_title = title;
    update();
}

void PlotWidget::setAxisLabels(const QString& xLabel, const QString& yLabel)
{
    m_xLabel = xLabel;
    m_yLabel = yLabel;
    update();
}

void PlotWidget::setEmptyText(const QString& text)
{
    m_emptyText = text;
    update();
}

void PlotWidget::updateDataBounds()
{
    bool any = false;
    double lo = 1e300, hi = -1e300;
    for (const Series& s : m_series) {
        if (s.xs.isEmpty()) continue;
        any = true;
        lo = std::min(lo, s.xs.first());
        hi = std::max(hi, s.xs.last());
    }
    m_hasData = any;
    if (any && hi > lo) {
        m_dataX0 = lo;
        m_dataX1 = hi;
    }
    setCursor(m_hasData ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

QRectF PlotWidget::plotRect() const
{
    const qreal left = 66.0;
    const qreal right = 16.0;
    const qreal top = m_title.isEmpty() ? 14.0 : 32.0;
    const qreal bottom = 42.0;
    QRectF r = QRectF(rect()).adjusted(left, top, -right, -bottom);
    if (r.width() < 10.0) r.setWidth(10.0);
    if (r.height() < 10.0) r.setHeight(10.0);
    return r;
}

void PlotWidget::recomputeY()
{
    if (!m_hasData) return;
    double lo = 1e300, hi = -1e300;
    bool any = false;
    for (const Series& s : m_series) {
        if (s.ys.isEmpty()) continue;
        const int i0 = lowerIndex(s.xs, m_x0);
        const int i1 = std::min(static_cast<int>(s.ys.size()) - 1, lowerIndex(s.xs, m_x1));
        if (i1 < i0) continue;
        const int stride = std::max(1, (i1 - i0 + 1) / 20000);
        for (int i = i0; i <= i1; i += stride) {
            const double v = s.ys[static_cast<size_t>(i)];
            if (!std::isfinite(v)) continue;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            any = true;
        }
        const double last = s.ys[static_cast<size_t>(i1)];
        if (std::isfinite(last)) { lo = std::min(lo, last); hi = std::max(hi, last); }
    }
    if (!any) return;
    double span = hi - lo;
    if (span <= 1e-12) span = std::max(1e-6, std::fabs(hi) * 0.1);
    const double pad = span * 0.08;
    m_y0 = lo - pad;
    m_y1 = hi + pad;
}

QPointF PlotWidget::toPixel(double x, double y, const QRectF& r) const
{
    const double sx = (m_x1 > m_x0) ? r.width() / (m_x1 - m_x0) : 0.0;
    const double sy = (m_y1 > m_y0) ? r.height() / (m_y1 - m_y0) : 0.0;
    return QPointF(r.left() + (x - m_x0) * sx, r.bottom() - (y - m_y0) * sy);
}

void PlotWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QColor parentBg = parentWidget() ? parentWidget()->palette().window().color()
                                           : QColor(0xEE, 0xF2, 0xF7);
    p.fillRect(rect(), parentBg);

    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10.0, 10.0);
    p.fillPath(card, kCardBg);
    p.setPen(QPen(kCardBorder, 1.0));
    p.drawPath(card);

    const QRectF r = plotRect();

    if (!m_title.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(std::max(8.5, f.pointSizeF()));
        f.setBold(true);
        p.setFont(f);
        p.setPen(kTitleText);
        p.drawText(QRectF(r.left(), 8.0, r.width(), 18.0),
                   Qt::AlignLeft | Qt::AlignVCenter, m_title);
    }

    if (!m_hasData) {
        paintEmpty(p, r);
        return;
    }

    if (m_autoY) recomputeY();
    if (!(m_y1 > m_y0)) { m_y0 = -1.0; m_y1 = 1.0; }

    paintGrid(p, r);

    p.save();
    p.setClipRect(r);
    paintMarkers(p, r);
    for (const Series& s : m_series) paintSeries(p, s, r);
    paintCursor(p, r);
    p.restore();

    paintLegend(p, r);
}

void PlotWidget::paintEmpty(QPainter& p, const QRectF& r)
{
    QFont f = font();
    f.setPointSizeF(9.0);
    p.setFont(f);
    p.setPen(QColor(0xB6, 0xC2, 0xD2));
    p.drawText(r, Qt::AlignCenter, m_emptyText);
}

void PlotWidget::paintGrid(QPainter& p, const QRectF& r)
{
    QFont f = font();
    f.setPointSizeF(7.8);
    p.setFont(f);

    const double spanX = m_x1 - m_x0;
    const double spanY = m_y1 - m_y0;

    const double stepX = niceStep(spanX, std::max(3, static_cast<int>(r.width() / 110.0)));
    const double stepY = niceStep(spanY, std::max(2, static_cast<int>(r.height() / 42.0)));

    p.setPen(QPen(kGrid, 1.0));

    // 竖向网格 + X 轴刻度
    if (stepX > 0.0 && spanX / stepX < 400.0) {
        double x = std::ceil(m_x0 / stepX) * stepX;
        for (int guard = 0; x <= m_x1 + 1e-9 && guard < 400; ++guard, x += stepX) {
            const double px = r.left() + (x - m_x0) / spanX * r.width();
            p.setPen(QPen(kGrid, 1.0));
            p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
            p.setPen(kAxisText);
            p.drawText(QRectF(px - 45.0, r.bottom() + 5.0, 90.0, 15.0),
                       Qt::AlignHCenter | Qt::AlignTop, formatTick(x, stepX));
        }
    }

    // 横向网格 + Y 轴刻度
    if (stepY > 0.0 && spanY / stepY < 200.0) {
        double y = std::ceil(m_y0 / stepY) * stepY;
        for (int guard = 0; y <= m_y1 + 1e-12 && guard < 200; ++guard, y += stepY) {
            const double py = r.bottom() - (y - m_y0) / spanY * r.height();
            p.setPen(QPen(kGrid, 1.0));
            p.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));
            p.setPen(kAxisText);
            p.drawText(QRectF(4.0, py - 8.0, 56.0, 16.0),
                       Qt::AlignRight | Qt::AlignVCenter, formatTick(y, stepY));
        }
    }

    // 零线
    if (m_y0 < 0.0 && m_y1 > 0.0) {
        const double py = r.bottom() - (0.0 - m_y0) / spanY * r.height();
        p.setPen(QPen(kZeroLine, 1.0));
        p.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));
    }

    // 坐标轴边框
    p.setPen(QPen(QColor(0xD5, 0xDF, 0xEA), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawLine(r.bottomLeft(), r.bottomRight());
    p.drawLine(r.topLeft(), r.bottomLeft());

    // 轴标签
    p.setPen(QColor(0xA8, 0xB5, 0xC6));
    if (!m_xLabel.isEmpty()) {
        p.drawText(QRectF(r.right() - 160.0, r.bottom() + 21.0, 160.0, 14.0),
                   Qt::AlignRight | Qt::AlignVCenter, m_xLabel);
    }
    if (!m_yLabel.isEmpty()) {
        p.save();
        p.translate(13.0, r.center().y());
        p.rotate(-90.0);
        p.drawText(QRectF(-r.height() / 2.0, -8.0, r.height(), 16.0),
                   Qt::AlignCenter, m_yLabel);
        p.restore();
    }
}

void PlotWidget::paintMarkers(QPainter& p, const QRectF& r)
{
    const double spanX = m_x1 - m_x0;
    if (spanX <= 0.0) return;

    // 标记过密时（间距不足 2 px）不再画线，避免糊成一片
    const int total = std::max(std::max(m_markerX.size(), m_sbMarkerX.size()), m_manualX.size());
    if (total > 0 && r.width() / static_cast<double>(total) < 2.0) return;

    QPen pen(m_markerColor);
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DashLine);
    QColor c = m_markerColor;
    c.setAlpha(110);
    pen.setColor(c);
    p.setPen(pen);

    for (double x : m_markerX) {
        if (x < m_x0 || x > m_x1) continue;
        const double px = r.left() + (x - m_x0) / spanX * r.width();
        p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
    }

    QColor c2 = m_sbMarkerColor;
    c2.setAlpha(130);
    QPen pen2(c2);
    pen2.setWidthF(1.2);
    pen2.setStyle(Qt::DotLine);
    p.setPen(pen2);
    for (double x : m_sbMarkerX) {
        if (x < m_x0 || x > m_x1) continue;
        const double px = r.left() + (x - m_x0) / spanX * r.width();
        p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));
    }

    // 人工增删过的 R 峰：实线 + 顶端菱形，和算法检出区分开
    QColor c3 = m_manualColor;
    c3.setAlpha(150);
    QPen pen3(c3);
    pen3.setWidthF(1.3);
    p.setPen(pen3);
    p.setBrush(m_manualColor);
    for (double x : m_manualX) {
        if (x < m_x0 || x > m_x1) continue;
        const double px = r.left() + (x - m_x0) / spanX * r.width();
        p.drawLine(QPointF(px, r.top() + 6.0), QPointF(px, r.bottom()));
        QPolygonF diamond;
        diamond << QPointF(px, r.top() + 1.0) << QPointF(px + 3.5, r.top() + 6.0)
                << QPointF(px, r.top() + 11.0) << QPointF(px - 3.5, r.top() + 6.0);
        p.drawPolygon(diamond);
    }
    p.setBrush(Qt::NoBrush);
}

void PlotWidget::paintSeries(QPainter& p, const Series& s, const QRectF& r)
{
    const int n = s.ys.size();
    if (n == 0) return;

    const double spanX = m_x1 - m_x0;
    if (spanX <= 0.0) return;

    int i0 = lowerIndex(s.xs, m_x0);
    int i1 = lowerIndex(s.xs, m_x1);
    if (i1 < n) ++i1;
    i0 = std::max(0, i0 - 1);
    i1 = std::min(n, i1 + 1);
    if (i1 - i0 < 1) return;

    QPen pen(s.color);
    pen.setWidthF(s.width);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);

    if (s.style == Style::Dots) {
        pen.setStyle(Qt::SolidLine);
        p.setPen(Qt::NoPen);
        p.setBrush(s.color);
        const qreal rad = std::max<qreal>(1.8, s.width);
        for (int i = i0; i < i1; ++i) {
            if (!std::isfinite(s.ys[static_cast<size_t>(i)])) continue;
            p.drawEllipse(toPixel(s.xs[static_cast<size_t>(i)], s.ys[static_cast<size_t>(i)], r),
                          rad, rad);
        }
        p.setBrush(Qt::NoBrush);
        return;
    }

    if (s.style == Style::Dashed) pen.setStyle(Qt::DashLine);
    p.setPen(pen);

    QPolygonF poly;
    const int count = i1 - i0;
    const double plotW = std::max(2.0, r.width());

    if (count <= plotW * 2.0) {
        poly.reserve(count);
        for (int i = i0; i < i1; ++i) {
            const double y = s.ys[static_cast<size_t>(i)];
            if (!std::isfinite(y)) continue;
            poly << toPixel(s.xs[static_cast<size_t>(i)], y, r);
        }
    } else {
        const int cols = static_cast<int>(plotW);
        poly.reserve(cols * 2 + 4);
        for (int c = 0; c < cols; ++c) {
            const int a = i0 + static_cast<int>(static_cast<double>(count) * c / cols);
            int b = i0 + static_cast<int>(static_cast<double>(count) * (c + 1) / cols);
            if (b <= a) b = a + 1;
            if (b > i1) b = i1;

            double mn = 1e300, mx = -1e300;
            int imn = a, imx = a;
            bool any = false;
            for (int i = a; i < b; ++i) {
                const double y = s.ys[static_cast<size_t>(i)];
                if (!std::isfinite(y)) continue;
                any = true;
                if (y < mn) { mn = y; imn = i; }
                if (y > mx) { mx = y; imx = i; }
            }
            if (!any) continue;

            const double px = r.left() + (s.xs[static_cast<size_t>((a + b) / 2)] - m_x0) / spanX * r.width();
            if (s.xs[static_cast<size_t>((a + b) / 2)] < m_x0 || s.xs[static_cast<size_t>((a + b) / 2)] > m_x1)
                continue;

            const double pymin = r.bottom() - (mn - m_y0) / (m_y1 - m_y0) * r.height();
            const double pymax = r.bottom() - (mx - m_y0) / (m_y1 - m_y0) * r.height();
            if (imn <= imx) {
                poly << QPointF(px, pymin) << QPointF(px, pymax);
            } else {
                poly << QPointF(px, pymax) << QPointF(px, pymin);
            }
        }
    }

    if (poly.size() > 1) p.drawPolyline(poly);
    else if (poly.size() == 1) p.drawPoint(poly.first());
}

void PlotWidget::paintLegend(QPainter& p, const QRectF& r)
{
    QVector<int> items;
    for (int i = 0; i < m_series.size(); ++i) {
        if (!m_series[i].name.isEmpty() && !m_series[i].ys.isEmpty()) items.push_back(i);
    }
    if (items.isEmpty()) return;

    QFont f = font();
    f.setPointSizeF(7.8);
    p.setFont(f);
    const QFontMetricsF fm(f);

    qreal textW = 0.0;
    for (int idx : items) textW = std::max(textW, fm.horizontalAdvance(m_series[idx].name));

    const qreal lineW = 18.0;
    const qreal rowH = fm.height() + 2.0;
    const qreal boxW = lineW + textW + 20.0;
    const qreal boxH = rowH * items.size() + 10.0;
    QRectF box(r.left() + 10.0, r.top() + 8.0, boxW, boxH);
    if (box.width() > r.width() - 20.0) box.setWidth(r.width() - 20.0);

    QPainterPath path;
    path.addRoundedRect(box, 6.0, 6.0);
    p.fillPath(path, QColor(255, 255, 255, 225));
    p.setPen(QPen(QColor(0xE4, 0xEB, 0xF3), 1.0));
    p.drawPath(path);

    qreal y = box.top() + 5.0;
    for (int idx : items) {
        const Series& s = m_series[idx];
        const qreal cy = y + rowH / 2.0;
        QPen pen(s.color);
        pen.setWidthF(std::max<qreal>(1.4, s.width));
        if (s.style == Style::Dashed) pen.setStyle(Qt::DashLine);
        if (s.style == Style::Dots) {
            p.setPen(Qt::NoPen);
            p.setBrush(s.color);
            p.drawEllipse(QPointF(box.left() + 12.0, cy), 2.4, 2.4);
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(pen);
            p.drawLine(QPointF(box.left() + 7.0, cy), QPointF(box.left() + 7.0 + lineW, cy));
        }
        p.setPen(QColor(0x5B, 0x6B, 0x82));
        p.drawText(QRectF(box.left() + lineW + 12.0, y, textW + 6.0, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, s.name);
        y += rowH;
    }
}

void PlotWidget::setXRange(double x0, double x1, bool notify)
{
    if (!(x1 > x0) || !std::isfinite(x0) || !std::isfinite(x1)) return;

    // 尚无数据时不做跨度限制，等数据到达后再统一收敛
    if (!m_hasData) {
        m_x0 = x0;
        m_x1 = x1;
        update();
        if (notify) emit xRangeChanged(m_x0, m_x1);
        return;
    }

    double span = x1 - x0;
    const double dataSpan = std::max(1e-6, m_dataX1 - m_dataX0);
    const double minSpan = std::max(1e-4, dataSpan * 1e-4);
    const double maxSpan = dataSpan * 8.0 + 1e-6;
    if (span < minSpan) {
        const double c = 0.5 * (x0 + x1);
        x0 = c - minSpan / 2.0;
        x1 = c + minSpan / 2.0;
        span = minSpan;
    }
    if (span > maxSpan) {
        const double c = 0.5 * (x0 + x1);
        x0 = c - maxSpan / 2.0;
        x1 = c + maxSpan / 2.0;
    }
    m_x0 = x0;
    m_x1 = x1;
    if (m_autoY) recomputeY();
    update();
    if (notify) emit xRangeChanged(m_x0, m_x1);
}

void PlotWidget::fitView()
{
    if (!m_hasData) return;
    m_x0 = m_dataX0;
    m_x1 = m_dataX1;
    m_autoY = true;
    recomputeY();
    update();
    emit xRangeChanged(m_x0, m_x1);
}

void PlotWidget::paintCursor(QPainter& p, const QRectF& r)
{
    if (!m_hasCursor || !(m_x1 > m_x0)) return;
    if (m_cursorX < m_x0 || m_cursorX > m_x1) return;

    const double px = r.left() + (m_cursorX - m_x0) / (m_x1 - m_x0) * r.width();
    QPen pen(QColor(0x25, 0x63, 0xEB, 165));
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    p.drawLine(QPointF(px, r.top()), QPointF(px, r.bottom()));

    QFont f = font();
    f.setPointSizeF(7.6);
    p.setFont(f);
    const QString text = QStringLiteral("%1 s").arg(QString::number(m_cursorX, 'f', 3));
    const QFontMetricsF fm(f);
    const qreal w = fm.horizontalAdvance(text) + 10.0;
    QRectF box(px + 3.0, r.top() + 2.0, w, fm.height() + 4.0);
    if (box.right() > r.right()) box.moveLeft(px - 3.0 - w);
    p.setBrush(QColor(0x25, 0x63, 0xEB, 235));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(box, 3.0, 3.0);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, text);
    p.setBrush(Qt::NoBrush);
}

void PlotWidget::setCursorTime(double t)
{
    if (!std::isfinite(t)) return;
    if (m_hasCursor && std::fabs(t - m_cursorX) < 1e-9) return;
    m_cursorX = t;
    m_hasCursor = true;
    update();
    emit cursorMoved(m_cursorX);
}

void PlotWidget::setCursorVisible(bool visible)
{
    if (m_hasCursor == visible) return;
    m_hasCursor = visible;
    update();
}

void PlotWidget::panByFraction(double frac)
{
    if (!m_hasData || !(m_x1 > m_x0)) return;
    const double span = m_x1 - m_x0;
    double x0 = m_x0 + span * frac;
    double x1 = m_x1 + span * frac;

    const double lo = m_dataX0 - 0.15 * span;
    const double hi = m_dataX1 + 0.15 * span;
    if (x0 < lo) { x0 = lo; x1 = lo + span; }
    if (x1 > hi) { x1 = hi; x0 = hi - span; }
    setXRange(x0, x1, true);
}

void PlotWidget::wheelEvent(QWheelEvent* e)
{
    if (!m_hasData) { QWidget::wheelEvent(e); return; }
    const QRectF r = plotRect();
    const double spanX = m_x1 - m_x0;
    if (spanX <= 0.0 || r.width() <= 0.0) return;

    const double factor = (e->angleDelta().y() > 0) ? 0.82 : 1.2195;
    const double px = std::clamp(e->position().x(), r.left(), r.right());
    const double xc = m_x0 + (px - r.left()) / r.width() * spanX;

    setXRange(xc - (xc - m_x0) * factor, xc + (m_x1 - xc) * factor, true);
    e->accept();
}

void PlotWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && m_hasData) {
        const QRectF r0 = plotRect();
        if (r0.width() > 0.0 && e->position().x() >= r0.left() && e->position().x() <= r0.right()) {
            const double t = m_x0 + (e->position().x() - r0.left()) / r0.width() * (m_x1 - m_x0);
            setCursorTime(t);
        }
        m_dragging = true;
        m_dragPos = e->position();
        m_dragX0 = m_x0;
        m_dragX1 = m_x1;
        m_dragY0 = m_y0;
        m_dragY1 = m_y1;
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void PlotWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging) {
        if (m_hasData) {
            const QRectF r = plotRect();
            if (r.width() > 0.0 && e->position().x() >= r.left() && e->position().x() <= r.right()) {
                const double t = m_x0 + (e->position().x() - r.left()) / r.width() * (m_x1 - m_x0);
                setCursorTime(t);
            }
        }
        QWidget::mouseMoveEvent(e);
        return;
    }
    const QRectF r = plotRect();
    if (r.width() <= 0.0) return;

    const double dx = e->position().x() - m_dragPos.x();
    const double spanX = m_dragX1 - m_dragX0;
    const double shift = -dx * spanX / r.width();

    double y0 = m_dragY0, y1 = m_dragY1;
    if (!m_autoY && r.height() > 0.0) {
        const double dy = e->position().y() - m_dragPos.y();
        const double shiftY = dy * (m_dragY1 - m_dragY0) / r.height();
        y0 += shiftY;
        y1 += shiftY;
    }

    m_x0 = m_dragX0 + shift;
    m_x1 = m_dragX1 + shift;
    if (!m_autoY) { m_y0 = y0; m_y1 = y1; }
    if (m_autoY) recomputeY();
    update();
    emit xRangeChanged(m_x0, m_x1);
    e->accept();
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(m_hasData ? Qt::OpenHandCursor : Qt::ArrowCursor);
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void PlotWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && m_hasData) {
        fitView();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void PlotWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (m_autoY) recomputeY();
}
