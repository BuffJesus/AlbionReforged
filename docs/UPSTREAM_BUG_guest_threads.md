# ReXGlue bug report: guest threads never execute (only host threads run)

**Reporter:** Fable II recomp (Fable2Recomp), ReXGlue SDK **v0.8.0** (win-amd64 prebuilt).
**Severity:** blocks boot of any title whose main thread spawns the engine on a *guest* thread and
then `WaitForSingleObject`s it (very common CRT `_beginthread`/`ExCreateThread` startup pattern).
**Status in our project:** worked around locally (see bottom); reporting so it can be fixed at the
runtime level and the workaround retired.

---

## One-line summary

A **guest** thread created via `ExCreateThread` / the CRT `_beginthread` path returns a valid
handle but its body **never executes** — the recompiled entry at `start_address` is never entered.
Every thread that actually runs in our build is a runtime-owned **host** thread. A `WaitForSingleObject`
on the dead-on-arrival guest thread then returns `WAIT_OBJECT_0` immediately (symptom, not a second
bug — the host carrier exits at once, signaling its handle).

## Environment

- SDK: `rexglue-sdk/win-amd64` v0.8.0, `rexruntime.dll` (release).
- Title: Fable II GOTY, TU1-patched XEX (`default_tu1.xex`), recompiled with `rexglue -f codegen`.
- Host: Windows 11, x64, Clang 19 / MSVC 14.38.
- Guest entry chain (from Ghidra on the TU1 XEX; addresses are guest):
  - guest `main` `sub_822EA1C0` → CRT `_beginthread` `sub_82CA9CD0(entry=0x822EA228, stack=0x40000)`
    then `WaitForSingleObject(handle, INFINITE)` (`0x821969C8`) then `CloseHandle`.
  - `0x822EA228` = `rex_lhGameThreadInit` — the real engine entry (named in `Fable2_config.toml`).

## Observed behavior

From our runtime log (`Fable2_0NN.log`), the ONLY threads that ever emit `XThread::Execute` are the
runtime's own, all tagged `<host>`:

```
XThread::Execute thid 1 (handle=F8000010, 'XMA Decoder (F8000010)',    native=..., <host>)
XThread::Execute thid 2 (handle=F8000014, 'Audio Worker (F8000014)',   native=..., <host>)
XThread::Execute thid 3 (handle=F8000018, 'GPU Commands (F8000018)',   native=..., <host>)
XThread::Execute thid 4 (handle=F800001C, 'GPU VSync (F800001C)',      native=..., <host>)
XThread::Execute thid 5 (handle=F8000024, 'Kernel Dispatch (F8000024)',native=..., <host>)
```

No guest thread ever appears. When guest `main` calls `_beginthread` for the game thread:

- `sub_82CA9CD0` allocates the CRT per-thread block, then (in the original guest code) calls
  `ExCreateThread`/`rex_CreateThread` (`0x82CC3C18`) with `CREATE_SUSPENDED` and then
  `ResumeThread` (`sub_82CC7E68` → `NtResumeThread`).
- We instrumented this exact sequence (see repro): `CreateThread` returns a valid handle
  (`0xF800002C`), `ResumeThread` returns `1` (success) — **but no `XThread::Execute` line is ever
  logged for that handle, and the game never initializes.**
- We also tried creating **non-suspended** (`creation_flags = 0`, which per Xenia's
  `XThread::Create` should auto-`Resume()`): still no execution.
- We even constructed a guest `XThread` directly:
  `make_object<XThread>(kernel_state(), stack, /*xapi_thread_startup=*/0, entry, ctx,
  /*creation_flags=*/0, /*guest_thread=*/true)->Create()` → returns `X_STATUS_SUCCESS` and a handle,
  **but the thread body never runs.**

Because the guest thread's host carrier returns immediately (never entering guest code), its OS
handle becomes signaled at once, so `WaitForSingleObject(handle, INFINITE)` returns `WAIT_OBJECT_0`
instantly. Guest `main` then falls through, returns, runs the guest CRT exit path
(`rex_doexit` → `sub_832B7688`), and dies with an access violation walking the CRT cleanup list.
That AV was our long-standing "boot crash" — but it is downstream of the real defect.

## Expected behavior (per Xenia, which the SDK's `XThread` is adapted from)

`rexglue-sdk/include/rex/system/xthread.h` header: *"Xenia … Adapted for ReXGlue runtime."*
In Xenia (`xenia/src/xenia/kernel/xthread.cc`):

- `XThread::Create()` creates the host carrier suspended, then, if `X_CREATE_SUSPENDED` is not set,
  calls `thread_->Resume()` to start it (lines ~425-428).
- The host carrier's body sets `running_ = true` and calls `XThread::Execute()`, which trampolines
  into guest code: if `xapi_thread_startup` is set it runs that with `start_address`/`start_context`
  as args, else it runs `start_address` directly via the processor/function dispatcher
  (`xthread.cc` ~516-528).
- `NtResumeThread` → `XThread::Resume()` decrements the guest `suspend_count` **and** calls the
  host `thread_->Resume()` (both are required to actually start a suspended thread).
- A thread object signals (`header.signal_state = 1`) **only** in `Exit()`/`Terminate()`.

So a correctly-run guest thread should: emit an `XThread::Execute`, enter `rex_lhGameThreadInit`,
and NOT signal its handle until it exits — making `main`'s INFINITE wait block for the life of the
game, exactly as on hardware.

## Hypothesis / where to look

The guest-thread `Execute()` → recompiled-entry path appears not to be wired (or the suspended host
carrier is never resumed) in the v0.8.0 prebuilt runtime. Candidate spots, by analogy to Xenia:

1. `XThread::Create()` for `guest_thread_ == true`: is the host carrier `thread_->Resume()` ever
   called for `creation_flags == 0`? (Our non-suspended attempt didn't run either.)
2. `XThread::Execute()` for guest threads: does it actually invoke the recompiled function at
   `start_address` (via the function dispatcher / `HostToGuestFunction`)? Host threads work because
   `XHostThread::Execute()` runs a `std::function` directly and never touches this path.
3. `NtResumeThread`/`XThread::Resume()`: does it release the host carrier's suspend, not just the
   guest `suspend_count`?

A minimal fix likely restores guest-thread execution for both the suspended+resume and the
non-suspended creation paths.

## Minimal repro (from our project)

`Fable2Recomp/src/BootTrace.cpp` (weak overrides of `sub_822EA1C0` guest main and `sub_82CA9CD0`
CRT `_beginthread`). The instrumented `_beginthread` reproduced the guest `ExCreateThread` +
`NtResumeThread` sequence and logged the valid-handle / no-execute result. Full guest→recomp
bindings and Ghidra decompiler evidence are in `docs/HANDOFF.md` and `ghidra_out/`.

## Our local workaround (to retire once fixed)

Spawn the game thread as a `rex::system::XHostThread` whose body calls the guest CRT thread shim
(`sub_82CA9C50(ptd)`) via a typed `REX_IMPORT` — i.e. route the game thread through the same
host-thread mechanism the runtime's own working threads use — and park guest `main` forever
(hardware-faithful, since the wait is meant to never return). With this the process stays alive with
~36 threads / 300+ MB resident instead of the immediate CRT-exit AV.

## Downstream symptom that likely shares the same root

Even with the XHostThread workaround, the game thread does not yet reach the engine proper: it
stalls **inside the guest CRT thread-startup shim `sub_82CA9C50`** (the `_threadstartex`-equivalent
that installs per-thread CRT state and then calls the entry). The process stays alive but the engine
never initializes. To localize it we temporarily bypassed the shim and called `rex_lhGameThreadInit`
directly: the engine then advances to its own log-init and **AVs in the CRT** with this host stack:

```
rex__flsbuf + 0x719          <- fault (0xC0000005)
rex_sprintf_0 + 0x458
sub_82B3DB90 + 0x150         (engine log-init)
rex_lhGameThreadInit + 0xb0  (game entry)
<XHostThread body>
```

i.e. the CRT (`_flsbuf` via `sprintf`) dereferences per-thread CRT state that the shim would have
installed but which is absent because we bypassed it. Read together: *with* the shim the thread
hangs in it; *without* it the CRT faults for lack of per-thread state. This strongly suggests the
CRT thread-startup shim depends on the guest-thread execution/TLS environment that a real guest
thread would provide — the same environment missing because of the primary bug above. A proper
guest-thread execution path would likely resolve both the hang and this AV together.
