# Albion Reforged

An unofficial Fable II PC port effort using a hybrid approach: retain a playable
static recompilation as the behavior reference while moving verified engine
systems into independent C++ services and removing Xbox 360 runtime dependencies.

## Repository structure

| Path | Role |
|---|---|
| [native/](native/README.md) | Active independent engine services, tests and comparison diagnostics |
| [tools/](tools/README.md) | Resource capture readers and optional corpus reference helpers |
| [docs/](docs/README.md) | Current direction, integration gates and historical research |
| [Fable2Native/](Fable2Native/README.md) | Discontinued standalone reconstruction; retained for reusable code and tools |
| `lua-docs/` | Scripting reference documentation |

The playable GOTY baseline, isolated TU1 candidates and Albion runtime/renderer
currently live in separate local checkouts. This repository is not yet a
self-contained build of the running game. No guest call path has been retired;
the playable recomp still depends on ReXGlue compatibility services.

## Build the independent services

Requires CMake 3.25+, a C++17 compiler and Python 3. No game data or SDK is needed
for the synthetic contract tests. miniz is pinned under `native/vendor/` with its license.

```powershell
cmake -S native -B build/native -DBUILD_TESTING=ON
cmake --build build/native --config Release --parallel 4
ctest --test-dir build/native -C Release --output-on-failure
```

The Windows suite contains 15 tests covering archive/range reads, ownership,
lookup, PCM/WAVE parsing, manifest handling and capture/comparison helpers.
Original-PPC and real-bank corpus checks additionally require user-owned inputs;
fixture success is not a claim of live game parity.

Read [current status](docs/CURRENT_STATUS.md), [repository layout](docs/REPOSITORY_LAYOUT.md)
and [contributing](CONTRIBUTING.md). Game dumps, extracted assets, SDKs, captures
and generated outputs must not be committed.
