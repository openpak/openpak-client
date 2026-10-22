// SPDX-License-Identifier: GPL-2.0-or-later
// The friends module's reply parsers and REST calls (openpak/baas.h), pinned against the friends
// contract: which fields are required, what types they must have, what a missing one does, and
// which route, body and content type each call puts on the wire. Ported from Ryujinx's
// OpenPakBaasTests.cs and the parsing half of OpenPakInvitationTests.cs; the transport is
// replaced, so nothing here reaches a network.
// The checks are the test: a Release build defines NDEBUG, and must not compile them away.
#undef NDEBUG
#include <cassert>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "openpak/baas.h"
#include "openpak/platform.h"

namespace baas = openpak::baas;
using baas::Json;

namespace {

Json Parse(const std::string& text) {
    return Json::parse(text);
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// ---- a transport that records what was sent and answers from a queue ----

struct Sent {
    std::string method;
    std::string host;
    std::string path;
    std::string content_type;
    std::string body;
};

std::vector<Sent> sent;
std::deque<baas::Reply> replies;

void Install() {
    baas::SetTransportForTest([](const std::string& method, const std::string& host,
                                 const std::string& path, const std::string& content_type,
                                 const std::string& body) {
        sent.push_back({method, host, path, content_type, body});
        if (replies.empty()) {
            return baas::Reply{};
        }
        baas::Reply reply = replies.front();
        replies.pop_front();
        return reply;
    });
}

void Reset(const std::string& user = "00000000000000aa") {
    baas::Detach();
    baas::Attach(user);
    sent.clear();
    replies.clear();
}

void Answer(int status, std::string body) {
    replies.push_back({status, std::move(body)});
}

// ---- fixtures ----

const std::string FriendItem = R"({"friendId":"aaaaaaaaaaaa0002",
 "friend":{"nickname":"Leia","thumbnailUrl":"https://example/1/a.jpg",
  "presence":{"state":"PLAYING","updatedAt":1789000000,
   "extras":{"friends":{"appInfo:appId":"0100a5a020d5e000","appInfo:acdIndex":1,
    "appInfo:presenceGroupId":"0100a5a020d5e000","appField":"{\"k\":\"v\"}"}}},
  "extras":{"self":{"playLog":"[{\"appInfo:appId\":\"0100a5a020d5e000\",\"appInfo:presenceGroupId\":\"0100a5a020d5e000\",\"totalPlayCount\":2,\"totalPlayTime\":60,\"firstPlayedAt\":1788000000,\"lastPlayedAt\":1788900000}]"}}},
 "isFavorite":true,"extras":{"self":{"isConfirmed":true,"isOnlineNotification":true,
  "route:appInfo:appId":"0100a5a020d5e000","route:name":"Leia"}},
 "createdAt":1788000001,"friendNote":"hi","channels":["FRIEND_CODE"]})";

const std::string InboxItem = R"({"id":"a1","channels":["IN_APP"],"state":"pending","senderId":"aaaaaaaaaaaa0002",
 "sender":{"nickname":"Leia","thumbnailUrl":"https://example/1/a.jpg"},
 "createdAt":1788000000,
 "extras":{"receiver":{"read":true},
  "senderAndReceiver":{"route:appInfo:appId":"0100a5a020d5e000","route:name":"Leia"}}})";

const std::string InvitationItem = R"({"id":1789287484056138,"invitation_group_id":1789287484056137,
 "sender_id":"aaaaaaaaaaaa0002","application_id":"0100ed9024eb8000",
 "application_group_id":"0100ed9024eb8000","application_data":"RUNLQUFM",
 "read":false,"created_at":1789287484,"updated_at":1789287484})";

// One item exactly as a live OpenPak answered on 2026-09-13 for an invitation sent from a
// console -- numeric ids included, which is the part a careless parser gets wrong.
const std::string LiveInvitationItem = R"({"acd_index":0,"application_data":"AAEAAAD/////AQAAAAAAAAAGAQAAAAZVUVdGSEUL",
 "application_group_id":"0100ed9024eb8000","application_id":"0100ed9024eb8000",
 "application_id_match":false,"created_at":1789287484,
 "extras":{"receiver":{"read":false}},"id":1789287484056138,
 "invitation_group_id":1789287484056137,"invitation_type":"friend","read":false,
 "receiver_id":"aaaaaaaaaaaa0001","sender_id":"aaaaaaaaaaaa0002","updated_at":1789287484})";

const std::string GroupJson = R"({"id":9,"sender_id":"aaaaaaaaaaaa0002","application_id":"0100ed9024eb8000",
 "application_group_id":"0100ed9024eb8000","application_data":null,
 "created_at":1789287484,"updated_at":1789287484,
 "messages":{"en-US":"hi","ja":"あ"},
 "invitations":[{"receiver_id":"aaaaaaaaaaaa0001"}]})";

// ---- ids ----

void Ids() {
    for (const char* text : {"aaaaaaaaaaaa0002", "0xaaaaaaaaaaaa0002", "0XAAAAAAAAAAAA0002",
                             "  aaaaaaaaaaaa0002", "+aaaaaaaaaaaa0002"}) {
        std::uint64_t value = 0;
        assert(baas::TryHex(Json(text), value));
        assert(value == 0xaaaaaaaaaaaa0002ULL);
    }

    for (const char* text : {"-", "-1", "0x", "", "   ", "12345678901234567"}) {
        std::uint64_t value = 0;
        assert(!baas::TryHex(Json(text), value));
    }

    std::uint64_t value = 0;
    assert(!baas::TryHex(Parse("1234"), value));
}

// ---- friend list (contract A.1) ----

void FriendList() {
    const auto parsed = baas::ParseFriend(Parse(FriendItem));
    assert(parsed);
    const baas::Friend& f = *parsed;
    assert(f.id == 0xaaaaaaaaaaaa0002ULL);
    assert(f.nickname == "Leia");
    assert(f.state == 2);
    assert(f.updated_at == 1789000000);
    assert(f.application_id == 0x0100a5a020d5e000ULL);
    assert(f.presence_group_id == 0x0100a5a020d5e000ULL);
    assert(f.acd_index == 1);
    assert(f.app_field == "{\"k\":\"v\"}");
    assert(f.is_favorite);
    assert(!f.is_newly);
    assert(f.is_online_notification);
    assert(f.created_at == 1788000001);
    assert(f.note == "hi");
    assert(f.channel == 2);
    assert(f.route.name == "Leia");
    assert(f.play_log.size() == 1);

    // Offline falls back to logoutAt.
    assert(baas::ParseFriend(Parse(R"({"friendId":"1","friend":{"nickname":"n","thumbnailUrl":"t",
        "presence":{"state":"OFFLINE","updatedAt":10,"logoutAt":20}}})"))->updated_at == 20);

    // An absent confirm flags the friend newly.
    assert(baas::ParseFriend(
               Parse(R"({"friendId":"1","friend":{"nickname":"n","thumbnailUrl":"t"},"extras":{}})"))
               ->is_newly);

    // A non-object appField is ignored.
    assert(baas::ParseFriend(Parse(R"({"friendId":"1","friend":{"nickname":"n","thumbnailUrl":"t",
        "presence":{"state":"ONLINE","extras":{"friends":{"appField":"nope"}}}}})"))
               ->app_field.empty());

    // The three required fields.
    for (const char* json : {R"({"friend":{"nickname":"n","thumbnailUrl":"t"}})",
                             R"({"friendId":"1","friend":{"thumbnailUrl":"t"}})",
                             R"({"friendId":"1","friend":{"nickname":"n"}})",
                             R"({"friendId":"1","friend":{"nickname":7,"thumbnailUrl":"t"}})"}) {
        assert(!baas::ParseFriend(Parse(json)));
    }

    // The incomplete are skipped.
    const auto friends = baas::ParseFriendList(Parse(
        R"({"items":[{"friendId":"1"},{"friendId":"2","friend":{"nickname":"n","thumbnailUrl":"t"}}]})"));
    assert(friends.size() == 1 && friends[0].id == 2);
}

// ---- play log (contract A.1) ----

void PlayLogs() {
    const auto log = baas::ParsePlayLog(
        R"([{"appInfo:appId":"0100a5a020d5e000","appInfo:acdIndex":0,"appInfo:presenceGroupId":"0100a5a020d5e000","totalPlayCount":12,"totalPlayTime":3600,"firstPlayedAt":1758000000,"lastPlayedAt":1758290000}])");
    assert(log.size() == 1);
    assert(log[0].application_id == 0x0100a5a020d5e000ULL);
    assert(log[0].total_play_count == 12);

    // Repeated appIds merge, the larger counters winning.
    const auto merged = baas::ParsePlayLog(
        R"([{"appInfo:appId":"1","appInfo:presenceGroupId":"1","totalPlayCount":2,"totalPlayTime":9,"firstPlayedAt":4,"lastPlayedAt":5},{"appInfo:appId":"1","appInfo:presenceGroupId":"1","totalPlayCount":7,"totalPlayTime":3,"firstPlayedAt":8,"lastPlayedAt":6}])");
    assert(merged.size() == 1);
    assert(merged[0].total_play_count == 7);
    assert(merged[0].total_play_time == 9);
    assert(merged[0].first_played_at == 8);
    assert(merged[0].last_played_at == 6);

    for (const char* text : {"", "{}", "[]x", "not json", R"([{"appInfo:appId":"1"}])"}) {
        assert(baas::ParsePlayLog(text).empty());
    }
}

// ---- requests (contract A.4) ----

void Requests() {
    const auto request = baas::ParseRequest(Parse(InboxItem), true);
    assert(request);
    assert(request->id == 0xa1);
    assert(request->channel == 3);
    assert(request->state == 1);
    assert(request->other_id == 0xaaaaaaaaaaaa0002ULL);
    assert(request->nickname == "Leia");
    assert(request->created_at == 1788000000);
    assert(request->read);
    assert(request->route.application_id == 0x0100a5a020d5e000ULL);
    assert(request->route.name == "Leia");

    // The outbox reads names from the sender extras.
    const auto outbox = baas::ParseRequest(Parse(R"({"id":"a1","channels":["FRIEND_CODE"],"state":"AUTHORIZED","receiverId":"2",
        "receiver":{"nickname":"n","thumbnailUrl":"t"},
        "extras":{"sender":{"route:name":"Seen"},"senderAndReceiver":{"route:name":"Both"}}})"),
                                             false);
    assert(outbox->state == 3);
    assert(outbox->route.name == "Seen");
    assert(!outbox->read);

    // The six required fields.
    assert(!baas::ParseRequest(Parse(R"({"id":"a1"})"), true));
    assert(!baas::ParseRequest(Parse(Replace(InboxItem, "\"sender\":", "\"receiver\":")), true));

    // An overlong name fails the request.
    assert(!baas::ParseRequest(
        Parse(Replace(InboxItem, "\"Leia\"", "\"" + std::string(0x21, 'n') + "\"")), true));

    // The badge splits read from unread.
    const auto count = baas::ParseRequestCount(Parse(
        R"({"items":[{},{"extras":{"receiver":{"read":true}}},{"extras":{"receiver":{"read":false}}}]})"));
    assert(count.first == 2 && count.second == 1);
}

// ---- users and self (contract A.6, A.7) ----

void Users() {
    const auto user = baas::ParseUser(Parse(R"({"id":"1","nickname":"n","thumbnailUrl":"t"})"));
    assert(user && user->id == 1);

    const auto setting = baas::ParseUserSetting(Parse(R"({"id":"1","nickname":"n","thumbnailUrl":"t",
        "permissions":{"presence":"FRIENDS","friendRequestReception":true},
        "links":{"friendCode":{"id":"SW-1234","regenerableAt":99}},
        "extras":{"friends":{"playLog":"[]"}}})"));
    assert(setting->presence_permission == 2);
    assert(setting->play_log_permission == 3);
    assert(setting->friend_request_reception);
    assert(setting->friend_code == "SW-1234");
    assert(setting->friend_code_regenerable_at == 99);
    assert(setting->play_log_text == "[]");

    // The first group in the document holds the log, whatever its name sorts as.
    assert(baas::ParseUserSetting(Parse(R"({"id":"1","nickname":"n","thumbnailUrl":"t",
        "extras":{"self":{"playLog":"[]"},"everyone":{"playLog":"[]"}}})"))
               ->play_log_permission == 1);

    // EVERYONE presence becomes self.
    assert(baas::ParseUserSetting(Parse(
               R"({"id":"1","nickname":"n","thumbnailUrl":"t","permissions":{"presence":"EVERYONE"}})"))
               ->presence_permission == 0);

    const baas::Relationship relationship = baas::ParseRelationship(
        Parse(R"({"isFriend":true,"isBlocking":false,"sentFriendRequestIds":["a1"]})"));
    assert(relationship.is_friend && !relationship.is_blocking && relationship.is_request_sent);
}

// ---- blocks (contract A.8) ----

void Blocks() {
    const auto blocks = baas::ParseBlocks(Parse(R"({"items":[{"targetUserId":"5","targetUser":{"nickname":"n","thumbnailUrl":"t"},
        "createdAt":7,"extras":{"self":{"reason":"IN_APP"}}}]})"));
    assert(blocks && blocks->size() == 1);
    assert((*blocks)[0].id == 5);
    assert((*blocks)[0].reason == 3);
    assert((*blocks)[0].created_at == 7);

    // One bad block fails the whole parse.
    assert(!baas::ParseBlocks(Parse(
        R"({"items":[{"targetUserId":"5","targetUser":{"nickname":"n"}},{"targetUserId":"6"}]})")));

    // So does a 101st.
    std::string items;
    for (int i = 0; i < 101; i++) {
        items += (i ? "," : "") + std::string(R"({"targetUserId":"1","targetUser":{"nickname":"n"}})");
    }
    assert(!baas::ParseBlocks(Parse("{\"items\":[" + items + "]}")));
}

// ---- invitations (invitations doc 2a, 2b) ----

void Invitations() {
    const auto invitations = baas::ParseInvitations(Parse("{\"items\":[" + InvitationItem + "]}"));
    assert(invitations && invitations->size() == 1);
    const baas::Invitation& invitation = (*invitations)[0];
    assert(invitation.id == 1789287484056138ULL);
    assert(invitation.group_id == 1789287484056137ULL);
    assert(invitation.sender_id == 0xaaaaaaaaaaaa0002ULL);
    assert(invitation.application_id == 0x0100ed9024eb8000ULL);
    assert((invitation.application_data == std::vector<std::uint8_t>{0x45, 0x43, 0x4b, 0x41, 0x41, 0x4c}));
    assert(!invitation.read);
    assert(invitation.created_at == 1789287484);

    // The live item: a u64 id read as a double loses its last digits, and would then name an
    // invitation that does not exist.
    const auto live = baas::ParseInvitations(Parse("{\"items\":[" + LiveInvitationItem + "]}"));
    assert(live && live->size() == 1);
    assert((*live)[0].id == 1789287484056138ULL);
    assert((*live)[0].application_id == 0x0100ed9024eb8000ULL);
    assert((*live)[0].application_data.size() == 30);

    assert(baas::ParseInvitations(Parse(R"({"items":[]})"))->empty());
    assert(!baas::ParseInvitations(Parse(R"({"items":[{"id":1}]})")));

    const auto group = baas::ParseInvitationGroup(Parse(GroupJson));
    assert(group);
    assert(group->receivers.size() == 1);
    assert(group->messages.size() == 16);
    assert(group->messages[0] == "hi");
    assert(group->messages[2] == "\xE3\x81\x82");
    assert(group->messages[1].empty());
    assert(group->application_data.empty());

    for (const char* json :
         {R"({"id":9})",
          R"({"id":9,"sender_id":"1","application_id":"1","application_group_id":"1","application_data":null,"created_at":1,"updated_at":1,"messages":{},"invitations":[]})"}) {
        assert(!baas::ParseInvitationGroup(Parse(json)));
    }
}

// ---- request calls (contract A.4) ----

void RequestBodies() {
    for (const bool inbox : {true, false}) {
        const std::string box = inbox ? "inbox" : "outbox";
        assert(baas::RequestBoxUrl("h", "u", inbox, 8, 100) ==
               "https://h/2.0.0/users/u/friend_requests/" + box +
                   "?offset=8&count=100&sort=createdAt:desc&filter.state.$eq=PENDING");
    }

    assert(baas::RequestUrl("h", 0xa1) == "https://h/2.0.0/friend_requests/00000000000000a1");

    for (const char* state : {"CANCELED", "AUTHORIZED", "REJECTED"}) {
        assert(baas::AnswerRequestBody(state) ==
               std::string(R"([{"op":"replace","path":"/state","value":")") + state + "\"}]");
    }

    for (const char* state : {"CANCELLED", "cancelled", "", "APPROVED"}) {
        bool threw = false;
        try {
            baas::AnswerRequestBody(state);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    assert(baas::ReadRequestBody() == R"([{"op":"add","path":"/extras/receiver/read","value":true}])");

    assert(baas::SendRequestBody("FRIEND_CODE", 1, 2) ==
           R"({"channels":["FRIEND_CODE"],"senderId":"0000000000000001","receiverId":"0000000000000002","extras":{"sender":{"route:sender":"me"}}})");

    assert(baas::SendRequestBody("IN_APP", 1, 2, 0x0100a5a020d5e000ULL, 0, 0x0100a5a020d5e000ULL,
                                 "Seen", "en-US", "Mine", "ja") ==
           R"({"channels":["IN_APP"],"senderId":"0000000000000001","receiverId":"0000000000000002","extras":{"sender":{"route:sender":"me","route:name":"Seen","route:name:language":"en-US"},"senderAndReceiver":{"route:appInfo:appId":"0100a5a020d5e000","route:appInfo:acdIndex":0,"route:appInfo:presenceGroupId":"0100a5a020d5e000","route:name":"Mine","route:name:language":"ja"}}})");

    assert(baas::SendRequestCatalogBody("IN_APP", 1, 2, "c", "Seen", "en-US", "Mine", "ja") ==
           R"({"channels":["IN_APP"],"senderId":"0000000000000001","receiverId":"0000000000000002","extras":{"sender":{"route:sender":"me","route:name":"Seen","route:name:language":"en-US"},"senderAndReceiver":{"route:candidate:catalogId":"c","route:name":"Mine","route:name:language":"ja"}}})");

    assert(baas::SendRequestNnidBody("NNID", 1, 2, "Mii", "p") ==
           R"({"channels":["NNID"],"senderId":"0000000000000001","receiverId":"0000000000000002","extras":{"sender":{"route:sender":"me","route:nnid:miiName":"Mii","route:nnid:miiImageUrlParam":"p"},"senderAndReceiver":{"route:nnid:miiName":"Mii","route:nnid:miiImageUrlParam":"p"}}})");

    // UTF-8 goes out as it is, the relaxed escaping the console writes with.
    assert(baas::SendRequestBody("IN_APP", 1, 2, 1, 0, 1, "\xE3\x81\x82", "ja", "", "") .find("\xE3\x81\x82") != std::string::npos);

    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    assert(baas::TryCatalogId("0100a5a020d5e0000100a5a020d5e000", hi, lo));
    assert(hi == 0x0100a5a020d5e000ULL && lo == 0x0100a5a020d5e000ULL);

    for (const char* id : {"", "1", "0100a5a020d5e000", "0100a5a020d5e0000100a5a020d5e00000",
                           "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"}) {
        assert(!baas::TryCatalogId(id, hi, lo));
    }

    assert(baas::PlayLogPermissionBody(3, "[]") ==
           R"([{"op":"add","path":"/extras/self/playLog","value":""},{"op":"add","path":"/extras/favoriteFriends/playLog","value":""},{"op":"add","path":"/extras/friends/playLog","value":"[]"},{"op":"add","path":"/extras/everyone/playLog","value":""}])");

    assert(baas::UserPatchBody("/permissions/presence", "FRIENDS") ==
           R"([{"op":"replace","path":"/permissions/presence","value":"FRIENDS"}])");
}

// ---- channels ----

void ChannelTable() {
    for (const char* channel : {"NX_FACED", "FRIEND_CODE", "IN_APP", "NINTENDO_ACCOUNT", "3DS",
                                "NNID", "FACEBOOK", "TWITTER", "WECHAT", "CAMPUS"}) {
        assert(baas::ChannelOf(channel) > 0);
    }
    assert(baas::ChannelOf("BOGUS") == 0);
    assert(baas::ChannelOf("") == 0);
}

// ---- results (§1.2) ----

void Results() {
    assert(baas::BaasError({204, ""}) == baas::Ok);
    assert(baas::BaasError({400, R"({"status":400,"errorCode":"invalid_params"})"}) == 2001);
    assert(baas::BaasError({400, R"({"status":400,"errorCode":"receiver_must_be_different_from_sender"})"}) == 5);
    // The body's status picks the row, not the HTTP one.
    assert(baas::BaasError({500, R"({"status":404,"errorCode":"deleted_user"})"}) == 2224);
    // An unknown code falls back to the body's status.
    assert(baas::BaasError({500, R"({"status":404,"errorCode":"nope"})"}) == 3404);
    // No body the parser understood: the status alone decides.
    assert(baas::BaasError({302, ""}) == 3302);
    assert(baas::BaasError({301, "x"}) == 3399);
    assert(baas::BaasError({402, ""}) == 3499);
    assert(baas::BaasError({418, ""}) == 3499);
    assert(baas::BaasError({502, ""}) == 3599);
    assert(baas::BaasError({503, ""}) == 3503);
    assert(baas::BaasError({504, ""}) == 3504);
    assert(baas::BaasError({0, ""}) == baas::Unknown);
    assert(baas::BaasError({400, "[]"}) == 3400);
    assert(baas::BaasError({400, R"({"status":400,"errorCode":")" + std::string(0x40, 'x') + "\"}"}) == 3400);

    assert(baas::FiveError({200, "nonsense"}) == baas::Ok);
    assert(baas::FiveError({400, R"({"error":{"code":"0003","message":"m"}})"}) == 2903);
    assert(baas::FiveError({400, R"({"error":{"code":"0010","message":"m"}})"}) == 2910);
    assert(baas::FiveError({400, R"({"error":{"code":"9999","message":"m"}})"}) == 2999);
    // Without a message, or with a code of the wrong length, the status decides.
    assert(baas::FiveError({400, R"({"error":{"code":"0003"}})"}) == 3400);
    assert(baas::FiveError({404, R"({"error":{"code":"03","message":"m"}})"}) == 3404);
}

// ---- the calls, through the transport ----

void NotSignedIn() {
    baas::Detach();
    sent.clear();
    assert(!baas::Ready());
    assert(baas::SyncFriendList(true) == baas::Unknown);
    assert(baas::SyncBlockList() == baas::Unknown);
    assert(baas::SyncUserSetting() == baas::Unknown);
    // A write without a user is a request that could not be made at all.
    assert(baas::PatchFriend(1, "replace", "/isFavorite", true) == baas::Unknown);
    assert(sent.empty());
}

void FriendListSync() {
    Reset();
    int changes = 0;
    baas::SetFriendListChangedHook([&] { changes++; });

    const std::string list = "{\"items\":[" + FriendItem + "]}";

    Answer(200, list);
    assert(baas::SyncFriendList(false) == baas::Ok);
    assert(sent.size() == 1);
    assert(sent[0].method == "GET" && sent[0].host == baas::BaasHost);
    assert(sent[0].path == "/2.0.0/users/00000000000000aa/friends?count=300");
    assert(sent[0].content_type.empty());
    assert(baas::FriendListAvailable());
    assert(baas::Friends()->size() == 1);
    assert(changes == 1);

    // Inside the cooldown nothing is asked.
    assert(baas::SyncFriendList(false) == baas::Ok);
    assert(sent.size() == 1);

    // Forced, the same list says nothing to anyone.
    Answer(200, list);
    assert(baas::SyncFriendList(true) == baas::Ok);
    assert(sent.size() == 2 && changes == 1);

    // A presence that moved is a changed list.
    Answer(200, Replace(list, "1789000000", "1789000001"));
    assert(baas::SyncFriendList(true) == baas::Ok);
    assert(changes == 2);

    // A failure maps and leaves the cache as it was.
    Answer(503, R"({"status":503,"errorCode":"under_maintenance"})");
    assert(baas::SyncFriendList(true) == 2111);
    assert(baas::Friends()->size() == 1);

    // A 2xx that is not JSON is 3999.
    Answer(200, "not json");
    assert(baas::SyncFriendList(true) == baas::Unknown);

    // The local half of a queued change.
    baas::Update(0xaaaaaaaaaaaa0002ULL, [](baas::Friend& f) { f.is_favorite = false; });
    assert(!baas::FindFriend(0xaaaaaaaaaaaa0002ULL)->is_favorite);
    assert(!baas::FindFriend(7));

    // PATCH: one op, the whole relationship back, the cached entry replaced.
    sent.clear();
    Answer(200, Replace(FriendItem, "\"friendNote\":\"hi\"", "\"friendNote\":\"new\""));
    assert(baas::PatchFriend(0xaaaaaaaaaaaa0002ULL, "replace", "/friendNote", "new") == baas::Ok);
    assert(sent[0].method == "PATCH");
    assert(sent[0].path == "/2.0.0/users/00000000000000aa/friends/aaaaaaaaaaaa0002");
    assert(sent[0].content_type == "application/json-patch+json");
    assert(sent[0].body == R"([{"op":"replace","path":"/friendNote","value":"new"}])");
    assert(baas::FindFriend(0xaaaaaaaaaaaa0002ULL)->note == "new");
    assert(changes == 3);

    // A reply that is not a relationship is 2121-2411.
    Answer(200, "{}");
    assert(baas::PatchFriend(0xaaaaaaaaaaaa0002ULL, "add", "/extras/self/isConfirmed", true) ==
           baas::FriendNotFound);
    assert(sent.back().body == R"([{"op":"add","path":"/extras/self/isConfirmed","value":true}])");

    // DELETE: success syncs inline, forced.
    sent.clear();
    Answer(204, "");
    Answer(200, R"({"items":[]})");
    assert(baas::DeleteFriend(0xaaaaaaaaaaaa0002ULL) == baas::Ok);
    assert(sent.size() == 2);
    assert(sent[0].method == "DELETE" && sent[0].content_type.empty());
    assert(sent[0].path == "/2.0.0/users/00000000000000aa/friends/aaaaaaaaaaaa0002");
    assert(sent[1].method == "GET");
    assert(baas::Friends()->empty());

    // A friend the server does not know is dropped locally and reported as 2411.
    Answer(200, list);
    assert(baas::SyncFriendList(true) == baas::Ok);
    const int before = changes;
    Answer(404, R"({"status":404,"errorCode":"resource_is_not_found"})");
    assert(baas::DeleteFriend(0xaaaaaaaaaaaa0002ULL) == baas::FriendNotFound);
    assert(baas::Friends()->empty());
    assert(changes == before + 1);

    // Anything else is just the error.
    Answer(403, R"({"status":403,"errorCode":"forbidden"})");
    assert(baas::DeleteFriend(1) == 2022);

    baas::SetFriendListChangedHook({});
}

void UsersAndBlocks() {
    Reset();

    // At most 100 de-duplicated ids, comma-joined.
    std::vector<std::uint64_t> ids{1, 1, 2};
    for (std::uint64_t id = 3; id < 200; id++) {
        ids.push_back(id);
    }
    Answer(200, R"({"items":[{"id":"1","nickname":"n","thumbnailUrl":"t"},{"id":"2"}]})");
    const auto [error, users] = baas::FetchUsers(ids);
    assert(error == baas::Ok && users.size() == 1);
    const std::string& path = sent[0].path;
    assert(path.starts_with("/1.0.0/users?filter.id.$in=0000000000000001,0000000000000002,"));
    assert(std::count(path.begin(), path.end(), ',') == 99);

    assert(baas::FetchUsers({}).first == baas::Ok);
    assert(sent.size() == 1);

    // The code goes over the wire exactly as typed, escaped as data; nobody is 2221.
    sent.clear();
    Answer(200, R"({"items":[]})");
    assert(baas::UserByFriendCode("SW-1234 5").first == 2221);
    assert(sent[0].path == "/1.0.0/users?filter.links.friendCode.id.$in=SW-1234%205");

    Answer(200, R"({"items":[{"id":"9","nickname":"n","thumbnailUrl":"t"}]})");
    assert(baas::UserByFriendCode("1234").second->id == 9);

    // A missing relationship is 2221.
    sent.clear();
    Answer(404, "");
    assert(baas::FetchRelationship(5).first == 2221);
    assert(sent[0].path == "/2.0.0/users/00000000000000aa/relationships/0000000000000005");
    Answer(200, R"({"isFriend":true})");
    assert(baas::FetchRelationship(5).second->is_friend);

    // Blocks.
    sent.clear();
    Answer(200, R"({"items":[{"targetUserId":"5","targetUser":{"nickname":"n"}}]})");
    assert(baas::SyncBlockList() == baas::Ok);
    assert(sent[0].path == "/1.0.0/users/00000000000000aa/blocks?count=100");
    assert(baas::BlockListAvailable() && baas::Blocks()->size() == 1);
    Answer(200, R"({"items":[{"targetUserId":"5"}]})");
    assert(baas::SyncBlockList() == baas::Unknown);
    assert(baas::Blocks()->size() == 1);

    // Warming: fetched once per id, answered from the cache after.
    sent.clear();
    Answer(200, R"({"items":[{"id":"7","nickname":"seven","thumbnailUrl":"t"}]})");
    baas::WarmUsers({7, 0});
    assert(sent.size() == 1 && sent[0].path == "/1.0.0/users?filter.id.$in=0000000000000007");
    assert(baas::CachedUser(7)->nickname == "seven");
    baas::WarmUsers({7});
    assert(sent.size() == 1);

    // A failure is retried on the next ask.
    Answer(500, "");
    baas::WarmUsers({8});
    Answer(200, R"({"items":[{"id":"8","nickname":"eight","thumbnailUrl":"t"}]})");
    baas::WarmUsers({8});
    assert(sent.size() == 3 && baas::CachedUser(8));

    // A user put in by hand keeps its picture: that is how the local player is drawn.
    baas::RememberUser({9, "nine", "https://cdn/1/9", {}});
    assert(baas::CachedUser(9)->thumbnail_url == "https://cdn/1/9");
}

void BlockCalls() {
    Reset();

    // The body: targetUserId and the reason, plus the route keys for an in-app block.
    const Json plain = Parse(baas::BlockBody(0xaaaaaaaaaaaa0002, "BAD_FRIEND", std::nullopt));
    assert(plain["targetUserId"] == "aaaaaaaaaaaa0002");
    assert(plain["extras"]["self"].size() == 1 && plain["extras"]["self"]["reason"] == "BAD_FRIEND");

    baas::Route route;
    route.application_id = 0x0100a5a020d5e000;
    route.acd_index = 1;
    route.presence_group_id = 0x0100a5a020d5e000;
    route.name = "Leia";
    route.language = "en-US";
    const Json in_app = Parse(baas::BlockBody(5, "IN_APP", route))["extras"]["self"];
    assert(in_app["route:appInfo:appId"] == "0100a5a020d5e000");
    assert(in_app["route:appInfo:acdIndex"] == 1);
    assert(in_app["route:name:language"] == "en-US");

    // A block posts, then re-syncs blocks, the friend list and both request boxes.
    Answer(201, "{}");
    Answer(200, R"({"items":[{"targetUserId":"aaaaaaaaaaaa0002","targetUser":{"nickname":"Leia"},"extras":{"self":{"reason":"BAD_FRIEND"}}}]})");
    Answer(200, R"({"items":[]})");
    Answer(200, R"({"items":[]})");
    Answer(200, R"({"items":[]})");
    assert(baas::BlockUser(0xaaaaaaaaaaaa0002, 2, std::nullopt) == baas::Ok);
    assert(sent[0].method == "POST" && sent[0].path == "/1.0.0/users/00000000000000aa/blocks");
    assert(sent[0].content_type == "application/json");
    assert(sent[1].path.find("/blocks?count=100") != std::string::npos);
    assert(sent[2].path.find("/friends?count=300") != std::string::npos);
    assert(sent[3].path.find("friend_requests/inbox") != std::string::npos);
    assert(baas::Blocks()->size() == 1 && baas::Blocks()->front().reason == 2);

    // Refused: remapped, and the optimistic entry goes with the re-sync.
    Reset();
    Answer(409, R"({"status":409,"errorCode":"resource_already_exists"})");
    Answer(200, R"({"items":[]})");
    assert(baas::BlockUser(7, 1, std::nullopt) == 2701);
    assert(baas::Blocks()->empty());

    // A reason outside the table never leaves.
    sent.clear();
    assert(baas::BlockUser(7, 9, std::nullopt) == baas::InvalidArgument);
    assert(sent.empty());

    // Unblocking one the server does not know is 2711, with nothing after it.
    Answer(404, R"({"status":404,"errorCode":"resource_is_not_found"})");
    assert(baas::UnblockUser(7) == 2711);
    assert(sent.size() == 1 && sent[0].method == "DELETE" &&
           sent[0].path == "/1.0.0/users/00000000000000aa/blocks/0000000000000007");
}

void OwnUser() {
    Reset();

    // No friend code yet: the one issued is kept.
    Answer(200, R"({"id":"aa","nickname":"me","thumbnailUrl":"t"})");
    Answer(200, R"({"id":"aa","nickname":"me","thumbnailUrl":"t","links":{"friendCode":{"id":"1234-5678-9012"}}})");
    assert(baas::SyncUserSetting() == baas::Ok);
    assert(sent.size() == 2);
    assert(sent[0].method == "GET" && sent[0].path == "/1.0.0/users/00000000000000aa");
    assert(sent[1].method == "POST" && sent[1].path == "/1.0.0/users/00000000000000aa/generate_code");
    assert(sent[1].content_type == "application/x-www-form-urlencoded" && sent[1].body == "type=NX");
    assert(baas::OwnUserSetting()->friend_code == "1234-5678-9012");

    // A PATCH reply replaces what is held; one that is not a user leaves it standing.
    sent.clear();
    const std::string body = baas::UserPatchBody("/permissions/presence", "FRIENDS");
    Answer(200, R"({"id":"aa","nickname":"me","thumbnailUrl":"t","permissions":{"presence":"FRIENDS"}})");
    assert(baas::PatchUser(body) == baas::Ok);
    assert(sent[0].method == "PATCH" && sent[0].path == "/1.0.0/users/00000000000000aa");
    assert(sent[0].content_type == "application/json-patch+json" && sent[0].body == body);
    assert(baas::OwnUserSetting()->presence_permission == 2);
    Answer(200, "not json");
    assert(baas::PatchUser(body) == baas::Ok);
    assert(baas::OwnUserSetting()->presence_permission == 2);
}

void InvitationCalls() {
    Reset();

    // The inbox, and each group it names fetched once.
    const std::string inbox = "{\"items\":[" + InvitationItem + "," +
                              Replace(InvitationItem, "1789287484056138", "1789287484056139") + "]}";
    const std::string group = Replace(GroupJson, "\"id\":9", "\"id\":1789287484056137");

    Answer(200, inbox);
    Answer(200, group);
    assert(baas::SyncInvitations() == baas::Ok);
    assert(sent.size() == 2);
    assert(sent[0].host == baas::FiveHost);
    assert(sent[0].path == "/v2/users/00000000000000aa/invitations/inbox?invitation_types=friend");
    assert(sent[1].path == "/v1/invitation_groups/1789287484056137");
    assert(baas::Invitations()->size() == 2);
    assert(baas::CachedInvitationGroup(1789287484056137ULL)->messages[0] == "hi");

    Answer(200, inbox);
    assert(baas::SyncInvitations() == baas::Ok);
    assert(sent.size() == 3);

    // A Five error maps through the Five table, and the inbox stays.
    Answer(401, R"({"error":{"code":"0002","message":"m"}})");
    assert(baas::SyncInvitations() == 2902);
    assert(baas::Invitations()->size() == 2);

    // One incomplete item fails the whole inbox.
    Answer(200, R"({"items":[{"id":1}]})");
    assert(baas::FetchInvitations().first == baas::Unknown);

    // Sending: keys in the module's order, empty application_data left out.
    sent.clear();
    Answer(201, "");
    assert(baas::SendInvitation({"aaaaaaaaaaaa0001"}, 0x0100ed9024eb8000ULL, 1,
                                0x0100ed9024eb8000ULL, {}, {{"en-US", "hi"}, {"ja", "\xE3\x81\x82"}},
                                true) == baas::Ok);
    assert(sent[0].method == "POST" && sent[0].host == baas::FiveHost);
    assert(sent[0].path == "/v2/invitation_groups" && sent[0].content_type == "application/json");
    assert(sent[0].body ==
           "{\"receiver_ids\":[\"aaaaaaaaaaaa0001\"],\"invitation_type\":\"friend\","
           "\"application_id\":\"0100ed9024eb8000\",\"acd_index\":1,"
           "\"application_group_id\":\"0100ed9024eb8000\","
           "\"messages\":{\"en-US\":\"hi\",\"ja\":\"\xE3\x81\x82\"},\"application_id_match\":true}");

    Answer(201, "");
    assert(baas::SendInvitation({"1"}, 1, 0, 1, {0x45, 0x43, 0x4b, 0x41, 0x41, 0x4c}, {}, false) ==
           baas::Ok);
    assert(sent[1].body.find(R"("application_group_id":"0000000000000001","application_data":"RUNLQUFM","messages":{})") != std::string::npos);

    // A body over 0x2400 bytes never leaves.
    const std::size_t before = sent.size();
    assert(baas::SendInvitation({"1"}, 1, 0, 1, std::vector<std::uint8_t>(0x2000, 1), {}, false) ==
           baas::TooLarge);
    assert(sent.size() == before);

    // Marking read: decimal ids joined by %2C, at most 100, or the mark-all form.
    sent.clear();
    std::vector<std::uint64_t> ids;
    for (std::uint64_t id = 1; id <= 101; id++) {
        ids.push_back(id);
    }
    Answer(200, "");
    assert(baas::ReadInvitations({1789287484056138ULL, 2}) == baas::Ok);
    assert(sent[0].method == "PATCH" && sent[0].path == "/v1/invitations");
    assert(sent[0].content_type == "application/x-www-form-urlencoded");
    assert(sent[0].body == "read=true&ids=1789287484056138%2C2");
    Answer(200, "");
    assert(baas::ReadInvitations(ids) == baas::Ok);
    assert(sent[1].body.ends_with("%2C100"));
    Answer(200, "");
    assert(baas::ReadInvitations({}) == baas::Ok);
    assert(sent[2].path == "/v1/users/00000000000000aa/invitations/mark_as_read");
    assert(sent[2].content_type == "application/json" && sent[2].body.empty());
}

void FriendRequestCalls() {
    Reset();
    int arrivals = 0;
    baas::SetFriendRequestArrivedHook([&] { arrivals++; });

    const std::string unread = Replace(InboxItem, "\"read\":true", "\"read\":false");

    // Both boxes, one malformed item skipped, the new unread one announced.
    Answer(200, "{\"items\":[" + unread + ",{\"id\":\"b2\"}]}");
    Answer(200, R"({"items":[]})");
    baas::RefreshFriendRequests();
    assert(sent.size() == 2);
    assert(sent[0].path == "/2.0.0/users/00000000000000aa/friend_requests/inbox?offset=0&count=100&sort=createdAt:desc&filter.state.$eq=PENDING");
    assert(sent[1].path == "/2.0.0/users/00000000000000aa/friend_requests/outbox?offset=0&count=100&sort=createdAt:desc&filter.state.$eq=PENDING");
    assert(baas::InboxRequests()->size() == 1 && baas::OutboxRequests()->empty());
    assert(arrivals == 1);

    // The same box again says nothing.
    Answer(200, "{\"items\":[" + unread + "]}");
    Answer(200, R"({"items":[]})");
    baas::RefreshFriendRequests();
    assert(arrivals == 1);

    // A failed box leaves both as they were.
    Answer(500, "");
    baas::RefreshFriendRequests();
    assert(baas::InboxRequests()->size() == 1);

    // Answering: PATCH the state, then both boxes again.
    sent.clear();
    Answer(200, InboxItem);
    Answer(200, R"({"items":[]})");
    Answer(200, R"({"items":[]})");
    baas::AnswerFriendRequest(0xa1, "AUTHORIZED");
    assert(sent.size() == 3);
    assert(sent[0].method == "PATCH" && sent[0].path == "/2.0.0/friend_requests/00000000000000a1");
    assert(sent[0].content_type == "application/json-patch+json");
    assert(sent[0].body == R"([{"op":"replace","path":"/state","value":"AUTHORIZED"}])");
    assert(baas::InboxRequests()->empty());

    // A state the module never sends is not sent.
    sent.clear();
    baas::AnswerFriendRequest(0xa1, "CANCELLED");
    assert(sent.empty());

    // A write that failed is not followed by a refresh.
    Answer(400, "");
    baas::ReadFriendRequest(0xa1);
    assert(sent.size() == 1);
    assert(sent[0].body == R"([{"op":"add","path":"/extras/receiver/read","value":true}])");

    // Sending to somebody on the friend list: their id is already the BAAS user.
    Answer(200, "{\"items\":[" + FriendItem + "]}");
    assert(baas::SyncFriendList(true) == baas::Ok);
    assert(baas::ReceiverId(0xaaaaaaaaaaaa0002ULL) == "aaaaaaaaaaaa0002");
    // Anybody else, with nothing linked to look them up through, goes as given.
    assert(baas::ReceiverId(0x42) == "0000000000000042");

    sent.clear();
    baas::FriendRequestSend send;
    send.target_id = 0xaaaaaaaaaaaa0002ULL;
    send.channel = "FRIEND_CODE";
    Answer(201, R"({"id":"c3","channels":["FRIEND_CODE"],"state":"PENDING","receiverId":"aaaaaaaaaaaa0002",
        "receiver":{"nickname":"n","thumbnailUrl":"t"}})");
    baas::SendFriendRequest(send);
    assert(sent.size() == 1);
    assert(sent[0].method == "POST" && sent[0].path == "/2.0.0/friend_requests");
    assert(sent[0].content_type == "application/json");
    assert(sent[0].body == baas::SendRequestBody("FRIEND_CODE", 0xaa, 0xaaaaaaaaaaaa0002ULL));

    // The receiver had already asked: AUTHORIZED, and the boxes are read at once.
    sent.clear();
    send.channel = "IN_APP";
    send.application_id = 0x0100a5a020d5e000ULL;
    send.presence_group_id = 0x0100a5a020d5e000ULL;
    send.target_name = "Seen";
    send.target_language = "en-US";
    send.own_name = "Mine";
    send.own_language = "ja";
    Answer(201, R"({"id":"c3","channels":["IN_APP"],"state":"AUTHORIZED","receiverId":"aaaaaaaaaaaa0002",
        "receiver":{"nickname":"n","thumbnailUrl":"t"}})");
    Answer(200, R"({"items":[]})");
    Answer(200, R"({"items":[]})");
    baas::SendFriendRequest(send);
    assert(sent.size() == 3);
    assert(sent[0].body == baas::SendRequestBody("IN_APP", 0xaa, 0xaaaaaaaaaaaa0002ULL,
                                                 0x0100a5a020d5e000ULL, 0, 0x0100a5a020d5e000ULL,
                                                 "Seen", "en-US", "Mine", "ja"));

    // The catalog and NNID routes each carry their own extras.
    sent.clear();
    send.catalog_id = "0100a5a020d5e0000100a5a020d5e000";
    baas::SendFriendRequest(send);
    assert(sent[0].body.find("route:candidate:catalogId") != std::string::npos);
    sent.clear();
    send.catalog_id.clear();
    send.mii_name = "Mii";
    send.mii_image_url_param = "p";
    baas::SendFriendRequest(send);
    assert(sent[0].body == baas::SendRequestNnidBody("IN_APP", 0xaa, 0xaaaaaaaaaaaa0002ULL, "Mii", "p"));

    // Over 0x400 bytes it never leaves.
    sent.clear();
    send.mii_name.clear();
    send.own_name = std::string(0x400, 'x');
    baas::SendFriendRequest(send);
    assert(sent.empty());

    baas::SetFriendRequestArrivedHook({});
}

void AttachDetach() {
    Reset("00000000000000aa");
    Answer(200, "{\"items\":[" + FriendItem + "]}");
    assert(baas::SyncFriendList(true) == baas::Ok);
    Answer(200, "{\"items\":[" + InvitationItem + "]}");
    Answer(500, "");
    assert(baas::SyncInvitations() == baas::Ok);

    // The same user again keeps what it had.
    baas::Attach("00000000000000aa");
    assert(baas::Friends()->size() == 1 && baas::Invitations()->size() == 1);

    // Another user reads nothing of the last one's.
    baas::Attach("00000000000000bb");
    assert(baas::UserId() == "00000000000000bb");
    assert(baas::Friends()->empty() && !baas::FriendListAvailable());
    assert(baas::Invitations()->empty() && !baas::CachedUser(7));

    baas::Detach();
    assert(!baas::Ready() && baas::UserId().empty());
}

// ---- the background beat (the session's heartbeat calls this every third beat) ----

bool SentTo(std::size_t index, const std::string& part) {
    return index < sent.size() && sent[index].path.find(part) != std::string::npos;
}

void BackgroundSyncOrder() {
    Reset("00000000000000aa");
    Answer(200, R"({"items":[]})"); // inbox
    Answer(200, R"({"items":[]})"); // outbox
    Answer(200, "{\"items\":[" + FriendItem + "]}");
    Answer(200, R"({"items":[]})"); // blocks
    Answer(200, R"({"id":"00000000000000aa","nickname":"Me","thumbnailUrl":"https://example/me.jpg",
 "links":{"friendCode":{"id":"1234-5678-9012"}}})");
    Answer(200, R"({"items":[]})"); // invitations

    baas::BackgroundSync();

    // The earlier port's order: the request boxes, the friend list at its cooldown, then the module
    // caches -- blocks, the own user, the invitation inbox.
    assert(SentTo(0, "/friend_requests/inbox"));
    assert(SentTo(1, "/friend_requests/outbox"));
    assert(SentTo(2, "/2.0.0/users/00000000000000aa/friends?count=300"));
    assert(SentTo(3, "/1.0.0/users/00000000000000aa/blocks"));
    assert(SentTo(4, "/1.0.0/users/00000000000000aa"));
    assert(SentTo(5, "/invitations/inbox"));
    assert(baas::FriendListAvailable() && baas::BlockListAvailable() && baas::OwnUserSetting());

    // The next beat inside the 30 s cooldown leaves the friend list alone.
    sent.clear();
    baas::BackgroundSync();
    for (const auto& one : sent) {
        assert(one.path.find("/friends?count=300") == std::string::npos);
    }

    // Nothing signed in, nothing asked.
    baas::Detach();
    sent.clear();
    baas::BackgroundSync();
    assert(sent.empty());
}

void RunsInTheBackground() {
    std::mutex mutex;
    std::condition_variable done;
    std::vector<int> order;
    const std::thread::id caller = std::this_thread::get_id();
    bool elsewhere = true;

    for (int i = 0; i < 3; i++) {
        baas::RunInBackground([&, i] {
            std::lock_guard lock{mutex};
            elsewhere = elsewhere && std::this_thread::get_id() != caller;
            order.push_back(i);
            done.notify_all();
        });
    }

    // One that throws is dropped, and the worker carries on.
    baas::RunInBackground([] { throw std::runtime_error("dropped"); });
    baas::RunInBackground([&] {
        std::lock_guard lock{mutex};
        order.push_back(3);
        done.notify_all();
    });

    std::unique_lock lock{mutex};
    assert(done.wait_for(lock, std::chrono::seconds(10), [&] { return order.size() == 4; }));
    assert((order == std::vector<int>{0, 1, 2, 3}));
    assert(elsewhere);
}

} // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "openpak-baas-test";
    std::filesystem::remove_all(dir);
    openpak::Platform::SetDirectories(dir, dir / "cache");

    Ids();
    FriendList();
    PlayLogs();
    Requests();
    Users();
    Blocks();
    Invitations();
    RequestBodies();
    ChannelTable();
    Results();

    Install();
    NotSignedIn();
    FriendListSync();
    UsersAndBlocks();
    BlockCalls();
    OwnUser();
    InvitationCalls();
    FriendRequestCalls();
    AttachDetach();
    BackgroundSyncOrder();
    RunsInTheBackground();

    std::filesystem::remove_all(dir);
    return 0;
}
