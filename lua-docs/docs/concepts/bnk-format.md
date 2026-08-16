# BNK container format

Fable II packs its scripts into **BNK** containers. The gameplay scripts live in
`data/gamescripts_r.bnk` (553 entries); the front-end scripts live in
`data/guiscripts.bnk`. This is the **v3** format, byte-verified against the retail
bank.

## Layout

All multi-byte integers are **big-endian** `u32`.

```
offset 0   u32  base_offset            (= 0x8000)
offset 4   u32  version                (= 3)
offset 8   u8   compress_file_data     (0 or 1)
offset 9   ── file table ──
             ( u32 comp_size, u32 decomp_size, u8[comp_size] )*   terminated by comp_size == 0
             the comp bytes concatenated form ONE zlib stream → the TOC blob
```

### The TOC blob

Inflate the file-table streams and you get the table of contents:

```
u32 file_count
per entry:
    u32        name_len
    u8[name_len] name            (trailing NUL; lowercased, backslash-separated)
    u32        rel_off           (data at base_offset + rel_off)
    u32        decomp_size
    u32        comp_size
    u32        chunk_count
    u32        chunk_decomp_size[chunk_count]
```

An entry's compressed payload is at `file[base_offset + rel_off .. + comp_size]`.

## Two compression schemes

This is the subtle part — and the source of a real bug (below). An entry's payload
is **not** one zlib stream with a fixed stride. There are two cases, keyed on the
compressed size:

=== "Large entries (`comp_size > 32768`)"

    The payload is split into **independent zlib streams every 32768 *compressed*
    bytes**. Inflate each 32 KB block on its own and concatenate.

=== "Small entries (`comp_size ≤ 32768`)"

    The payload is a sequence of **back-to-back zlib streams**. There is no fixed
    stride — you inflate one stream, see how many compressed bytes it consumed, and
    start the next stream there. The TOC's per-chunk decompressed sizes are the
    targets. Most entries (including all quests) are a single stream here.

Finally, truncate the concatenated output to `decomp_size`.

## The bug that hid the quests

The first implementation assumed a fixed `0x8000` **compressed** stride for every
entry. That happens to work for single-block miscellaneous scripts, so the boot
scripts loaded — but every `quests/*` entry silently produced **zero bytes**. The
"extraction works" unit test that should have caught it was compiled under `NDEBUG`
(so its `assert` was stripped) and never actually ran.

Two fixes were needed:

1. **Match the real chunking** (large = 32 KB blocks, small = sequential streams by
   consumed bytes), mirroring the reference extractor in
   `tools/lua_mod/script_index.py`.
2. **Tolerate a miniz quirk.** When the output buffer is exactly the decompressed
   size, `miniz` returns `MZ_BUF_ERROR` instead of `MZ_STREAM_END` even though the
   data decompressed correctly. The extractor now accepts a fully-consumed stream as
   success (it validates against `decomp_size` and the LuaQ signature anyway).

!!! note "Reference implementations"
    - **C++ runtime:** `Fable2Native/src/native_bnk.cpp` (`BnkReader`).
    - **Python tooling:** `tools/lua_mod/bnk_repack.py` (read/repack) and
      `tools/lua_mod/script_index.py` (`entry_bytes`, the canonical inflate).

## Verifying an entry

The payload of a script entry is LuaQ bytecode, so a correct extraction starts with
the LuaQ signature:

```
1B 4C 75 61 51 00  01 04 04 04 04 00
└─ "\x1bLuaQ" ──┘  └─ header ─────┘
```

If your extraction starts with `1B 4C 75 61 51`, you inflated it correctly. What
that header means is the subject of the [next page](luaq-bytecode.md).
