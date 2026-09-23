// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

// OpenPak's words, one table for every emulator (UX spec §7): strings/en.json is the English,
// strings/{lang}.json the translations, and tools/strings/generate.py turns them into what each
// toolkit reads. For Qt that is qt/i18n/openpak_{lang}.ts, compiled into the library and looked up
// in the "OpenPak" context by the English text itself.

#pragma once

#include <string>

#include <QString>

namespace openpak::qt {

// The table's English (with %1, %2 where the table has {0}, {1}), in the language loaded below.
// Only a literal from strings/en.json belongs here: tools/strings/generate.py check says which
// are not.
QString Tr(const char* english);

// What the client library's English error means, in the table's words (UX spec §3.12): the known
// ones by their key, a sentence the server wrote behind "OpenPak said:", and anything else --
// exception text, HTTP codes -- to the log only, with a plain sentence in its place.
QString ErrorText(const std::string& error);

// Loads the OpenPak translation for a language ("de", "pt_BR"; empty for the system's) beside
// the host's own translator, replacing the one loaded before. English needs none. False when the
// library was built without translations or has none for that language.
bool LoadTranslations(const QString& language = {});

} // namespace openpak::qt
