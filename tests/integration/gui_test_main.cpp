#include <QApplication>
#include <QByteArray>

#include <gtest/gtest.h>

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    QApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
