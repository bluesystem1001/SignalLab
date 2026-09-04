#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSqlDatabase>

TEST(SmokeTest, BasicArithmeticWorks)
{
    const int result = 2 + 2;

    EXPECT_EQ(result, 4);
}

TEST(SmokeTest, SQLiteDriverIsAvailable)
{
    int argumentCount = 1;
    char programName[] = "signallab_tests";
    char* arguments[] = {programName, nullptr};
    QCoreApplication application(argumentCount, arguments);

    EXPECT_TRUE(QSqlDatabase::isDriverAvailable("QSQLITE"));
}
