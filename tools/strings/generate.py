#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 OpenPak
# SPDX-License-Identifier: GPL-2.0-or-later
"""OpenPak's words for every emulator, from one table (UX spec section 7).

strings/en.json is the English: a flat map of key to text, placeholders {0}, {1}.
strings/{lang}.json holds the translations under the same keys. This turns them into what each
toolkit reads:

  qt [DIR]          qt/i18n/openpak_{lang}.ts, context "OpenPak", {0} as %1 (the library embeds them)
  ryujinx FILE      assets/Locales/OpenPak.json, IDs OpenPak_<PascalCaseKey>, all 20 slots
  android RES_DIR   values*/openpak_strings.xml, names openpak_<snake_key>, {0} as %1$s
  po DIR            openpak.pot and openpak_{lang}.po for wx _(), {0} as %1$s

With --check an emitter writes nothing and fails when what is on disk differs, for a fork's CI
to prove its copy matches the library it pins.

  seed ARB_DIR      fills strings/{lang}.json from the phone app's ARB files (app/lib/l10n) where
                    the app's English is the table's, without overwriting a translation
  check [FILE...]   every Tr("...") literal in the library's Qt sources (or the files named) must
                    be the English of some key; lists those that are not
"""

import argparse
import ast
import json
import pathlib
import re
import sys
from xml.sax.saxutils import escape

ROOT = pathlib.Path(__file__).resolve().parents[2]
STRINGS = ROOT / "strings"

# Ryujinx's language slots, and which of the table's languages fills each.
RYUJINX_SLOTS = ["ar_SA", "de_DE", "el_GR", "en_US", "es_ES", "fr_FR", "he_IL", "it_IT", "ja_JP",
                 "ko_KR", "no_NO", "pl_PL", "pt_BR", "ru_RU", "sv_SE", "th_TH", "tr_TR", "uk_UA",
                 "zh_CN", "zh_TW"]


def load(lang):
    path = STRINGS / f"{lang}.json"
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def languages():
    return sorted(p.stem for p in STRINGS.glob("*.json") if p.stem != "en")


def translation(lang, key, cache={}):
    """The key in lang, falling back from a region to its language (pt_BR -> pt)."""
    for candidate in (lang, lang.split("_")[0]):
        if candidate not in cache:
            cache[candidate] = load(candidate) if candidate != "en" else {}
        text = cache[candidate].get(key, "")
        if text:
            return text
    return ""


def positional(text, pattern):
    """{0} -> pattern % 1 (Qt %1, printf %1$s)."""
    return re.sub(r"\{(\d+)\}", lambda m: pattern.format(int(m.group(1)) + 1), text)


def qt_text(text):
    return positional(text, "%{}")


def printf_text(text):
    # A literal % in a format string is %%; the placeholders become %1$s after that.
    if re.search(r"\{\d+\}", text):
        text = text.replace("%", "%%")
    return positional(text, "%{}$s")


def emit(path, content, check):
    path = pathlib.Path(path)
    if check:
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            print(f"{path} differs from the table; run tools/strings/generate.py", file=sys.stderr)
            return False
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def cmd_qt(args):
    english = load("en")
    out = pathlib.Path(args.dir or ROOT / "qt" / "i18n")
    ok = True
    for lang in languages():
        # One message per English text: the same words under two keys are one entry to Qt.
        seen = {}
        for key, text in english.items():
            source = qt_text(text)
            if source not in seen or not seen[source]:
                seen[source] = qt_text(translation(lang, key))
        lines = ['<?xml version="1.0" encoding="utf-8"?>', "<!DOCTYPE TS>",
                 f'<TS version="2.1" language="{lang}" sourcelanguage="en">', "<context>",
                 "    <name>OpenPak</name>"]
        for source, target in seen.items():
            lines.append("    <message>")
            lines.append(f"        <source>{escape(source)}</source>")
            if target:
                lines.append(f"        <translation>{escape(target)}</translation>")
            else:
                lines.append('        <translation type="unfinished"></translation>')
            lines.append("    </message>")
        lines += ["</context>", "</TS>", ""]
        ok &= emit(out / f"openpak_{lang}.ts", "\n".join(lines), args.check)
    return ok


def pascal(key):
    return "".join(part[:1].upper() + part[1:] for part in re.split(r"[._]", key))


def cmd_ryujinx(args):
    english = load("en")
    locales = []
    for key, text in english.items():
        slots = {}
        for slot in RYUJINX_SLOTS:
            # de_DE reads strings/de_DE.json, else strings/de.json; empty falls back to English.
            slots[slot] = text if slot == "en_US" else translation(slot, key)
        locales.append({"ID": "OpenPak_" + pascal(key), "Translations": slots})
    content = json.dumps({"Locales": locales}, indent=2, ensure_ascii=False) + "\n"
    return emit(args.file, content, args.check)


def android_escape(text):
    text = escape(text)
    text = text.replace("\\", "\\\\").replace("'", "\\'").replace('"', '\\"')
    if text.startswith(("@", "?")):
        text = "\\" + text
    return text.replace("\n", "\\n")


def android_dir(lang):
    if "_" in lang:
        language, region = lang.split("_", 1)
        return f"values-{language}-r{region}"
    return f"values-{lang}"


def cmd_android(args):
    english = load("en")
    res = pathlib.Path(args.res_dir)
    ok = True
    for lang in ["en"] + languages():
        lines = ['<?xml version="1.0" encoding="utf-8"?>',
                 "<!-- Generated by openpak-client/tools/strings/generate.py; edit strings/*.json. -->",
                 "<resources>"]
        for key, text in english.items():
            value = text if lang == "en" else load(lang).get(key, "")
            if not value:
                continue
            name = "openpak_" + re.sub(r"[.]", "_", key)
            formatted = "" if re.search(r"\{\d+\}", value) else ' formatted="false"' if "%" in value else ""
            lines.append(f'    <string name="{name}"{formatted}>{android_escape(printf_text(value))}</string>')
        lines += ["</resources>", ""]
        folder = "values" if lang == "en" else android_dir(lang)
        ok &= emit(res / folder / "openpak_strings.xml", "\n".join(lines), args.check)
    return ok


def po_quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def cmd_po(args):
    english = load("en")
    out = pathlib.Path(args.dir)
    ok = True
    for lang in [None] + languages():
        lines = ['msgid ""', 'msgstr ""', '"Content-Type: text/plain; charset=UTF-8\\n"']
        if lang:
            lines.append(f'"Language: {lang}\\n"')
        lines.append("")
        seen = set()
        for key, text in english.items():
            source = printf_text(text)
            if source in seen:
                continue
            seen.add(source)
            target = printf_text(translation(lang, key)) if lang else ""
            lines += [f"#. {key}", f"msgid {po_quote(source)}", f"msgstr {po_quote(target)}", ""]
        name = "openpak.pot" if lang is None else f"openpak_{lang}.po"
        ok &= emit(out / name, "\n".join(lines), args.check)
    return ok


def normal(text):
    return re.sub(r"[\s.…]+$", "", text.strip()).casefold()


def cmd_seed(args):
    arb_dir = pathlib.Path(args.arb_dir)
    english = load("en")
    app_en = json.loads((arb_dir / "app_en.arb").read_text(encoding="utf-8"))
    by_text = {}
    for arb_key, text in app_en.items():
        if arb_key.startswith("@") or not isinstance(text, str) or "{" in text:
            continue
        by_text.setdefault(normal(text), arb_key)
    for arb in sorted(arb_dir.glob("app_*.arb")):
        lang = arb.stem[len("app_"):]
        if lang == "en":
            continue
        app = json.loads(arb.read_text(encoding="utf-8"))
        table = load(lang)
        added = 0
        for key, text in english.items():
            if table.get(key) or "{" in text:
                continue
            arb_key = by_text.get(normal(text))
            value = app.get(arb_key) if arb_key else None
            if not value:
                continue
            # The app's punctuation, the table's shape: a trailing full stop or ellipsis follows
            # the table.
            core = re.sub(r"[\s.…]+$", "", value)
            tail = text[len(re.sub(r"[\s.…]+$", "", text)):]
            table[key] = core + tail
            added += 1
        ordered = {key: table[key] for key in english if key in table}
        ordered.update({key: value for key, value in table.items() if key not in english})
        (STRINGS / f"{lang}.json").write_text(
            json.dumps(ordered, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"{lang}: {added} seeded, {len(ordered)} of {len(english)} translated")
    return True


LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def tr_literals(source):
    """The English of every Tr("..." "...") call: adjacent literals joined, as the compiler does."""
    for match in re.finditer(r"\bTr\(\s*", source):
        at = match.end()
        parts = []
        while True:
            literal = LITERAL.match(source, at)
            if not literal:
                break
            parts.append(ast.literal_eval(literal.group(0)))
            at = literal.end()
            while at < len(source) and source[at] in " \t\r\n":
                at += 1
        if parts:
            yield "".join(parts), source.count("\n", 0, match.start()) + 1


def cmd_check(args):
    known = {qt_text(text) for text in load("en").values()}
    files = [pathlib.Path(f) for f in args.files] or sorted((ROOT / "qt").rglob("*.cpp"))
    missing = 0
    for path in files:
        for english, line in tr_literals(path.read_text(encoding="utf-8")):
            if english not in known:
                print(f"{path}:{line}: not in strings/en.json: {english!r}")
                missing += 1
    return missing == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    qt = sub.add_parser("qt")
    qt.add_argument("dir", nargs="?")
    ryujinx = sub.add_parser("ryujinx")
    ryujinx.add_argument("file")
    android = sub.add_parser("android")
    android.add_argument("res_dir")
    po = sub.add_parser("po")
    po.add_argument("dir")
    for emitter in (qt, ryujinx, android, po):
        emitter.add_argument("--check", action="store_true")
    seed = sub.add_parser("seed")
    seed.add_argument("arb_dir")
    check = sub.add_parser("check")
    check.add_argument("files", nargs="*")
    args = parser.parse_args()
    ok = {"qt": cmd_qt, "ryujinx": cmd_ryujinx, "android": cmd_android, "po": cmd_po,
          "seed": cmd_seed, "check": cmd_check}[args.command](args)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
