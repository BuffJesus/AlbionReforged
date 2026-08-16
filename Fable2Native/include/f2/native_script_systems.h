#pragma once

// Master gameplay script-systems tick driver.
//
// Faithful transcription of the retail script-systems tick hub Function_82281FE0
// (ghidra_out/gamestate_save_restore.txt + quest_system.txt + p2_feel_constants.txt).
// The hub runs three gated managers each frame in a FIXED order:
//     QUEST (gate +0x183) -> GENERAL (gate +0x184) -> AI (gate +0x182)
// each a gated resume (skipped when its enable byte is clear).
//
// This driver reserves that exact ordering seam. The managers are INERT
// placeholders today (the standalone native app embeds no Lua VM yet); real
// quest/general/AI logic drops into `update` without reshaping the frame. The
// full Lua embed is a later phase (P6).
//
// NB: 0x82ca93fc / 0x82ca9408 near the hub are codegen thunks (empty stub /
// __savegprlr_28), NOT tick steps — deliberately excluded.

#include <functional>

namespace f2 {

// One gated script manager slot. `enabled` mirrors the retail per-manager gate
// byte; `update` is the resume callback (empty = inert).
struct ScriptManager {
    bool enabled = false;
    std::function<void(double)> update;  // dt in seconds
};

// Runs the three managers in the retail-recovered order every fixed step.
struct ScriptSystems {
    ScriptManager quest;    // gate +0x183, runs first
    ScriptManager general;  // gate +0x184, runs second
    ScriptManager ai;       // gate +0x182, runs last

    // Executes QUEST -> GENERAL -> AI, invoking each enabled manager's update.
    // Order is load-bearing and asserted by tests.
    void tick(double dt) {
        if (quest.enabled && quest.update) quest.update(dt);
        if (general.enabled && general.update) general.update(dt);
        if (ai.enabled && ai.update) ai.update(dt);
    }
};

}  // namespace f2
