# openpak-client

The OpenPak client library and Qt dialogs shared by the OpenPak emulator builds: sign-in,
friends and requests, invitations, presence, recent players, play history, cloud saves, mods,
status, and the network profile with its signed ceiling. GPL-2.0-or-later.

## Consumers

Each consumer vendors the library as a git submodule and `add_subdirectory`s it.

| Emulator | Family | Submodule | Links |
| --- | --- | --- | --- |
| Azahar | 3DS | `externals/openpak-client` | `openpak::qt`; Android: `openpak::client` |
| melonDS | DS | `Externals/openpak-client` | `openpak::qt` |
| Marina (Dolphin fork) | Wii | `Externals/openpak-client` | `openpak::qt`; Android: `openpak::client` |
| Quay (Cemu fork) | Wii U | `dependencies/openpak-client` | `openpak::client` only: its UI is wxWidgets |

Harbor, the Switch emulator, deliberately has its own implementation: its OpenPak services run
inside the emulated console, and its UI is Borealis, not Qt.

The library began as the OpenPak integration of an earlier Switch emulator build; the namespaces
(`WebService::OpenPakApi`, `Common::OpenPakAccount`) still carry that build's layout.

## Parts

- `client/` — `openpak::client`: the HTTP client (`WebService::OpenPakApi`), the account file
  (`Common::OpenPakAccount`, including bearer-only accounts for families whose console identity
  the host keeps itself), the friend cache and outgoing-request bookkeeping, the network profile
  (`openpak::NetworkProfile`: fetch, validate, cache, and filter through the signed ceiling),
  and `openpak::ZipStore`, a store-only zip archiver for directory-shaped cloud saves. Needs a
  `Platform` set-up call for its directories and an optional log sink. libcurl-free: cpp-httplib
  over OpenSSL, nlohmann/json, fmt. The saves API path segment and device name are the host's
  choice (`SetSavesPlatform`/`SetSaveDevice`). The failures a host words itself are named in
  `WebService::OpenPakApi::Errors`.
- `qt/` — `openpak::qt`: the surfaces every Qt host shows the same way. The OpenPak window
  (Account, Friends, Invitations, Cloud saves, Mods, News, Status), the OpenPak menu
  (`host_kit.h`), the sign-in dialog, the shared prompts (`prompts.h`: setup and connect, profile
  picker, sign-out, invitation, save conflict), the friend picker, the settings section
  (`settings_section.h`) and the toasts; also the avatar cache, online counts, save sync, network
  probe and chat client. A host implements one interface, `openpak::qt::Host`
  (`qt/openpak/qt/host.h`): game names and icons, launching, settings, the save directory of a
  title, gamepad navigation.
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
