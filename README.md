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
- `qt/` — `openpak::qt`: the account dialog (home, friends, players, history, cloud saves),
  delegates, avatar cache, toasts, online counts, save sync, network probe, chat client. A host
  implements one interface, `openpak::qt::Host` (`qt/openpak/qt/host.h`): game names and
  icons, launching, settings, the save directory of a title, gamepad navigation.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # -DOPENPAK_BUILD_QT=OFF for the client only
cmake --build build
```

Consumers vendor it as a submodule and `add_subdirectory`, then link `openpak::qt` (or
`openpak::client`). Namespaces still carry their Citron-era names; they will be renamed once
the second consumer is on the library.
