#pragma once

// The mod / debug-jump menu MODEL (no UI — a renderer draws whatever this reports).
//
// The recomp shipped a pause-menu "Mod Menu" folder with a hardcoded set of actions. This is the
// native replacement, and it is better in three specific ways:
//
//   1. DATA-DRIVEN, not hardcoded. Entries are discovered from the GAME'S OWN tables at runtime:
//      * `Gameflow.DebugQuestStartTable` — every quest the game itself registers as jumpable, via
//        Gameflow:RegisterDebugQuest(enum, questName, gameflowName, level, startMarker)
//        (gameflow.lua main.proto[2]; the childhood is registered at instrs 274-281 as
//        {QC010_Childhood, Childhood, BWSSlums, QC010_ChildhoodStart}). So the list stays correct
//        for every quest, including ones nobody hand-listed, and it cannot drift from the data.
//      * `Gameflow.ChildhoodVars.SkipTo*` — the game's OWN skip functions
//        (SkipToWino / SkipToLL / SkipToLL2 / SkipToBuyMusicBox / SkipToLuciensStudy,
//        qc010_childhood.lua:174-237). Each sets real gameflow state — sub-quest completion flags,
//        the gold counter, Rose's follow state, the level handoff — so invoking one is the game
//        skipping itself, not us faking a state we guessed at.
//
//   2. MOD-EXTENSIBLE. A mod adds its own entry from Lua with
//      `ModMenu.Register(category, label, function() ... end)`, so mods extend the menu without
//      touching the port. Mod entries are listed alongside the game's own.
//
//   3. HONEST ABOUT FAILURE. Every action runs under pcall and the error is REPORTED
//      (last_error()), not swallowed. Silent failure is what hid the childhood's frame-0 death
//      for this entire project; the menu does not repeat it.
//
// Discovery is by REFLECTION over live Lua tables, so nothing here needs updating when the game
// data or a mod changes. It is safe to call rebuild() at any time; entries are stable ids.

#include <string>
#include <vector>

namespace f2 {

class NativeScriptVM;

struct ModMenuEntry {
    std::string category;  // "Skip", "Quest", "Mod" — for grouping in whatever draws this
    std::string label;     // human text
    std::string action;    // the Lua expression to run (built by discovery, never user input)
    std::string detail;    // e.g. the level + start marker a quest jump uses; may be empty
};

class ModMenu {
public:
    // Re-discover entries from the live VM. Returns the number found. Safe if scripting is not
    // booted (returns 0). Never throws; a malformed table is skipped.
    int rebuild(NativeScriptVM& vm);

    [[nodiscard]] const std::vector<ModMenuEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    // Run entry `index`. Returns false and sets last_error() if the index is out of range or the
    // action raised. The action runs in the VM's global state, exactly as the game's own debug
    // paths do.
    bool invoke(NativeScriptVM& vm, std::size_t index);

private:
    std::vector<ModMenuEntry> entries_;
    std::string last_error_;
};

}  // namespace f2
