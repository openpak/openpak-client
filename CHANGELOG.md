# Changelog — openpak-client

Generated from git history on 2026-09-15. `git log` stays the source
of truth; this file is the readable summary.

## History — no releases tagged yet

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

