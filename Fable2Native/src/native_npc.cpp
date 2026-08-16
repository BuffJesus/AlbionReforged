#include "f2/native_npc.h"

#include <cmath>

namespace f2 {

bool NpcController::can_see(const NativeCollisionWorld& world,
                            const std::array<float, 3>& target) const {
    const std::array<float, 3>& p = controller.position;
    const float dx = target[0] - p[0];
    const float dz = target[2] - p[2];
    const float planar = std::sqrt(dx * dx + dz * dz);
    if (planar > perception.sight_range) return false;
    // Eye-to-eye ray (LOS unobstructed by walls).
    const std::array<float, 3> eye{p[0], p[1] + perception.eye_height, p[2]};
    const std::array<float, 3> tgt{target[0], target[1] + perception.eye_height, target[2]};
    return world.line_of_sight(eye, tgt);
}

bool NpcController::update_lod(const std::array<float, 3>& lod_centre, float radius) {
    const std::array<float, 3>& p = controller.position;
    const float dx = p[0] - lod_centre[0];
    const float dz = p[2] - lod_centre[2];
    active = (dx * dx + dz * dz) <= radius * radius;
    return active;
}

void NpcController::face(const std::array<float, 3>& toward) {
    const std::array<float, 3>& p = controller.position;
    const float dx = toward[0] - p[0];
    const float dz = toward[2] - p[2];
    if (dx * dx + dz * dz > 1e-6f) facing_yaw_ = std::atan2(dx, dz);
}

bool NpcController::move_to(const NativeCollisionWorld& world, const std::array<float, 3>& goal,
                            float dt) {
    const std::array<float, 3>& p = controller.position;
    const float dx = goal[0] - p[0];
    const float dz = goal[2] - p[2];
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (dist <= movement.arrive_radius) {
        controller.move(world, {0.0f, 0.0f}, dt);  // still integrate gravity/ground-clamp
        return true;
    }
    const float inv = 1.0f / dist;
    const std::array<float, 2> vel{dx * inv * movement.walk_speed, dz * inv * movement.walk_speed};
    controller.move(world, vel, dt);
    face(goal);
    return false;
}

void NpcController::set_patrol_goal(const std::array<float, 3>& goal) {
    patrol_goal_ = goal;
    have_patrol_goal_ = true;
    state = NpcState::Patrol;
}

void NpcController::flee_from(const std::array<float, 3>& threat) {
    flee_threat_ = threat;
    flee_timer_ = 3.0f;  // ENGINEERING flee duration (villagers scatter briefly)
    state = NpcState::Flee;
}

void NpcController::update(const NativeCollisionWorld& world, const std::array<float, 3>& target,
                           const std::array<float, 3>& lod_centre, float lod_radius, float dt) {
    if (!alive) { controller.move(world, {0.0f, 0.0f}, dt); return; }  // dead: hold + ground-clamp
    if (!update_lod(lod_centre, lod_radius)) return;  // culled by LOD -> no tick

    // Flee takes priority: run directly away from the threat until the timer expires.
    if (flee_timer_ > 0.0f) {
        flee_timer_ -= dt;
        state = NpcState::Flee;
        const std::array<float, 3>& p = controller.position;
        float ax = p[0] - flee_threat_[0], az = p[2] - flee_threat_[2];
        const float m = std::sqrt(ax * ax + az * az);
        if (m > 1e-4f) { ax /= m; az /= m; } else { ax = 1.0f; az = 0.0f; }
        move_to(world, {p[0] + ax * 10.0f, p[1], p[2] + az * 10.0f}, dt);
        if (flee_timer_ <= 0.0f) state = NpcState::Idle;
        return;
    }

    // Stand-in brain: notice the target when it is visible and close; otherwise idle or
    // continue an externally-set patrol. (Real DECIDE = Lua behaviour scoring, gap/P6.)
    const std::array<float, 3>& p = controller.position;
    const float tdx = target[0] - p[0];
    const float tdz = target[2] - p[2];
    const bool target_near = (tdx * tdx + tdz * tdz) <= perception.notice_range * perception.notice_range;

    if (target_near && can_see(world, target)) {
        state = NpcState::Notice;
        face(target);
        controller.move(world, {0.0f, 0.0f}, dt);  // stop and look
        return;
    }

    if (state == NpcState::Patrol && have_patrol_goal_) {
        if (move_to(world, patrol_goal_, dt)) {
            have_patrol_goal_ = false;
            state = NpcState::Idle;
        }
        return;
    }

    state = NpcState::Idle;
    controller.move(world, {0.0f, 0.0f}, dt);  // idle: hold position, stay grounded
}

}  // namespace f2
