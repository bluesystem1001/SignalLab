#include "interactive_chart_view.hpp"

#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QString>
#include <QWheelEvent>

#include <QtCharts/QAbstractAxis>
#include <QtCharts/QChart>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace {

bool scaleValueAxes(
    const QList<QAbstractAxis*>& axes,
    double factor,
    std::optional<double> requestedCenter = {})
{
    if (!std::isfinite(factor) || factor <= 0.0) {
        return false;
    }

    bool changed = false;
    for (QAbstractAxis* abstractAxis : axes) {
        auto* axis = qobject_cast<QValueAxis*>(abstractAxis);
        if (axis == nullptr) {
            continue;
        }
        const double oldMinimum = axis->min();
        const double oldMaximum = axis->max();
        const double center = requestedCenter.value_or(
            oldMinimum + (oldMaximum - oldMinimum) / 2.0);
        if (!std::isfinite(oldMinimum) || !std::isfinite(oldMaximum)
            || !std::isfinite(center) || oldMinimum >= oldMaximum) {
            continue;
        }
        const double minimum =
            center - (center - oldMinimum) * factor;
        const double maximum =
            center + (oldMaximum - center) * factor;
        if (std::isfinite(minimum) && std::isfinite(maximum)
            && minimum < maximum) {
            axis->setRange(minimum, maximum);
            changed = true;
        }
    }
    return changed;
}

} // namespace

InteractiveChartView::InteractiveChartView(
    QChart* chart,
    QWidget* parent)
    : QChartView(chart, parent)
{
    setRenderHint(QPainter::Antialiasing);
    setMouseTracking(true);
    setZoomOrientations(zoomOrientations_);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleDescription(
        "图表支持滚轮缩放、拖动框选缩放、Shift 拖动局部选择、"
        "双击复位和光标读数。");

    resizeTimer_.setSingleShot(true);
    resizeTimer_.setInterval(100);
    QObject::connect(&resizeTimer_, &QTimer::timeout, this, [this]() {
        if (resizeSettledCallback_) {
            resizeSettledCallback_();
        }
    });
}

void InteractiveChartView::setCursorCallback(CursorCallback callback)
{
    cursorCallback_ = std::move(callback);
}

void InteractiveChartView::setSelectionCallback(
    SelectionCallback callback)
{
    selectionCallback_ = std::move(callback);
}

void InteractiveChartView::setSelectionClearedCallback(
    SelectionClearedCallback callback)
{
    selectionClearedCallback_ = std::move(callback);
}

void InteractiveChartView::setResizeSettledCallback(
    ResizeSettledCallback callback)
{
    resizeSettledCallback_ = std::move(callback);
}

void InteractiveChartView::setZoomOrientations(
    Qt::Orientations orientations)
{
    zoomOrientations_ = orientations
        & Qt::Orientations(Qt::Horizontal | Qt::Vertical);

    QChartView::RubberBands rubberBands = QChartView::NoRubberBand;
    if (zoomOrientations_.testFlag(Qt::Horizontal)) {
        rubberBands |= QChartView::HorizontalRubberBand;
    }
    if (zoomOrientations_.testFlag(Qt::Vertical)) {
        rubberBands |= QChartView::VerticalRubberBand;
    }
    setRubberBand(rubberBands);
}

Qt::Orientations InteractiveChartView::zoomOrientations() const noexcept
{
    return zoomOrientations_;
}

bool InteractiveChartView::zoomHorizontally(double factor)
{
    return scaleValueAxes(
        chart()->axes(Qt::Horizontal),
        factor);
}

void InteractiveChartView::clearSelection()
{
    selecting_ = false;
    selectedRectangle_.reset();
    if (selectionClearedCallback_) {
        selectionClearedCallback_();
    }
    viewport()->update();
}

void InteractiveChartView::captureHomeRanges()
{
    const auto horizontalAxes = chart()->axes(Qt::Horizontal);
    const auto verticalAxes = chart()->axes(Qt::Vertical);
    if (!horizontalAxes.empty()) {
        if (const auto* axis = qobject_cast<QValueAxis*>(
                horizontalAxes.front())) {
            homeHorizontalRange_ = AxisRange{axis->min(), axis->max()};
        }
    }
    if (!verticalAxes.empty()) {
        if (const auto* axis = qobject_cast<QValueAxis*>(
                verticalAxes.front())) {
            homeVerticalRange_ = AxisRange{axis->min(), axis->max()};
        }
    }
}

void InteractiveChartView::resetView()
{
    chart()->zoomReset();
    const auto horizontalAxes = chart()->axes(Qt::Horizontal);
    const auto verticalAxes = chart()->axes(Qt::Vertical);
    if (homeHorizontalRange_.has_value() && !horizontalAxes.empty()) {
        if (auto* axis = qobject_cast<QValueAxis*>(horizontalAxes.front())) {
            axis->setRange(
                homeHorizontalRange_->minimum,
                homeHorizontalRange_->maximum);
        }
    }
    if (homeVerticalRange_.has_value() && !verticalAxes.empty()) {
        if (auto* axis = qobject_cast<QValueAxis*>(verticalAxes.front())) {
            axis->setRange(
                homeVerticalRange_->minimum,
                homeVerticalRange_->maximum);
        }
    }
    clearSelection();
}

bool InteractiveChartView::saveImage(
    const QString& fileName,
    QString* errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (fileName.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "导出路径为空。";
        }
        return false;
    }
    if (viewport()->width() <= 0 || viewport()->height() <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "图表当前没有可导出的可见区域。";
        }
        return false;
    }

    QImage image(viewport()->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    viewport()->render(&painter);
    painter.end();
    if (!image.save(fileName, "PNG")) {
        if (errorMessage != nullptr) {
            *errorMessage = "无法写入 PNG 图片，请检查目录权限和扩展名。";
        }
        return false;
    }
    return true;
}

void InteractiveChartView::drawForeground(
    QPainter* painter,
    const QRectF& rectangle)
{
    QChartView::drawForeground(painter, rectangle);
    const QRectF plotRectangle = chart()->mapRectToScene(chart()->plotArea());

    if (cursorVisible_ && plotRectangle.contains(cursorScenePosition_)) {
        QPen cursorPen(QColor(70, 90, 120, 190));
        cursorPen.setStyle(Qt::DashLine);
        cursorPen.setWidthF(0.0);
        painter->setPen(cursorPen);
        painter->drawLine(
            QPointF(cursorScenePosition_.x(), plotRectangle.top()),
            QPointF(cursorScenePosition_.x(), plotRectangle.bottom()));
        painter->drawLine(
            QPointF(plotRectangle.left(), cursorScenePosition_.y()),
            QPointF(plotRectangle.right(), cursorScenePosition_.y()));
    }

    const std::optional<QRect> activeRectangle = selecting_
        ? std::optional<QRect>(
              QRect(selectionStart_, selectionCurrent_).normalized())
        : selectedRectangle_;
    if (activeRectangle.has_value() && !activeRectangle->isNull()) {
        const QRectF sceneSelection(
            mapToScene(activeRectangle->topLeft()),
            mapToScene(activeRectangle->bottomRight()));
        const QRectF clipped = sceneSelection.normalized()
            .intersected(plotRectangle);
        painter->fillRect(clipped, QColor(40, 120, 210, 35));
        QPen selectionPen(QColor(40, 120, 210));
        selectionPen.setStyle(Qt::DashLine);
        selectionPen.setWidthF(0.0);
        painter->setPen(selectionPen);
        painter->drawRect(clipped);
    }
}

void InteractiveChartView::leaveEvent(QEvent* event)
{
    cursorVisible_ = false;
    if (cursorCallback_) {
        cursorCallback_({}, false);
    }
    viewport()->update();
    QChartView::leaveEvent(event);
}

void InteractiveChartView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton
        && event->modifiers() == Qt::NoModifier) {
        resetView();
        event->accept();
        return;
    }
    QChartView::mouseDoubleClickEvent(event);
}

void InteractiveChartView::mouseMoveEvent(QMouseEvent* event)
{
    updateCursor(event->position().toPoint());
    if (selecting_) {
        selectionCurrent_ = clampToPlotArea(event->position().toPoint());
        viewport()->update();
        event->accept();
        return;
    }
    QChartView::mouseMoveEvent(event);
}

void InteractiveChartView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton
        && event->modifiers().testFlag(Qt::ShiftModifier)
        && plotAreaInViewport().contains(event->position().toPoint())) {
        selecting_ = true;
        selectionStart_ = clampToPlotArea(event->position().toPoint());
        selectionCurrent_ = selectionStart_;
        selectedRectangle_.reset();
        viewport()->update();
        event->accept();
        return;
    }
    QChartView::mousePressEvent(event);
}

void InteractiveChartView::mouseReleaseEvent(QMouseEvent* event)
{
    if (selecting_ && event->button() == Qt::LeftButton) {
        selectionCurrent_ = clampToPlotArea(event->position().toPoint());
        selecting_ = false;
        selectedRectangle_ = QRect(
            selectionStart_,
            selectionCurrent_).normalized();
        const double first = valueAt(selectionStart_).x();
        const double last = valueAt(selectionCurrent_).x();
        if (selectionCallback_ && std::isfinite(first)
            && std::isfinite(last) && first != last) {
            selectionCallback_(std::min(first, last), std::max(first, last));
        }
        viewport()->update();
        event->accept();
        return;
    }
    QChartView::mouseReleaseEvent(event);
}

void InteractiveChartView::resizeEvent(QResizeEvent* event)
{
    QChartView::resizeEvent(event);
    resizeTimer_.start();
}

void InteractiveChartView::wheelEvent(QWheelEvent* event)
{
    if (event->angleDelta().y() == 0) {
        QChartView::wheelEvent(event);
        return;
    }

    const QPoint viewportPosition = event->position().toPoint();
    if (!plotAreaInViewport().contains(viewportPosition)) {
        QChartView::wheelEvent(event);
        return;
    }
    const QPointF centerValue = valueAt(viewportPosition);
    const double scale = event->angleDelta().y() > 0 ? 0.8 : 1.25;

    bool changed = false;
    if (zoomOrientations_.testFlag(Qt::Horizontal)) {
        changed = scaleValueAxes(
            chart()->axes(Qt::Horizontal), scale, centerValue.x())
            || changed;
    }
    if (zoomOrientations_.testFlag(Qt::Vertical)) {
        changed = scaleValueAxes(
            chart()->axes(Qt::Vertical), scale, centerValue.y())
            || changed;
    }
    if (changed) {
        event->accept();
        return;
    }
    QChartView::wheelEvent(event);
}

QRect InteractiveChartView::plotAreaInViewport() const
{
    const QRectF scenePlotArea = chart()->mapRectToScene(chart()->plotArea());
    return mapFromScene(scenePlotArea).boundingRect();
}

QPoint InteractiveChartView::clampToPlotArea(const QPoint& position) const
{
    const QRect plotArea = plotAreaInViewport();
    if (plotArea.isEmpty()) {
        return position;
    }
    return QPoint(
        std::clamp(position.x(), plotArea.left(), plotArea.right()),
        std::clamp(position.y(), plotArea.top(), plotArea.bottom()));
}

QPointF InteractiveChartView::valueAt(
    const QPoint& viewportPosition) const
{
    const QPointF scenePosition = mapToScene(viewportPosition);
    const QPointF chartPosition = chart()->mapFromScene(scenePosition);
    return chart()->mapToValue(chartPosition);
}

void InteractiveChartView::updateCursor(const QPoint& viewportPosition)
{
    cursorScenePosition_ = mapToScene(viewportPosition);
    const QRectF plotRectangle = chart()->mapRectToScene(chart()->plotArea());
    cursorVisible_ = plotRectangle.contains(cursorScenePosition_);
    if (cursorCallback_) {
        cursorCallback_(valueAt(viewportPosition), cursorVisible_);
    }
    viewport()->update();
}
