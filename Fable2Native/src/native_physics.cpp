#include "f2/native_physics.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace f2 {

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

std::array<float, 3> rotate_euler(const std::array<float, 3>& v, const std::array<float, 3>& r) {
    const float cy = std::cos(r[1]), sy = std::sin(r[1]);
    float x1 = v[0] * cy + v[2] * sy;
    float z1 = -v[0] * sy + v[2] * cy;
    float y1 = v[1];
    const float cx = std::cos(r[0]), sx = std::sin(r[0]);
    float y2 = y1 * cx - z1 * sx;
    float z2 = y1 * sx + z1 * cx;
    const float cz = std::cos(r[2]), sz = std::sin(r[2]);
    float x3 = x1 * cz - y2 * sz;
    float y3 = x1 * sz + y2 * cz;
    return {x3, y3, z2};
}

// Closest point (XZ) on triangle (a,b,c) to point p; sets `inside` if p projects inside.
std::array<float, 2> closest_on_tri_2d(const std::array<float, 2>& p, const std::array<float, 2>& a,
                                       const std::array<float, 2>& b, const std::array<float, 2>& c,
                                       bool& inside) {
    // Barycentric test in 2D.
    const float v0x = b[0] - a[0], v0y = b[1] - a[1];
    const float v1x = c[0] - a[0], v1y = c[1] - a[1];
    const float v2x = p[0] - a[0], v2y = p[1] - a[1];
    const float d00 = v0x * v0x + v0y * v0y;
    const float d01 = v0x * v1x + v0y * v1y;
    const float d11 = v1x * v1x + v1y * v1y;
    const float d20 = v2x * v0x + v2y * v0y;
    const float d21 = v2x * v1x + v2y * v1y;
    const float denom = d00 * d11 - d01 * d01;
    inside = false;
    if (std::abs(denom) > 1e-12f) {
        const float v = (d11 * d20 - d01 * d21) / denom;
        const float w = (d00 * d21 - d01 * d20) / denom;
        const float u = 1.0f - v - w;
        if (u >= 0.0f && v >= 0.0f && w >= 0.0f) { inside = true; return p; }
    }
    // Otherwise the closest point is on one of the three edges.
    auto closest_on_seg = [&](const std::array<float, 2>& s, const std::array<float, 2>& e) {
        float ex = e[0] - s[0], ey = e[1] - s[1];
        float len2 = ex * ex + ey * ey;
        float t = len2 > 1e-12f ? ((p[0] - s[0]) * ex + (p[1] - s[1]) * ey) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        return std::array<float, 2>{s[0] + ex * t, s[1] + ey * t};
    };
    auto d2 = [&](const std::array<float, 2>& q) {
        float dx = q[0] - p[0], dz = q[1] - p[1];
        return dx * dx + dz * dz;
    };
    std::array<float, 2> best = closest_on_seg(a, b);
    std::array<float, 2> q = closest_on_seg(b, c);
    if (d2(q) < d2(best)) best = q;
    q = closest_on_seg(c, a);
    if (d2(q) < d2(best)) best = q;
    return best;
}
}  // namespace

void NativeCollisionWorld::build_from_scene(const NativeScene& scene) {
    grid_.clear(); grid_dim_ = 0;
    walls_.clear(); wall_cells_.clear(); wcells_dim_ = 0;

    // Pass 1: pick the largest-footprint mesh instance as the terrain (for the ground
    // heightfield), and track the whole-scene XZ bounds (for the wall bucket grid).
    std::size_t terrain_instance = scene.instances.size();
    float terrain_area = 0.0f;
    std::array<float, 3> tmin{}, tmax{};
    std::array<float, 2> smin{1e30f, 1e30f}, smax{-1e30f, -1e30f};

    auto instance_world = [&](const NativeInstance& inst, std::uint32_t idx) {
        const NativeMesh& mesh = scene.meshes[inst.mesh];
        const NativeVertex& v = mesh.vertices[idx];
        std::array<float, 3> local{v.position[0] * inst.scale, v.position[1] * inst.scale,
                                   v.position[2] * inst.scale};
        std::array<float, 3> w = rotate_euler(local, inst.rotation);
        return std::array<float, 3>{w[0] + inst.position[0], w[1] + inst.position[1],
                                    w[2] + inst.position[2]};
    };

    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        const NativeInstance& inst = scene.instances[i];
        if (inst.mesh >= scene.meshes.size()) continue;
        const NativeMesh& mesh = scene.meshes[inst.mesh];
        if (mesh.vertices.empty()) continue;
        std::array<float, 3> bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
        for (std::uint32_t vi = 0; vi < mesh.vertices.size(); ++vi) {
            std::array<float, 3> w = instance_world(inst, vi);
            for (int k = 0; k < 3; ++k) { bmin[k] = std::min(bmin[k], w[k]); bmax[k] = std::max(bmax[k], w[k]); }
            smin[0] = std::min(smin[0], w[0]); smin[1] = std::min(smin[1], w[2]);
            smax[0] = std::max(smax[0], w[0]); smax[1] = std::max(smax[1], w[2]);
        }
        const float area = (bmax[0] - bmin[0]) * (bmax[2] - bmin[2]);
        if (area > terrain_area) { terrain_area = area; terrain_instance = i; tmin = bmin; tmax = bmax; }
    }
    if (terrain_instance >= scene.instances.size()) return;

    // Pass 2a: terrain heightfield (max Y per cell) from the terrain triangles.
    {
        const NativeInstance& tinst = scene.instances[terrain_instance];
        const NativeMesh& tmesh = scene.meshes[tinst.mesh];
        const float minx = tmin[0], minz = tmin[2];
        const float ext = std::max(tmax[0] - minx, tmax[2] - minz);
        if (ext > 0.0f && tmesh.indices.size() >= 3) {
            grid_dim_ = 128;
            grid_cell_ = ext / static_cast<float>(grid_dim_);
            grid_min_x_ = minx; grid_min_z_ = minz;
            grid_.assign(static_cast<std::size_t>(grid_dim_) * grid_dim_, kNaN);
            for (std::size_t t = 0; t + 2 < tmesh.indices.size(); t += 3) {
                std::array<float, 3> a = instance_world(tinst, tmesh.indices[t]);
                std::array<float, 3> b = instance_world(tinst, tmesh.indices[t + 1]);
                std::array<float, 3> c = instance_world(tinst, tmesh.indices[t + 2]);
                float txmin = std::min({a[0], b[0], c[0]}), txmax = std::max({a[0], b[0], c[0]});
                float tzmin = std::min({a[2], b[2], c[2]}), tzmax = std::max({a[2], b[2], c[2]});
                int cx0 = std::max(0, int((txmin - minx) / grid_cell_));
                int cx1 = std::min(grid_dim_ - 1, int((txmax - minx) / grid_cell_));
                int cz0 = std::max(0, int((tzmin - minz) / grid_cell_));
                int cz1 = std::min(grid_dim_ - 1, int((tzmax - minz) / grid_cell_));
                const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
                if (std::abs(d) < 1e-9f) continue;
                for (int cz = cz0; cz <= cz1; ++cz) for (int cx = cx0; cx <= cx1; ++cx) {
                    float px = minx + (cx + 0.5f) * grid_cell_, pz = minz + (cz + 0.5f) * grid_cell_;
                    float w0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
                    float w1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
                    float y = w0 * a[1] + w1 * b[1] + w2 * c[1];
                    float& cell = grid_[static_cast<std::size_t>(cz) * grid_dim_ + cx];
                    if (std::isnan(cell) || y > cell) cell = y;
                }
            }
        }
    }

    // Pass 2b: wall triangles = near-vertical triangles from every NON-terrain instance.
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        if (i == terrain_instance) continue;
        const NativeInstance& inst = scene.instances[i];
        if (inst.mesh >= scene.meshes.size()) continue;
        const NativeMesh& mesh = scene.meshes[inst.mesh];
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            std::array<float, 3> a = instance_world(inst, mesh.indices[t]);
            std::array<float, 3> b = instance_world(inst, mesh.indices[t + 1]);
            std::array<float, 3> c = instance_world(inst, mesh.indices[t + 2]);
            // Face normal; keep only near-vertical (wall) triangles (|n.y| small).
            std::array<float, 3> u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
            std::array<float, 3> w{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            std::array<float, 3> n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2],
                                   u[0] * w[1] - u[1] * w[0]};
            float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl < 1e-6f) continue;
            if (std::abs(n[1] / nl) > 0.7f) continue;  // floor/ceiling -> ground handles it
            WallTri wt;
            wt.a = a; wt.b = b; wt.c = c;
            wt.ymin = std::min({a[1], b[1], c[1]});
            wt.ymax = std::max({a[1], b[1], c[1]});
            walls_.push_back(wt);
        }
    }

    // Bucket walls into an XZ grid for broadphase.
    if (!walls_.empty() && smax[0] > smin[0] && smax[1] > smin[1]) {
        wcells_dim_ = 96;
        wcell_min_x_ = smin[0]; wcell_min_z_ = smin[1];
        wcell_size_ = std::max((smax[0] - smin[0]), (smax[1] - smin[1])) / wcells_dim_;
        if (wcell_size_ <= 0.0f) wcell_size_ = 1.0f;
        wall_cells_.assign(static_cast<std::size_t>(wcells_dim_) * wcells_dim_, {});
        for (std::uint32_t wi = 0; wi < walls_.size(); ++wi) {
            const WallTri& t = walls_[wi];
            float xmin = std::min({t.a[0], t.b[0], t.c[0]}), xmax = std::max({t.a[0], t.b[0], t.c[0]});
            float zmin = std::min({t.a[2], t.b[2], t.c[2]}), zmax = std::max({t.a[2], t.b[2], t.c[2]});
            int cx0 = std::clamp(int((xmin - wcell_min_x_) / wcell_size_), 0, wcells_dim_ - 1);
            int cx1 = std::clamp(int((xmax - wcell_min_x_) / wcell_size_), 0, wcells_dim_ - 1);
            int cz0 = std::clamp(int((zmin - wcell_min_z_) / wcell_size_), 0, wcells_dim_ - 1);
            int cz1 = std::clamp(int((zmax - wcell_min_z_) / wcell_size_), 0, wcells_dim_ - 1);
            for (int cz = cz0; cz <= cz1; ++cz) for (int cx = cx0; cx <= cx1; ++cx)
                wall_cells_[static_cast<std::size_t>(cz) * wcells_dim_ + cx].push_back(wi);
        }
    }
}

float NativeCollisionWorld::grid_at(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= grid_dim_ || cz >= grid_dim_) return kNaN;
    return grid_[static_cast<std::size_t>(cz) * grid_dim_ + cx];
}

float NativeCollisionWorld::sample_ground(float x, float z, float reference_y) const {
    if (grid_dim_ <= 0) return kNaN;
    int cx = int((x - grid_min_x_) / grid_cell_);
    int cz = int((z - grid_min_z_) / grid_cell_);
    float best = kNaN;
    for (int r = 0; r <= 2; ++r) {
        for (int dz = -r; dz <= r; ++dz) for (int dx = -r; dx <= r; ++dx) {
            if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
            float y = grid_at(cx + dx, cz + dz);
            if (std::isnan(y)) continue;
            if (y <= reference_y + 1.0f && (std::isnan(best) || y > best)) best = y;
        }
        if (!std::isnan(best)) break;
    }
    return best;
}

std::array<float, 2> NativeCollisionWorld::resolve_walls(std::array<float, 2> c, float r,
                                                         float y0, float y1) const {
    if (wcells_dim_ <= 0) return c;
    int cx = std::clamp(int((c[0] - wcell_min_x_) / wcell_size_), 0, wcells_dim_ - 1);
    int cz = std::clamp(int((c[1] - wcell_min_z_) / wcell_size_), 0, wcells_dim_ - 1);
    const int span = std::max(1, int(r / wcell_size_) + 1);
    for (int dz = -span; dz <= span; ++dz) for (int dx = -span; dx <= span; ++dx) {
        int gx = cx + dx, gz = cz + dz;
        if (gx < 0 || gz < 0 || gx >= wcells_dim_ || gz >= wcells_dim_) continue;
        for (std::uint32_t wi : wall_cells_[static_cast<std::size_t>(gz) * wcells_dim_ + gx]) {
            const WallTri& t = walls_[wi];
            if (t.ymax < y0 || t.ymin > y1) continue;  // no vertical overlap
            bool inside = false;
            std::array<float, 2> q = closest_on_tri_2d(
                c, {t.a[0], t.a[2]}, {t.b[0], t.b[2]}, {t.c[0], t.c[2]}, inside);
            float dx2 = c[0] - q[0], dz2 = c[1] - q[1];
            float d = std::sqrt(dx2 * dx2 + dz2 * dz2);
            if (inside) {
                // Centre is inside the wall footprint (thick wall): eject perpendicular
                // to the triangle's longest XZ edge by the radius.
                float ex = t.b[0] - t.a[0], ez = t.b[2] - t.a[2];
                float nlen = std::sqrt(ex * ex + ez * ez);
                if (nlen > 1e-6f) { c[0] = q[0] + (-ez / nlen) * r; c[1] = q[1] + (ex / nlen) * r; }
                continue;
            }
            if (d < r && d > 1e-6f) {
                c[0] = q[0] + (dx2 / d) * r;
                c[1] = q[1] + (dz2 / d) * r;
            }
        }
    }
    return c;
}

float NativeCollisionWorld::raycast_walls(const std::array<float, 3>& origin,
                                          const std::array<float, 3>& dir, float max_dist) const {
    if (wcells_dim_ <= 0 || walls_.empty()) return max_dist;
    const std::array<float, 3> end{origin[0] + dir[0] * max_dist, origin[1] + dir[1] * max_dist,
                                   origin[2] + dir[2] * max_dist};
    // Broadphase: walk the XZ cells the segment's bounding box covers.
    int cx0 = int((std::min(origin[0], end[0]) - wcell_min_x_) / wcell_size_) - 1;
    int cx1 = int((std::max(origin[0], end[0]) - wcell_min_x_) / wcell_size_) + 1;
    int cz0 = int((std::min(origin[2], end[2]) - wcell_min_z_) / wcell_size_) - 1;
    int cz1 = int((std::max(origin[2], end[2]) - wcell_min_z_) / wcell_size_) + 1;
    cx0 = std::clamp(cx0, 0, wcells_dim_ - 1); cx1 = std::clamp(cx1, 0, wcells_dim_ - 1);
    cz0 = std::clamp(cz0, 0, wcells_dim_ - 1); cz1 = std::clamp(cz1, 0, wcells_dim_ - 1);

    float nearest = max_dist;
    constexpr float kEps = 1e-6f;
    for (int cz = cz0; cz <= cz1; ++cz) for (int cx = cx0; cx <= cx1; ++cx) {
        for (std::uint32_t wi : wall_cells_[static_cast<std::size_t>(cz) * wcells_dim_ + cx]) {
            const WallTri& t = walls_[wi];
            // Moeller-Trumbore ray/triangle.
            std::array<float, 3> e1{t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2]};
            std::array<float, 3> e2{t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2]};
            std::array<float, 3> h{dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
                                   dir[0] * e2[1] - dir[1] * e2[0]};
            float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
            if (det > -kEps && det < kEps) continue;  // parallel
            float inv = 1.0f / det;
            std::array<float, 3> s{origin[0] - t.a[0], origin[1] - t.a[1], origin[2] - t.a[2]};
            float u = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
            if (u < 0.0f || u > 1.0f) continue;
            std::array<float, 3> q{s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                                   s[0] * e1[1] - s[1] * e1[0]};
            float v = inv * (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]);
            if (v < 0.0f || u + v > 1.0f) continue;
            float dist = inv * (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]);
            if (dist > kEps && dist < nearest) nearest = dist;
        }
    }
    return nearest;
}

bool NativeCollisionWorld::line_of_sight(const std::array<float, 3>& a,
                                         const std::array<float, 3>& b) const {
    std::array<float, 3> d{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len <= 1e-4f) return true;
    d[0] /= len; d[1] /= len; d[2] /= len;
    // Visible iff the nearest wall hit is at/after the target (small epsilon so a wall
    // exactly at the target doesn't self-occlude).
    return raycast_walls(a, d, len) >= len - 0.01f;
}

std::array<float, 3> NativeCollisionWorld::slide_move(const std::array<float, 3>& pos,
                                                      const std::array<float, 3>& delta,
                                                      float radius, float height) const {
    std::array<float, 3> np{pos[0] + delta[0], pos[1] + delta[1], pos[2] + delta[2]};
    std::array<float, 2> c{np[0], np[2]};
    const float y0 = np[1] + 0.1f;             // above the feet (ignore the floor edge)
    const float y1 = np[1] + height;           // capsule top
    // A few relaxation passes so inside corners (two intersecting walls) settle.
    for (int pass = 0; pass < 4; ++pass) {
        c = resolve_walls(c, radius, y0, y1);
    }
    np[0] = c[0]; np[2] = c[1];
    return np;
}

void CharacterController::move(const NativeCollisionWorld& world,
                               const std::array<float, 2>& desired_planar_velocity, float dt) {
    std::array<float, 3> delta{desired_planar_velocity[0] * dt, 0.0f, desired_planar_velocity[1] * dt};
    position = world.slide_move(position, delta, config.capsule_radius, config.capsule_height);

    const float gy = world.sample_ground(position[0], position[2], position[1]);
    if (!std::isnan(gy) && position[1] - gy <= config.ground_clamp && gy <= position[1] + 0.01f) {
        position[1] = gy; velocity[1] = 0.0f; on_ground = true;
    } else if (!std::isnan(gy) && gy > position[1]) {
        position[1] = gy; velocity[1] = 0.0f; on_ground = true;
    } else {
        on_ground = false;
        velocity[1] -= config.gravity * dt;
        position[1] += velocity[1] * dt;
        if (!std::isnan(gy) && position[1] < gy) { position[1] = gy; velocity[1] = 0.0f; on_ground = true; }
    }
}

}  // namespace f2
