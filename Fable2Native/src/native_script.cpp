#include "f2/native_script.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <fstream>
#include <iterator>
#include <unordered_set>

namespace {
#include "f2/native_lua_catalog.inc"  // kNativeClassNames[], kNativeGlobalNames[]

// The set of every name the retail binary exposes as a Lua native (class or global),
// from ghidra_out/lua_natives5_catalog.tsv, plus a manual supplement for native CLASSES the
// catalog records only by their bare method names (so it never tagged the class itself).
// The auto-stub only fabricates a stub for names IN this set; everything else reads as nil so
// the game's own script-defined globals (quest types, managers) register without collision.
//
// Native CLASS tables the catalog records only by bare method names (so it never tagged the
// class) — plus a few whose defining scripts aren't in our boot set. Shared between
// native_name_set() (answers __is_native) and the autostub PRE-CREATE loop (raw _G stubs that
// survive the save system's _G metatable swap). Real ones we register (Debug/MessageEvents/...)
// are skipped at pre-create by a rawget guard. FLAGGED: AIManager/CameraManager/Gossip/etc. are
// no-op stubs until those systems are wired. NOTE: BaseObjects is NOT stubbed (aliased to _G).
const char* const kSupplementClassNames[] = {
    "GUI", "Debug", "MessageEvents", "Timing", "TutorialManager", "Breadcrumber",
    "ScriptFunction", "SearchTools", "AIManager", "GameVersion", "Gossip", "Layers",
    "CameraManager", "Look", "GroupMindManager", "Timers", "Weather", "LocationManager",
    "Follow", "Stats", "Player", "QuestTracker", "Inventory", "Physics", "Money",
};

const std::unordered_set<std::string>& native_name_set() {
    static const std::unordered_set<std::string> set = [] {
        std::unordered_set<std::string> s;
        for (const char* n : kNativeClassNames) s.insert(n);
        for (const char* n : kNativeGlobalNames) s.insert(n);
        for (const char* n : kSupplementClassNames) s.insert(n);
        for (const char* n : {"EMessageEventType", "Platform", "ScriptEnum"}) s.insert(n);  // enums
        return s;
    }();
    return set;
}
}  // namespace

namespace f2 {

namespace {
lua_State* L(void* s) { return static_cast<lua_State*>(s); }

// Trampoline for every bound native: upvalue 1 = the VM, upvalue 2 = the native index.
// `s` is the ACTUAL calling state — which is a coroutine thread, not the main state, whenever
// the native is invoked from inside a resumed quest coroutine. dispatch_from routes the arg/
// push helpers to `s` for the duration of the call (see cur_()).
int native_trampoline(lua_State* s) {
    auto* vm = static_cast<NativeScriptVM*>(lua_touserdata(s, lua_upvalueindex(1)));
    const int idx = static_cast<int>(lua_tointeger(s, lua_upvalueindex(2)));
    return vm->dispatch_from(s, idx);
}

// Get-or-create a table stored in the registry under `key`, left on the stack top. When
// `weak_mode` is non-null, a freshly created table gets a {__mode=weak_mode} metatable.
void get_registry_table(lua_State* s, const char* key, const char* weak_mode) {
    lua_getfield(s, LUA_REGISTRYINDEX, key);          // [t?]
    if (lua_istable(s, -1)) return;
    lua_pop(s, 1);
    lua_newtable(s);                                  // [t]
    if (weak_mode) {
        lua_newtable(s);                              // [t, mt]
        lua_pushstring(s, weak_mode);
        lua_setfield(s, -2, "__mode");                // mt.__mode = weak_mode
        lua_setmetatable(s, -2);                      // [t]
    }
    lua_pushvalue(s, -1);                             // [t, t]
    lua_setfield(s, LUA_REGISTRYINDEX, key);          // [t]
}

// A capture-less no-op used as the read-only __newindex on enum tables.
int enum_readonly_newindex(lua_State*) { return 0; }
}  // namespace

NativeScriptVM::NativeScriptVM() {
    lua_State* s = luaL_newstate();  // lhCreateLuaState analogue
    if (!s) {
        last_error_ = "luaL_newstate failed (out of memory)";
        return;
    }
    luaL_openlibs(s);  // base/string/table/math/os/io/coroutine — the stdlib
    state_ = s;
}

NativeScriptVM::~NativeScriptVM() {
    if (state_) lua_close(L(state_));
}

int NativeScriptVM::dispatch_(int fn_index) {
    if (fn_index < 0 || fn_index >= static_cast<int>(natives_.size())) return 0;
    return natives_[static_cast<std::size_t>(fn_index)](*this);
}

int NativeScriptVM::dispatch_from(void* call_state, int fn_index) {
    void* prev = call_state_;
    call_state_ = call_state;
    const int r = dispatch_(fn_index);
    call_state_ = prev;
    return r;
}

// The lua_State the arg/push helpers operate on: the current native call's state (a coroutine
// thread when called from a resumed quest), falling back to the main state outside a call.
lua_State* NativeScriptVM::cur_() const {
    return static_cast<lua_State*>(call_state_ ? call_state_ : state_);
}

void NativeScriptVM::register_native(const char* class_name, const char* method,
                                     ScriptNativeFn fn) {
    if (!state_) return;
    lua_State* s = L(state_);
    const int idx = static_cast<int>(natives_.size());
    natives_.push_back(fn);

    // Get-or-create the global class table.
    lua_getglobal(s, class_name);
    if (!lua_istable(s, -1)) {
        lua_pop(s, 1);
        lua_newtable(s);
        lua_pushvalue(s, -1);
        lua_setglobal(s, class_name);
    }
    // Set method = closure(trampoline){vm, idx}.
    lua_pushlightuserdata(s, this);
    lua_pushinteger(s, idx);
    lua_pushcclosure(s, &native_trampoline, 2);
    lua_setfield(s, -2, method);
    lua_pop(s, 1);  // pop the class table
}

void NativeScriptVM::register_global(const char* name, ScriptNativeFn fn) {
    if (!state_) return;
    lua_State* s = L(state_);
    const int idx = static_cast<int>(natives_.size());
    natives_.push_back(fn);
    lua_pushlightuserdata(s, this);
    lua_pushinteger(s, idx);
    lua_pushcclosure(s, &native_trampoline, 2);
    lua_setglobal(s, name);
}

void NativeScriptVM::register_object_method(const char* tag, const char* method,
                                            ScriptNativeFn fn) {
    if (!state_) return;
    lua_State* s = L(state_);
    const int idx = static_cast<int>(natives_.size());
    natives_.push_back(fn);
    const std::string key = std::string("f2.methods.") + tag;
    get_registry_table(s, key.c_str(), nullptr);      // [methods]
    lua_pushlightuserdata(s, this);
    lua_pushinteger(s, idx);
    lua_pushcclosure(s, &native_trampoline, 2);        // [methods, closure]
    lua_setfield(s, -2, method);                        // methods[method] = closure
    lua_pop(s, 1);
}

void NativeScriptVM::push_handle(const char* tag, std::uint64_t id) {
    if (!state_) return;
    lua_State* s = cur_();
    if (id == 0) { lua_pushnil(s); return; }  // null handle -> nil (scripts' `if not e`)
    void* key = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    const std::string cachekey = std::string("f2.objcache.") + tag;
    get_registry_table(s, cachekey.c_str(), "v");     // [cache]  (weak VALUES)
    lua_pushlightuserdata(s, key);
    lua_rawget(s, -2);                                 // [cache, obj?]
    if (lua_istable(s, -1)) { lua_remove(s, -2); return; }  // cached -> [obj]
    lua_pop(s, 1);                                     // [cache]
    // Build a fresh wrapper: { __id = <lightuserdata id>, __tag = tag } with a metatable
    // whose __index is the shared per-tag methods table (so obj:Method() dispatches).
    lua_newtable(s);                                   // [cache, obj]
    lua_pushlightuserdata(s, key);
    lua_setfield(s, -2, "__id");
    lua_pushstring(s, tag);
    lua_setfield(s, -2, "__tag");
    lua_newtable(s);                                   // [cache, obj, mt]
    const std::string mkey = std::string("f2.methods.") + tag;
    get_registry_table(s, mkey.c_str(), nullptr);      // [cache, obj, mt, methods]
    lua_setfield(s, -2, "__index");                    // mt.__index = methods
    lua_setmetatable(s, -2);                            // [cache, obj]
    lua_pushlightuserdata(s, key);                     // [cache, obj, key]
    lua_pushvalue(s, -2);                              // [cache, obj, key, obj]
    lua_rawset(s, -4);                                 // cache[key] = obj  -> [cache, obj]
    lua_remove(s, -2);                                 // [obj]
}

std::uint64_t NativeScriptVM::arg_handle(int index) const {
    if (!state_) return 0;
    lua_State* s = cur_();
    if (!lua_istable(s, index)) return 0;
    lua_getfield(s, index, "__id");                    // [__id]
    std::uint64_t id = 0;
    if (lua_islightuserdata(s, -1))
        id = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(lua_touserdata(s, -1)));
    lua_pop(s, 1);
    return id;
}

void NativeScriptVM::register_enum(const char* name, const ScriptEnumConst* consts,
                                   std::size_t count) {
    if (!state_) return;
    lua_State* s = L(state_);
    lua_newtable(s);                                   // [t]
    for (std::size_t i = 0; i < count; ++i) {
        lua_pushinteger(s, static_cast<lua_Integer>(consts[i].value));
        lua_setfield(s, -2, consts[i].key);
    }
    // Give it a metatable so (a) the auto-stub skips it (getmetatable != nil) and (b) it is
    // read-only; unknown keys read nil rather than becoming stub no-ops.
    lua_newtable(s);                                   // [t, mt]
    lua_pushcfunction(s, &enum_readonly_newindex);
    lua_setfield(s, -2, "__newindex");
    lua_setmetatable(s, -2);                           // [t]
    lua_setglobal(s, name);
}

int NativeScriptVM::ref_arg(int index) {
    if (!state_) return -2;
    lua_State* s = cur_();
    lua_pushvalue(s, index);
    return luaL_ref(s, LUA_REGISTRYINDEX);
}

int NativeScriptVM::ref_arg_field(int index, const char* field) {
    if (!state_) return -2;
    lua_State* s = cur_();
    if (!lua_istable(s, index)) return -2;
    lua_getfield(s, index, field);
    return luaL_ref(s, LUA_REGISTRYINDEX);
}

bool NativeScriptVM::call_ref(int ref) {
    if (!state_ || ref < 0) return false;
    lua_State* s = L(state_);
    lua_rawgeti(s, LUA_REGISTRYINDEX, ref);
    if (!lua_isfunction(s, -1)) { lua_pop(s, 1); return false; }
    if (lua_pcall(s, 0, 0, 0) != 0) {
        last_error_ = lua_tostring(s, -1) ? lua_tostring(s, -1) : "runtime error";
        lua_pop(s, 1);
        return false;
    }
    last_error_.clear();
    return true;
}

void NativeScriptVM::unref(int ref) {
    if (state_ && ref >= 0) luaL_unref(L(state_), LUA_REGISTRYINDEX, ref);
}

bool NativeScriptVM::load_and_run_(const void* data, std::size_t size, const char* name) {
    if (!state_) {
        last_error_ = "VM not initialised";
        return false;
    }
    lua_State* s = L(state_);
    // luaL_loadbuffer auto-detects source vs LuaQ bytecode by the leading signature byte.
    if (luaL_loadbuffer(s, static_cast<const char*>(data), size, name) != 0) {
        last_error_ = lua_tostring(s, -1) ? lua_tostring(s, -1) : "load error";
        lua_pop(s, 1);
        return false;
    }
    if (lua_pcall(s, 0, 0, 0) != 0) {
        last_error_ = lua_tostring(s, -1) ? lua_tostring(s, -1) : "runtime error";
        lua_pop(s, 1);
        return false;
    }
    last_error_.clear();
    return true;
}

bool NativeScriptVM::run_source(const char* source, const char* chunk_name) {
    return load_and_run_(source, std::char_traits<char>::length(source), chunk_name);
}

bool NativeScriptVM::run_bytecode(const void* data, std::size_t size, const char* chunk_name) {
    return load_and_run_(data, size, chunk_name);
}

bool NativeScriptVM::run_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        last_error_ = std::string("cannot open '") + path + "'";
        return false;
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_and_run_(buf.data(), buf.size(), path);
}

bool NativeScriptVM::load_only(const void* data, std::size_t size, const char* chunk_name) {
    if (!state_) {
        last_error_ = "VM not initialised";
        return false;
    }
    lua_State* s = L(state_);
    if (luaL_loadbuffer(s, static_cast<const char*>(data), size, chunk_name) != 0) {
        last_error_ = lua_tostring(s, -1) ? lua_tostring(s, -1) : "load error";
        lua_pop(s, 1);
        return false;
    }
    lua_pop(s, 1);  // discard the compiled function; we only verified it loads
    last_error_.clear();
    return true;
}

bool NativeScriptVM::call_global(const char* fn_name, double dt) {
    if (!state_) return false;
    lua_State* s = L(state_);
    lua_getglobal(s, fn_name);
    if (!lua_isfunction(s, -1)) {
        lua_pop(s, 1);
        last_error_ = std::string("no global function '") + fn_name + "'";
        return false;
    }
    lua_pushnumber(s, dt);
    if (lua_pcall(s, 1, 0, 0) != 0) {
        last_error_ = lua_tostring(s, -1) ? lua_tostring(s, -1) : "runtime error";
        lua_pop(s, 1);
        return false;
    }
    last_error_.clear();
    return true;
}

bool NativeScriptVM::call_method(const char* global, const char* method, double dt) {
    if (!state_) return false;
    lua_State* s = L(state_);
    lua_getglobal(s, global);                 // [tbl]
    if (!lua_istable(s, -1)) { lua_pop(s, 1); return false; }
    lua_getfield(s, -1, method);              // [tbl, fn]
    if (!lua_isfunction(s, -1)) { lua_pop(s, 2); return false; }
    lua_pushvalue(s, -2);                      // [tbl, fn, self]
    lua_pushnumber(s, dt);                     // [tbl, fn, self, dt]
    if (lua_pcall(s, 2, 0, 0) != 0) {          // [tbl (,err)]
        last_error_ = lua_tostring(s, -1) ? lua_tostring(s, -1) : "runtime error";
        lua_pop(s, 1);
    } else {
        last_error_.clear();
    }
    lua_pop(s, 1);                             // pop the manager table
    return true;                               // found + invoked (runtime error -> last_error)
}

void NativeScriptVM::log_stub_miss(const char* name) {
    if (name && *name) stub_misses_.emplace_back(name);
}

bool NativeScriptVM::install_autostub() {
    if (!state_) return false;
    lua_State* s = L(state_);
    // Register __stub_log as a plain global native (the metatables call it on a miss).
    const int idx = static_cast<int>(natives_.size());
    natives_.push_back([](NativeScriptVM& vm) -> int {
        vm.log_stub_miss(vm.arg_string(1));
        return 0;
    });
    lua_pushlightuserdata(s, this);
    lua_pushinteger(s, idx);
    lua_pushcclosure(s, &native_trampoline, 2);
    lua_setglobal(s, "__stub_log");

    // __is_native(name) -> bool: is `name` a retail Lua native (from the catalog)? The _G
    // auto-stub only fabricates a stub for these; unknown Capitalized names read as nil so the
    // game's own globals define cleanly.
    const int nidx = static_cast<int>(natives_.size());
    natives_.push_back([](NativeScriptVM& vm) -> int {
        const char* name = vm.arg_string(1);
        vm.push_bool(name && native_name_set().count(name) != 0);
        return 1;
    });
    lua_pushlightuserdata(s, this);
    lua_pushinteger(s, nidx);
    lua_pushcclosure(s, &native_trampoline, 2);
    lua_setglobal(s, "__is_native");

    // The metatable bootstrap: a chainable black-hole NIL + a class-table __index that
    // manufactures a cached no-op returning NIL (or false for predicates), + a _G __index
    // that turns an unknown Capitalized global into a stub class table. Each unique miss
    // is logged once. So the game's scripts never hit "attempt to call a nil value".
    static const char* kBootstrap = R"LUA(
        local seen = {}
        local NIL
        -- Arithmetic/compare/concat/len metamethods keep the black-hole from crashing when a
        -- stubbed getter's value is used numerically (e.g. `GameVersion.GetX() + 1`) — it
        -- degrades to 0 / false / "" instead of "arithmetic on a table value". FLAGGED: this is
        -- stub behaviour; a getter whose value actually matters needs a real native.
        local function zero() return 0 end
        local function no() return false end
        local nilmt = {
          __index = function() return NIL end,
          __call = function() return NIL end,
          __newindex = function() end,
          __tostring = function() return "nil" end,
          __add = zero, __sub = zero, __mul = zero, __div = zero, __mod = zero,
          __pow = zero, __unm = zero, __len = zero,
          __lt = no, __le = no, __eq = no,
          __concat = function() return "" end,
        }
        NIL = setmetatable({}, nilmt)
        _G.__F2_NIL = NIL
        local function predicate(m)
          return type(m) == "string" and (m:match("^Is") or m:match("^Has")
            or m:match("^Find") or m:match("^Exists") or m:match("^Can") or m:match("Loading"))
        end
        local function classmeta(cn)
          return {
            __index = function(t, m)
              if type(m) == "string" and m:sub(1, 2) == "__" then return nil end
              local key = cn .. "." .. tostring(m)
              if not seen[key] then seen[key] = true; __stub_log(key) end
              -- Predicates get a callable returning false (used in conditions). Everything
              -- else resolves to the black-hole NIL itself, which is BOTH callable and
              -- indexable, so it survives `Class.Method()`, `Class.Field.Sub`, and being
              -- stored then chained — no "attempt to index a function value".
              local v
              if predicate(m) then v = function() return false end else v = NIL end
              rawset(t, m, v)
              return v
            end,
            -- Many Capitalized globals are FUNCTIONS, not class tables (GetPlatform(),
            -- GetPlayerHenchman(), ...). Make the stub callable so `Foo()` yields the
            -- black-hole (chainable) instead of "attempt to call a table value". Predicate-
            -- named globals return false to avoid truthiness drift.
            __call = function(t, ...)
              if not seen[cn] then seen[cn] = true; __stub_log(cn .. "()") end
              if predicate(cn) then return false end
              return NIL
            end,
          }
        end
        for cn, t in pairs(_G) do
          -- Only wrap NATIVE class tables. Real game tables already loaded (GeneralScriptManager,
          -- QuestManager, quest types) must stay unwrapped, or their nil-field reads would turn
          -- into stubs and break their own logic (e.g. GeneralScriptManager.Update walking its
          -- CurrentlyRunningScripts list).
          if type(t) == "table" and type(cn) == "string" and cn:match("^%u")
             and getmetatable(t) == nil and __is_native(cn) then
            setmetatable(t, classmeta(cn))
          end
        end
        setmetatable(_G, { __index = function(t, k)
          -- Only fabricate a stub for names the retail binary actually exposes as natives.
          -- Unknown Capitalized names (the game's own script-defined quest types/managers)
          -- read as nil, so `if _G.X == nil then define X end` registration works.
          if type(k) == "string" and __is_native(k) then
            local tbl = setmetatable({}, classmeta(k))
            rawset(t, k, tbl)
            return tbl
          end
          return nil
        end })
        -- Expose a pre-creator: a native class stub as a RAW _G entry. The game's save system
        -- replaces _G's metatable at load (saveloadsystem.lua), which would kill the mint-index
        -- above for any class not yet cached — so the runtime pre-creates the known class
        -- tables here as raw entries that survive the swap.
        function __mkclassstub(cn)
          if rawget(_G, cn) == nil then rawset(_G, cn, setmetatable({}, classmeta(cn))) end
        end
    )LUA";
    if (!run_source(kBootstrap, "=autostub")) return false;

    // Pre-create the known native CLASS tables as raw _G entries (survive a metatable swap by
    // the save system). Real classes we've already registered are skipped (rawget guard).
    std::string pre;
    for (const char* n : kNativeClassNames) { pre += "__mkclassstub('"; pre += n; pre += "') "; }
    for (const char* n : kSupplementClassNames) { pre += "__mkclassstub('"; pre += n; pre += "') "; }
    return run_source(pre.c_str(), "=mkclassstubs");
}

int NativeScriptVM::arg_count() const { return state_ ? lua_gettop(cur_()) : 0; }

double NativeScriptVM::arg_number(int index) const {
    return state_ ? static_cast<double>(lua_tonumber(cur_(), index)) : 0.0;
}

const char* NativeScriptVM::arg_string(int index) const {
    if (!state_) return "";
    const char* str = lua_tostring(cur_(), index);
    return str ? str : "";
}

bool NativeScriptVM::arg_bool(int index) const {
    return state_ ? lua_toboolean(cur_(), index) != 0 : false;
}

void NativeScriptVM::push_number(double v) {
    if (state_) lua_pushnumber(cur_(), v);
}
void NativeScriptVM::push_string(const char* v) {
    if (state_) lua_pushstring(cur_(), v);
}
void NativeScriptVM::push_bool(bool v) {
    if (state_) lua_pushboolean(cur_(), v ? 1 : 0);
}
void NativeScriptVM::push_nil() {
    if (state_) lua_pushnil(cur_());
}
void NativeScriptVM::push_new_table() {
    if (state_) lua_newtable(cur_());
}
void NativeScriptVM::push_handle_list(const char* tag, const std::uint64_t* ids,
                                      std::size_t count) {
    if (!state_) return;
    lua_State* s = cur_();
    lua_createtable(s, static_cast<int>(count), 0);
    for (std::size_t i = 0; i < count; ++i) {
        push_handle(tag, ids[i]);                       // pushes the handle onto `s`
        lua_rawseti(s, -2, static_cast<int>(i + 1));    // t[i+1] = handle (pops it)
    }
}

}  // namespace f2
