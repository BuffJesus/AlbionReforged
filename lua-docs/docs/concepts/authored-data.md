# Authored data: GDB & text

Scripts are only half the game. The other half is **authored data** — every quest's
cast, every marker's position, every line of dialogue, every entity archetype —
and it lives outside Lua, in the **GDB** database and the **`book.babel`** text
blob. A quest that runs but has nothing to say is a quest reading no data.

Both formats are now decoded, so the port speaks **the game's own words** and
places **the game's own cast**, instead of invented English and hardcoded
positions.

## GDB — the game database

A `.gdb` file is a flat set of **records** keyed by an opaque 32-bit **GUID**.
A record is a list of `{name_hash, type, value}` fields. Header is big-endian,
magic `GDB\0`:

| Piece | What |
|---|---|
| Record table | GUID → byte offset of the record body |
| **Name table** | `fnv1(name)` → GUID — the only way to go from a *name* like `QC010_Rose` to a record |
| **String table** | interned strings: every field *name*, and every type-4 field *value* |
| `parent` field | a record inherits its parent's fields — resolution walks the chain |

Field types:

| Type | Meaning |
|---|---|
| 0 | bool |
| 1 | s32 |
| 2 | u32 |
| 3 | float |
| 4 | string (interned) |
| 6 | sub-record (inline) |
| 7 | record reference (by GUID) |

Two things bite when reading GDB:

!!! warning "Field names repeat — enumerate, don't look up"
    A record can hold **many fields with the same name** (a cutscene's
    `SceneElements` is a run of `SayLine`, `Wait`, `SayLine`, …). A
    name→value map silently keeps one. Enumerate the field list in order —
    and **field order is meaningful**: for a scene element list it *is* play order.

!!! warning "A type-4 value is not always a pooled string"
    On animation slots the raw u32 **is the bank clip key**, not a string-pool
    index. A dump tool printing it as `0x…` is just showing an unpooled value.

### From Lua

```lua
if GDB.RecordExists("QC010_Rose") then
  local rose  = GDB.GetRecord("QC010_Rose")
  local phys  = rose:GetRecord("PhysicsSimulationCharacterNavigatorComponent")
  local pos   = phys:GetRecord("Position")
  local x     = pos:GetFloat("X")
end
```

`GdbRecord` exposes `GetBool` / `GetInt` / `GetFloat` / `GetString` / `GetRecord` /
`GetID`.

!!! danger "`GetRecord` on a missing field returns a **null record**, never `nil`"
    The game's scripts chain `a:GetRecord(x):GetRecord(y)` with no nil checks. A
    `nil` return crashes the script one call later, far from the cause. A null
    record answers every getter with a zero value and keeps the chain alive.

!!! danger "A GUID cannot round-trip through Lua — hand out tokens"
    `lua_Number` here is **`float32`**. `0x8EB51907` comes back as `0x8EB51900`.
    Record ids therefore cross the boundary as **dense tokens** (`gdb_token_for` /
    `gdb_guid_for_token`), small integers a float32 represents exactly. Any 32-bit
    id you pass to script needs the same treatment — see [the VM](the-vm.md).

## `book.babel` — the localised text

The long-standing "unknown bulk-text codec" turned out to be **plain zlib**. The
layout is a hash index over compressed blocks, all big-endian:

```
0x00  u32  version/magic          (0x5B010000 in the shipped en-uk file)
0x04  u32  entry_count            (56000 in en-uk)
0x08  entry_count × { u32 fnv1(TextTag), u32 blockKey, u32 byteOffsetInBlock }
                                   sorted ascending by hash
then  u32  block_count            (333)
      block_count × { u32 blockKey, u32 compressedSize, u32 uncompressedSize }
                     + compressedSize bytes of a zlib stream (0x78 0xDA),
                       inflating to ~16 KB of packed strings

a string at byteOffsetInBlock:  u32 length in UTF-16 code units (including the
                                trailing NUL), then that many UTF-16 BIG-ENDIAN units
```

So `text(tag) = string_at(inflate(block[idx[fnv1(tag)].blockKey]), idx[…].offset)`.
Verified end to end: `TEXT_LEVEL_FAIRFAX_CASTLE` → *"Castle Fairfax"*.

The runtime doesn't parse `book.babel` directly — `tools/babel_text.py --cook`
bakes a flat `F2TEXT` lookup package (`{hash, offset}` rows over one UTF-8 blob)
that `TextTable` mmap-reads. From Lua it's just:

```lua
local name = GetText("TEXT_LEVEL_FAIRFAX_CASTLE")   -- "Castle Fairfax"
```

## Why this pair matters

Nearly every authored thing is a **tag in GDB resolved through babel**: a quest's
display name, a cutscene line's `TextTag`, an item's description. Once both are
decoded, a script beat like *"Rose says `TEXT_QC010_ROSE_02`"* renders as the
sentence the game shipped.

!!! note "Where"
    `Fable2Native/{include/f2,src}/native_gdb.*`, `native_text.*`; bindings in
    `native_bindings.cpp`. Tools: `tools/babel_text.py`, `tools/gdb_record_dump.py`,
    `tools/gdb_entity_dump.py`, `tools/cook_quest_markers.py`.
