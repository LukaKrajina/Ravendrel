#pragma once

#include <QString>
#include <QStringList>
#include <QHash>

namespace rvdlrsp::gui
{

    // 轻量级多语言支持（English / 中文 / Русский / Français / Deutsch）。
    //
    // 运行时使用内置字典（RVDLTR 宏即时查表），同时可在 i18n/ 目录维护
    // .ts 标准翻译源文件供 Qt Linguist 编辑。
    //
    // 用法：把界面文案包一层 RVDLTR(...)，例如：
    //     file->addAction(RVDLTR("退出(&Q)"), ...);
    //     details_tabs_->addTab(teleop_, RVDLTR("遥操作"));
    class I18n
    {
    public:
        static I18n &instance();

        // 支持的语言代码列表（en/zh/ru/fr/de）
        static const QStringList &languages();
        // 语言的原生显示名（用于菜单）
        static QString languageName(const QString &code);

        QString current() const;                 // 当前语言代码
        void setLanguage(const QString &code);   // 切换语言并持久化到 QSettings

        // 翻译：查当前语言字典，未命中返回原文（source）
        QString tr(const QString &source) const;

    private:
        I18n();
        void add(const char *zh, const char *en, const char *ru, const char *fr, const char *de);
        void load_saved();

        QHash<QString, QHash<QString, QString>> dict_; // 中文原文 -> {lang -> 译文}
        QString current_;
    };
}

// 便捷宏：RVDLTR("文件") → 当前语言文案
#define RVDLTR(src) (rvdlrsp::gui::I18n::instance().tr(QStringLiteral(src)))