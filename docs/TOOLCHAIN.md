# Toolchain — tools & exact commands

Reproducible recipes. Paths assume this repo at `D:\Documents\Fable2RE`.

## Tools & where they are
| Tool | Location / how | Use |
|---|---|---|
| **rexglue** | `rexglue-sdk/win-amd64/bin/rexglue.exe` (set `REXSDK` to `...win-amd64`) | PPC→C++ codegen |
| **Clang 19 / MSVC** | VS 2022 `C:\Program Files\Microsoft Visual Studio\2022\Community` | build the recomp |
| **CMake / Ninja** | on PATH | build |
| **cdb** (WinDbg) | `C:\Program Files\WindowsApps\Microsoft.WinDbg_*\amd64\cdb.exe` | debug crashes |
| **xextool 6.3** | `xextool_extract/xextool.exe` | apply/inspect XEX patches |
| **Ghidra 12.1** | `D:\Subuwu\tools\ghidra-public` (Java 21) | disassemble/decompile |
| **XEXLoaderWV / GhidraMCP** | installed in Ghidra `Extensions/` (from `REPlugins/`) | load XEX / AI-drive RE |
| **Fable2AssetBrowser** | build in `Fable2AssetBrowser/source/build` | asset/level/terrain decode |
| **Python 3.14** | `python` | extraction scripts |
| **7-Zip** | `C:\Programs\7-Zip` | archives; Windows `tar.exe` also reads .rar |

## Extract game data from the ISO
XDVDFS parser (game partition base `0xFD90000`). See `scripts` in session history; extracts
`default.xex` and the full `/data` tree to `Fable2Recomp/assets/game`. STFS extractor pulls
`tu1_data.bnk` + `default.xexp` from the TU package `716F0A0D/TU_...`.

## Apply the TU1 patch  (★ produces the correct executable)
```
cd xextool_extract
./xextool.exe -p D:\Documents\Fable2RE\default.xexp \
              -o D:\Documents\Fable2RE\default_tu1.xex \
              D:\Documents\Fable2RE\Fable2Recomp\assets\game\default.xex.gold_backup
```
`-l` lists xex info; `-c u` / `-e u` force uncompressed/decrypted; `-u` bakes the patch standalone.
Then copy `default_tu1.xex` → `Fable2Recomp/assets/game/default.xex`.

## Codegen (recompile)
```
cd Fable2Recomp
set REXSDK=D:\Documents\Fable2RE\rexglue-sdk\win-amd64
"%REXSDK%\bin\rexglue.exe" -f codegen fable2_manifest.toml
python tools\fix_dangling_gotos.py generated        # ALWAYS after codegen
```
Config: `fable2_manifest.toml` includes `Fable2_config.toml` (the `[functions]` boundary table).

## Build (PowerShell, VS dev shell)
```powershell
$vs="C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$env:PATH="$vs\VC\Tools\Llvm\x64\bin;$env:PATH"; $env:REXSDK="D:\Documents\Fable2RE\rexglue-sdk\win-amd64"
cd D:\Documents\Fable2RE\Fable2Recomp
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH="D:/Documents/Fable2RE/rexglue-sdk/win-amd64"
cmake --build out/build/win-amd64-release --parallel 10 -- -k 0
```

## Run
```
cd Fable2Recomp\out\build\win-amd64-release
Fable2.exe --allow_game_relative_writes true --game_data_root D:\Documents\Fable2RE\Fable2Recomp\assets\game
```
Logs: `logs/Fable2_NNN.log`. Requires `assets/update/data/tu1_data.bnk` + `build_version.txt`.

## Debug a crash (cdb)
```
cdb -c "g; r; kb 20; q" Fable2.exe --allow_game_relative_writes true --game_data_root <assets\game>
```
- `g` runs to the (2nd-chance) AV; `kb`/`k` = call stack; symbolize host addr via `Fable2.map`
  (`target = 0x140000000 + (hostaddr - modulebase)`).
- Guest reg from ctx: at the crash `rsi`≈ctx; e.g. `r28 = [rsi+0xE0]`. membase `0x100000000`.
- Data breakpoint (find who writes a field): break at a committed point, then `ba w4 <hostaddr>`.
- `crash_stack.txt` (written by `src/DiagnosticHooks.cpp`) has the guest call chain RVAs.

## Ghidra (decompilation)
```
D:\Subuwu\tools\ghidra-public\support\analyzeHeadless.bat D:\Documents\Fable2RE\ghidra_proj \
    Fable2_TU1 -import D:\Documents\Fable2RE\default_tu1.xex -max-cpu 12          # (already done)
```
- To drive RE: open Ghidra GUI on `ghidra_proj` → enable GhidraMCP (Configure > plugins) → start
  its server (Tools > GhidraMCP, port 8089). Then `curl http://127.0.0.1:8089/get_version`,
  `/decompile`, `/list_functions`, etc. — 249 endpoints. Or run the MCP bridge
  `python REPlugins/GhidraMCP/bridge_mcp_ghidra.py`.
- Headless queries: `analyzeHeadless ghidra_proj Fable2_TU1 -process default_tu1.xex -postScript
  <YourScript.java/.py>` (no re-analysis).

## Build the AssetBrowser
```powershell
# in VS dev shell
cmake -S Fable2AssetBrowser\source -B Fable2AssetBrowser\source\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Fable2AssetBrowser\source\build --parallel 10
```
All deps auto-fetched (ImGui/zlib/stb/miniaudio/DirectXMath). The native runtime uses the
Windows SDK D3D12 stack; the legacy AssetBrowser may still use D3D11 for its preview tool.
