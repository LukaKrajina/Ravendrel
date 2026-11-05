#include <QtTest/QtTest>
#include <QSettings>

#include "theme_manager.h"

using namespace rvdlrsp::gui;

class TestThemeManager : public QObject
{
    Q_OBJECT
private slots:
    void test_set_theme();
    void test_toggle();
    void test_theme_changed_signal();
    void test_viewport_theme();
    void test_toggle_text();
    void test_persistence();
};

void TestThemeManager::test_set_theme()
{
    auto &tm = ThemeManager::instance();

    tm.setTheme(ThemeManager::Theme::Dark);
    QVERIFY(tm.isDark());
    QVERIFY(tm.current() == ThemeManager::Theme::Dark);

    tm.setTheme(ThemeManager::Theme::Light);
    QVERIFY(!tm.isDark());
    QVERIFY(tm.current() == ThemeManager::Theme::Light);
}

void TestThemeManager::test_toggle()
{
    auto &tm = ThemeManager::instance();

    tm.setTheme(ThemeManager::Theme::Dark);
    tm.toggle();
    QVERIFY(!tm.isDark()); // Dark → Light
    tm.toggle();
    QVERIFY(tm.isDark());  // Light → Dark
}

void TestThemeManager::test_theme_changed_signal()
{
    auto &tm = ThemeManager::instance();
    tm.setTheme(ThemeManager::Theme::Dark); // 稳定到 Dark

    QSignalSpy spy(&tm, &ThemeManager::themeChanged);
    tm.setTheme(ThemeManager::Theme::Light);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), false); // Light → dark=false

    // 再次设置相同主题，不应重复发射
    tm.setTheme(ThemeManager::Theme::Light);
    QCOMPARE(spy.count(), 1);
}

void TestThemeManager::test_viewport_theme()
{
    const auto dark = ThemeManager::viewportTheme(true);
    const auto light = ThemeManager::viewportTheme(false);

    // 白天应比夜间更亮（更强的太阳与光照），背景色更浅
    QVERIFY(light.sun_intensity > dark.sun_intensity);
    QVERIFY(light.light_intensity > dark.light_intensity);
    QVERIFY(light.clear_r > dark.clear_r);
    QVERIFY(light.clear_g > dark.clear_g);
    QVERIFY(light.clear_b > dark.clear_b);
}

void TestThemeManager::test_toggle_text()
{
    const QString dark_text = ThemeManager::toggleText(ThemeManager::Theme::Dark);
    const QString light_text = ThemeManager::toggleText(ThemeManager::Theme::Light);
    QVERIFY(dark_text.contains("夜间"));
    QVERIFY(light_text.contains("白天"));
}

void TestThemeManager::test_persistence()
{
    auto &tm = ThemeManager::instance();

    tm.setTheme(ThemeManager::Theme::Light);
    QCOMPARE(QSettings().value("theme").toString(), QString("light"));

    tm.setTheme(ThemeManager::Theme::Dark);
    QCOMPARE(QSettings().value("theme").toString(), QString("dark"));
}

QObject *createTestThemeManager() { return new TestThemeManager; }

#include "test_theme_manager.moc"