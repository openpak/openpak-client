// SPDX-License-Identifier: GPL-2.0-or-later
// The shipped list answers offline; what the site says overrides it, and only a real difference
// counts as the list being out of date.
#undef NDEBUG // the checks are the point of this binary, in every build type
#include <cassert>
#include "openpak/compatibility.h"
#include "openpak/platform.h"

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

    // Names: the shipped one until the site says otherwise, nothing for a title nobody knows.
    assert(C::CatalogueName(arms) == "ARMS");
    assert(C::CatalogueName(0x0100000000000003ULL).empty());
    C::ApplyNames({});                                                     // a silent site keeps them
    assert(C::CatalogueName(arms) == "ARMS");
    C::ApplyNames({{"0100000000000003", "Somewhere"}, {"short", "Nope"}});
    assert(C::CatalogueName(0x0100000000000003ULL) == "Somewhere");
    assert(C::CatalogueName(arms) == "ARMS"); // not in the site's answer: the shipped name stands

    // Playability: how far this emulator runs the title, read from its own key and nothing else.
    // Synthetic ids throughout: the shipped list is regenerated from the site, so asserting about
    // a real title here would break the test the day someone enters a verdict for it.
    constexpr std::uint64_t one = 0x0100000000000004ULL;
    constexpr std::uint64_t two = 0x0100000000000005ULL;
    const std::string one_hex = "0100000000000004";
    using P = C::Playability;
    assert(C::ParsePlayability("playable") == P::Playable);
    assert(C::ParsePlayability("InGame") == P::InGame);  // the site's case is not the client's problem
    assert(C::ParsePlayability("menus") == P::Menus);
    assert(C::ParsePlayability("boots") == P::Boots);
    assert(C::ParsePlayability("nothing") == P::Nothing);
    assert(!C::ParsePlayability("perfect")); // a word this build does not know is untested
    assert(!C::ParsePlayability(""));
    assert(std::string{C::PlayabilityName(P::InGame)} == "ingame"); // the key the emulators translate

    assert(!C::FindPlayability(one)); // no host has said which emulator this is
    openpak::Platform::SetClient("citron", "test");
    assert(!C::FindPlayability(one)); // nothing shipped or heard for it: untested

    assert(!C::ApplyPlayability({}));                                   // a silent site
    assert(!C::ApplyPlayability({{one_hex, {{"Ryujinx", "playable"}}}})); // another emulator's key
    assert(!C::FindPlayability(one));
    assert(!C::ApplyPlayability({{one_hex, {{"Citron", "perfect"}}}}));   // a word we do not know
    assert(!C::FindPlayability(one));
    assert(!C::ApplyPlayability({{one_hex, {}}}));                        // "playability": {}
    assert(!C::FindPlayability(one));
    assert(!C::ApplyPlayability({{"nonsense", {{"Citron", "playable"}}}})); // not a title id

    // The site saying something this build did not is news, and it is what Find answers with.
    assert(C::ApplyPlayability({{one_hex, {{"citron", "menus"}}},
                                {"0100000000000005", {{"Citron", "playable"}}}}));
    assert(C::FindPlayability(one) == P::Menus); // whatever case the site keyed the emulator in
    assert(C::FindPlayability(two) == P::Playable);

    // An empty answer is a site that could not say: what is already known stands.
    assert(!C::ApplyPlayability({}));
    assert(C::FindPlayability(one) == P::Menus);
    // So does an answer that is only about other emulators.
    assert(!C::ApplyPlayability({{one_hex, {{"Eden", "boots"}}}}));
    assert(C::FindPlayability(one) == P::Menus);

    // A title the site did not mention falls through to the shipped list.
    // ponytail: that list is empty until the website publishes the playability field, so the
    // fall-through can only be asserted as "untested" today. Once tools/update-compatibility.py
    // writes real rows, assert one of them here and that the site can override it.
    assert(!C::FindPlayability(0x0100000000000006ULL));

    openpak::Platform::SetClient("", ""); // a host that names no emulator tests nothing
    assert(!C::FindPlayability(arms));
    return 0;
}
