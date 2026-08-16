#include "f2/native_physics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace f2 {

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Rotate a local offset by an euler rotation (matches NativeInstance render transform:
// Y-up render space; apply Y (yaw), X (pitch), Z (roll) — order matches the cook's
// instance transform which is dominated by yaw for props).
std::array<float, 3> rotate_euler(const std::array<float, 3>& v, const std::array<float, 3>& r) {
    // Yaw about Y.
    float cy = std::cos(r[1]), sy = std::sin(r[1]);
    float x1 = v[0] * cy + v[2] * sy;
    float z1 = -v[0] * sy + v[2] * cy;
    float y1 = v[1];
    // Pitch about X.
    float cx = std::cos(r[0]), sx = std::sin(r[0]);
    float y2 = y1 * cx - z1 * sx;
    float z2 = y1 * sx + z1 * cx;
    // Roll about Z.
    float cz = std::cos(r[2]), sz = std::sin(r[2]);
    float x3 = x1 * cz - y2 * sz;
    float y3 = x1 * sz + y2 * cz;
    return {x3, y3, z2};
}
}  // namespace

void NativeCollisionWorld::build_from_scene(const NativeScene& scene) {
    boxes_.clear();
    grid_.clear();
    grid_dim_ = 0;
    terrain_box_ = -1;

    // 1) Per-instance world AABBs (walls) from transformed mesh vertex bounds.
    // 2) Pick the largest-footprint mesh instance as the terrain for the heightfield.
    std::size_t terrain_instance = scene.instances.size();
    float terrain_area = 0.0f;
    std::array<float, 3> scene_min{1e30f, 1e30f, 1e30f}, scene_max{-1e30f, -1e30f, -1e30f};

    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const NativeInstance& inst = scene.instances[i];
        if (inst.mesh >= scene.meshes.size()) continue;
        const NativeMesh& mesh = scene.meshes[inst.mesh];
        if (mesh.vertices.empty()) continue;

        std::array<float, 3> bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
        for (const NativeVertex& v : mesh.vertices) {
            std::array<float, 3> local{v.position[0] * inst.scale, v.position[1] * inst.scale,
                                       v.position[2] * inst.scale};
            std::array<float, 3> w = rotate_euler(local, inst.rotation);
            w[0] += inst.position[0]; w[1] += inst.position[1]; w[2] += inst.position[2];
            for (int k = 0; k < 3; ++k) {
                bmin[k] = std::min(bmin[k], w[k]);
                bmax[k] = std::max(bmax[k], w[k]);
                scene_min[k] = std::min(scene_min[k], w[k]);
                scene_max[k] = std::max(scene_max[k], w[k]);
            }
        }
        boxes_.push_back(Aabb{bmin, bmax});

        const float area = (bmax[0] - bmin[0]) * (bmax[2] - bmin[2]);
        if (area > terrain_area) {
            terrain_area = area;
            terrain_instance = i;
            terrain_box_ = static_cast<int>(boxes_.size()) - 1;  // box index != instance index
        }
    }

    // Build a heightfield grid from the terrain instance's triangles (max Y per cell).
    if (terrain_instance >= scene.instances.size()) return;
    const NativeInstance& tinst = scene.instances[terrain_instance];
    const NativeMesh& tmesh = scene.meshes[tinst.mesh];
    if (tmesh.indices.size() < 3) return;

    const float minx = boxes_[terrain_box_].min[0];
    const float minz = boxes_[terrain_box_].min[2];
    const float extx = boxes_[terrain_box_].max[0] - minx;
    const float extz = boxes_[terrain_box_].max[2] - minz;
    const float ext = std::max(extx, extz);
    if (ext <= 0.0f) return;

    grid_dim_ = 128;  // resolution of the ground heightfield
    grid_cell_ = ext / static_cast<float>(grid_dim_);
    grid_min_x_ = minx;
    grid_min_z_ = minz;
    grid_.assign(static_cast<std::size_t>(grid_dim_) * grid_dim_, kNaN);

    auto world_vertex = [&](std::uint32_t idx) -> std::array<float, 3> {
        const NativeVertex& v = tmesh.vertices[idx];
        std::array<float, 3> local{v.position[0] * tinst.scale, v.position[1] * tinst.scale,
                                   v.position[2] * tinst.scale};
        std::array<float, 3> w = rotate_euler(local, tinst.rotation);
        return {w[0] + tinst.position[0], w[1] + tinst.position[1], w[2] + tinst.position[2]};
    };

    // Rasterize each triangle: for every grid cell whose center is inside the triangle
    // (in XZ), store the barycentric-interpolated Y if it is higher than what is there.
    for (std::size_t t = 0; t + 2 < tmesh.indices.size(); t += 3) {
        std::array<float, 3> a = world_vertex(tmesh.indices[t]);
        std::array<float, 3> b = world_vertex(tmesh.indices[t + 1]);
        std::array<float, 3> c = world_vertex(tmesh.indices[t + 2]);

        float tminx = std::min({a[0], b[0], c[0]}), tmaxx = std::max({a[0], b[0], c[0]});
        float tminz = std::min({a[2], b[2], c[2]}), tmaxz = std::max({a[2], b[2], c[2]});
        int cx0 = std::max(0, static_cast<int>((tminx - minx) / grid_cell_));
        int cx1 = std::min(grid_dim_ - 1, static_cast<int>((tmaxx - minx) / grid_cell_));
        int cz0 = std::max(0, static_cast<int>((tminz - minz) / grid_cell_));
        int cz1 = std::min(grid_dim_ - 1, static_cast<int>((tmaxz - minz) / grid_cell_));

        const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (std::abs(d) < 1e-9f) continue;

        for (int cz = cz0; cz <= cz1; ++cz) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                float px = minx + (cx + 0.5f) * grid_cell_;
                float pz = minz + (cz + 0.5f) * grid_cell_;
                float w0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
                float w1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;  // outside triangle
                float y = w0 * a[1] + w1 * b[1] + w2 * c[1];
                float& cell = grid_[static_cast<std::size_t>(cz) * grid_dim_ + cx];
                if (std::isnan(cell) || y > cell) cell = y;
            }
        }
    }
}

float NativeCollisionWorld::grid_at(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= grid_dim_ || cz >= grid_dim_) return kNaN;
    return grid_[static_cast<std::size_t>(cz) * grid_dim_ + cx];
}

float NativeCollisionWorld::sample_ground(float x, float z, float reference_y) const {
    if (grid_dim_ <= 0) return kNaN;
    int cx = static_cast<int>((x - grid_min_x_) / grid_cell_);
    int cz = static_cast<int>((z - grid_min_z_) / grid_cell_);
    // Nearest valid cell (search a small neighbourhood so edges/holes still clamp).
    float best = kNaN;
    for (int r = 0; r <= 2; ++r) {
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;  // ring only
                float y = grid_at(cx + dx, cz + dz);
                if (std::isnan(y)) continue;
                // Prefer the highest surface at/under the reference (+ tolerance).
                if (y <= reference_y + 1.0f && (std::isnan(best) || y > best)) best = y;
            }
        }
        if (!std::isnan(best)) break;
    }
    return best;
}

std::array<float, 3> NativeCollisionWorld::slide_move(const std::array<float, 3>& pos,
                                                      const std::array<float, 3>& delta,
                                                      float radius) const {
    std::array<float, 3> np{pos[0] + delta[0], pos[1] + delta[1], pos[2] + delta[2]};
    // Axis-aligned pushout against any box the capsule (radius, full height) penetrates
    // horizontally. Single pass = collide-and-slide (movement along the box face survives).
    for (std::size_t bi = 0; bi < boxes_.size(); ++bi) {
        if (static_cast<int>(bi) == terrain_box_) continue;  // ground = heightfield, not a wall
        const Aabb& box = boxes_[bi];
        // Vertical overlap gate: only boxes near the capsule's height band collide.
        if (np[1] + 2.0f < box.min[1] || np[1] - 0.1f > box.max[1]) continue;
        float minx = box.min[0] - radius, maxx = box.max[0] + radius;
        float minz = box.min[2] - radius, maxz = box.max[2] + radius;
        if (np[0] <= minx || np[0] >= maxx || np[2] <= minz || np[2] >= maxz) continue;  // no penetration

        // Push out along the axis of least penetration (so motion slides along the face).
        float penL = np[0] - minx, penR = maxx - np[0];
        float penB = np[2] - minz, penF = maxz - np[2];
        float penX = std::min(penL, penR);
        float penZ = std::min(penB, penF);
        if (penX < penZ) {
            np[0] = (penL < penR) ? minx : maxx;
        } else {
            np[2] = (penB < penF) ? minz : maxz;
        }
    }
    return np;
}

void CharacterController::move(const NativeCollisionWorld& world,
                               const std::array<float, 2>& desired_planar_velocity, float dt) {
    // Horizontal move: desired planar velocity, collide-and-slide against static boxes.
    std::array<float, 3> delta{desired_planar_velocity[0] * dt, 0.0f, desired_planar_velocity[1] * dt};
    position = world.slide_move(position, delta, config.capsule_radius);

    // Ground clamp (GetGroundPositionAtPosition): snap to terrain when within reach.
    const float gy = world.sample_ground(position[0], position[2], position[1]);
    if (!std::isnan(gy) && position[1] - gy <= config.ground_clamp && gy <= position[1] + 0.01f) {
        position[1] = gy;
        velocity[1] = 0.0f;
        on_ground = true;
    } else if (!std::isnan(gy) && gy > position[1]) {
        // Below ground (spawned/pushed under): pop up onto it.
        position[1] = gy;
        velocity[1] = 0.0f;
        on_ground = true;
    } else {
        // Airborne: integrate gravity.
        on_ground = false;
        velocity[1] -= config.gravity * dt;
        position[1] += velocity[1] * dt;
        if (!std::isnan(gy) && position[1] < gy) { position[1] = gy; velocity[1] = 0.0f; on_ground = true; }
    }
}

}  // namespace f2
