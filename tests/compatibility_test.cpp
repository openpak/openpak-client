// SPDX-License-Identifier: GPL-2.0-or-later
// The shipped list answers offline; what the site says overrides it, and only a real difference
// counts as the list being out of date.
#undef NDEBUG // the checks are the point of this binary, in every build type
#include <cassert>
#include "openpak/compatibility.h"

int main() {
    namespace C = openpak::compatibility;
    constexpr std::uint64_t arms = 0x01009B500007C000ULL;

    const auto shipped = C::Find(arms);
    assert(shipped && shipped->name == "ARMS" && shipped->backend == "NEX");
    assert(!C::Find(0x0100000000000001ULL));

    assert(!C::Apply({}));                                   // a silent site changes nothing
    assert(!C::Apply({{"01009B500007C000", "live"}}));       // agreeing is not news
    assert(C::Apply({{"01009B500007C000", "beta"}}));        // a demotion is
    assert(C::Find(arms)->status == C::Status::Beta && C::Find(arms)->backend == "NEX");

    assert(C::Apply({{"0100000000000001", "Alpha"}, {"nonsense", "live"}, {"0100000000000002", "planned"}}));
    const auto only_site = C::Find(0x0100000000000001ULL);
    assert(only_site && only_site->status == C::Status::Alpha && only_site->backend.empty());
    assert(!C::Find(0x0100000000000002ULL)); // not a playable status
    assert(C::Find(arms)->status == C::Status::Live); // the site no longer says beta: shipped again
    return 0;
}
