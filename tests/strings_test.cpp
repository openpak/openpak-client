// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// The translations compiled into openpak_qt load by language and answer in it, and English needs
// none (UX spec §7).

#include <cstdio>

#include <QCoreApplication>

#include "openpak/qt/strings.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](const QString& got, const char* want) {
        if (got != QString::fromUtf8(want)) {
            std::fprintf(stderr, "expected '%s', got '%s'\n", want, got.toUtf8().constData());
            ++failures;
        }
    };

    expect(openpak::qt::Tr("Account"), "Account");
#ifdef OPENPAK_TEST_TRANSLATIONS
    if (!openpak::qt::LoadTranslations(QStringLiteral("de"))) {
        std::fprintf(stderr, "the German translation did not load\n");
        return 1;
    }
    expect(openpak::qt::Tr("Account"), "Konto");
    // A regional language falls back to its language's file.
    if (!openpak::qt::LoadTranslations(QStringLiteral("de_AT"))) {
        std::fprintf(stderr, "de_AT did not fall back to de\n");
        return 1;
    }
    expect(openpak::qt::Tr("Friends"), "Freunde");
    // An untranslated string stays English.
    expect(openpak::qt::Tr("Mods"), "Mods");
#endif
    return failures == 0 ? 0 : 1;
}
