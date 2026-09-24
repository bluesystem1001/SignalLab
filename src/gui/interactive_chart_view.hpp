#pragma once

#include <QtCharts/QChartView>

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QTimer>

#include <functional>
#include <optional>

class QMouseEvent;
class QPainter;
class QResizeEvent;
class QWheelEvent;

class InteractiveChartView final : public QChartView
{
    Q_OBJECT

public:
    using CursorCallback = std::function<void(QPointF, bool)>;
    using SelectionCallback = std::function<void(double, double)>;
    using ResizeSettledCallback = std::function<void()>;

    explicit InteractiveChartView(QChart* chart, QWidget* parent = nullptr);

    void setCursorCallback(CursorCallback callback);
    void setSelectionCallback(SelectionCallback callback);
    void setResizeSettledCallback(ResizeSettledCallback callback);

    void captureHomeRanges();
    void resetView();
    [[nodiscard]] bool saveImage(
        const QString& fileName,
        QString* errorMessage = nullptr);

protected:
    void drawForeground(QPainter* painter, const QRectF& rectangle) override;
    void leaveEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct AxisRange {
        double minimum{0.0};
        double maximum{1.0};
    };

    [[nodiscard]] QRect plotAreaInViewport() const;
    [[nodiscard]] QPoint clampToPlotArea(const QPoint& position) const;
    [[nodiscard]] QPointF valueAt(const QPoint& viewportPosition) const;
    void updateCursor(const QPoint& viewportPosition);

    CursorCallback cursorCallback_;
    SelectionCallback selectionCallback_;
    ResizeSettledCallback resizeSettledCallback_;
    QTimer resizeTimer_;
    QPointF cursorScenePosition_;
    bool cursorVisible_{false};
    bool selecting_{false};
    QPoint selectionStart_;
    QPoint selectionCurrent_;
    std::optional<QRect> selectedRectangle_;
    std::optional<AxisRange> homeHorizontalRange_;
    std::optional<AxisRange> homeVerticalRange_;
};
