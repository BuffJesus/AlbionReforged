// propdump — dump a level's renderable .gdb/.save ENTITY PROPS (name, game pos,
// yaw, model-path) as JSON for the native world prop-cook (cook_levels.py --props).
//
// The engine_level's type-2/21 blocks only cover the ~16 STATIC prop archetypes.
// The town's real density (furniture, crates, barrels, doors, walls, railings,
// and the ~37 townhouse FACADES) lives as ~1800 placed ENTITIES in the .save/.gdb
// layer. Each such entity carries a GraphicAppearanceStaticMeshComponent whose
// referenced model-resource subrecord names the render model (.mdl). We recover
// that plus the placement transform, so the cooker can glue+place each prop.
//
// Chain (ghidra_out/gdb_component_schemas.txt + gdb_entity_spec.txt, and the
// existing AssetBrowser GdbParser::TryReadStaticMeshModelPathHash):
//   entity record --findField(0x29CF50D1, type6, parent-walked)--> model-resource GUID
//   lookup(GUID) --> resource record
//   findField(0x0C17DB4E ModelFile, type 3/4/6, parent-walked) --> model-path HASH
//     (FNV-1(lowercase) of the .mdl path; type-4 strings are stored as their hash)
//   reverse the hash via a name->hash scan of the level.gdb + globals.gdb string
//   blocks (every interned string is preceded by its BE FNV-1 hash) --> model path.
// Placement reuses the transform chain from npc_markerdump/lightdump: one of the
// THREE transform-component field hashes (0x619F96CF / 0x6B177DD0 / 0xF73572C4) ->
// Position (0xBD7C27D4) / Rotation (0x21EBC83B) vec3.
//
// Usage:
//   propdump <level>.save <level>.gdb [--globals-gdb globals.gdb] [--out props.json]
//
// Output JSON: {"props":[{"name","guid","pos":[gx,gy,gz],"yaw":f,"model":".."}...],
//   "hit":N,"no_model":M,"no_xform":K,"markers_skipped":S}
// pos = GAME space (VecX,VecY,VecZ); yaw = Rotation.VecX. The cook applies the
// {x,z,y} game->render swap itself (mirrors cook_levels._instance_transform).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Skybox/GdbReaderInternal.h"

using namespace Gdb::detail;

// Transform-component field hashes (npc_spawn_re.txt / level_lights_effects_re.txt).
static constexpr uint32_t kHashSimpleTransformComponent = 0x619F96CFu;
static constexpr uint32_t kHashKeyframedComponent = 0x6B177DD0u;  // PhysicsSimulationKeyframed
static constexpr uint32_t kHashTransformComponent = 0xF73572C4u;
static constexpr uint32_t kHashPosition = 0xBD7C27D4u;
static constexpr uint32_t kHashRotation = 0x21EBC83Bu;

// Static-mesh render-model chain (gdb_component_schemas.txt:154,322 / GdbParser.cpp).
static constexpr uint32_t kHashStaticMeshComponent = 0x29CF50D1u;         // type6 ref -> model-resource
static constexpr uint32_t kHashStaticMultipleMeshComponent = 0xCE642A15u; // type6 ref -> multi-mesh
static constexpr uint32_t kHashModelFile = 0x0C17DB4Eu;                   // type3/4/6 -> model-path hash
static constexpr uint32_t kHashModelFile1 = 0x578E3BFBu;
static constexpr uint32_t kHashModelFile2 = 0x578E3BF8u;
static constexpr uint32_t kHashStaticMultipleModelFile = 0x1372D766u;
static constexpr uint32_t kHashNull = 0x811C9DC5u;

// -------- FNV-1 (lowercase) path hash + the reverse name<-hash scan --------
// The GDB string blocks store each interned string preceded by its BE FNV-1
// (case-sensitive, basis 0x811C9DC5, prime 0x01000193) hash. We validate that
// guard hash, then also index the string by its FNV-1-LOWER hash, because the
// ModelFile type-4 value is FNV-1 of the LOWERCASED model path (GdbParser.cpp
// Fnv1LowerStr). So a model-path hash reverse-maps to the literal path.
static uint32_t fnv1_lower(const uint8_t* p, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; ++i) {
        h = uint32_t(h * 0x01000193u);
        h ^= uint8_t(std::tolower(p[i]));
    }
    return h;
}

// Scan a GDB blob for [BE FNV-1 exact hash][ascii string][NUL] and index the
// string by its FNV-1-lower hash. Verbatim port of GdbParser::CollectGdbNameStrings.
static void collect_name_strings(const std::vector<uint8_t>& bytes,
                                 std::unordered_map<uint32_t, std::string>& lower_out)
{
    const uint8_t* p = bytes.data();
    const size_t n = bytes.size();
    size_t i = 4;
    while (i + 1 < n) {
        if (p[i] < 0x20 || p[i] >= 0x7f) { ++i; continue; }
        size_t j = i;
        while (j < n && p[j] >= 0x20 && p[j] < 0x7f) ++j;
        const size_t len = j - i;
        if (len >= 1 && j < n && p[j] == 0 && i >= 4) {
            const uint32_t stored = ReadBeU32(p + i - 4);
            uint32_t h = 0x811C9DC5u;
            for (size_t k = i; k < j; ++k) { h = uint32_t(h * 0x01000193u); h ^= p[k]; }
            if (h == stored) {
                lower_out.emplace(fnv1_lower(p + i, len),
                                  std::string(reinterpret_cast<const char*>(p + i), len));
            }
        }
        i = j + 1;
    }
}

// Read the transform-component -> Position/Rotation vec3 chain for an entity.
// Tries each of the three transform-component hashes (props use a mix). Returns
// true if Position resolved; rotation is optional (defaults 0).
static bool prop_transform(const GdbView& v, const std::vector<uint8_t>& b, size_t rec,
                           float& x, float& y, float& z, float& rx, float& ry, float& rz)
{
    static const uint32_t kTransformHashes[] = {
        kHashSimpleTransformComponent, kHashKeyframedComponent, kHashTransformComponent};
    for (uint32_t th : kTransformHashes) {
        size_t stc = 0;
        if (!v.findLocal(rec, th, 6, stc, nullptr)) continue;
        uint32_t sth = ReadBeU32(b.data() + stc);
        size_t st = 0;
        if (!v.lookup(sth, st)) continue;
        // Position
        size_t pslot = 0;
        if (!v.findLocal(st, kHashPosition, 6, pslot, nullptr)) continue;
        uint32_t ph = ReadBeU32(b.data() + pslot);
        size_t pr = 0;
        if (!v.lookup(ph, pr) || !v.readVec3Record(pr, x, y, z)) continue;
        // Rotation (optional)
        rx = ry = rz = 0.0f;
        size_t rslot = 0;
        if (v.findLocal(st, kHashRotation, 6, rslot, nullptr)) {
            uint32_t rh = ReadBeU32(b.data() + rslot);
            size_t rr = 0;
            float vx, vy, vz;
            if (v.lookup(rh, rr) && v.readVec3Record(rr, vx, vy, vz)) { rx = vx; ry = vy; rz = vz; }
        }
        return true;
    }
    return false;
}

// Read a model-path hash from a resource record's ModelFile field (accept 3/4/6),
// walking kHashParent for archetype inheritance.
static bool read_model_field(const GdbView& v, const std::vector<uint8_t>& b, size_t rec,
                             uint32_t field_hash, uint32_t& out_hash)
{
    size_t slot = 0;
    uint8_t type = 0;
    if (!v.findField(rec, field_hash, 0xFF, slot, &type)) return false;
    if (type != 3 && type != 4 && type != 6) return false;
    out_hash = ReadBeU32(b.data() + slot);
    return out_hash != 0 && out_hash != kHashNull;
}

// Resolve an entity's render-model path HASH via the static-mesh component chain.
// Returns 0 if the entity has no static render model.
static uint32_t entity_model_hash(const GdbView& v, const std::vector<uint8_t>& b, size_t rec)
{
    // Single static mesh: component (type6, parent-walked) -> resource -> ModelFile.
    size_t comp_slot = 0;
    if (v.findField(rec, kHashStaticMeshComponent, 6, comp_slot, nullptr)) {
        uint32_t res_hash = ReadBeU32(b.data() + comp_slot);
        size_t res_rec = 0;
        if (v.lookup(res_hash, res_rec)) {
            uint32_t mh = 0;
            if (read_model_field(v, b, res_rec, kHashModelFile, mh)) return mh;
            if (read_model_field(v, b, res_rec, kHashModelFile1, mh)) return mh;
            if (read_model_field(v, b, res_rec, kHashModelFile2, mh)) return mh;
        }
    }
    // Multiple-mesh: component -> resource; take the first StaticMultipleModelFile.
    if (v.findField(rec, kHashStaticMultipleMeshComponent, 6, comp_slot, nullptr)) {
        uint32_t res_hash = ReadBeU32(b.data() + comp_slot);
        size_t res_rec = 0;
        if (v.lookup(res_hash, res_rec)) {
            uint32_t mh = 0;
            if (read_model_field(v, b, res_rec, kHashStaticMultipleModelFile, mh)) return mh;
            if (read_model_field(v, b, res_rec, kHashModelFile, mh)) return mh;
        }
    }
    return 0;
}

struct SaveEntry { uint32_t hash; std::string name; };

// Parse <level>.save (XML <Entity name="X">0xHASH</Entity>). Copied verbatim from
// npc_markerdump.cpp / lightdump.cpp (the .save format is the same for every level).
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

// Skip pure markers / spawn points / AI verbs — they carry no renderable static
// model (the model resolve would fail anyway, but skip early to keep the census
// honest and cheap). Matches the task's skip list.
static bool is_marker_like(const std::string& n)
{
    static const char* kSkip[] = {
        "Marker", "SpawnPoint", "Spawn Point", "GeneratedEntity", "Generator",
        "Sleep", "Clean", "Urinate", "Trigger", "Region", "Zone", "PathNode",
        "Waypoint", "Volume", "CameraPoint", "AudioMarker", "NavMesh"};
    for (const char* s : kSkip)
        if (n.find(s) != std::string::npos) return true;
    return false;
}

static std::string json_escape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) { if (c == '\\' || c == '"') o.push_back('\\'); o.push_back(c); }
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
        std::fprintf(stderr, "usage: propdump <level>.save <level>.gdb "
                             "[--globals-gdb globals.gdb] [--out props.json] [--all]\n");
        return 2;
    }
    const char* save_path = argv[1];
    const char* gdb_path = argv[2];
    std::string out_path, globals_path;
    std::vector<std::string> models_lists;  // newline-delimited model-bank entry names
    bool keep_markers = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_path = argv[++i];
        else if (std::strcmp(argv[i], "--globals-gdb") == 0 && i + 1 < argc) globals_path = argv[++i];
        else if (std::strcmp(argv[i], "--models-list") == 0 && i + 1 < argc) models_lists.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--all") == 0) keep_markers = true;  // don't skip markers
    }

    std::vector<uint8_t> save_bytes = read_file(save_path);
    std::vector<uint8_t> gdb_bytes = read_file(gdb_path);
    if (save_bytes.empty()) { std::fprintf(stderr, "cannot read .save: %s\n", save_path); return 1; }
    if (gdb_bytes.empty()) { std::fprintf(stderr, "cannot read .gdb: %s\n", gdb_path); return 1; }

    std::string xml((const char*)save_bytes.data(), save_bytes.size());
    std::vector<SaveEntry> entries = parse_save(xml);

    GdbView v(gdb_bytes);
    if (!v.ok) { std::fprintf(stderr, "not a valid GDB: %s\n", gdb_path); return 1; }

    // Build the hash->path name map. The ModelFile value is FNV-1(lowercase) of the
    // full backslash .mdl path; those path strings do NOT live in the gdb string
    // blocks — they live as MODEL-BANK entry names. So the primary reverse source is
    // --models-list files (one bank entry name per line: the level's
    // <scenario>_models.bnk + globals_model_headers.bnk name tables). We still scan
    // the gdbs too, so inline string archetypes (rare) resolve as well.
    std::unordered_map<uint32_t, std::string> name_by_lower;
    collect_name_strings(gdb_bytes, name_by_lower);
    std::vector<uint8_t> globals_bytes;
    if (!globals_path.empty()) {
        globals_bytes = read_file(globals_path.c_str());
        if (!globals_bytes.empty()) collect_name_strings(globals_bytes, name_by_lower);
        else std::fprintf(stderr, "# warning: cannot read globals gdb %s\n", globals_path.c_str());
    }
    for (const std::string& lst : models_lists) {
        std::ifstream in(lst);
        if (!in) { std::fprintf(stderr, "# warning: cannot read models-list %s\n", lst.c_str()); continue; }
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                     line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty()) continue;
            // Hash the full name AND the .gmd-stripped form (bank entries are
            // <path>.mdl.gmd; the entity path is <path>.mdl -> same FNV once .gmd
            // is dropped). Emit the clean .mdl path as the resolved model.
            std::string clean = line;
            if (clean.size() >= 4) {
                std::string tail = clean.substr(clean.size() - 4);
                for (char& c : tail) c = char(std::tolower((unsigned char)c));
                if (tail == ".gmd") clean = clean.substr(0, clean.size() - 4);
            }
            name_by_lower.emplace(
                fnv1_lower(reinterpret_cast<const uint8_t*>(clean.data()), clean.size()), clean);
            name_by_lower.emplace(
                fnv1_lower(reinterpret_cast<const uint8_t*>(line.data()), line.size()), clean);
        }
    }

    std::string json = "{\n  \"props\": [\n";
    int hit = 0, no_model = 0, no_xform = 0, markers_skipped = 0, unresolved_name = 0;
    bool first = true;
    for (const SaveEntry& e : entries) {
        if (!keep_markers && is_marker_like(e.name)) { markers_skipped++; continue; }
        size_t rec = 0;
        if (!v.lookup(e.hash, rec)) continue;

        uint32_t mh = entity_model_hash(v, gdb_bytes, rec);
        if (mh == 0) { no_model++; continue; }
        auto it = name_by_lower.find(mh);
        std::string model;
        if (it != name_by_lower.end()) {
            model = it->second;
        } else {
            // Name not in the GDB string blocks: emit the raw hash so the cooker
            // can resolve it against the model banks (which are keyed by the same
            // FNV-1-lower model-path hash).
            unresolved_name++;
            char hbuf[24];
            std::snprintf(hbuf, sizeof(hbuf), "hash:0x%08X", mh);
            model = hbuf;
        }

        float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0;
        if (!prop_transform(v, gdb_bytes, rec, x, y, z, rx, ry, rz)) { no_xform++; continue; }

        hit++;
        std::string buf;
        buf.reserve(model.size() + 256);
        char head[640];
        std::snprintf(head, sizeof(head),
            "%s    {\"name\": \"%s\", \"guid\": \"0x%08X\", "
            "\"pos\": [%.6f, %.6f, %.6f], \"yaw\": %.6f, \"model\": \"",
            first ? "" : ",\n", json_escape(e.name).c_str(), e.hash, x, y, z, rx);
        buf += head;
        buf += json_escape(model);
        buf += "\"}";
        json += buf;
        first = false;
    }
    json += "\n  ],\n";
    {
        char tail[256];
        std::snprintf(tail, sizeof(tail),
            "  \"hit\": %d,\n  \"no_model\": %d,\n  \"unresolved_name\": %d,\n"
            "  \"no_xform\": %d,\n  \"markers_skipped\": %d\n}\n",
            hit, no_model, unresolved_name, no_xform, markers_skipped);
        json += tail;
    }

    if (!out_path.empty()) {
        std::ofstream o(out_path, std::ios::binary);
        o.write(json.data(), (std::streamsize)json.size());
        std::fprintf(stderr, "# props hit=%d no_model=%d unresolved_name=%d no_xform=%d "
                             "markers_skipped=%d -> %s\n",
                     hit, no_model, unresolved_name, no_xform, markers_skipped, out_path.c_str());
    } else {
        std::fwrite(json.data(), 1, json.size(), stdout);
        std::fprintf(stderr, "# props hit=%d no_model=%d unresolved_name=%d no_xform=%d "
                             "markers_skipped=%d\n",
                     hit, no_model, unresolved_name, no_xform, markers_skipped);
    }
    return 0;
}
