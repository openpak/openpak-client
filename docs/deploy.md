# Deploy: openpak-client

## Consumers

Each consumer vendors the library as a git submodule and `add_subdirectory`s it.

| Emulator | Family | Submodule | Links |
| --- | --- | --- | --- |
| Azahar | 3DS | `externals/openpak-client` | `openpak::qt`; Android: `openpak::client` |
| melonDS | DS | `Externals/openpak-client` | `openpak::qt` |
| Marina (Dolphin fork) | Wii | `Externals/openpak-client` | `openpak::qt`; Android: `openpak::client` |
| Quay (Cemu fork) | Wii U | `dependencies/openpak-client` | `openpak::client` only: its UI is wxWidgets |

## Strings

- `strings/` — the words: `en.json` is the English table, `{lang}.json` the translations.
  `tools/strings/generate.py` turns them into each toolkit's files: `qt` writes
  `qt/i18n/openpak_{lang}.ts`, which the library compiles in (with Qt's LinguistTools) and a host
  loads with `openpak::qt::LoadTranslations(language)`; `android` and `po` write Android's
  `strings.xml` and wx's `.po`; `seed` fills translations from the phone app's ARB files, and
  `check` lists any `Tr("...")` in the library that is not in the table.

## Building

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # -DOPENPAK_BUILD_QT=OFF for the client only
cmake --build build
```

A host that already builds fmt, nlohmann/json, cpp-httplib or its own TLS library defines those
targets (`fmt::fmt`, `nlohmann_json::nlohmann_json`, `httplib::httplib`, `OpenSSL::SSL`,
`OpenSSL::Crypto`) before `add_subdirectory`, and the library uses them instead of fetching its
own. A host without Qt sets `OPENPAK_BUILD_QT=OFF` and links `openpak::client`.
