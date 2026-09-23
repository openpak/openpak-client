// SPDX-License-Identifier: GPL-2.0-or-later
// The guest's friends structs and the presence state, ported from the the earlier Ryujinx port's
// OpenPakFriendStructTests.cs and the struct parts of OpenPakFriendRequestTests.cs: sizes, the
// offsets the audit pins, what the builders put in them, the appField <-> blob conversion both
// ways, and the presence declaration rules of OpenPakPresence.cs.
//
// The sizes matter on their own: a struct of the wrong size is the wrong element stride, and a
// guest buffer filled at the wrong stride is read as garbage.
#undef NDEBUG
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "openpak/friend_structs.h"
#include "openpak/presence_state.h"

using namespace openpak;
using namespace openpak::friends;

namespace {

template <std::size_t N>
std::string Utf8(const char (&field)[N]) {
    return Text(std::span<const char>(field, N));
}

template <std::size_t N>
std::string Utf8(const std::uint8_t (&field)[N]) {
    return Text(std::span<const std::uint8_t>(field, N));
}

std::uint64_t ReadLittleEndian(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

/// A zero-padded 0xC0 blob holding the literal's bytes, embedded NULs included.
template <std::size_t N>
std::vector<std::uint8_t> Blob(const char (&pairs)[N]) {
    std::vector<std::uint8_t> blob(kAppKeyValueSize, 0);
    std::memcpy(blob.data(), pairs, N - 1);
    return blob;
}

std::string Repeat(std::string_view unit, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += unit;
    }
    return out;
}

bool AllZero(const std::uint8_t* bytes, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

// ---- Sizes and offsets (the static_asserts already pin them; these keep the tests' list) ----

void Sizes() {
    static_assert(sizeof(ProfileImpl) == 0x100);
    static_assert(sizeof(ProfileExtraImpl) == 0x400);
    static_assert(sizeof(ProfileExtraImplV2) == 0x4A8);
    static_assert(sizeof(BlockedUserImpl) == 0x200);
    static_assert(sizeof(BlockedUserImplV2) == 0x200);
    static_assert(sizeof(UserSettingImpl) == 0x800);
    static_assert(sizeof(UserSettingImplV2) == 0x800);
    static_assert(sizeof(FriendSettingImpl) == 0x40);
    static_assert(sizeof(FriendSettingImplV2) == 0x80);
    static_assert(sizeof(FriendImpl) == 0x200);
    static_assert(sizeof(FriendForViewerImpl) == 0x200);
    static_assert(sizeof(FriendDetailedInfoImpl) == 0x800);
    static_assert(sizeof(FriendInvitationForViewerImpl) == 0x500);
    static_assert(sizeof(FriendInvitationForViewerImplV2) == 0x500);
    static_assert(sizeof(FriendInvitationGroupImpl) == 0x1400);
    static_assert(sizeof(FriendInvitationGroupImplV2) == 0x1400);
    static_assert(sizeof(FriendPresenceImpl) == 0xE0);
    static_assert(sizeof(UserPresenceImpl) == 0xE0);
    static_assert(sizeof(UserPresenceViewImpl) == 0xE0);
    static_assert(sizeof(ApplicationInfoV2) == 0x18);
    static_assert(sizeof(FriendNote) == 0x58);
    static_assert(sizeof(Relationship) == 8);
    static_assert(sizeof(FriendCode) == 0x20);
    static_assert(sizeof(FriendInvitationGameModeDescription) == 0xC00);
    static_assert(sizeof(FriendRequestImpl) == 0x200);
    static_assert(sizeof(FriendRequestImplV2) == 0x200);
    static_assert(sizeof(InAppScreenName) == 0x48);
}

void Offsets() {
    static_assert(offsetof(ProfileImpl, thumbnail_url) == 0x30);
    static_assert(offsetof(ProfileImpl, is_valid) == 0xD0);
    static_assert(offsetof(ProfileExtraImpl, play_log) == 0xD0);
    static_assert(offsetof(ProfileExtraImpl, is_valid) == 0x3F0);
    static_assert(offsetof(ProfileExtraImplV2, is_valid) == 0x490);

    static_assert(offsetof(BlockedUserImpl, reason) == 0xE0);
    static_assert(offsetof(BlockedUserImpl, route_name) == 0xF8);
    static_assert(offsetof(BlockedUserImpl, is_valid) == 0x148);
    static_assert(offsetof(BlockedUserImplV2, acd_index) == 0xF4);
    static_assert(offsetof(BlockedUserImplV2, is_valid) == 0x150);

    static_assert(offsetof(UserSettingImpl, friend_code) == 0x20);
    static_assert(offsetof(UserSettingImpl, network_user_id) == 0x48);
    static_assert(offsetof(UserSettingImpl, thumbnail_url) == 0x78);
    static_assert(offsetof(UserSettingImpl, is_valid) == 0x438);
    static_assert(offsetof(UserSettingImplV2, is_valid) == 0x4D8);

    static_assert(offsetof(FriendImpl, presence) == 0x40);
    static_assert(offsetof(FriendImpl, is_favourite) == 0x120);
    static_assert(offsetof(FriendImpl, is_valid) == 0x128);
    static_assert(offsetof(FriendForViewerImpl, presence) == 0xE0);
    static_assert(offsetof(FriendForViewerImpl, is_valid) == 0x1D0);

    static_assert(offsetof(FriendInvitationForViewerImpl, application_data) == 0xE0);
    static_assert(offsetof(FriendInvitationForViewerImpl, is_valid) == 0x3A);
    static_assert(offsetof(FriendInvitationForViewerImplV2, acd_index) == 0x24);
    static_assert(offsetof(FriendInvitationForViewerImplV2, is_valid) == 0x42);

    static_assert(offsetof(FriendInvitationGroupImpl, messages) == 0xA8);
    static_assert(offsetof(FriendInvitationGroupImpl, is_valid) == 0xCB9);
    static_assert(offsetof(FriendInvitationGroupImpl, application_data) == 0xD60);
    static_assert(offsetof(FriendInvitationGroupImplV2, acd_index) == 0xA4);
    static_assert(offsetof(FriendInvitationGroupImplV2, is_valid) == 0xCC1);

    // Requests, V1 then V2.
    static_assert(offsetof(FriendRequestImpl, user_id) == 0x000);
    static_assert(offsetof(FriendRequestImpl, request_id) == 0x010);
    static_assert(offsetof(FriendRequestImpl, other_user_id) == 0x018);
    static_assert(offsetof(FriendRequestImpl, nickname) == 0x020);
    static_assert(offsetof(FriendRequestImpl, thumbnail_url) == 0x048);
    static_assert(offsetof(FriendRequestImpl, list_type) == 0x0E8);
    static_assert(offsetof(FriendRequestImpl, channel) == 0x0EC);
    static_assert(offsetof(FriendRequestImpl, state) == 0x0F0);
    static_assert(offsetof(FriendRequestImpl, route_application_id) == 0x0F8);
    static_assert(offsetof(FriendRequestImpl, route_presence_group_id) == 0x100);
    static_assert(offsetof(FriendRequestImpl, route_name) == 0x108);
    static_assert(offsetof(FriendRequestImpl, route_language) == 0x148);
    static_assert(offsetof(FriendRequestImpl, created_at) == 0x150);
    static_assert(offsetof(FriendRequestImpl, is_read) == 0x158);
    static_assert(offsetof(FriendRequestImpl, is_valid) == 0x159);
    static_assert(offsetof(FriendRequestImpl, route) == 0x160);
    static_assert(offsetof(FriendRequestImplV2, route_application_id) == 0x0F8);
    static_assert(offsetof(FriendRequestImplV2, route_acd_index) == 0x104);
    static_assert(offsetof(FriendRequestImplV2, route_presence_group_id) == 0x108);
    static_assert(offsetof(FriendRequestImplV2, route_name) == 0x110);
    static_assert(offsetof(FriendRequestImplV2, route_language) == 0x150);
    static_assert(offsetof(FriendRequestImplV2, created_at) == 0x158);
    static_assert(offsetof(FriendRequestImplV2, is_read) == 0x160);
    static_assert(offsetof(FriendRequestImplV2, is_valid) == 0x161);
    static_assert(offsetof(FriendRequestImplV2, route) == 0x168);
}

// ---- Builders (OpenPakFriendStructTests.cs) ----

void MapsAUser() {
    baas::User user;
    user.id = 0x2306080700000001;
    user.nickname = "Rosa";
    user.thumbnail_url = "https://example/1/r.jpg";
    const ProfileImpl impl = ToProfileImpl(user);
    assert(impl.network_user_id == 0x2306080700000001);
    assert(Utf8(impl.nickname.name) == "Rosa");
    assert(Utf8(impl.thumbnail_url) == "https://example/1/r.jpg");
    assert(impl.is_valid == 1);

    const ProfileExtraImplV2 extra = ToProfileExtraImplV2(user);
    assert(extra.is_valid == 1 && AllZero(extra.play_log, sizeof(extra.play_log)));
}

void MapsABlock() {
    baas::Block block;
    block.id = 0x2618050200000002;
    block.nickname = "Nox";
    block.thumbnail_url = "https://example/1/n.jpg";
    block.created_at = 1788000123;
    block.reason = 2;
    block.route.application_id = 0x0100a5a020d5e000;
    block.route.acd_index = 3;
    block.route.presence_group_id = 0x0100a5a020d5e000;
    block.route.name = "Lobby";
    block.route.language = "en-US";

    const Uid uid{2, 1};
    const BlockedUserImpl impl = ToBlockedUserImpl(block, uid);
    const BlockedUserImplV2 impl_v2 = ToBlockedUserImplV2(block, uid);
    assert(impl.user_id == uid);
    assert(impl.network_user_id == 0x2618050200000002);
    assert(Utf8(impl.nickname.name) == "Nox");
    assert(impl.reason == 2);
    assert(impl.application_id == 0x0100a5a020d5e000);
    assert(Utf8(impl.route_name) == "Lobby");
    assert(Utf8(impl.route_language) == "en-US");
    assert(impl.created_at == 1788000123);
    assert(impl.is_valid == 1);
    assert(impl_v2.acd_index == 3);
    assert(impl_v2.presence_group_id == 0x0100a5a020d5e000);
    assert(Utf8(impl_v2.route_name) == "Lobby");
    assert(impl_v2.is_valid == 1);
}

void MapsTheOwnUser() {
    baas::UserSetting setting;
    setting.id = 0x2306080700000001;
    setting.nickname = "Rosa";
    setting.thumbnail_url = "https://example/1/r.jpg";
    setting.presence_permission = 2;
    setting.play_log_permission = 3;
    setting.friend_request_reception = true;
    setting.friend_code = "1234-5678-9012";
    setting.friend_code_regenerable_at = 1790000000;

    const Uid uid{4, 3};
    const UserSettingImpl impl = ToUserSettingImpl(setting, uid);
    assert(impl.user_id == uid);
    assert(impl.presence_permission == 2);
    assert(impl.play_log_permission == 3);
    assert(impl.friend_request_reception == 1);
    assert(Utf8(impl.friend_code.code) == "1234-5678-9012");
    assert(impl.friend_code_regenerable_at == 1790000000);
    assert(impl.network_user_id == 0x2306080700000001);
    assert(Utf8(impl.nickname.name) == "Rosa");
    assert(impl.is_valid == 1);
    assert(ToUserSettingImplV2(setting, uid).is_valid == 1);
}

void MapsAnInvitation() {
    baas::Invitation invitation;
    invitation.id = 41;
    invitation.group_id = 7;
    invitation.sender_id = 0x2306080700000001;
    invitation.application_id = 0x0100a5a020d5e000;
    invitation.application_group_id = 0x0100a5a020d5e000;
    invitation.acd_index = 1;
    invitation.application_data = {'E', 'C', 'K', 'A', 'A', 'L'};
    invitation.created_at = 1788000500;
    invitation.read = true;
    invitation.application_id_match = true;

    const FriendInvitationForViewerImpl impl = ToInvitationImpl(invitation);
    const FriendInvitationForViewerImplV2 impl_v2 = ToInvitationImplV2(invitation);
    assert(impl.invitation_id == 41);
    assert(impl.group_id == 7);
    assert(impl.sender_id == 0x2306080700000001);
    assert(impl.application_data_size == 6);
    assert(Utf8(impl.application_data) == "ECKAAL");
    assert(impl.is_read == 1);
    assert(impl.is_valid == 1);
    assert(impl_v2.acd_index == 1);
    assert(impl_v2.application_group_id == 0x0100a5a020d5e000);
    assert(impl_v2.application_data_size == 6);
    assert(impl_v2.is_valid == 1);

    // More than 0x400 bytes of data are cut to what the field holds.
    invitation.application_data.assign(0x500, 0xAB);
    assert(ToInvitationImpl(invitation).application_data_size == 0x400);
}

void MapsAnInvitationGroup() {
    baas::InvitationGroup group;
    group.id = 7;
    group.sender_id = 0x2306080700000001;
    group.receivers = {0x1111, 0x2222};
    group.application_id = 0x0100a5a020d5e000;
    group.application_group_id = 0x0100a5a020d5e000;
    group.acd_index = 1;
    group.messages = {"Join me", "", "いっしょに"};
    group.application_data = {1, 2, 3};
    group.created_at = 1788000600;
    group.application_id_match = true;

    const FriendInvitationGroupImpl impl = ToInvitationGroupImpl(group);
    assert(impl.group_id == 7);
    assert(impl.receiver_count == 2);
    assert(impl.receivers[0] == 0x1111u);
    assert(impl.receivers[1] == 0x2222u);
    assert(impl.application_data_size == 3);
    assert(impl.is_valid == 1);

    // Slot 0 is en-US and slot 2 is ja, in the module's language order.
    assert(Utf8(impl.messages[0]) == "Join me");
    assert(Utf8(impl.messages[1]).empty());
    assert(Utf8(impl.messages[2]) == "いっしょに");

    const FriendInvitationGroupImplV2 impl_v2 = ToInvitationGroupImplV2(group);
    assert(impl_v2.acd_index == 1 && impl_v2.receiver_count == 2 && impl_v2.is_valid == 1);
    assert(Utf8(impl_v2.messages[2]) == "いっしょに");

    // At most sixteen receivers.
    group.receivers.assign(20, 9);
    assert(ToInvitationGroupImpl(group).receiver_count == 16);
}

void KeepsAWholeMultiByteTail() {
    // A name that fits keeps its last character; one that does not is cut on a character
    // boundary rather than mid-sequence.
    assert(Utf8(ToNickname("いっしょに").name) == "いっしょに");
    assert(Utf8(ToNickname(Repeat("あ", 12)).name) == Repeat("あ", 10));
    assert(Utf8(ToNickname(std::string(40, 'a')).name) == std::string(32, 'a'));

    // The same rule in any fixed field: N-1 bytes at most, never half a character.
    char field[8];
    FixedUtf8(field, "abcdéf"); // é is two bytes at 4..5, so 7 bytes fit whole.
    assert(Utf8(field) == "abcdéf");
    FixedUtf8(field, "abcdeéf"); // 8 bytes: the f is cut, the é before it kept whole.
    assert(Utf8(field) == "abcdeé");
    FixedUtf8(field, "abcdefé"); // é starts at byte 6 and would end at 8.
    assert(Utf8(field) == "abcdef" && field[6] == 0 && field[7] == 0);
}

void InvitationMessagesAreTheFilledSlots() {
    FriendInvitationGameModeDescription description{};
    std::memcpy(description.messages[0], "Come in", 7);
    std::memcpy(description.messages[3], "Entrez", 6);

    const auto messages = InvitationMessages(description);
    assert(messages.size() == 2);
    assert(messages[0].first == "en-US" && messages[0].second == "Come in");
    assert(messages[1].first == "fr" && messages[1].second == "Entrez");
}

void ScreenNames() {
    InAppScreenName name{};
    std::memcpy(name.name, "Rosa", 4);
    std::memcpy(name.language_code, "en-US", 5);
    const auto [text, language] = ScreenName(name);
    assert(text == "Rosa" && language == "en-US");
}

// ---- Requests (OpenPakFriendRequestTests.cs) ----

baas::Request Request() {
    baas::Request request;
    request.id = 0xa1;
    request.channel = 3;
    request.state = 1;
    request.other_id = 0xaaaaaaaaaaaa0002;
    request.nickname = "Leia";
    request.thumbnail_url = "https://example/1/a.jpg";
    request.created_at = 1788000000;
    request.read = true;
    request.route.application_id = 0x0100a5a020d5e000;
    request.route.acd_index = 1;
    request.route.presence_group_id = 0x0100a5a020d5e000;
    request.route.catalog_id = "0100a5a020d5e0000100a5a020d5e000";
    request.route.name = "Seen";
    request.route.language = "en-US";
    return request;
}

void MapsARequest() {
    const Uid uid{8, 7};
    const FriendRequestImpl impl = ToRequestImpl(Request(), uid, 2);
    assert(impl.user_id == uid);
    assert(impl.request_id == 0xa1);
    assert(impl.other_user_id == 0xaaaaaaaaaaaa0002);
    assert(Utf8(impl.nickname.name) == "Leia");
    assert(Utf8(impl.thumbnail_url) == "https://example/1/a.jpg");
    assert(impl.list_type == 2);
    assert(impl.channel == 3);
    assert(impl.state == 1);
    assert(impl.route_application_id == 0x0100a5a020d5e000);
    assert(impl.route_presence_group_id == 0x0100a5a020d5e000);
    assert(Utf8(impl.route_name) == "Seen");
    assert(Utf8(impl.route_language) == "en-US");
    assert(impl.created_at == 1788000000);
    assert(impl.is_read == 1);
    assert(impl.is_valid == 1);
    assert(ReadLittleEndian(impl.route.catalog_id) == 0x0100a5a020d5e000);
    assert(ReadLittleEndian(impl.route.catalog_id + 8) == 0x0100a5a020d5e000);
}

void MapsARequestV2() {
    const FriendRequestImplV2 impl = ToRequestImplV2(Request(), Uid{8, 7}, 1);
    assert(impl.request_id == 0xa1);
    assert(impl.list_type == 1);
    assert(impl.route_acd_index == 1);
    assert(impl.route_presence_group_id == 0x0100a5a020d5e000);
    assert(Utf8(impl.route_name) == "Seen");
    assert(impl.created_at == 1788000000);
    assert(impl.is_read == 1);
    assert(impl.is_valid == 1);
}

void MiiUnionWhenNoCatalog() {
    baas::Request request = Request();
    request.route = baas::Route{};
    request.route.mii_name = "Mii";
    request.route.mii_image_url_param = "param";
    const FriendRequestImpl impl = ToRequestImpl(request, Uid{}, 2);
    assert(Utf8(impl.route.nnid.mii_name) == "Mii");
    assert(Utf8(impl.route.nnid.mii_image_url_param) == "param");
}

void NullRouteGivesZeros() {
    baas::Request request = Request();
    request.route = baas::Route{};
    const FriendRequestImpl impl = ToRequestImpl(request, Uid{}, 2);
    assert(impl.route_application_id == 0);
    assert(Utf8(impl.route_name).empty());
    assert(AllZero(impl.route.raw, sizeof(impl.route.raw)));
}

void CatalogIds() {
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    assert(TryCatalogId("0100a5a020d5e0000100a5a020d5e000", high, low));
    assert(high == 0x0100a5a020d5e000 && low == 0x0100a5a020d5e000);
    assert(!TryCatalogId("", high, low));
    assert(!TryCatalogId("0100a5a020d5e000", high, low));
    assert(!TryCatalogId("zz00a5a020d5e0000100a5a020d5e000", high, low));
}

// ---- Friends, presence blob and the filter ----

baas::Friend Friend() {
    baas::Friend friend_;
    friend_.id = 0xaaaaaaaaaaaa0002;
    friend_.nickname = "Leia";
    friend_.thumbnail_url = "https://example/1/a.jpg";
    friend_.state = 2;
    friend_.updated_at = 1788000000;
    friend_.application_id = 0x0100a5a020d5e000;
    friend_.presence_group_id = 0x0100a5a020d5e000;
    friend_.app_field = R"({"sys_description":"Lobby","room":"42"})";
    friend_.is_favorite = true;
    friend_.is_newly = true;
    friend_.is_online_notification = true;
    friend_.channel = 3;
    friend_.note = "from school";
    return friend_;
}

void MapsAFriend() {
    const Uid uid{8, 7};
    const baas::Friend friend_ = Friend();

    const FriendImpl same = ToFriendImpl(friend_, uid, 0x0100a5a020d5e000, false);
    assert(same.user_id == uid);
    assert(same.network_user_id == 0xaaaaaaaaaaaa0002);
    assert(Utf8(same.nickname.name) == "Leia");
    assert(same.presence.application_id == 0x0100a5a020d5e000);
    assert(same.presence.last_update_timestamp == 1788000000);
    assert(same.presence.status == PresenceStatus::OnlinePlay);
    assert(same.presence.same_presence_group_application == 1);
    assert(BlobToAppField(same.presence.app_key_value_storage) == friend_.app_field);
    assert(same.is_favourite == 1 && same.is_new == 1 && same.is_valid == 1);

    // Another group: no blob unless the port is a viewer.
    const FriendImpl other = ToFriendImpl(friend_, uid, 0x0100000000010000, false);
    assert(other.presence.same_presence_group_application == 0);
    assert(AllZero(other.presence.app_key_value_storage, kAppKeyValueSize));
    const FriendImpl viewer = ToFriendImpl(friend_, uid, 0x0100000000010000, true);
    assert(BlobToAppField(viewer.presence.app_key_value_storage) == friend_.app_field);

    const FriendForViewerImpl for_viewer = ToFriendForViewerImpl(friend_, uid, 0);
    assert(Utf8(for_viewer.thumbnail_url) == "https://example/1/a.jpg");
    assert(BlobToAppField(for_viewer.presence.app_key_value_storage) == friend_.app_field);
    assert(for_viewer.is_favourite == 1 && for_viewer.is_valid == 1);

    // Offline: the time and title when known, never the blob.
    baas::Friend offline = friend_;
    offline.state = 0;
    const FriendImpl gone = ToFriendImpl(offline, uid, 0x0100a5a020d5e000, true);
    assert(gone.presence.status == PresenceStatus::Offline);
    assert(gone.presence.application_id == 0x0100a5a020d5e000);
    assert(AllZero(gone.presence.app_key_value_storage, kAppKeyValueSize));
    offline.updated_at = 0;
    assert(ToFriendImpl(offline, uid, 0x0100a5a020d5e000, true).presence.application_id == 0);

    const FriendDetailedInfoImpl detail = ToDetailedInfoImpl(friend_, uid);
    assert(detail.channel == 3 && detail.is_valid == 1 && detail.is_play_log_valid == 0);
    const FriendSettingImplV2 setting = ToFriendSettingImplV2(friend_, uid);
    assert(Utf8(setting.friend_note) == "from school");
    assert(setting.is_favourite == 1 && setting.is_new == 1 && setting.is_online_notification == 1);
    assert(ToFriendSettingImpl(friend_, uid).is_online_notification == 1);
}

void FiltersFriends() {
    std::vector<baas::Friend> friends(4, Friend());
    friends[0].state = 1;
    friends[1].state = 2;
    friends[2].state = 0;
    friends[3].state = 1;
    friends[3].is_favorite = false;
    friends[3].application_id = 0x0100000000010000;
    friends[3].presence_group_id = 0x0100000000010000;
    friends[3].play_log.push_back(baas::PlayLog{.presence_group_id = 0x0100a5a020d5e000});

    const std::uint64_t own = 0x0100a5a020d5e000;
    SizedFriendFilter filter{};
    assert(Filtered(friends, filter, 0, own).size() == 4);
    assert(Filtered(friends, filter, 3, own).size() == 1);
    assert(Filtered(friends, filter, 9, own).empty());

    filter.presence_status = PresenceStatusFilter::Online;
    assert(Filtered(friends, filter, 0, own).size() == 2);
    filter.presence_status = PresenceStatusFilter::OnlinePlay;
    assert(Filtered(friends, filter, 0, own).size() == 1);
    filter.presence_status = PresenceStatusFilter::OnlineOrOnlinePlay;
    assert(Filtered(friends, filter, 0, own).size() == 3);

    filter = SizedFriendFilter{};
    filter.is_favorite_only = 1;
    assert(Filtered(friends, filter, 0, own).size() == 3);

    filter = SizedFriendFilter{};
    filter.is_same_app_presence_only = 1;
    assert(Filtered(friends, filter, 0, own).size() == 3);
    filter = SizedFriendFilter{};
    filter.is_same_app_played_only = 1; // friends[3] played it
    assert(Filtered(friends, filter, 0, own).size() == 4);
    filter = SizedFriendFilter{};
    filter.is_arbitrary_app_played_only = 1;
    filter.presence_group_id = 0x0100000000010000;
    assert(Filtered(friends, filter, 0, own).size() == 1);
}

// ---- appField <-> blob ----

void BlobRoundTrips() {
    const auto blob = Blob("sys_description\0Lobby\0room\0" "42\0");
    const std::string app_field = BlobToAppField(blob);
    assert(app_field == R"({"sys_description":"Lobby","room":"42"})");

    std::vector<std::uint8_t> back(kAppKeyValueSize, 0xFF);
    AppFieldToBlob(app_field, back);
    assert(back == blob);

    // Multi-byte values and JSON escapes survive both ways; the text is raw UTF-8.
    const std::string rich = "{\"sys_description\":\"いっしょに \\\"quoted\\\" \\\\ \\n\"}";
    std::vector<std::uint8_t> rich_blob(kAppKeyValueSize);
    AppFieldToBlob(rich, rich_blob);
    assert(BlobToAppField(rich_blob) == rich);
    assert(Text(std::span<const std::uint8_t>(rich_blob).subspan(16)) ==
           "いっしょに \"quoted\" \\ \n");
}

void BlobThatDoesNotValidateIsEmptyObject() {
    assert(BlobToAppField(Blob("")) == "{}");
    // A repeated key.
    assert(BlobToAppField(Blob("a\0" "1\0a\0" "2\0")) == "{}");
    // A key outside [A-Za-z0-9_].
    assert(BlobToAppField(Blob("a-b\0" "1\0")) == "{}");
    assert(BlobToAppField(Blob("\xC3\xA9\0" "1\0")) == "{}");
    // A value that is not UTF-8 (a lone continuation byte, an overlong, a surrogate).
    assert(BlobToAppField(Blob("a\0\x80\0")) == "{}");
    assert(BlobToAppField(Blob("a\0\xC0\xAF\0")) == "{}");
    assert(BlobToAppField(Blob("a\0\xED\xA0\x80\0")) == "{}");
    // A pair without its terminator: the whole 0xC0 is key.
    std::vector<std::uint8_t> full(kAppKeyValueSize, 'a');
    assert(BlobToAppField(full) == "{}");
    // A value that runs to the end without a NUL.
    std::vector<std::uint8_t> open = Blob("k\0");
    std::fill(open.begin() + 2, open.end(), 'v');
    assert(BlobToAppField(open) == "{}");
    // An empty value is fine.
    assert(BlobToAppField(Blob("k\0\0")) == R"({"k":""})");
}

void AppFieldThatIsNotAnObjectIsSkipped() {
    std::vector<std::uint8_t> blob(kAppKeyValueSize, 0xFF);
    for (const std::string_view text : {"", "[]", " {}", "{} ", "\"x\"", "{", "{\"a\":}", "{\"a\":\"1\",}"}) {
        std::fill(blob.begin(), blob.end(), 0xFF);
        AppFieldToBlob(text, blob);
        assert(AllZero(blob.data(), blob.size()));
    }

    // Non-string members and invalid keys are left out; nested objects are walked over.
    AppFieldToBlob(R"({"n":1,"b":true,"x":null,"o":{"in":"no"},"a":["no"],"bad-key":"no","k":"v"})",
                   blob);
    assert(BlobToAppField(blob) == R"({"k":"v"})");

    // Duplicates are kept as the document has them (the blob reader then refuses them).
    AppFieldToBlob(R"({"k":"1","k":"2"})", blob);
    assert(std::memcmp(blob.data(), "k\0" "1\0k\0" "2\0", 8) == 0);
}

void AppFieldPairsThatDoNotFitAreLeftOut() {
    const std::string big(0xBA, 'x'); // 4 + 1 + 0xBA + 2 = 0xC1: one byte too many
    std::vector<std::uint8_t> blob(kAppKeyValueSize);
    AppFieldToBlob("{\"a\":\"1\",\"b\":\"" + big + "\",\"c\":\"3\"}", blob);
    // "a\01\0" fits; b does not, and nothing after it is tried.
    assert(BlobToAppField(blob) == R"({"a":"1"})");

    // Exactly 0xC0 fits; one byte more does not.
    AppFieldToBlob("{\"k\":\"" + std::string(0xBD, 'y') + "\"}", blob);
    assert(blob[0] == 'k' && blob[1] == 0 && blob[2] == 'y' && blob[0xBE] == 'y' && blob[0xBF] == 0);
    AppFieldToBlob("{\"k\":\"" + std::string(0xBE, 'y') + "\"}", blob);
    assert(AllZero(blob.data(), blob.size()));
}

// ---- Presence state (OpenPakPresence.cs) ----

void PresenceRules() {
    std::int64_t now = 1000;
    PresenceState state([&now] { return now; });
    int changes = 0;
    state.SetChangedCallback([&changes] { ++changes; });

    // Nothing runs: INACTIVE with no title.
    PresencePublication current = state.Current("p");
    assert(current.state == "INACTIVE" && current.application_id == 0 && current.app_field == "{}");
    assert(PresencePatch(current) ==
           R"([{"op":"replace","path":"/presence/state","value":"INACTIVE"},)"
           R"({"op":"add","path":"/presence/extras/friends/appField","value":"{}"},)"
           R"({"op":"add","path":"/presence/extras/friends/appInfo:appId","value":"0000000000000000"},)"
           R"({"op":"add","path":"/presence/extras/friends/appInfo:acdIndex","value":0},)"
           R"({"op":"add","path":"/presence/extras/friends/appInfo:presenceGroupId","value":"0000000000000000"}])");
    assert(state.OwnPresenceView("p").state == 0);

    // A title without a NACP group is its own group.
    state.SetRunningApplication(0x0100a5a020d5e000, 0);
    assert(state.Application().second == 0x0100a5a020d5e000);
    state.SetRunningApplication(0x0100a5a020d5e000, 0x0100a5a020d5f000);
    assert(state.OwnPresenceGroupId() == 0x0100a5a020d5f000);

    // Launched: ONLINE with "{}", nothing declared yet.
    current = state.Current("p");
    assert(current.state == "ONLINE" && current.app_field == "{}");
    assert(current.presence_group_id == 0x0100a5a020d5f000);

    // 10600 opens the session.
    now = 1001;
    assert(state.DeclareOpenOnlinePlaySession("p"));
    assert(changes == 1);
    assert(state.Current("p").state == "PLAYING");
    assert(state.For("p").updated_at == 1001);

    // 10610 with declaration 0 keeps the session and sets the pairs.
    UserPresenceImpl presence{};
    const char pairs[] = "sys_description\0Lobby\0";
    std::memcpy(presence.app_key_value_storage, pairs, sizeof(pairs) - 1);
    now = 1002;
    assert(state.UpdateUserPresence("p", presence));
    assert(changes == 2);
    current = state.Current("p");
    assert(current.state == "PLAYING" && current.app_field == R"({"sys_description":"Lobby"})");
    assert(PresencePatch(current).find(
               R"("value":"{\"sys_description\":\"Lobby\"}"})") != std::string::npos);
    assert(PresencePatch(current).find(R"("value":"0100a5a020d5f000"})") != std::string::npos);

    // The same commit again changes nothing and does not restamp.
    now = 1003;
    assert(!state.UpdateUserPresence("p", presence));
    assert(changes == 2 && state.For("p").updated_at == 1002);

    // 10601 closes it, the pairs kept.
    assert(state.DeclareCloseOnlinePlaySession("p"));
    current = state.Current("p");
    assert(current.state == "ONLINE" && current.app_field == R"({"sys_description":"Lobby"})");

    // The own presence view reflects it.
    const UserPresenceViewImpl view = state.OwnPresenceView("p");
    assert(view.state == 1 && view.application_id == 0x0100a5a020d5e000);
    assert(view.presence_group_id == 0x0100a5a020d5f000 && view.last_update_timestamp == 1003);
    assert(BlobToAppField(view.app_key_value_storage) == R"({"sys_description":"Lobby"})");

    // 10610 can open it too, with the declaration byte.
    presence.online_play_declaration = DeclarationOpen;
    assert(state.UpdateUserPresence("p", presence));
    assert(state.Current("p").state == "PLAYING");

    // Other profiles are their own.
    assert(state.Current("q").state == "ONLINE");

    // Another title: the declaration does not carry over.
    state.SetRunningApplication(0x0100000000010000, 0);
    current = state.Current("p");
    assert(current.state == "ONLINE" && current.app_field == "{}");
    assert(state.For("p").updated_at == 0);

    // Nothing runs again.
    state.SetRunningApplication(0, 0);
    assert(state.Current("p").state == "INACTIVE");
    const UserPresenceViewImpl idle = state.OwnPresenceView("p");
    assert(idle.state == 0 && AllZero(idle.app_key_value_storage, kAppKeyValueSize));
}

} // namespace

int main() {
    Sizes();
    Offsets();
    MapsAUser();
    MapsABlock();
    MapsTheOwnUser();
    MapsAnInvitation();
    MapsAnInvitationGroup();
    KeepsAWholeMultiByteTail();
    InvitationMessagesAreTheFilledSlots();
    ScreenNames();
    MapsARequest();
    MapsARequestV2();
    MiiUnionWhenNoCatalog();
    NullRouteGivesZeros();
    CatalogIds();
    MapsAFriend();
    FiltersFriends();
    BlobRoundTrips();
    BlobThatDoesNotValidateIsEmptyObject();
    AppFieldThatIsNotAnObjectIsSkipped();
    AppFieldPairsThatDoNotFitAreLeftOut();
    PresenceRules();
    return 0;
}
