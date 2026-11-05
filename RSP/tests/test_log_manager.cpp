#include <QtTest/QtTest>
#include <QFile>

#include "log_manager.h"

using namespace rvdlrsp::gui;

class TestLogManager : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void test_log_file_path();
    void test_min_level();
    void test_write_and_filter();
    void test_rotation();
};

void TestLogManager::initTestCase()
{
    LogManager::instance().init();
    LogManager::instance().set_min_level(LogManager::Level::Debug);
}

void TestLogManager::test_log_file_path()
{
    LogManager::instance().init();
    QVERIFY(!LogManager::instance().log_dir().isEmpty());
    QVERIFY(!LogManager::instance().log_file_path().isEmpty());
    QVERIFY(LogManager::instance().log_file_path().endsWith("rvdlrsp_gui.log"));
}

void TestLogManager::test_min_level()
{
    auto &lm = LogManager::instance();
    lm.set_min_level(LogManager::Level::Warning);
    QVERIFY(lm.min_level() == LogManager::Level::Warning);
    lm.set_min_level(LogManager::Level::Debug); // 恢复
    QVERIFY(lm.min_level() == LogManager::Level::Debug);
}

void TestLogManager::test_write_and_filter()
{
    auto &lm = LogManager::instance();
    lm.init();
    lm.set_min_level(LogManager::Level::Info);

    QFile before(lm.log_file_path());
    qint64 base = before.exists() ? before.size() : 0;

    // 应写入：Info >= min_level
    lm.write(LogManager::Level::Info, "unittest", "hello-info");
    // 应被过滤：Debug < min_level
    lm.write(LogManager::Level::Debug, "unittest", "hello-debug");

    QFile after(lm.log_file_path());
    QVERIFY(after.exists());
    QVERIFY(after.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(after.readAll());

    QVERIFY(content.contains("hello-info"));
    QVERIFY(content.contains("[INFO]"));
    QVERIFY(content.contains("[unittest]"));
    QVERIFY(!content.contains("hello-debug")); // 被级别过滤

    QVERIFY(after.size() > base); // 文件确实增长（写入了内容）
}

void TestLogManager::test_rotation()
{
    auto &lm = LogManager::instance();
    lm.init();
    lm.set_min_level(LogManager::Level::Debug);

    // 设置小的轮转阈值，触发文件轮转
    lm.set_max_bytes(256);
    lm.set_max_backups(2);

    for (int i = 0; i < 20; ++i)
        lm.write(LogManager::Level::Info, "rotation",
                 QStringLiteral("this is a log line to fill the file and trigger rotation %1").arg(i));

    // 超限后应生成 .1 备份
    QFile backup(lm.log_file_path() + ".1");
    QVERIFY(backup.exists());

    // 恢复默认阈值
    lm.set_max_bytes(5 * 1024 * 1024);
    lm.set_max_backups(3);
}

QObject *createTestLogManager() { return new TestLogManager; }

#include "test_log_manager.moc"