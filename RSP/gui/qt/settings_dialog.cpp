#include "settings_dialog.h"
#include "theme_manager.h"
#include "log_manager.h"
#include "i18n/i18n.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>

namespace rvdlrsp::gui
{
    SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
    {
        setWindowTitle(RVDLTR("设置"));
        build_ui();
        load_current();
    }

    void SettingsDialog::build_ui()
    {
        auto *lay = new QVBoxLayout(this);

        auto *form = new QFormLayout();

        theme_combo_ = new QComboBox(this);
        theme_combo_->addItem(RVDLTR("夜间模式"), static_cast<int>(ThemeManager::Theme::Dark));
        theme_combo_->addItem(RVDLTR("白天模式"), static_cast<int>(ThemeManager::Theme::Light));
        form->addRow(RVDLTR("主题"), theme_combo_);

        lang_combo_ = new QComboBox(this);
        for (const QString &code : I18n::languages())
            lang_combo_->addItem(I18n::languageName(code), code);
        form->addRow(RVDLTR("语言"), lang_combo_);

        log_combo_ = new QComboBox(this);
        log_combo_->addItem(QStringLiteral("Debug"), static_cast<int>(LogManager::Level::Debug));
        log_combo_->addItem(QStringLiteral("Info"), static_cast<int>(LogManager::Level::Info));
        log_combo_->addItem(QStringLiteral("Warning"), static_cast<int>(LogManager::Level::Warning));
        log_combo_->addItem(QStringLiteral("Error"), static_cast<int>(LogManager::Level::Error));
        log_combo_->addItem(QStringLiteral("Fatal"), static_cast<int>(LogManager::Level::Fatal));
        form->addRow(RVDLTR("日志级别"), log_combo_);

        lay->addLayout(form);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::apply);
        connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
        lay->addWidget(buttons);
    }

    void SettingsDialog::load_current()
    {
        const int ti = theme_combo_->findData(static_cast<int>(ThemeManager::instance().current()));
        if (ti >= 0)
            theme_combo_->setCurrentIndex(ti);

        const int li = lang_combo_->findData(I18n::instance().current());
        if (li >= 0)
            lang_combo_->setCurrentIndex(li);

        const int gi = log_combo_->findData(static_cast<int>(LogManager::instance().min_level()));
        if (gi >= 0)
            log_combo_->setCurrentIndex(gi);
    }

    void SettingsDialog::apply()
    {
        // 主题：切换并应用全局 QSS（同时触发 themeChanged，编辑器自动跟随）
        ThemeManager::instance().setTheme(
            static_cast<ThemeManager::Theme>(theme_combo_->currentData().toInt()));
        ThemeManager::instance().apply(*qApp);

        // 语言：切换并持久化（主窗口收到 settingsChanged 后 retranslate）
        I18n::instance().setLanguage(lang_combo_->currentData().toString());

        // 日志级别
        LogManager::instance().set_min_level(
            static_cast<LogManager::Level>(log_combo_->currentData().toInt()));

        emit settingsChanged();
    }
}