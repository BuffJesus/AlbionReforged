#pragma once

// Embedded Lua 5.1 scripting VM for the native port.
//
// Grounded in ghidra_out/lua_vm_scripting_engine.txt: the game's VM is Lua 5.1 (its
// scripts are LuaQ = 5.1 bytecode), booted like lhCreateLuaState (newstate + open the
// stdlib), with natives bound via the register-class-method pattern (a global class
// table + a closure set as a field, retail 0x821AC8B0). The three script managers
// (Quest/General/AI) are resumed each frame by the tick dispatcher (sub_82281FE0); the
// per-thread coroutine scheduling itself lives in Lua manager scripts — so the native
// side just calls each manager's Lua Update every tick and lets Lua drive its coroutines.
//
// NOTE (flagged): embedding stock Lua 5.1 in the standalone app is the synthesis's
// EXTRAPOLATION — the recomp reuses the game's own VM; here we embed a fresh 5.1 that is
// bytecode-compatible with the game's LuaQ. The stdlib + bindings differ from retail
// until each native is reimplemented.
//
// lua_State is kept opaque (no lua.h in this header) so consumers don't pull in Lua.

#include <cstddef>
#include <string>
#include <vector>

namespace f2 {

class NativeScriptVM;

// A native function callable from Lua. Reads its args + pushes results via the VM's
// helpers; returns the number of values it pushed (like a lua_CFunction, but Lua-free).
using ScriptNativeFn = int (*)(NativeScriptVM& vm);

class NativeScriptVM {
public:
    NativeScriptVM();   // lhCreateLuaState analogue: newstate + luaL_openlibs
    ~NativeScriptVM();
    NativeScriptVM(const NativeScriptVM&) = delete;
    NativeScriptVM& operator=(const NativeScriptVM&) = delete;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    // Opaque context a bound native can retrieve (e.g. the NativeGame) to reach game
    // state — the natives only receive the VM, so this is how they find their world.
    void set_user_data(void* ptr) noexcept { user_data_ = ptr; }
    [[nodiscard]] void* user_data() const noexcept { return user_data_; }

    // Bind fn as Class.Method (get-or-create the global `class_name` table, set the
    // closure as `method`). The retail register-class-method pattern.
    void register_native(const char* class_name, const char* method, ScriptNativeFn fn);

    // Bind fn as a bare GLOBAL function (e.g. RunScript) rather than Class.Method.
    void register_global(const char* name, ScriptNativeFn fn);

    // Load + run a chunk. run_source takes Lua text; run_bytecode takes compiled LuaQ
    // (the game's scripts). Both return false + set last_error() on a load/runtime error.
    bool run_source(const char* source, const char* chunk_name = "=chunk");
    bool run_bytecode(const void* data, std::size_t size, const char* chunk_name = "=bytecode");

    // Load + run a file (Lua source OR compiled LuaQ — auto-detected). The basis for
    // mod scripts and the game's own script files. Returns false + sets last_error().
    bool run_file(const char* path);

    // Compile/undump a chunk WITHOUT running it — verifies the loader accepts it (e.g.
    // that the float VM accepts the game's LuaQ header). Returns false + last_error on a
    // load/undump error. The compiled function is discarded.
    bool load_only(const void* data, std::size_t size, const char* chunk_name = "=chunk");

    // Call a global function `fn_name(dt)`. Returns false if it is missing or errors
    // (last_error set). This is how a ScriptSystems manager drives its Lua Update.
    bool call_global(const char* fn_name, double dt);

    // Call `global:method(dt)` (a method on a manager table — retail resumes the manager
    // object's Update METHOD, not a free global). Returns true if the method was found and
    // invoked (even if it errored — see last_error); false only if the table/method is
    // absent (so the caller can fall back).
    bool call_method(const char* global, const char* method, double dt);

    // Install the missing-native auto-stub: metatables on _G and the class tables so an
    // unbound Class.Method resolves to a chainable no-op (returns a black-hole value, or
    // real false for Is*/Has*/Find*/Exists predicates to avoid truthiness drift), logging
    // each unique miss once (stub_misses()). Call AFTER the real natives are registered so
    // it only fires for MISSING ones. Lets the game's scripts run without every native.
    bool install_autostub();

    // Record a missing-native reference (called by the auto-stub) + read the ranked list.
    void log_stub_miss(const char* name);
    [[nodiscard]] const std::vector<std::string>& stub_misses() const noexcept { return stub_misses_; }

    // --- helpers for native fns (operate on the current call's Lua stack) ---
    [[nodiscard]] int arg_count() const;
    [[nodiscard]] double arg_number(int index) const;   // 1-based; 0 if not a number
    [[nodiscard]] const char* arg_string(int index) const;  // "" if not a string
    [[nodiscard]] bool arg_bool(int index) const;
    void push_number(double v);
    void push_string(const char* v);
    void push_bool(bool v);

    // Dispatched from the bound closures — internal.
    int dispatch_(int fn_index);

private:
    bool load_and_run_(const void* data, std::size_t size, const char* name);

    void* state_ = nullptr;                 // lua_State*
    void* user_data_ = nullptr;             // opaque game context for bound natives
    std::vector<ScriptNativeFn> natives_;   // registered fns, indexed by closure upvalue
    std::vector<std::string> stub_misses_;  // unique missing-native worklist
    std::string last_error_;
};

}  // namespace f2
