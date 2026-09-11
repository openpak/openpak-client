// SPDX-License-Identifier: GPL-2.0-or-later
#include "openpak/qt/host.h"
namespace openpak::qt {
static Host* g_current = nullptr;
Host* Host::Current() { return g_current; }
void Host::SetCurrent(Host* host) { g_current = host; }
} // namespace openpak::qt
