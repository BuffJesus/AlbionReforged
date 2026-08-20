#include "f2/native_mod_menu.h"

#include "f2/native_script.h"

namespace f2 {
namespace {

// Discovery runs IN Lua (it is reflection over live tables) and reports back through a global
// string, one entry per line: category \t label \t action \t detail.
//
// Sources, all of them the game's own data:
//   * Gameflow.DebugQuestStartTable — rows written by Gameflow:RegisterDebugQuest(enum, questName,
//     gameflowName, level, startMarker). Every quest the game itself declares jumpable.
//   * Gameflow.ChildhoodVars.SkipTo* — the game's own skip functions, which set real gameflow
//     state (sub-quest completion flags, gold counter, Rose's follow state, the level handoff).
//   * __f2_modmenu_entries — whatever mods registered via ModMenu.Register.
// Nothing is hardcoded, so the menu tracks the data and any loaded mod automatically.
constexpr const char* kDiscover = R"LUA(
__f2_modmenu = ""
local out = {}
local function esc(s) return (tostring(s):gsub('[\t\n]', ' ')) end
local function add(cat, label, action, detail)
  out[#out + 1] = esc(cat) .. '\t' .. esc(label) .. '\t' .. action .. '\t' .. esc(detail or '')
end

-- 1. The game's own skip functions (any SkipTo* on ChildhoodVars).
if Gameflow and type(rawget(Gameflow, 'ChildhoodVars')) == 'table' then
  local names = {}
  for k, v in pairs(Gameflow.ChildhoodVars) do
    if type(k) == 'string' and type(v) == 'function' and k:sub(1, 6) == 'SkipTo' then
      names[#names + 1] = k
    end
  end
  table.sort(names)
  for _, k in ipairs(names) do
    add('Skip', k, 'Gameflow.ChildhoodVars.' .. k .. '()', 'the game\'s own skip function')
  end
end

-- 2. Every quest the gameflow registers as jumpable.
if Gameflow and type(rawget(Gameflow, 'DebugQuestStartTable')) == 'table' then
  local rows = {}
  for k, v in pairs(Gameflow.DebugQuestStartTable) do
    if type(v) == 'table' and type(rawget(v, 'QuestName')) == 'string' then
      rows[#rows + 1] = {k = k, q = v.QuestName, g = rawget(v, 'GameflowName'),
                         lv = rawget(v, 'TeleportName'), mk = rawget(v, 'MarkerName')}
    end
  end
  table.sort(rows, function(a, b) return a.q < b.q end)
  for _, r in ipairs(rows) do
    local detail = tostring(r.lv or '?') .. ' / ' .. tostring(r.mk or '?')
    add('Quest', r.q, 'Gameflow:StartQuest(' ..
        (type(r.k) == 'number' and tostring(r.k) or string.format('%q', tostring(r.k))) .. ')',
        detail)
  end
end

-- 3. Mod-registered entries.
if type(__f2_modmenu_entries) == 'table' then
  for i, e in ipairs(__f2_modmenu_entries) do
    if type(e) == 'table' and type(e.fn) == 'function' then
      add(e.category or 'Mod', e.label or ('mod entry ' .. i),
          '__f2_modmenu_entries[' .. i .. '].fn()', e.detail or '')
    end
  end
end

__f2_modmenu = table.concat(out, '\n')
)LUA";

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t at = s.find(sep, start);
        if (at == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, at - start));
        start = at + 1;
    }
    return out;
}

}  // namespace

int ModMenu::rebuild(NativeScriptVM& vm) {
    entries_.clear();
    last_error_.clear();
    if (!vm.valid()) return 0;
    if (!vm.run_source(kDiscover, "=modmenu_discover")) {
        last_error_ = vm.last_error();
        return 0;
    }
    const std::string blob = vm.global_string("__f2_modmenu");
    if (blob.empty()) return 0;
    for (const std::string& line : split(blob, '\n')) {
        if (line.empty()) continue;
        const std::vector<std::string> col = split(line, '\t');
        if (col.size() < 3) continue;
        entries_.push_back({col[0], col[1], col[2], col.size() > 3 ? col[3] : std::string{}});
    }
    return static_cast<int>(entries_.size());
}

bool ModMenu::invoke(NativeScriptVM& vm, std::size_t index) {
    last_error_.clear();
    if (index >= entries_.size()) {
        last_error_ = "mod menu: index out of range";
        return false;
    }
    if (!vm.valid()) {
        last_error_ = "mod menu: no script VM";
        return false;
    }
    // Run under pcall and REPORT the error rather than swallowing it. Silent failure is exactly
    // what hid the childhood's frame-0 death for this whole project; the menu will not repeat it.
    const std::string chunk =
        "__f2_modmenu_err = nil local ok, err = pcall(function() " + entries_[index].action +
        " end) if not ok then __f2_modmenu_err = tostring(err) end";
    if (!vm.run_source(chunk.c_str(), "=modmenu_invoke")) {
        last_error_ = vm.last_error();
        return false;
    }
    const std::string err = vm.global_string("__f2_modmenu_err");
    if (!err.empty()) {
        last_error_ = err;
        return false;
    }
    return true;
}

}  // namespace f2
