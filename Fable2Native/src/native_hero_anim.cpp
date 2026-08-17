#include "f2/native_hero_anim.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace f2 {

namespace {
// Little-endian cursor over the whole file (Windows is LE; the cook writes LE).
struct Cursor {
    const std::uint8_t* p;
    std::size_t n;
    std::size_t at = 0;
    bool bad = false;

    template <typename T>
    T read() {
        T v{};
        if (at + sizeof(T) > n) { bad = true; return v; }
        std::memcpy(&v, p + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    void read_into(void* dst, std::size_t bytes) {
        if (at + bytes > n) { bad = true; return; }
        std::memcpy(dst, p + at, bytes);
        at += bytes;
    }
};
}  // namespace

HeroAnimData load_hero_anim(const std::string& path) {
    HeroAnimData out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < 16) return out;
    Cursor c{bytes.data(), bytes.size()};

    char magic[4];
    c.read_into(magic, 4);
    if (std::memcmp(magic, "F2HA", 4) != 0) return out;
    const std::uint32_t version = c.read<std::uint32_t>();
    if (version != 1) return out;
    out.bone_count = static_cast<int>(c.read<std::uint32_t>());
    if (out.bone_count <= 0 || out.bone_count > 4096) return out;

    const std::uint32_t clip_count = c.read<std::uint32_t>();
    if (clip_count > 64) return out;
    out.clips.reserve(clip_count);
    out.root_speeds.reserve(clip_count);
    for (std::uint32_t i = 0; i < clip_count; ++i) {
        AnimClip clip;
        clip.hash = c.read<std::uint32_t>();
        clip.frame_count = static_cast<int>(c.read<std::uint32_t>());
        clip.fps = c.read<float>();
        const float root_speed = c.read<float>();
        clip.bone_count = out.bone_count;
        if (clip.frame_count <= 0 || clip.frame_count > 65536) return out;
        const std::size_t mats = static_cast<std::size_t>(clip.frame_count) * out.bone_count;
        clip.skin.resize(mats);
        c.read_into(clip.skin.data(), mats * sizeof(std::array<float, 12>));
        if (c.bad) return out;
        out.clips.push_back(std::move(clip));
        out.root_speeds.push_back(root_speed);
    }

    const std::uint32_t geom_count = c.read<std::uint32_t>();
    if (geom_count > 4096) return out;
    out.geom_bind.resize(geom_count);
    for (std::uint32_t g = 0; g < geom_count; ++g) {
        const std::uint32_t vcount = c.read<std::uint32_t>();
        if (c.bad || vcount > 5'000'000) return out;
        std::vector<SkinnedVertex>& verts = out.geom_bind[g];
        verts.resize(vcount);
        for (std::uint32_t v = 0; v < vcount; ++v) {
            SkinnedVertex& sv = verts[v];
            c.read_into(sv.position.data(), 3 * sizeof(float));
            c.read_into(sv.bones.data(), 4 * sizeof(std::uint16_t));
            c.read_into(sv.weights.data(), 4 * sizeof(float));
        }
        if (c.bad) return out;
    }

    const std::uint32_t ref_count = c.read<std::uint32_t>();
    if (ref_count > 4096) return out;
    out.ref_pose.resize(ref_count);
    for (std::uint32_t g = 0; g < ref_count; ++g) {
        const std::uint32_t vcount = c.read<std::uint32_t>();
        if (c.bad || vcount > 5'000'000) return out;
        std::vector<std::array<float, 3>>& posed = out.ref_pose[g];
        posed.resize(vcount);
        c.read_into(posed.data(), static_cast<std::size_t>(vcount) * sizeof(std::array<float, 3>));
        if (c.bad) return out;
    }

    out.ok = !c.bad;
    return out;
}

void compute_hero_pose(const AnimationPlayer& player,
                       const std::vector<std::vector<SkinnedVertex>>& geom_bind, float delta_yaw,
                       std::vector<std::vector<std::array<float, 3>>>& out) {
    out.resize(geom_bind.size());
    const float cs = std::cos(delta_yaw), sn = std::sin(delta_yaw);
    std::vector<std::array<float, 3>> skinned;
    for (std::size_t gi = 0; gi < geom_bind.size(); ++gi) {
        player.skin(geom_bind[gi], skinned);  // MDL-space posed positions
        std::vector<std::array<float, 3>>& o = out[gi];
        o.resize(skinned.size());
        for (std::size_t v = 0; v < skinned.size(); ++v) {
            const float rx = skinned[v][0], ry = skinned[v][2], rz = skinned[v][1];  // {x,z,y} swap
            // Y-rotate by delta_yaw (place_vertex convention) -> renderer's baked hero_yaw = facing.
            o[v] = {rx * cs + rz * sn, ry, -rx * sn + rz * cs};
        }
    }
}

}  // namespace f2
