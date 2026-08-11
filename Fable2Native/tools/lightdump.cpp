// lightdump — dump a level's LOCAL POINT LIGHTS (lamp posts, lanterns, braziers,
// placeable accents) as JSON for the native world-light cook.
//
// A Fable II level's point/spot lights are ENTITIES in the .save/.gdb layer (NOT
// the engine_level graphics file). Each light entity carries a LIGHT component
// (GDB field-name hash 0xBB61B654) holding Intensity/Range/Colour/LightType, plus
// a SimpleTransformComponent (0x619F96CF) -> Position (0xBD7C27D4) for its world
// placement. Authority: ghidra_out/level_lights_effects_re.txt (§1) — 116 lights
// validated on chapter2slums (49 white LampPosts, warm HangingLanterns 255,163,47,
// fire Braziers 255,167,51, coloured PlaceableLights).
//
// This reuses the byte-exact GdbView reader (GdbReaderInternal.h) exactly like
// npc_markerdump.cpp: the transform read is identical; we add the 0xBB61B654 light
// component read (Intensity/Range = t3 f32; Colour = t6 subrecord -> Red/Green/Blue
// t1 u8; LightType = t5 u32), using the PARENT-WALKING accessor (findFieldOwner) so
// per-instance override records inherit the archetype's defaults (spec §1.2).
//
// Usage:
//   lightdump <level>.save <level>.gdb [--out lights.json]
//
// Output JSON: {"lights":[{"name","guid","pos":[gx,gy,gz],"rgb":[r,g,b],
//   "intensity":f,"range":f,"type":u}, ...],"hit":N,"miss":M}
// pos = GAME space (VecX,VecY,VecZ); rgb = 0..255; range = world units. The cook
// applies the {x,z,y} game->render swap + rgb/255, exactly like the other cookers.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Skybox/GdbReaderInternal.h"

using namespace Gdb::detail;

// Transform chain (shared with npc_markerdump / ghidra_out/npc_spawn_re.txt).
static constexpr uint32_t kHashSimpleTransformComponent = 0x619F96CFu;
static constexpr uint32_t kHashPosition = 0xBD7C27D4u;

// Light component + its fields (ghidra_out/level_lights_effects_re.txt §1.2).
static constexpr uint32_t kHashLightComponent = 0xBB61B654u;  // t6 subrecord
static constexpr uint32_t kHashIntensity = 0x8F89F9D4u;       // t3 f32
static constexpr uint32_t kHashRange = 0xF21E235Cu;           // t3 f32
static constexpr uint32_t kHashColour = 0xB76325F5u;          // t6 subrecord
static constexpr uint32_t kHashRed = 0x3A232172u;             // t1 u8
static constexpr uint32_t kHashGreen = 0x608C9792u;           // t1 u8
static constexpr uint32_t kHashBlue = 0xB1911CC9u;            // t1 u8
static constexpr uint32_t kHashLightType = 0x141A09CBu;       // t5 u32

// A resolver over the level gdb PLUS an optional globals gdb. Fable II light
// archetypes chain up kHashParent (0x5F6317D5) into data\Globals\globals.gdb: a
// level's placed light record overrides Intensity/Range locally but INHERITS its
// Colour + LightType + spot params from the globals archetype (verified on
// chapter2slums — the LampPost light record 0x2E031357 holds I/R in-level and its
// parent 0xE3FFF791 with the Colour lives only in globals.gdb). GdbView chains
// within one buffer, so we walk the parent chain by hand across both views.
struct LightResolver {
    const GdbView& level;
    const std::vector<uint8_t>& lb;
    const GdbView* globals = nullptr;         // optional
    const std::vector<uint8_t>* gb = nullptr;

    // Resolve a record GUID in the level first, then globals. Fills view/bytes/rec.
    bool resolve(uint32_t hash, const GdbView*& view, const std::vector<uint8_t>*& bytes,
                 size_t& rec) const
    {
        if (level.lookup(hash, rec)) { view = &level; bytes = &lb; return true; }
        if (globals && gb && globals->lookup(hash, rec)) {
            view = globals; bytes = gb; return true;
        }
        return false;
    }

    // Find a field of a given type by walking kHashParent across both gdbs. On hit,
    // returns the owning view/bytes + slot. Mirrors GdbView::findFieldOwner but the
    // parent lookup can cross into globals.
    bool findField(uint32_t start_hash, uint32_t field_hash, uint8_t type,
                   const GdbView*& out_view, const std::vector<uint8_t>*& out_bytes,
                   size_t& out_slot) const
    {
        uint32_t cur_hash = start_hash;
        for (int depth = 0; depth < 64; ++depth) {
            const GdbView* view = nullptr;
            const std::vector<uint8_t>* bytes = nullptr;
            size_t rec = 0;
            if (!resolve(cur_hash, view, bytes, rec)) return false;
            size_t slot = 0;
            if (view->findLocal(rec, field_hash, type, slot, nullptr)) {
                out_view = view; out_bytes = bytes; out_slot = slot;
                return true;
            }
            size_t pslot = 0;
            if (!view->findLocal(rec, kHashParent, 6, pslot, nullptr)) return false;
            uint32_t parent = ReadBeU32(bytes->data() + pslot);
            if (parent == 0 || parent == cur_hash) return false;
            cur_hash = parent;
        }
        return false;
    }

    bool read_f32(uint32_t start_hash, uint32_t field_hash, float& out) const
    {
        const GdbView* view; const std::vector<uint8_t>* bytes; size_t slot;
        if (!findField(start_hash, field_hash, 3, view, bytes, slot)) return false;
        out = ReadBeF32(bytes->data() + slot);
        return true;
    }
    bool read_u32(uint32_t start_hash, uint32_t field_hash, uint32_t& out) const
    {
        const GdbView* view; const std::vector<uint8_t>* bytes; size_t slot;
        if (!findField(start_hash, field_hash, 5, view, bytes, slot)) return false;
        out = ReadBeU32(bytes->data() + slot);
        return true;
    }
    // Read a t1 u8 (0..255): the value sits in a 4-byte big-endian slot, so a u8
    // lands in the LOW byte (raw & 0xFF).
    bool read_u8(uint32_t start_hash, uint32_t field_hash, uint32_t& out) const
    {
        const GdbView* view; const std::vector<uint8_t>* bytes; size_t slot;
        if (!findField(start_hash, field_hash, 1, view, bytes, slot)) return false;
        out = ReadBeU32(bytes->data() + slot) & 0xFFu;
        return true;
    }
};

// Read the transform-component -> Position vec3 for a level entity. Fable II uses
// THREE transform-component field hashes (level_lights_effects_re.txt §1.1 /
// npc_spawn_re.txt): 0x619F96CF, 0x6B177DD0, 0xF73572C4 — LampPosts use 0x6B177DD0,
// PlaceableLights 0x619F96CF. Try each; the transform record + Position are in-level.
static bool light_position(const GdbView& v, const std::vector<uint8_t>& b,
                           size_t rec, float& x, float& y, float& z)
{
    static const uint32_t kTransformHashes[] = {kHashSimpleTransformComponent,
                                                0x6B177DD0u, 0xF73572C4u};
    for (uint32_t th : kTransformHashes) {
        size_t stc = 0;
        if (!v.findLocal(rec, th, 6, stc, nullptr)) continue;
        uint32_t sth = ReadBeU32(b.data() + stc);
        size_t st = 0;
        if (!v.lookup(sth, st)) continue;
        size_t pslot = 0;
        if (!v.findLocal(st, kHashPosition, 6, pslot, nullptr)) continue;
        uint32_t ph = ReadBeU32(b.data() + pslot);
        size_t pr = 0;
        if (!v.lookup(ph, pr)) continue;
        if (v.readVec3Record(pr, x, y, z)) return true;
    }
    return false;
}

struct SaveEntry {
    uint32_t hash;
    std::string name;
};

// Parse <level>.save (XML <Entity name="X">0xHASH</Entity>). Copied verbatim from
// npc_markerdump.cpp (the .save format is the same for every level).
static std::vector<SaveEntry> parse_save(const std::string& xml)
{
    std::vector<SaveEntry> out;
    const std::string kName = "<Entity name=\"";
    size_t p = 0;
    while ((p = xml.find(kName, p)) != std::string::npos) {
        size_t ns = p + kName.size();
        size_t ne = xml.find('"', ns);
        if (ne == std::string::npos) break;
        std::string name = xml.substr(ns, ne - ns);
        size_t gt = xml.find('>', ne);
        if (gt == std::string::npos) break;
        size_t hx = xml.find("0x", gt);
        size_t close = xml.find("</Entity>", gt);
        p = ne;
        if (hx == std::string::npos || close == std::string::npos || hx > close) continue;
        uint32_t h = (uint32_t)strtoul(xml.c_str() + hx + 2, nullptr, 16);
        out.push_back({h, name});
    }
    return out;
}

static std::string json_escape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

static std::vector<uint8_t> read_file(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: lightdump <level>.save <level>.gdb "
                             "[--globals-gdb globals.gdb] [--out lights.json]\n");
        return 2;
    }
    const char* save_path = argv[1];
    const char* gdb_path = argv[2];
    std::string out_path;
    std::string globals_path;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (std::strcmp(argv[i], "--globals-gdb") == 0 && i + 1 < argc)
            globals_path = argv[++i];
    }

    std::vector<uint8_t> save_bytes = read_file(save_path);
    std::vector<uint8_t> gdb_bytes = read_file(gdb_path);
    if (save_bytes.empty()) { std::fprintf(stderr, "cannot read .save: %s\n", save_path); return 1; }
    if (gdb_bytes.empty()) { std::fprintf(stderr, "cannot read .gdb: %s\n", gdb_path); return 1; }

    std::string xml((const char*)save_bytes.data(), save_bytes.size());
    std::vector<SaveEntry> entries = parse_save(xml);

    GdbView v(gdb_bytes);
    if (!v.ok) { std::fprintf(stderr, "not a valid GDB: %s\n", gdb_path); return 1; }

    // Optional globals.gdb — light Colour/LightType archetypes chain into it.
    std::vector<uint8_t> globals_bytes;
    if (!globals_path.empty()) {
        globals_bytes = read_file(globals_path.c_str());
        if (globals_bytes.empty())
            std::fprintf(stderr, "# warning: cannot read globals gdb %s (colours may be default)\n",
                         globals_path.c_str());
    }
    GdbView gv(globals_bytes);
    LightResolver R{v, gdb_bytes};
    if (gv.ok) { R.globals = &gv; R.gb = &globals_bytes; }
    else if (!globals_path.empty())
        std::fprintf(stderr, "# warning: globals gdb not valid; colours may fall back to default\n");

    std::string json = "{\n  \"lights\": [\n";
    int hit = 0, miss = 0, skipped_off = 0, no_colour = 0;
    bool first = true;
    for (const SaveEntry& e : entries) {
        size_t rec = 0;
        if (!v.lookup(e.hash, rec)) continue;
        // A light entity is one that carries a light component (0xBB61B654) locally
        // or up its parent chain. Gate on that so we only enumerate lights.
        size_t light_slot = 0, light_owner = 0;
        if (!v.findFieldOwner(rec, kHashLightComponent, 6, light_slot, light_owner, nullptr))
            continue;
        uint32_t lh = ReadBeU32(gdb_bytes.data() + light_slot);

        float x = 0, y = 0, z = 0;
        if (!light_position(v, gdb_bytes, rec, x, y, z)) { miss++; continue; }

        // Light params — resolve from the light-component GUID, chaining kHashParent
        // across level->globals so a placed light inherits its archetype defaults.
        float intensity = 1.0f, range = 0.0f;
        R.read_f32(lh, kHashIntensity, intensity);
        R.read_f32(lh, kHashRange, range);
        uint32_t type = 0;
        R.read_u32(lh, kHashLightType, type);

        // Colour: t6 subrecord (0xB76325F5) -> Red/Green/Blue t1 u8. Find the colour
        // subrecord (parent-aware, across globals), then read its three bytes there.
        uint32_t r = 255, g = 255, b = 255;
        const GdbView* cview; const std::vector<uint8_t>* cbytes; size_t colour_slot;
        if (R.findField(lh, kHashColour, 6, cview, cbytes, colour_slot)) {
            uint32_t ch = ReadBeU32(cbytes->data() + colour_slot);
            R.read_u8(ch, kHashRed, r);
            R.read_u8(ch, kHashGreen, g);
            R.read_u8(ch, kHashBlue, b);
        } else {
            no_colour++;  // fell back to white (globals gdb missing or archetype gap)
        }

        // Range<=0 / Intensity<=0 = an OFF/culled light (spec §1.4) — count but skip.
        if (range <= 0.0f || intensity <= 0.0f) { skipped_off++; continue; }

        hit++;
        char buf[640];
        std::snprintf(buf, sizeof(buf),
            "%s    {\"name\": \"%s\", \"guid\": \"0x%08X\", "
            "\"pos\": [%.6f, %.6f, %.6f], \"rgb\": [%u, %u, %u], "
            "\"intensity\": %.6f, \"range\": %.6f, \"type\": %u}",
            first ? "" : ",\n", json_escape(e.name).c_str(), e.hash,
            x, y, z, r, g, b, intensity, range, type);
        json += buf;
        first = false;
    }
    json += "\n  ],\n";
    {
        char tail[220];
        std::snprintf(tail, sizeof(tail),
                      "  \"hit\": %d,\n  \"miss\": %d,\n  \"off\": %d,\n"
                      "  \"no_colour\": %d\n}\n",
                      hit, miss, skipped_off, no_colour);
        json += tail;
    }

    if (!out_path.empty()) {
        std::ofstream o(out_path, std::ios::binary);
        o.write(json.data(), (std::streamsize)json.size());
        std::fprintf(stderr, "# lights hit=%d miss=%d off=%d no_colour=%d -> %s\n", hit,
                     miss, skipped_off, no_colour, out_path.c_str());
    } else {
        std::fwrite(json.data(), 1, json.size(), stdout);
        std::fprintf(stderr, "# lights hit=%d miss=%d off=%d no_colour=%d\n", hit, miss,
                     skipped_off, no_colour);
    }
    return 0;
}
