# Resource evidence tools

- `read_tu1_table_capture.py`: strict table/name snapshot reader; optional raw record export.
- `read_tu1_manifest_capture.py`: bounded manifest sidecar reader preserving counted UTF-16 units.
- `lua_mod/bnk_repack.py` and `lua_mod/script_index.py`: optional reference helpers for bank corpus checks.

Use `--help` where supported. Original banks and captures are user-owned local
inputs. No game data is included. Most broader RE/build automation remains in the
local integration workspace pending its own source/dependency review.
