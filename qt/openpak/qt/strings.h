// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// OpenPak's words, one table for every emulator (UX spec §7): strings/en.json is the English,
// strings/{lang}.json the translations, and tools/strings/generate.py turns them into what each
// toolkit reads. For Qt that is qt/i18n/openpak_{lang}.ts, compiled into the library and looked up
// in the "OpenPak" context by the English text itself.

#pragma once

#include <QString>

namespace openpak::qt {

// The table's English (with %1, %2 where the table has {0}, {1}), in the language loaded below.
// Only a literal from strings/en.json belongs here: tools/strings/generate.py check says which
// are not.
QString Tr(const char* english);

// Loads the OpenPak translation for a language ("de", "pt_BR"; empty for the system's) beside
// the host's own translator, replacing the one loaded before. English needs none. False when the
// library was built without translations or has none for that language.
bool LoadTranslations(const QString& language = {});

} // namespace openpak::qt
