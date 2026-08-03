# Contributing to Albion Reforged

Albion Reforged is an unofficial native C++23 PC port/reconstruction project. The public
repository contains code, schemas, tools, tests, and documentation only.

Do not commit game dumps, ISOs, XEX files, BNKs, videos, textures, models, extracted install
directories, generated build output, captures, or credentials. The runtime and cookers operate
on a user-owned source selected during installation.

## Build and test

```powershell
cmake -S Fable2Native -B Fable2Native/build -DBUILD_TESTING=ON
cmake --build Fable2Native/build --config RelWithDebInfo
ctest --test-dir Fable2Native/build -C RelWithDebInfo --output-on-failure
```

New runtime code belongs in `Fable2Native` and should use readable C++23 interfaces. ReXGlue,
Ghidra, and the decompilation tools are behavioral references and research dependencies, not
shipping dependencies of the native executable.

Every change should leave the core tests passing. Renderer changes should also include a small
reproducible validation path or capture description when practical.
