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
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct lua_State;  // global fwd-decl (matches lua.h) so cur_() can name it without pulling in Lua

namespace f2 {

class NativeScriptVM;

// A native function callable from Lua. Reads its args + pushes results via the VM's
// helpers; returns the number of values it pushed (like a lua_CFunction, but Lua-free).
using ScriptNativeFn = int (*)(NativeScriptVM& vm);

// A named integer constant for register_enum (e.g. {"MESSAGE_EVENT_...", 42}).
struct ScriptEnumConst {
    const char* key;
    long long value;
};

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

    // --- object handles (entities, quests, ...) ---------------------------------------
    // The game's scripts hold OBJECTS (a hero entity, a quest) and call methods on them
    // (`hero:GetName()`, `quest:IsComplete()`). We represent each as a Lua table carrying an
    // integer handle + a class tag, with a shared per-tag metatable whose __index dispatches
    // to the registered methods. push_handle caches one wrapper per (tag,id) in the registry
    // so repeated pushes return the SAME table — object identity + `==` + field-stashing all
    // work the way retail scripts expect.
    //
    // Register a method on object class `tag` (e.g. tag="Entity", method="GetPosition").
    void register_object_method(const char* tag, const char* method, ScriptNativeFn fn);
    // Push the object wrapper for (tag,id). id==0 pushes nil (the null handle) so scripts'
    // `if not e then` checks behave. Returns nothing; leaves one value on the stack.
    void push_handle(const char* tag, std::uint64_t id);
    // Read the integer handle from a self/object arg (its __id field); 0 if not an object.
    [[nodiscard]] std::uint64_t arg_handle(int index) const;

    // Define a read-only enum/constant table: global `name` gets a table of the given
    // {key,value} integer constants (e.g. EMessageEventType). Scripts read name.KEY.
    void register_enum(const char* name, const struct ScriptEnumConst* consts, std::size_t count);

    // --- persistent callable references (manager Update fns) ---------------------------
    // The game's managers register themselves via natives (SetGeneralScriptManager(tbl),
    // SetQuestUpdateFunction(fn)) and the native tick must call them back each frame. These
    // capture a Lua value into the registry and invoke it later. Returns a ref handle (>=0)
    // or a negative sentinel; call_ref/unref accept the sentinel harmlessly.
    int ref_arg(int index);                              // ref the arg at `index`
    int ref_arg_field(int index, const char* field);     // ref arg[index][field]
    bool call_ref(int ref);                              // call the ref'd function (0 args)
    void unref(int ref);

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

    // Compile a chunk and LEAVE the resulting function on the Lua stack (does not run it) —
    // for a require() loader that returns a module's chunk. Returns false + last_error on a
    // load error (nothing pushed).
    bool push_loaded_chunk(const void* data, std::size_t size, const char* chunk_name = "=chunk");

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

    // Record a missing-native CALL (called by the auto-stub on every invocation, unlike
    // log_stub_miss which fires once per unique name at first *reference*). stub_call_counts()
    // is the empirical "what does this quest actually hit, and how hard" census — the ordering
    // signal for which natives to implement first.
    void log_stub_call(const char* name);
    [[nodiscard]] const std::map<std::string, int>& stub_call_counts() const noexcept {
        return stub_call_counts_;
    }

    // --- helpers for native fns (operate on the current call's Lua stack) ---
    [[nodiscard]] int arg_count() const;
    [[nodiscard]] double arg_number(int index) const;   // 1-based; 0 if not a number
    [[nodiscard]] const char* arg_string(int index) const;  // "" if not a string
    [[nodiscard]] bool arg_bool(int index) const;
    void push_number(double v);
    void push_string(const char* v);
    void push_bool(bool v);
    void push_nil();
    void push_new_table();  // push a fresh empty Lua table (e.g. an empty result list)
    // Push a Lua array (1-based) of object handles for `tag` (e.g. an entity search result).
    void push_handle_list(const char* tag, const std::uint64_t* ids, std::size_t count);

    // Dispatched from the bound closures — internal. dispatch_from routes the arg/push
    // helpers to the ACTUAL calling state (a coroutine thread when called inside a resumed
    // quest) for the duration of the call.
    int dispatch_(int fn_index);
    int dispatch_from(void* call_state, int fn_index);

private:
    bool load_and_run_(const void* data, std::size_t size, const char* name);
    ::lua_State* cur_() const;  // current call state (call_state_ or state_)

    void* state_ = nullptr;                 // lua_State* (main)
    void* call_state_ = nullptr;            // lua_State* of the in-flight native call (or null)
    void* user_data_ = nullptr;             // opaque game context for bound natives
    std::vector<ScriptNativeFn> natives_;   // registered fns, indexed by closure upvalue
    std::vector<std::string> stub_misses_;  // unique missing-native worklist
    std::map<std::string, int> stub_call_counts_;  // per-name CALL frequency (ranking signal)
    std::string last_error_;
};

}  // namespace f2
