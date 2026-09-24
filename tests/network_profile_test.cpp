// SPDX-License-Identifier: GPL-2.0-or-later
// The network profile as a DNS lookup uses it: the ceiling on a label boundary, a profile's shape
// validated (its names are the ceiling's business, see network_ceiling_test), and
// never > overrides > exact > suffixes.
#undef NDEBUG // the checks are the point of this binary, in every build type
#include <cassert>
#include <string>
#include "openpak/network_profile.h"

int main() {
    namespace np = openpak::NetworkProfile;

    auto const compiled = np::CompiledCeiling();
    auto const& families = compiled.FamiliesFor("switch");
    assert(np::NameInFamilies(".nintendo.net", families));
    assert(np::NameInFamilies("nintendo.net", families));
    assert(np::NameInFamilies("gamesync.npln.nintendo.net", families));
    assert(np::NameInFamilies("Matchmaker.Among.Us", families));
    assert(!np::NameInFamilies("evilnintendo.net", families));
    assert(!np::NameInFamilies("a.nintendo.net.evil.example", families));

    // Nothing applied yet: nothing is ours to answer.
    assert(!np::Loaded());
    assert(!np::RedirectFor("a.nintendo.net", "203.0.113.1"));

    std::string error;
    auto profile = np::Parse(R"({"version":3,"platform":"switch",
        "server":{"address":"198.51.100.7"},
        "redirect":{"suffixes":[".nintendo.net"],"exact":["matchmaker.among.us"],
                    "never":["ctest.cdn.nintendo.net"],
                    "overrides":{"nncs2-lp1.n.n.srv.nintendo.net":"198.51.100.8"}}})",
                             error);
    assert(profile);
    assert(!np::Validate(*profile, "switch"));
    assert(np::Validate(*profile, "wiiu"));

    // A name outside the ceiling is not a shape problem: it is dropped later, on its own.
    auto outside = *profile;
    outside.exact.push_back("example.com");
    assert(!np::Validate(outside, "switch"));
    auto loopback = *profile;
    loopback.server_address = "127.0.0.1";
    assert(np::Validate(loopback, "switch"));
    auto bad_override = *profile;
    bad_override.overrides.emplace_back("x.nintendo.net", "::1");
    assert(np::Validate(bad_override, "switch"));

    np::Apply(np::Result{true, np::Source::Cached, *profile, {}});
    assert(np::Loaded());
    assert(np::RedirectFor("baas.nintendo.net", "203.0.113.1") == "198.51.100.7");
    assert(np::RedirectFor("BAAS.Nintendo.NET", "203.0.113.1") == "198.51.100.7");
    assert(np::RedirectFor("matchmaker.among.us", "203.0.113.1") == "198.51.100.7");
    assert(np::RedirectFor("nncs2-lp1.n.n.srv.nintendo.net", "") == "198.51.100.8");
    assert(!np::RedirectFor("ctest.cdn.nintendo.net", "203.0.113.1"));
    assert(!np::RedirectFor("example.com", "203.0.113.1"));

    // The built-in list names no server: the host's own address is what it resolves to.
    np::Apply(np::Result{true, np::Source::BuiltIn, np::BuiltIn("switch"), {}});
    assert(!np::Loaded());
    assert(np::RedirectFor("accounts.nintendo.com", "203.0.113.1") == "203.0.113.1");
    assert(!np::RedirectFor("example.com", "203.0.113.1"));
    return 0;
}
