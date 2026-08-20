#pragma once

// The message-event bus that quest coroutines poll — the heart of the game's Lua
// scheduling. Grounded in ghidra_out (MessageEvents.PostMessage @0x823FF158,
// IsMessagePosted @0x8229BD08, GetMostRecentMessageID @0x821C7B00) and the decompiled
// QuestThreadBase wait idioms (questmanager.lua WaitForMessage/WaitForTriggerToFire/
// HasTriggerFired): a quest yields each frame and polls
//   local ev = MessageEvents.IsMessageSentTo(TYPE, entity, lastSeenId)
//   if ev then lastSeenId = ev:GetID(); ...react... else coroutine.yield() end
// So the consumer contract is: return the newest matching message with id > lastSeenId, or
// nil. Ids are monotonic; the per-consumer "last id" cursor makes already-seen messages
// invisible without any explicit consume/clear.

#include <cstdint>
#include <vector>

namespace f2 {

// One posted message. `type` is an EMessageEventType value (defined by the game's
// MessageEventEnum.lua). sent_by/sent_to are entity uids (0 = unspecified). extra is the
// numeric payload read as event:GetExtraDataAsNumber().
struct GameMessage {
    std::uint32_t id = 0;
    int type = 0;
    std::uint64_t sent_by = 0;
    std::uint64_t sent_to = 0;
    double extra = 0.0;
};

class MessageBus {
public:
    // Post a message; returns its (monotonic, non-zero) id.
    std::uint32_t post(int type, std::uint64_t sent_by, std::uint64_t sent_to, double extra) {
        GameMessage m{++last_id_, type, sent_by, sent_to, extra};
        messages_.push_back(m);
        if (messages_.size() > kCap)
            messages_.erase(messages_.begin(),
                            messages_.begin() + static_cast<std::ptrdiff_t>(messages_.size() - kCap));
        return m.id;
    }

    [[nodiscard]] std::uint32_t most_recent_id() const noexcept { return last_id_; }

    // Every retained message, oldest first. GetAllMessages(type, afterId) scans this — the
    // cutscene-finished protocol needs the whole matching set, not just the newest
    // (miscfunctions.lua:161-191).
    [[nodiscard]] const std::vector<GameMessage>& all() const noexcept { return messages_; }

    // Newest message of `type` with id > after_id, optionally constrained to a recipient
    // (to) and/or sender (by) uid. 0 for to/by means "don't care". Null if none.
    [[nodiscard]] const GameMessage* find(int type, std::uint32_t after_id,
                                          std::uint64_t to, std::uint64_t by) const noexcept {
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            if (it->id <= after_id) break;  // messages_ is id-ordered; nothing older matches
            if (it->type != type) continue;
            if (to != 0 && it->sent_to != to) continue;
            if (by != 0 && it->sent_by != by) continue;
            return &*it;
        }
        return nullptr;
    }

    [[nodiscard]] const GameMessage* by_id(std::uint32_t id) const noexcept {
        for (const auto& m : messages_)
            if (m.id == id) return &m;
        return nullptr;
    }

    void clear() noexcept { messages_.clear(); last_id_ = 0; }

private:
    static constexpr std::size_t kCap = 256;  // rolling cap; poll-cursor consumers only look forward
    std::vector<GameMessage> messages_;
    std::uint32_t last_id_ = 0;
};

}  // namespace f2
