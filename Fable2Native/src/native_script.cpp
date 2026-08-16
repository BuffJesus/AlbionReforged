#include "f2/native_script.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace f2 {

namespace {
lua_State* L(void* s) { return static_cast<lua_State*>(s); }

// Trampoline for every bound native: upvalue 1 = the VM, upvalue 2 = the native index.
int native_trampoline(lua_State* s) {
    auto* vm = static_cast<NativeScriptVM*>(lua_touserdata(s, lua_upvalueindex(1)));
    const int idx = static_cast<int>(lua_tointeger(s, lua_upvalueindex(2)));
    return vm->dispatch_(idx);
}
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

int NativeScriptVM::arg_count() const { return state_ ? lua_gettop(L(state_)) : 0; }

double NativeScriptVM::arg_number(int index) const {
    return state_ ? static_cast<double>(lua_tonumber(L(state_), index)) : 0.0;
}

const char* NativeScriptVM::arg_string(int index) const {
    if (!state_) return "";
    const char* str = lua_tostring(L(state_), index);
    return str ? str : "";
}

bool NativeScriptVM::arg_bool(int index) const {
    return state_ ? lua_toboolean(L(state_), index) != 0 : false;
}

void NativeScriptVM::push_number(double v) {
    if (state_) lua_pushnumber(L(state_), v);
}
void NativeScriptVM::push_string(const char* v) {
    if (state_) lua_pushstring(L(state_), v);
}
void NativeScriptVM::push_bool(bool v) {
    if (state_) lua_pushboolean(L(state_), v ? 1 : 0);
}

}  // namespace f2
