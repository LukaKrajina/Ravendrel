#include <QApplication>
#include <QStandardPaths>
#include <QtTest/QtTest>

QObject *createTestLogManager();
QObject *createTestProjectStore();
QObject *createTestAssetTypes();
QObject *createTestThemeManager();

int main(int argc, char *argv[])
{
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