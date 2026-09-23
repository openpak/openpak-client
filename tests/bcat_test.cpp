// SPDX-License-Identifier: GPL-2.0-or-later
// Which BCAT manifest paths fit the delivery cache, and the validity window (openpak/bcat.h).
// Ported from Ryujinx's OpenPakBcatTests.cs.
#undef NDEBUG
#include <cassert>
#include <string>

#include "openpak/bcat.h"

namespace bcat = openpak::bcat;

int main() {
    std::string dir, name;
    assert(bcat::Split("event/season.bin", dir, name) && dir == "event" && name == "season.bin");
    assert(bcat::Split("data/event_2026-09.byml", dir, name));
    for (const char* bad : {"season.bin", "a/b/c.bin", "../x.bin", "event/",
                            "event/a_name_that_is_far_too_long_for_bcat.bin", "ev ent/x.bin"}) {
        assert(!bcat::Split(bad, dir, name));
    }

    const std::time_t now = 1790121600; // 2026-09-23T00:00:00Z
    assert(bcat::InWindow(R"({"valid_from":"2026-09-01T00:00:00Z","valid_until":null})", now));
    assert(!bcat::InWindow(R"({"valid_from":"2026-10-01T00:00:00Z"})", now));
    assert(!bcat::InWindow(R"({"valid_from":"2026-09-01T00:00:00Z","valid_until":"2026-09-20T00:00:00Z"})", now));
    assert(!bcat::InWindow("not json", now));
    return 0;
}
