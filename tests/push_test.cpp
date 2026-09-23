// SPDX-License-Identifier: GPL-2.0-or-later
// The Penne frontline frames (openpak/push.h), read against frames nx-baas's own builders
// produced: a kind-1 message inside a PutRecord, the opening HandoverResult, and a Pong. Ported
// from Ryujinx's OpenPakPushTests.cs.
#undef NDEBUG
#include <cassert>
#include <string>
#include <vector>

#include "openpak/push.h"

namespace push = openpak::client::push;

namespace {

std::vector<std::uint8_t> Hex(const std::string& text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// penne.AppendFrame(penne.BuildMessage(...)) for a friend_request_received delivery.
const std::string MessageFrame = "50010000180000000000000000000e002000070008000c00100018000e000000000000042c00000018000000803bb16a000000009049b16a0000000000000000020000004e58000008000c0004000b00080000000800000000000001010000000c00000008000c000700080008000000000000011400000000000e00140004000a000c00000010000e000000640000000000010088000000040000004e0000007b2274797065223a22667269656e645f726571756573745f7265636569766564222c226964223a22636f72653a37222c22667269656e644964223a2261616161616161616161616130303032227d000017000000667269656e645f726571756573745f72656365697665640000000e00100000000400080000000c000e0000001c000000100000000400000000000000000000000000000000000000120000003078303130303030303030303030303030650000";
const std::string HandoverFrame = "280000000c00000008000c0007000800080000000000000c0c00000000000600080007000600000000000000";
const std::string PongFrame = "30000000100000000000000008000c000700080008000000000000110c000000000006000c000400060000000500000000000000";

std::string One(const std::string& hex) {
    push::FrameReader reader;
    assert(reader.Feed(Hex(hex)));
    auto frame = reader.Next();
    assert(frame);
    return *frame;
}

std::span<const std::uint8_t> Span(const std::string& frame) {
    return {reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size()};
}

} // namespace

int main() {
    const std::string message = One(MessageFrame);
    assert(push::EnvelopeType(Span(message)) == push::TypePutRecord);
    const auto read = push::ReadMessage(Span(message));
    assert(read && read->name == "friend_request_received");
    assert(read->body.find("\"friendId\":\"aaaaaaaaaaaa0002\"") != std::string::npos);
    assert(push::Kind(*read) == "friend_request_received");

    const std::string handover = One(HandoverFrame);
    assert(push::EnvelopeType(Span(handover)) == push::TypeHandoverResult);
    assert(!push::ReadMessage(Span(handover)));
    assert(!push::ReadMessage(Span(One(PongFrame))));

    // Frames follow one another, and arrive in pieces.
    push::FrameReader reader;
    const auto both = Hex(HandoverFrame + MessageFrame);
    for (std::size_t at = 0; at < both.size(); at += 7) {
        const std::size_t size = std::min<std::size_t>(7, both.size() - at);
        assert(reader.Feed({both.data() + at, size}));
    }
    assert(reader.Next() == handover);
    assert(reader.Next() == message);
    assert(!reader.Next());

    // A zero length is a broken stream.
    push::FrameReader zero;
    assert(!zero.Feed(Hex("00000000")));

    // Nothing cut short or made up is read outside itself.
    const auto garbage = Hex(MessageFrame.substr(8));
    for (std::size_t cut = 0; cut < garbage.size(); cut += 3) {
        push::ReadMessage({garbage.data(), cut});
    }
    const std::vector<std::uint8_t> wild{0xff, 0xff, 0xff, 0x7f, 1, 2, 3, 4};
    assert(!push::ReadMessage(wild));

    assert(push::Kind({"friend_deleted", "not json"}) == "friend_deleted");
    return 0;
}
