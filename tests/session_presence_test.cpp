// SPDX-License-Identifier: GPL-2.0-or-later
// What the session publishes as presence (openpak/session.h): the body is the friends module's
// own, built from what the host said runs and what that title declared through friend:u, for
// the active profile only. Ported from the presence half of Ryujinx's OpenPakSession tests;
// nothing here signs in, so nothing reaches a network.
// The checks are the test: a Release build defines NDEBUG, and must not compile them away.
#undef NDEBUG
#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>

#include "openpak/platform.h"
#include "openpak/presence_state.h"
#include "openpak/session.h"

namespace session = openpak::client::session;
using openpak::friends::PresencePatch;
using openpak::friends::PresencePublication;
using openpak::friends::PresenceState;
using openpak::friends::UserPresenceImpl;

namespace {

bool Has(const std::string& body, const std::string& part) {
    return body.find(part) != std::string::npos;
}

void PublishesWhatTheTitleDeclared() {
    std::string profile = "p1";
    openpak::Platform::SetProfileSource([&profile] { return profile; });

    // Nothing runs: INACTIVE, which is what the module publishes with no application, never
    // "PLAYING whenever there is a title".
    assert(session::PresenceBody() == PresencePatch(PresencePublication{}));

    // A title with no NACP group is its own group, and ONLINE until it declares a session.
    session::SetRunningApplication(0x0100a5a020d5e000, 0);
    std::string body = session::PresenceBody();
    assert(Has(body, R"("path":"/presence/state","value":"ONLINE")"));
    assert(Has(body, R"(appInfo:appId","value":"0100a5a020d5e000")"));
    assert(Has(body, R"(appInfo:presenceGroupId","value":"0100a5a020d5e000")"));
    assert(Has(body, R"(appField","value":"{}")"));

    // The NACP's group is what a friend's game compares against.
    session::SetRunningApplication(0x0100a5a020d5e000, 0x0100a5a020d5f000);
    body = session::PresenceBody();
    assert(Has(body, R"(appInfo:presenceGroupId","value":"0100a5a020d5f000")"));

    // PLAYING only while an online-play session is declared, and the appField is the blob the
    // title committed with UpdateUserPresence.
    UserPresenceImpl presence{};
    presence.online_play_declaration = 1;
    const char pairs[] = "sys_description\0Lobby\0";
    std::memcpy(presence.app_key_value_storage, pairs, sizeof(pairs) - 1);
    assert(PresenceState::Instance().UpdateUserPresence("p1", presence));
    body = session::PresenceBody();
    assert(Has(body, R"("path":"/presence/state","value":"PLAYING")"));
    assert(Has(body, R"(appField","value":"{\"sys_description\":\"Lobby\"}")"));

    // Another profile's declaration is not this one's.
    profile = "p2";
    assert(Has(session::PresenceBody(), R"("path":"/presence/state","value":"ONLINE")"));
    profile = "p1";

    // Closing the session is ONLINE again, the pairs kept.
    assert(PresenceState::Instance().DeclareCloseOnlinePlaySession("p1"));
    body = session::PresenceBody();
    assert(Has(body, R"("path":"/presence/state","value":"ONLINE")"));
    assert(Has(body, "Lobby"));

    // The title stopping is INACTIVE, and the next launch starts fresh.
    session::SetRunningApplication(0, 0);
    assert(session::PresenceBody() == PresencePatch(PresencePublication{}));
    session::SetRunningApplication(0x0100a5a020d5e000, 0);
    assert(Has(session::PresenceBody(), R"("path":"/presence/state","value":"ONLINE")"));
    session::SetRunningApplication(0, 0);

    openpak::Platform::SetProfileSource({});
}

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "openpak-session-presence-test";
    std::filesystem::remove_all(dir);
    openpak::Platform::SetDirectories(dir, dir / "cache");

    PublishesWhatTheTitleDeclared();

    std::filesystem::remove_all(dir);
    return 0;
}
