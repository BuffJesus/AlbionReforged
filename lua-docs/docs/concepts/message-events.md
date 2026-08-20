# Message events

The **message-event bus** is the heart of quest scheduling. Quests don't block on
threads or timers directly — they **poll** a message queue each frame and `yield`
until the event they're waiting for shows up. Almost every wait in the game routes
through it.

## The consumer contract

A quest polls with a *type* and the *last id it has seen*, and gets back the newest
matching event (or `nil`):

```lua
-- QuestThreadBase.WaitForTriggerToFire (condensed)
while true do
  local ev = MessageEvents.IsMessageSentBy(
               EMessageEventType.MESSAGE_EVENT_TRIGGERED, trigger, self.LastSeenId)
  if ev then
    self.LastSeenId = ev:GetID()      -- advance the cursor; react
    break
  else
    coroutine.yield()                 -- nothing yet; try again next frame
  end
end
```

The **last-id cursor** is what makes this work without an explicit consume step:
each poll asks for messages *newer than* what it has already handled, so old
messages are invisible and there's no shared mutable "read" flag.

## The query surface

| Native | Returns |
|---|---|
| `MessageEvents.GetMostRecentMessageID()` | the current tail id (a number) |
| `MessageEvents.IsMessagePosted(type, afterId)` | newest `type` event with `id > afterId`, or `nil` |
| `MessageEvents.IsMessageSentTo(type, entity, afterId)` | …constrained to a recipient |
| `MessageEvents.IsMessageSentBy(type, entity, afterId)` | …constrained to a sender |
| `MessageEvents.PostMessage(type, extra, sentBy, sentTo)` | posts a message (producer side) |

An **`Event`** object supports `:GetID()`, `:GetExtraDataAsNumber()`, and
`:GetEntitySentBy()`.

The event *types* (`EMessageEventType.MESSAGE_EVENT_*`) are **defined by the game's
own Lua** (`miscellaneous/messageeventenum.lua`), loaded at boot — the runtime does
not hardcode them. That matters: a native producer must post with the **enum value
from Lua**, not a guessed constant, so producers and consumers agree.

## The native bus

The runtime implements the queue in C++ (`MessageBus`, `native_message.h`): an
append-only list of `{id, type, sent_by, sent_to, extra}` with monotonically
increasing ids and a rolling cap. Lookups scan newest-first and stop at the cursor:

```cpp
struct GameMessage { uint32_t id; int type; uint64_t sent_by, sent_to; double extra; };
// find newest of `type` with id > after_id, optionally matching to/by
const GameMessage* MessageBus::find(int type, uint32_t after, uint64_t to, uint64_t by);
```

## Releasing a waiting quest

Because quests block on the bus, you release one by posting the event it awaits —
**symbolically, from Lua**, using the real enum:

```lua
-- e.g. release a "killed" wait for a specific entity
MessageEvents.PostMessage(EMessageEventType.MESSAGE_EVENT_KILLED, 0, nil, evilTwin)
```

On the next `QuestManager.Update`, the quest's poll finds the message, advances its
cursor, and falls through its wait.

!!! note "Where"
    `Fable2Native/include/f2/native_message.h` (the bus) and the `MessageEvents.*` /
    `Event` bindings in `Fable2Native/src/native_bindings.cpp`.
