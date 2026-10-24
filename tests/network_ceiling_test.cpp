// SPDX-License-Identifier: GPL-2.0-or-later
// The signed redirect ceiling (docs/signed-ceiling.md) as every client must treat it: signatures
// over the payload bytes under a pinned keyid, the family rules, rollback refused, the cache
// re-verified on load, names outside the ceiling dropped one by one, and a digest that only this
// platform's redirects move. Signed here with a fixture key; the production key is only pinned.
#undef NDEBUG // the checks are the point of this binary, in every build type
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "openpak/network_profile.h"
#include "openpak/platform.h"

namespace np = openpak::NetworkProfile;

namespace {

// RFC 8032 test 1's secret key: a fixture, never a real one.
constexpr std::array<unsigned char, 32> kSeed = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};

EVP_PKEY* FixtureKey() {
    static EVP_PKEY* key =
        EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, kSeed.data(), kSeed.size());
    assert(key != nullptr);
    return key;
}

np::PinnedKey FixturePinned() {
    std::array<unsigned char, 32> pub{};
    std::size_t len = pub.size();
    assert(EVP_PKEY_get_raw_public_key(FixtureKey(), pub.data(), &len) == 1 && len == 32);
    return np::MakePinnedKey(pub);
}

std::string Base64(std::string const& bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    int const n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<unsigned char const*>(bytes.data()),
                                  static_cast<int>(bytes.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string Sign(std::string const& payload) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    assert(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, FixtureKey()) == 1);
    std::string sig(64, '\0');
    std::size_t len = sig.size();
    assert(EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                          reinterpret_cast<unsigned char const*>(payload.data()),
                          payload.size()) == 1);
    EVP_MD_CTX_free(ctx);
    return sig;
}

std::string Envelope(std::string const& payload, std::string const& sig, std::string const& keyid) {
    return R"({"payload":")" + Base64(payload) + R"(","signatures":[{"keyid":")" + keyid +
           R"(","sig":")" + Base64(sig) + "\"}]}\n";
}

std::string Signed(std::string const& payload) {
    return Envelope(payload, Sign(payload), FixturePinned().keyid);
}

std::string Payload(int version, std::string const& switch_families,
                    std::string const& wiiu_families = R"(".nintendo.net",".openpak.org")") {
    return R"({"type":"openpak-ceiling","version":)" + std::to_string(version) +
           R"(,"issued":"2026-09-24T10:30:00Z","future_field":true,"platforms":{"switch":[)" +
           switch_families + R"(],"wiiu":[)" + wiiu_families + "]}}";
}

bool Rejects(std::string const& envelope, std::vector<np::PinnedKey> const& keys) {
    std::string error;
    bool const rejected = !np::VerifyCeiling(envelope, keys, error);
    assert(!rejected || !error.empty());
    return rejected;
}

}  // namespace

int main() {
    std::vector<np::PinnedKey> const keys = {FixturePinned()};
    std::string const good_payload = Payload(3, R"(".nintendo.net",".nintendo.com",".demonware.net")");
    std::string const good = Signed(good_payload);

    // The production key is the contract's, and its keyid is the sha256 of the raw key.
    assert(np::PinnedKeys().size() == 1);
    assert(np::PinnedKeys()[0].keyid ==
           "36e8bcdd93c2c1a1d7e5d87bbba05a7a4f97882131cf6878f1c053a25a377fe4");

    // Good: verified, and unknown fields ignored.
    {
        std::string error;
        auto const c = np::VerifyCeiling(good, keys, error);
        assert(c && error.empty());
        assert(c->version == 3 && c->issued == "2026-09-24T10:30:00Z");
        assert(c->FamiliesFor("switch").size() == 3);
        assert(c->FamiliesFor("3ds").empty());
    }

    // Tampered: another payload under the same signature, or a flipped signature byte.
    {
        std::string const other = Payload(3, R"(".nintendo.net",".evil.example")");
        assert(Rejects(Envelope(other, Sign(good_payload), keys[0].keyid), keys));
        std::string sig = Sign(good_payload);
        sig[10] = static_cast<char>(sig[10] ^ 0x01);
        assert(Rejects(Envelope(good_payload, sig, keys[0].keyid), keys));
        // Not base64 at all.
        assert(Rejects(R"({"payload":"!!!!","signatures":[{"keyid":")" + keys[0].keyid +
                           R"(","sig":"AAAA"}]})",
                       keys));
        assert(Rejects("not json", keys));
    }

    // Unknown keyid: a good signature under a keyid nobody pinned, and the fixture-signed file
    // against the production pins.
    assert(Rejects(Envelope(good_payload, Sign(good_payload), std::string(64, 'a')), keys));
    assert(Rejects(good, np::PinnedKeys()));

    // Malformed payloads reject the whole file.
    assert(Rejects(Signed(Payload(3, R"(".nintendo.net",".Nintendo.com")")), keys));   // case
    assert(Rejects(Signed(Payload(3, R"(".nintendo.net","nintendo.com")")), keys));    // no dot
    assert(Rejects(Signed(Payload(3, R"(".nintendo..net")")), keys));                  // empty label
    assert(Rejects(Signed(Payload(3, R"(".nintendo.net",42)")), keys));                // not a string
    assert(Rejects(Signed(Payload(3, R"(".x_y.com")")), keys));                        // character
    assert(Rejects(Signed(Payload(3, R"(".ok.com")", R"(".-bad.com")")), keys));       // other platform
    assert(Rejects(Signed(R"({"type":"something-else","version":3,"platforms":{}})"), keys));
    assert(Rejects(Signed(R"({"type":"openpak-ceiling","version":0,"platforms":{}})"), keys));
    assert(Rejects(Signed(R"({"type":"openpak-ceiling","version":"3","platforms":{}})"), keys));
    assert(Rejects(Signed(R"({"type":"openpak-ceiling","version":3})"), keys));

    // One-label families: a bare public suffix is never a family.
    assert(!np::FamilyWellFormed(".com"));
    assert(!np::FamilyWellFormed(".net"));
    assert(!np::FamilyWellFormed("."));
    assert(np::FamilyWellFormed(".ea.com"));
    assert(np::FamilyWellFormed(".co.uk")); // two labels: the rule the contract states
    assert(Rejects(Signed(Payload(3, R"(".nintendo.net",".com")")), keys));

    // Rollback: an older fetched file is refused and the cache stays; equal is accepted.
    {
        std::string const v5 = Signed(Payload(5, R"(".nintendo.net")"));
        std::string const v4 = Signed(Payload(4, R"(".nintendo.net",".demonware.net")"));
        auto d = np::DecideCeiling(v5, v4, 5, keys);
        assert(d.result.source == np::CeilingSource::Cached && d.result.ceiling.version == 5);
        assert(!d.store && d.highest_accepted == 5 && !d.result.error.empty());
        // Even with no cache, the highest version ever accepted holds.
        d = np::DecideCeiling(std::nullopt, v4, 5, keys);
        assert(d.result.source == np::CeilingSource::BuiltIn && !d.store);
        d = np::DecideCeiling(v4, v5, 4, keys);
        assert(d.result.source == np::CeilingSource::Fetched && d.store && d.highest_accepted == 5);
        d = np::DecideCeiling(v5, v5, 5, keys);
        assert(d.result.source == np::CeilingSource::Fetched);
        // Nothing verified anywhere: the compiled fallback.
        d = np::DecideCeiling(std::nullopt, std::nullopt, 0, keys);
        assert(d.result.source == np::CeilingSource::BuiltIn && d.result.ceiling.version == 0);
        assert(np::NameInFamilies("a.nintendo.net", d.result.ceiling.FamiliesFor("switch")));
    }

    // The cache: stored as received, re-verified on load, rollback remembered across it.
    {
        auto const dir = std::filesystem::temp_directory_path() /
                         ("openpak_ceiling_test_" + std::to_string(std::random_device{}()));
        std::filesystem::remove_all(dir);
        openpak::Platform::SetDirectories(dir, dir / "cache");
        auto const cached_path = dir / "openpak_network_ceiling.json";

        auto r = np::LoadCeiling(good, keys);
        assert(r.source == np::CeilingSource::Fetched && r.ceiling.version == 3);
        assert(openpak::Platform::ReadFile(cached_path) == good); // exact bytes
#ifndef _WIN32
        auto const perms = std::filesystem::status(cached_path).permissions();
        assert((perms & std::filesystem::perms::group_all) == std::filesystem::perms::none);
        assert((perms & std::filesystem::perms::others_all) == std::filesystem::perms::none);
#endif
        assert(np::NameAllowed("x.demonware.net", "switch"));
        assert(!np::NameAllowed("x.demonware.net", "wiiu"));

        r = np::LoadCeiling(std::nullopt, keys);
        assert(r.source == np::CeilingSource::Cached && r.ceiling.version == 3);

        // A cached file changed on disk is not trusted: the fallback applies.
        std::string tampered = good;
        tampered[tampered.find("payload") + 12] ^= 0x01;
        openpak::Platform::WriteFile(cached_path, tampered);
        r = np::LoadCeiling(std::nullopt, keys);
        assert(r.source == np::CeilingSource::BuiltIn && r.ceiling.version == 0);
        assert(!np::NameAllowed("x.demonware.net", "switch"));

        // Nor does the tampering make an older file acceptable: v3 was accepted.
        r = np::LoadCeiling(Signed(Payload(2, R"(".nintendo.net")")), keys);
        assert(r.source == np::CeilingSource::BuiltIn);
        r = np::LoadCeiling(good, keys);
        assert(r.source == np::CeilingSource::Fetched);
        std::filesystem::remove_all(dir);
    }

    // Filtering: a name outside the platform's ceiling goes, alone; the rest applies.
    std::string error;
    auto const profile = np::Parse(R"({"version":7,"platform":"switch",
        "server":{"address":"198.51.100.7"},
        "redirect":{"suffixes":[".nintendo.net",".demonware.net",".ea.com"],
                    "exact":["gamesync.npln.nintendo.net","matchmaker.among.us"],
                    "never":["ctest.cdn.nintendo.net"],
                    "overrides":{"nncs2-lp1.n.n.srv.nintendo.net":"198.51.100.8",
                                 "auth.ea.com":"198.51.100.9"}}})",
                                   error);
    assert(profile && !np::Validate(*profile, "switch"));
    std::string ceiling_error;
    auto const c3 = *np::VerifyCeiling(good, keys, ceiling_error);
    {
        auto filtered = *profile;
        auto const dropped = np::FilterToCeiling(filtered, c3.FamiliesFor("switch"));
        assert(dropped.size() == 3); // .ea.com, matchmaker.among.us, auth.ea.com
        assert((filtered.suffixes == std::vector<std::string>{".nintendo.net", ".demonware.net"}));
        assert((filtered.exact == std::vector<std::string>{"gamesync.npln.nintendo.net"}));
        assert(filtered.overrides.size() == 1);
        assert(filtered.never.size() == 1);

        np::Apply(np::Result{true, np::Source::Fetched, filtered, {}});
        assert(np::RedirectFor("baas.nintendo.net", "203.0.113.1") == "198.51.100.7");
        assert(np::RedirectFor("x.demonware.net", "203.0.113.1") == "198.51.100.7");
        assert(!np::RedirectFor("x.ea.com", "203.0.113.1"));
        assert(np::RedirectFor("nncs2-lp1.n.n.srv.nintendo.net", "") == "198.51.100.8");
    }

    // Services are held to the ceiling by their URL's host, and count in the digest.
    {
        auto with_services = *profile;
        np::FilterToCeiling(with_services, c3.FamiliesFor("switch"));
        auto const before = np::EffectiveDigest(with_services);
        with_services.services = {{"act", "https://account.nintendo.net/v1/api/"},
                                  {"olv", "https://olv.evil.example/"},
                                  {"nus", "https://user@nus.nintendo.net/"},
                                  {"ecs", "not a url"}};
        auto const dropped = np::FilterToCeiling(with_services, c3.FamiliesFor("switch"), false);
        assert(dropped.size() == 3);
        assert(with_services.services.size() == 1 && with_services.services[0].id == "act");
        assert(np::EffectiveDigest(with_services) != before);
    }

    // The digest: only this platform's effective set moves it.
    {
        auto a = *profile;
        np::FilterToCeiling(a, c3.FamiliesFor("switch"));
        auto const other_platform =
            *np::VerifyCeiling(Signed(Payload(4, R"(".nintendo.net",".nintendo.com",".demonware.net")",
                                              R"(".nintendo.net",".openpak.org",".nintendowifi.net")")),
                               keys, ceiling_error);
        auto b = *profile;
        np::FilterToCeiling(b, other_platform.FamiliesFor("switch"));
        assert(np::EffectiveDigest(a) == np::EffectiveDigest(b));

        // Order and case do not matter; an address, a family or an override does.
        auto reordered = a;
        std::swap(reordered.suffixes[0], reordered.suffixes[1]);
        reordered.suffixes[0] = ".DemonWare.net";
        assert(np::EffectiveDigest(reordered) == np::EffectiveDigest(a));
        auto moved = a;
        moved.server_address = "198.51.100.70";
        assert(np::EffectiveDigest(moved) != np::EffectiveDigest(a));
        auto wider = *profile;
        np::FilterToCeiling(wider, np::CompiledCeiling().FamiliesFor("switch"));
        assert(np::EffectiveDigest(wider) != np::EffectiveDigest(a));
        auto never_only = a;
        never_only.never.push_back("conntest.nintendowifi.net");
        assert(np::EffectiveDigest(never_only) == np::EffectiveDigest(a));

        // One notice per new digest, none for the one in use.
        np::Apply(np::Result{true, np::Source::Fetched, a, {}});
        assert(np::DigestInUse() == np::EffectiveDigest(a));
        assert(!np::ShouldAnnounce(np::EffectiveDigest(b)));
        assert(np::ShouldAnnounce(np::EffectiveDigest(wider)));
        assert(!np::ShouldAnnounce(np::EffectiveDigest(wider)));
        assert(np::ShouldAnnounce(np::EffectiveDigest(moved)));
    }

    std::puts("network_ceiling: ok");
    return 0;
}
