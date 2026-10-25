# Protocol: openpak-client

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
