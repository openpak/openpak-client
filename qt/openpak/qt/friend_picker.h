// SPDX-License-Identifier: GPL-2.0-or-later
// The friend picker MyPage's "invite friends" opens (openpak::my_page::SetFriendPicker), as
// Ryujinx shows it: the account's friends, online ones first, up to the number the game allows.
// Mouse and keyboard work as in any list; a controller works through the host's Navigation:
// up/down move through the friends, A picks or un-picks one, down past the last friend (or
// right) reaches Send invite and Cancel, A presses the button, B cancels.
#pragma once
#include <cstdint>
#include <vector>

class QWidget;

namespace openpak::qt {
class Host;

// Shows the picker and returns the friends picked, nothing when cancelled. UI thread only.
std::vector<std::uint64_t> PickFriends(Host* host, QWidget* parent, int max);

// Installs the picker as MyPage's. The applet asks from its worker thread; the dialog is shown
// on `parent`'s thread while the worker waits for the answer. `host` may be null (no gamepad,
// no guest-input suspension); Host::Current() is used when it is.
void InstallFriendPicker(Host* host, QWidget* parent);
} // namespace openpak::qt
