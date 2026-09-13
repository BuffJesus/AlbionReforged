# Independent native services

`f2data` provides checked BNK v3 archive/range access, retained byte sources,
resource selection, archive leases and PCM/WAVE parsing. `f2compat` provides TU1
manifest/name contracts and completed-read comparison helpers. Public service
interfaces do not require PPC registers or ReXGlue headers.

Build/test commands are in the [root README](../README.md). Fifteen synthetic
Windows suites pass without game data. Optional corpus and original-PPC checks
accept explicit user-owned input paths; those results cover their stated inputs,
not full game behavior. Archive ambiguity, limits and source ownership are
explicit API contracts; raw guest tokens are not native archive indexes.

`adapters/tu1_table_capture_hook.cpp` is an opt-in diagnostic compiled only by an
isolated game integration or the fake-ABI test harness. It forwards original
lookups, captures bounded records/names under the manager lock, then writes after
lookup returns. It is not a drop-in native game reader. See
[adapter details](adapters/README.md).

The vendored miniz copy preserves its upstream license and matches the codec
previously used by the local reconstruction; the active build no longer depends
on that retired project's tree. No guest call path has been retired yet.
