# Changelog — openpak-client

> **Current Switch client (2026-10-13): Harbor**, at
> `~/REPOS/Openpak/emulators/harbor`. Harbor is an independent
> OpenPak project based on Eden. Citron, Ryujinx (desktop and Android), and
> the former OpenPak Eden checkouts have been removed. References to those
> forks below record historical evidence only: do not run their commands,
> reinstall them, or select them for current work. Use Harbor's README and
> current handoff for builds and testing; old results do not establish Harbor parity.

Generated from git history on 2026-09-15. `git log` stays the source
of truth; this file is the readable summary.

## Unreleased

- Playability: what the catalogue says about how far *this* emulator runs a title, beside the
  OpenPak online status. `compatibility::FindPlayability(title_id)` answers for the emulator
  `Platform::SetClient` named and no other; nullopt is untested. Shipped in
  `client/openpak/playability_list.inc`, overridden by `Refresh()`
- X-OpenPak-Client names the OS: `<name>/<version> (<os>)`, one of windows, macos, linux,
  android, ios, chosen at compile time; no suffix on any other platform
- `WebService::OpenPakApi::Errors`: the sentences for not signed in, unreachable, expired,
  wrong credentials and rate limited, so a host with its own strings tells them apart
- `PingUrl(url)`: one timed GET to any http(s) URL (a console service the host addresses)
- The ceiling filters a profile's `services[]` by URL host, and the effective digest counts them
- `GetAccountFriends()` reads only the core's lists for a bearer-only account; the Switch adapter
  is asked only by a host signed in with a Switch identity

## v0.4.1 — 2026-09-25

- Network ceiling: signed, fetched and cached; names outside it dropped one by one [e180a57]
- Catalogue: register every regional title id, not only the first [00f3129]
- Build fixes: Windows `min`/`max` macro guards; `nat_check` timeval types on macOS

## v0.4.0 — 2026-09-23

Branch `release-prep`, merged:

- qt: the OpenPak window, shared dialogs, settings section and toasts as the UX spec has them;
  what the Switch hosts need from the shared dialogs
- strings: one English table for every emulator
- qt: 3DS and Wii U family rows and panels; a host may bring its own TLS library
- client: on Android without a root blob, the phone's own public roots

## v0.3.0 — 2026-09-23

- crash_reports: saved on a crash, offered at the next launch, sent only with consent

## v0.2.0 — 2026-09-23

- qt: what the Wii and DS hosts need from the UX spec

## v0.1.0 — 2026-09-23

Ported from OpenPak's earlier Ryujinx port so every host shares it:

- Sign-in chain bound to the running title; one OpenPak account per local profile; sign-in
  dialog; the account window's seven pages and its website calls; token revoked on sign-out
- Friends: BAAS and Five REST layer, friend structs and presence rules, block/unblock,
  MyPage friend picker and invitation types
- Session: presence from the title's declaration, friends caches on the heartbeat; a failing
  heartbeat is logged, never fatal
- Cloud saves as OpenPak's earlier Ryujinx port did them (pull before launch, push on exit), in the client half so
  Android syncs too; saves for every platform
- Network profile: one client for every host; `X-OpenPak-Client` on every request
- Penne push connection, BCAT datasets, NAT type test, catalogue statuses refreshed live
- Invitations, mods, news and status calls and pages
- Android: verify public TLS with the bundled CA store [e843f7a]
- api: sign-in sends the device name the host chose

Earlier history (also in v0.1.0):

- save restore: the swap that keeps a failed restore has a check behind it, and both save-restore checks run without a Qt build [09acc93]
- save restore: judge the archive before writing it, and swap it in whole [61bddfd]
- README: bearer-only accounts, network profile, ZipStore, saves platform setter [e75eb13]
- Actually define SourceName (third time is the definition) [b815e04]
- Define SourceName (declared in the header, never defined) [0f8747d]
- Fix ApplySystemCa linkage: the wrapper sat inside the anonymous namespace [4ecf8aa]
- Network profile: fetch, validate, cache, expose (EP-0) [bfde147]
- Saves for families without a console identity: platform setter, bearer-only sign-in, store-only zip [d10dc5d]
- Host: the ChatRawMessage signal the chat client relays [b24d41d]
- api: do not depend on httplib exposing the OpenSSL verify result (Eden's build does not) [0ea904d]
- CMake: libarchive optional (hosts without it keep the Windows save path only) [6cab840]
- CMake: reuse the host's fmt, json and httplib targets when embedded [50ced72]
- openpak-client: the OpenPak client and Qt dialogs, extracted from the Citron build [4443f62]

