#pragma once

#include <QString>
#include <QStringList>
#include <QHash>

namespace rvdlrsp::gui
{

    class I18n
    {
    public:
        static I18n &instance();

        static const QStringList &languages();
        static QString languageName(const QString &code);

        QString current() const;
        void setLanguage(const QString &code);

        QString tr(const QString &source) const;

    private:
        I18n();
        void add(const char *zh, const char *en, const char *ru, const char *fr, const char *de);
        void load_saved();

        QHash<QString, QHash<QString, QString>> dict_;
        QString current_;
    };
}

#define RVDLTR(src) (rvdlrsp::gui::I18n::instance().tr(QStringLiteral(src)))