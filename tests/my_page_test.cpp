// SPDX-License-Identifier: GPL-2.0-or-later
// MyPage as a game drives it: the picker's friends or the game's own invitees become one
// invitation, a cancel is a non-zero Result, and the friend screens complete at once.
#undef NDEBUG // the checks are the point of this binary, in every build type
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "openpak/baas.h"
#include "openpak/my_page.h"

namespace {

std::vector<std::string> posted;

std::vector<std::uint8_t> Argument(std::uint32_t type) {
    std::vector<std::uint8_t> argument(0x10A8);
    std::memcpy(argument.data(), &type, sizeof(type));
    return argument;
}

template <typename T>
void Put(std::vector<std::uint8_t>& argument, std::size_t offset, T value) {
    std::memcpy(argument.data() + offset, &value, sizeof(value));
}

} // namespace

int main() {
    namespace baas = openpak::baas;
    namespace my_page = openpak::my_page;

    baas::SetTransportForTest([](const std::string& method, const std::string&,
                                 const std::string& path, const std::string&,
                                 const std::string& body) {
        if (path.find("/friends?") != std::string::npos) {
            return baas::Reply{200, R"({"items":[{"friendId":"00000000000000b1"},{"friendId":"00000000000000b2"}]})"};
        }
        if (method == "POST" && path == "/v2/invitation_groups") {
            posted.push_back(body);
            return baas::Reply{201, ""};
        }
        return baas::Reply{};
    });
    baas::Attach("00000000000000aa");
    assert(baas::SyncFriendList(true) == baas::Ok);

    // Type 9: the game's own invitees, its data and the filled message slots.
    auto nine = Argument(9);
    Put<std::int32_t>(nine, 0x18, 2);
    Put<std::uint64_t>(nine, 0x20, 0xb1);
    Put<std::uint64_t>(nine, 0x28, 0xb2);
    Put<std::uint64_t>(nine, 0xA0, 3);
    std::memcpy(nine.data() + 0xA8, "abc", 3);
    std::memcpy(nine.data() + 0x4A8 + 2 * 0xC0, "salut", 5); // slot 2 is "ja"
    assert(my_page::Run(nine, 0x0100000000001000ULL, 0) == 0u);
    assert(posted.size() == 1);
    assert(posted[0].find(R"("receiver_ids":["00000000000000b1","00000000000000b2"])") != std::string::npos);
    assert(posted[0].find(R"("application_group_id":"0100000000001000")") != std::string::npos);
    assert(posted[0].find(R"("application_data":"YWJj")") != std::string::npos);
    assert(posted[0].find(R"("messages":{"ja":"salut"})") != std::string::npos);

    // Type 8 with no picker is a cancel, and nothing is sent.
    auto eight = Argument(8);
    Put<std::int32_t>(eight, 0x18, 1);
    assert(my_page::Run(eight, 1, 0) == my_page::ResultCancelled);
    assert(posted.size() == 1);

    // With one, at most maxInviteeCount of what it picked are invited.
    my_page::SetFriendPicker([](int max) {
        assert(max == 1);
        return std::vector<std::uint64_t>{0xb2, 0xb1};
    });
    assert(my_page::Run(eight, 1, 5) == 0u);
    assert(posted.size() == 2);
    assert(posted[1].find(R"("receiver_ids":["00000000000000b2"])") != std::string::npos);
    assert(posted[1].find(R"("application_group_id":"0000000000000005")") != std::string::npos);

    // The friend screens: type 2 answers 0, the others nothing at all.
    assert(my_page::Run(Argument(2), 1, 0) == 0u);
    assert(!my_page::Run(Argument(4), 1, 0));
    return 0;
}
