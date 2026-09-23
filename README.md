# openpak-client

The OpenPak client library and Qt dialogs shared by the OpenPak emulator builds: sign-in,
friends and requests, invitations, presence, recent players, play history, cloud saves, mods,
status. Extracted from the Citron build's integration (GPL-2.0-or-later, so this library is
too) so Eden, Azahar, Dolphin, melonDS and Cemu get the same experience without seven copies.
PRD: `Openpak/emulators/prds/emulator-integration-prd.md`.

- `client/` — `openpak::client`: the HTTP client (`WebService::OpenPakApi`), the account file
  (`Common::OpenPakAccount`, including bearer-only accounts for families without a console
  identity), the friend cache and outgoing-request bookkeeping, the network profile
  (`openpak::NetworkProfile` — fetch/validate/cache per `emulators/prds/emulator-network-profile-prd.md`),
  and `openpak::ZipStore`, a store-only zip archiver for directory-shaped cloud saves. Needs a
  `Platform` set-up call for its directories and an optional log sink. libcurl-free: cpp-httplib
  over OpenSSL, nlohmann/json, fmt. The saves API path segment and device name are the host's
  choice (`SetSavesPlatform`/`SetSaveDevice`: `wii` and `ds` beside `citron`).
- `qt/` — `openpak::qt`: everything `emulators/prds/openpak-ux-spec.md` has every Qt host show
  the same way. The OpenPak window (Account, Friends, Invitations, Cloud saves, Mods, News,
  Status), the OpenPak menu (`host_kit.h`), the sign-in dialog, the shared prompts
  (`prompts.h`: setup and connect, profile picker, sign-out, invitation, save conflict), the
  friend picker, the settings section (`settings_section.h`) and the toasts; also the avatar
  cache, online counts, save sync, network probe and chat client. A host implements one
  interface, `openpak::qt::Host` (`qt/openpak/qt/host.h`): game names and icons, launching,
  settings, the save directory of a title, gamepad navigation.
- `strings/` — the words (UX spec §7): `en.json` is the English table, `{lang}.json` the
  translations. `tools/strings/generate.py` turns them into each toolkit's files: `qt` writes
  `qt/i18n/openpak_{lang}.ts`, which the library compiles in (with Qt's LinguistTools) and a host
  loads with `openpak::qt::LoadTranslations(language)`; `ryujinx`, `android` and `po` write
  Ryujinx's locale JSON, Android's `strings.xml` and wx's `.po`; `seed` fills translations from
  the phone app's ARB files, and `check` lists any `Tr("...")` in the library that is not in the
  table.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # -DOPENPAK_BUILD_QT=OFF for the client only
cmake --build build
```

Consumers vendor it as a submodule and `add_subdirectory`, then link `openpak::qt` (or
`openpak::client`). Namespaces still carry their Citron-era names; they will be renamed once
the second consumer is on the library.
