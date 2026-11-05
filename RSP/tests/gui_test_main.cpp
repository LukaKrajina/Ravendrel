// Qt Test 测试入口：统一运行 GUI 纯逻辑模块的测试类。
#include <QApplication>
#include <QStandardPaths>
#include <QtTest/QtTest>

// 测试类工厂（在各测试 .cpp 中定义，返回 new 的测试对象）
QObject *createTestLogManager();
QObject *createTestProjectStore();
QObject *createTestAssetTypes();
QObject *createTestThemeManager();

int main(int argc, char *argv[])
{
    // 使用独立的 applicationName/organizationName，避免污染真实用户配置；
    // 同时开启 test mode，使 QStandardPaths / QSettings 落到测试临时目录。
    QApplication app(argc, argv);
    app.setApplicationName("rvdlRSPTest");
    app.setOrganizationName("RavendrelProjectTest");
    QStandardPaths::setTestModeEnabled(true);

    int status = 0;
    status |= QTest::qExec(createTestLogManager(), argc, argv);
    status |= QTest::qExec(createTestProjectStore(), argc, argv);
    status |= QTest::qExec(createTestAssetTypes(), argc, argv);
    status |= QTest::qExec(createTestThemeManager(), argc, argv);
    return status;
}
