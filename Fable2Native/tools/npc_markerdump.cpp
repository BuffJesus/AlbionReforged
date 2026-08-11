// npc_markerdump — dump a level's creature-spawn markers (name, game pos, yaw) as JSON.
//
// Reads a Fable II level's <level>.save (XML name->GUID registry) + <level>.gdb
// (per-GUID record table) and, for each creature-spawn marker, follows the
// SimpleTransformComponent (field 0x619F96CF) -> Position (0xBD7C27D4) /
// Rotation (0x21EBC83B) vec3 chain to recover the marker's game-space transform.
//
// This is the ONE piece of new read code the NPC cook needs: the AssetBrowser's
// GdbParser::LookupPlacement does NOT traverse SimpleTransformComponent, so the
// creature markers are silently dropped by that path (ghidra_out/npc_spawn_re.txt
// §1.2). We read the 0x619F96CF chain directly using the byte-exact GdbView reader
// (GdbReaderInternal.h) — validated 109/109 MarkerCreatureGeneratorSpawnPoint +
// 15/15 "Creature Generator Spawn Point" + 1/1 PlayerStart, miss=0, on chapter2slums.
//
// Usage:
//   npc_markerdump <level>.save <level>.gdb [--filter SUBSTR] [--out markers.json]
// Default filter matches creature-spawn markers; pass --filter "" for all entities.
//
// Output JSON: {"markers":[{"name":..,"guid":..,"pos":[gx,gy,gz],"rot":[rx,ry,rz],
//   "yaw":rx}, ...],"hit":N,"miss":M}
// pos/rot are GAME space (VecX,VecY,VecZ). yaw = Rotation.VecX. The cook applies the
// {x,z,y} game->render swap itself (mirrors cook_levels._instance_transform).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Skybox/GdbReaderInternal.h"

using namespace Gdb::detail;

// Field hashes for the SimpleTransformComponent chain (ghidra_out/npc_spawn_re.txt).
static constexpr uint32_t kHashSimpleTransformComponent = 0x619F96CFu;
static constexpr uint32_t kHashPosition = 0xBD7C27D4u;
static constexpr uint32_t kHashRotation = 0x21EBC83Bu;

// Read the SimpleTransformComponent (0x619F96CF) -> Position / Rotation vec3 chain
// for a record. Returns true if Position resolved; rotation is optional (defaults 0).
static bool marker_transform(const GdbView& v, const std::vector<uint8_t>& b,
                             size_t rec, float& x, float& y, float& z,
                             float& rx, float& ry, float& rz)
{
    size_t stc = 0;
    if (!v.findLocal(rec, kHashSimpleTransformComponent, 6, stc, nullptr)) return false;
    uint32_t sth = ReadBeU32(b.data() + stc);
    size_t st = 0;
    if (!v.lookup(sth, st)) return false;

    auto readvec = [&](uint32_t fieldHash, float& a, float& bb, float& c) -> bool {
        size_t s = 0;
        if (!v.findLocal(st, fieldHash, 6, s, nullptr)) return false;
        uint32_t vh = ReadBeU32(b.data() + s);
        size_t vr = 0;
        if (!v.lookup(vh, vr)) return false;
        float vx, vy, vz;
        if (!v.readVec3Record(vr, vx, vy, vz)) return false;
        a = vx; bb = vy; c = vz;
        return true;
    };

    bool hp = readvec(kHashPosition, x, y, z);   // Position (VecX,VecY,VecZ)
    readvec(kHashRotation, rx, ry, rz);          // Rotation (VecX = yaw); optional
    return hp;
}

struct SaveEntry {
    uint32_t hash;
    std::string name;
};

// Parse <level>.save: an XML entity registry of <Entity name="X">0xHASH</Entity>.
// Minimal scanner (no XML dep): find each <Entity name=" ... "> ... 0xHASH.
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
        // Find the 0xHASH after the closing '>' of this tag.
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

// Emit a JSON string with the minimal escaping our names need (backslash + quote).
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
        std::fprintf(stderr,
            "usage: npc_markerdump <level>.save <level>.gdb "
            "[--filter SUBSTR] [--out markers.json]\n");
        return 2;
    }
    const char* save_path = argv[1];
    const char* gdb_path = argv[2];
    // Default matches both creature-spawn marker classes (+ PlayerStart) via
    // substring OR; pass --filter "" for every entity.
    std::string filter = "";
    bool filter_set = false;
    std::string out_path;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
            filter_set = true;
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        }
    }

    std::vector<uint8_t> save_bytes = read_file(save_path);
    std::vector<uint8_t> gdb_bytes = read_file(gdb_path);
    if (save_bytes.empty()) { std::fprintf(stderr, "cannot read .save: %s\n", save_path); return 1; }
    if (gdb_bytes.empty()) { std::fprintf(stderr, "cannot read .gdb: %s\n", gdb_path); return 1; }

    std::string xml((const char*)save_bytes.data(), save_bytes.size());
    std::vector<SaveEntry> entries = parse_save(xml);

    GdbView v(gdb_bytes);
    if (!v.ok) { std::fprintf(stderr, "not a valid GDB: %s\n", gdb_path); return 1; }

    // Default marker prefixes when --filter not given: the creature-spawn classes.
    auto matches = [&](const std::string& n) -> bool {
        if (filter_set) return filter.empty() || n.find(filter) != std::string::npos;
        return n.find("MarkerCreatureGeneratorSpawnPoint") != std::string::npos ||
               n.find("Creature Generator Spawn Point") != std::string::npos;
    };

    std::string json = "{\n  \"markers\": [\n";
    int hit = 0, miss = 0;
    bool first = true;
    for (const SaveEntry& e : entries) {
        if (!matches(e.name)) continue;
        size_t rec = 0;
        if (!v.lookup(e.hash, rec)) { miss++; continue; }
        float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0;
        if (!marker_transform(v, gdb_bytes, rec, x, y, z, rx, ry, rz)) { miss++; continue; }
        hit++;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "%s    {\"name\": \"%s\", \"guid\": \"0x%08X\", "
            "\"pos\": [%.6f, %.6f, %.6f], \"rot\": [%.6f, %.6f, %.6f], \"yaw\": %.6f}",
            first ? "" : ",\n", json_escape(e.name).c_str(), e.hash,
            x, y, z, rx, ry, rz, rx);
        json += buf;
        first = false;
    }
    json += "\n  ],\n";
    {
        char tail[128];
        std::snprintf(tail, sizeof(tail), "  \"hit\": %d,\n  \"miss\": %d\n}\n", hit, miss);
        json += tail;
    }

    if (!out_path.empty()) {
        std::ofstream o(out_path, std::ios::binary);
        o.write(json.data(), (std::streamsize)json.size());
        std::fprintf(stderr, "# hit=%d miss=%d -> %s\n", hit, miss, out_path.c_str());
    } else {
        std::fwrite(json.data(), 1, json.size(), stdout);
        std::fprintf(stderr, "# hit=%d miss=%d\n", hit, miss);
    }
    return 0;
}
