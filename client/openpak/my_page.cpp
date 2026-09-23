// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-2.0-or-later

#include "openpak/my_page.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

#include "openpak/baas.h"
#include "openpak/log.h"

namespace openpak::my_page {

namespace {

constexpr std::size_t DataMax = 0x400;
constexpr std::size_t DescriptionSlot = 0xC0;

// The GameModeDescription slots, in the order the friends module and the Unity enum agree on.
constexpr const char* kLanguages[] = {
    "en-US", "en-GB", "ja", "fr", "de", "es-419", "es", "it",
    "nl",    "fr-CA", "pt", "ru", "zh-Hans", "zh-Hant", "ko", "pt-BR",
};

std::mutex g_mutex;
FriendPicker g_picker;

template <typename T>
T Read(std::span<const std::uint8_t> bytes, std::size_t offset) {
    T value{};
    if (offset + sizeof(T) <= bytes.size()) {
        std::memcpy(&value, bytes.data() + offset, sizeof(T)); // the guest is little-endian
    }
    return value;
}

std::vector<std::uint8_t> Data(std::span<const std::uint8_t> argument, std::size_t size_offset,
                               std::size_t data_offset) {
    const auto size = static_cast<std::size_t>(
        std::min<std::uint64_t>(Read<std::uint64_t>(argument, size_offset), DataMax));
    if (data_offset + size > argument.size()) {
        return {};
    }
    return {argument.begin() + static_cast<std::ptrdiff_t>(data_offset),
            argument.begin() + static_cast<std::ptrdiff_t>(data_offset + size)};
}

/// The GameModeDescription's filled slots: UTF-8, NUL-terminated, empty ones skipped.
std::vector<std::pair<std::string, std::string>> Messages(std::span<const std::uint8_t> argument,
                                                          std::size_t offset) {
    std::vector<std::pair<std::string, std::string>> messages;
    for (std::size_t slot = 0; slot < std::size(kLanguages); ++slot) {
        const std::size_t start = offset + slot * DescriptionSlot;
        if (start + DescriptionSlot > argument.size()) {
            break;
        }
        const auto* text = reinterpret_cast<const char*>(argument.data() + start);
        const std::size_t length = strnlen(text, DescriptionSlot);
        if (length > 0) {
            messages.emplace_back(kLanguages[slot], std::string(text, length));
        }
    }
    return messages;
}

std::uint32_t Send(const std::vector<std::string>& receivers,
                   const std::vector<std::uint8_t>& data,
                   const std::vector<std::pair<std::string, std::string>>& messages,
                   std::uint64_t application_id, std::uint64_t presence_group_id) {
    if (receivers.empty()) {
        return ResultCancelled;
    }
    const int result = baas::SendInvitation(
        receivers, application_id, 0, presence_group_id != 0 ? presence_group_id : application_id,
        data, messages, false);
    return result == baas::Ok ? 0 : ResultCancelled;
}

} // namespace

void SetFriendPicker(FriendPicker picker) {
    std::lock_guard lock{g_mutex};
    g_picker = std::move(picker);
}

std::optional<std::uint32_t> Run(std::span<const std::uint8_t> argument,
                                 std::uint64_t application_id, std::uint64_t presence_group_id) {
    const auto type = Read<std::uint32_t>(argument, 0);
    try {
        if (type == 8) {
            // StartFriendInvitation: the person picks up to maxInviteeCount friends.
            const int max = std::clamp(Read<std::int32_t>(argument, 0x18), 1, 16);
            const auto data = Data(argument, 0x20, 0x28);
            const auto messages = Messages(argument, 0x428);
            OPENPAK_LOG_INFO("MyPage StartFriendInvitation: up to {}, {} bytes of data", max,
                             data.size());

            FriendPicker picker;
            {
                std::lock_guard lock{g_mutex};
                picker = g_picker;
            }
            auto picked = picker ? picker(max) : std::vector<std::uint64_t>{};
            if (picked.size() > static_cast<std::size_t>(max)) {
                picked.resize(static_cast<std::size_t>(max));
            }
            std::vector<std::string> receivers;
            for (const auto id : picked) {
                receivers.push_back(baas::ReceiverId(id));
            }
            if (receivers.empty()) {
                OPENPAK_LOG_INFO("MyPage StartFriendInvitation: cancelled");
            }
            return Send(receivers, data, messages, application_id, presence_group_id);
        }
        if (type == 9) {
            // StartSendingFriendInvitation: the game chose the invitees itself.
            const int count = std::clamp(Read<std::int32_t>(argument, 0x18), 0, 16);
            const auto data = Data(argument, 0xA0, 0xA8);
            const auto messages = Messages(argument, 0x4A8);
            OPENPAK_LOG_INFO("MyPage StartSendingFriendInvitation: {} invitee(s), {} bytes of data",
                             count, data.size());
            std::vector<std::string> receivers;
            for (int index = 0; index < count; ++index) {
                receivers.push_back(baas::ReceiverId(
                    Read<std::uint64_t>(argument, 0x20 + static_cast<std::size_t>(index) * 8)));
            }
            return Send(receivers, data, messages, application_id, presence_group_id);
        }
    } catch (const std::exception& e) {
        // A game waiting on a library applet that never returns is a hung game: whatever went
        // wrong is at most a non-zero Result to it.
        OPENPAK_LOG_WARNING("MyPage type {}: {}", type, e.what());
        return ResultCancelled;
    }

    OPENPAK_LOG_INFO("MyPage type {} requested; the friend screens are in the OpenPak window", type);
    return type == 2 ? std::optional<std::uint32_t>{0} : std::nullopt;
}

} // namespace openpak::my_page
