// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/qt/strings.h"

#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>

namespace openpak::qt {

QString Tr(const char* english) {
    return QCoreApplication::translate("OpenPak", english);
}

bool LoadTranslations(const QString& language) {
    // One for the process, swapped when the host's language changes.
    static QTranslator* installed = nullptr;
    if (installed) {
        QCoreApplication::removeTranslator(installed);
        delete installed;
        installed = nullptr;
    }
    auto* translator = new QTranslator;
    const QString dir = QStringLiteral(":/openpak/i18n");
    const bool loaded =
        language.isEmpty()
            ? translator->load(QLocale(), QStringLiteral("openpak"), QStringLiteral("_"), dir)
            : translator->load(QStringLiteral("openpak_") + language, dir);
    if (!loaded || translator->isEmpty()) {
        delete translator;
        return false;
    }
    QCoreApplication::installTranslator(translator);
    installed = translator;
    return true;
}

} // namespace openpak::qt
