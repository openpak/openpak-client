# Notes: openpak-client

## Scope

The OpenPak client library and Qt dialogs shared by the OpenPak emulator builds: sign-in,
friends and requests, invitations, presence, recent players, play history, cloud saves, mods,
status, and the network profile with its signed ceiling. GPL-2.0-or-later.

## Design

Harbor, the Switch emulator, deliberately has its own implementation: its OpenPak services run
inside the emulated console, and its UI is Borealis, not Qt.

The library began as the OpenPak integration of an earlier Switch emulator build; the namespaces
(`WebService::OpenPakApi`, `Common::OpenPakAccount`) still carry that build's layout.
