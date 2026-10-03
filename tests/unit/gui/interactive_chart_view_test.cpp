#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImageReader>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>

#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "gui/interactive_chart_view.hpp"

namespace {

struct ChartFixture {
    InteractiveChartView* view{nullptr};
    QValueAxis* horizontalAxis{nullptr};
    QValueAxis* verticalAxis{nullptr};
};

ChartFixture makeChartView()
{
    auto* chart = new QChart;
    auto* series = new QLineSeries;
    series->append(0.0, -1.0);
    series->append(5.0, 1.0);
    series->append(10.0, 0.0);
    chart->addSeries(series);

    auto* horizontalAxis = new QValueAxis;
    auto* verticalAxis = new QValueAxis;
    horizontalAxis->setRange(0.0, 10.0);
    verticalAxis->setRange(-2.0, 2.0);
    chart->addAxis(horizontalAxis, Qt::AlignBottom);
    chart->addAxis(verticalAxis, Qt::AlignLeft);
    series->attachAxis(horizontalAxis);
    series->attachAxis(verticalAxis);

    auto* view = new InteractiveChartView(chart);
    view->resize(640, 360);
    view->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return {view, horizontalAxis, verticalAxis};
}

void processEventsFor(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

void sendWheel(InteractiveChartView* view, int angleDeltaY)
{
    const QPoint position = view->mapFromScene(
        view->chart()->mapToScene(view->chart()->plotArea().center()));
    QWheelEvent event(
        QPointF(position),
        QPointF(view->viewport()->mapToGlobal(position)),
        {},
        QPoint(0, angleDeltaY),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QCoreApplication::sendEvent(view->viewport(), &event);
}

TEST(InteractiveChartViewTest, SaveImageWritesReadablePng)
{
    const ChartFixture fixture = makeChartView();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString outputPath = directory.filePath("chart.png");

    QString errorMessage;
    EXPECT_TRUE(fixture.view->saveImage(outputPath, &errorMessage))
        << errorMessage.toStdString();

    QFile file(outputPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.read(8), QByteArray::fromHex("89504e470d0a1a0a"));
    QImageReader reader(outputPath, "PNG");
    EXPECT_TRUE(reader.canRead());
    EXPECT_FALSE(reader.read().isNull());

    delete fixture.view;
}

TEST(InteractiveChartViewTest, ResetViewRestoresCapturedAxisRanges)
{
    const ChartFixture fixture = makeChartView();
    fixture.view->captureHomeRanges();
    fixture.horizontalAxis->setRange(2.0, 4.0);
    fixture.verticalAxis->setRange(-0.5, 0.5);

    fixture.view->resetView();

    EXPECT_DOUBLE_EQ(fixture.horizontalAxis->min(), 0.0);
    EXPECT_DOUBLE_EQ(fixture.horizontalAxis->max(), 10.0);
    EXPECT_DOUBLE_EQ(fixture.verticalAxis->min(), -2.0);
    EXPECT_DOUBLE_EQ(fixture.verticalAxis->max(), 2.0);

    delete fixture.view;
}

TEST(InteractiveChartViewTest, DefaultWheelZoomChangesBothAxes)
{
    const ChartFixture fixture = makeChartView();
    EXPECT_TRUE(fixture.view->zoomOrientations().testFlag(Qt::Horizontal));
    EXPECT_TRUE(fixture.view->zoomOrientations().testFlag(Qt::Vertical));
    EXPECT_TRUE(fixture.view->rubberBand().testFlag(
        QChartView::HorizontalRubberBand));
    EXPECT_TRUE(fixture.view->rubberBand().testFlag(
        QChartView::VerticalRubberBand));

    sendWheel(fixture.view, 120);

    EXPECT_LT(
        fixture.horizontalAxis->max() - fixture.horizontalAxis->min(),
        10.0);
    EXPECT_LT(
        fixture.verticalAxis->max() - fixture.verticalAxis->min(),
        4.0);

    delete fixture.view;
}

TEST(InteractiveChartViewTest, HorizontalWheelZoomLeavesVerticalRangeUnchanged)
{
    const ChartFixture fixture = makeChartView();
    fixture.view->setZoomOrientations(Qt::Horizontal);
    EXPECT_TRUE(fixture.view->zoomOrientations().testFlag(Qt::Horizontal));
    EXPECT_FALSE(fixture.view->zoomOrientations().testFlag(Qt::Vertical));
    EXPECT_TRUE(fixture.view->rubberBand().testFlag(
        QChartView::HorizontalRubberBand));
    EXPECT_FALSE(fixture.view->rubberBand().testFlag(
        QChartView::VerticalRubberBand));

    sendWheel(fixture.view, 120);

    EXPECT_LT(
        fixture.horizontalAxis->max() - fixture.horizontalAxis->min(),
        10.0);
    EXPECT_DOUBLE_EQ(fixture.verticalAxis->min(), -2.0);
    EXPECT_DOUBLE_EQ(fixture.verticalAxis->max(), 2.0);

    delete fixture.view;
}

TEST(InteractiveChartViewTest, ProgrammaticHorizontalZoomUsesAxisCenter)
{
    const ChartFixture fixture = makeChartView();

    EXPECT_TRUE(fixture.view->zoomHorizontally(0.5));
    EXPECT_DOUBLE_EQ(fixture.horizontalAxis->min(), 2.5);
    EXPECT_DOUBLE_EQ(fixture.horizontalAxis->max(), 7.5);
    EXPECT_DOUBLE_EQ(fixture.verticalAxis->min(), -2.0);
    EXPECT_DOUBLE_EQ(fixture.verticalAxis->max(), 2.0);

    EXPECT_TRUE(fixture.view->zoomHorizontally(2.0));
    EXPECT_DOUBLE_EQ(fixture.horizontalAxis->min(), 0.0);
    EXPECT_DOUBLE_EQ(fixture.horizontalAxis->max(), 10.0);
    EXPECT_FALSE(fixture.view->zoomHorizontally(0.0));
    EXPECT_FALSE(fixture.view->zoomHorizontally(
        std::numeric_limits<double>::quiet_NaN()));

    delete fixture.view;
}

TEST(InteractiveChartViewTest, ResizeCallbackIsDebounced)
{
    const ChartFixture fixture = makeChartView();
    processEventsFor(130);
    int callbackCount = 0;
    fixture.view->setResizeSettledCallback([&callbackCount]() {
        ++callbackCount;
    });

    fixture.view->resize(650, 370);
    fixture.view->resize(660, 380);
    fixture.view->resize(670, 390);
    processEventsFor(60);
    EXPECT_EQ(callbackCount, 0);
    processEventsFor(80);
    EXPECT_EQ(callbackCount, 1);

    delete fixture.view;
}

} // namespace
