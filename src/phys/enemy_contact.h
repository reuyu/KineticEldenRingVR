#pragma once
// Kinetic Elden Ring VR, M2.5: your blade against enemy arms and weapons (src/phys/enemy_contact.*).
//
// Each frame the arms (upper arm, forearm) and the right-hand weapon of the enemies within reach become capsules
// in the world. The sword rig puts them in its PhysX scene as kinematic bodies: a blade that meets one is stopped
// by it (your arm's limited strength decides how far it gets pushed), and the impulse of every contact comes back
// here. The enemy's arm takes that impulse about its shoulder - its inertia and how hard it holds its pose decide
// how far it swings away - and springs back into its animation. The swing is drawn by overriding the upper arm
// (pose_override), so the hand, the weapon and its hitbox go with it.
#include "vrmath.h"

#include <cstdint>

namespace kinetic {

struct ContactPart {
    uint32_t key;          // stable per (enemy, side, kind) while the enemy stays near
    uint8_t* chr;
    int side;              // 0 left, 1 right
    int kind;              // 0 upper arm, 1 forearm, 2 weapon (these block and swing); 3 torso, 4 head, 5 thigh, 6 calf
                           // (the body: the blade cuts through, sticks in or glances off); 7 across the shoulders
                           // (side 0) or hips (side 1). The trunk is two kind-3 capsules side by side.
    ervr::Vec3 a, b;       // capsule segment, world
    float radius;
    ervr::Vec3 axis[3];    // kind 2: the weapon bone's local x, y, z in the world (the blade lies along one of them)
    float length;          // kind 2: EnemyWeaponLength
    int npcId;             // NpcParam row of the enemy (kind 2: whose learned blade direction this is)
    // kind 2 read from the model (game_shapes GameWeaponNodesRead): a box, the mesh's extent in its node's frame.
    bool box;
    bool shield;           // a node named *Shield*: blocks, never damages you
    ervr::Vec3 boneP;      // the node's origin (the grip) in the world, with the arm's swing applied
    ervr::Quat boneQ;      // its rotation in the world, with the swing
    ervr::Vec3 lo, hi;     // the box in that frame (m)
    float worldSpeed;      // how fast its centre moved in the world since the last frame (m/s), for the impact log
    ervr::Vec3 worldVel;   // that velocity (m/s, world); zero when not known
    float weaponSpeed;     // how fast this enemy's own weapons move in the world this frame (m/s; -1 none): a fast one
                           // means it is attacking - for the impact log
    ervr::Vec3 da, db;     // where the part really is: a/b with the arm's swing applied. For the weapon, a/b is
                           // where the ANIMATION wants it (the physics weapon is driven toward that) and da/db
                           // follows the physics weapon your blade may be holding off.
};

// Game thread, once per simulation frame from the physics layer: rebuild the capsules from the enemies' current
// poses, advance each arm's swing and declare the overrides (between PoseOverrideBegin/End).
void ContactFrame(uint8_t* player, double now);

// The capsules of the last frame (copied out, at most max). Any thread.
int ContactParts(ContactPart* out, int max);

// A contact's impulse on the enemy part (N*s, world) at a world point. Any thread.
void ContactImpulse(uint32_t key, const ervr::Vec3& point, const ervr::Vec3& impulse);

// The physics weapon or shield of an enemy (part key) this frame, in the world, while your blade is on it: its
// grip and the centre of its box, and how far its far end is off the animation's (m). Its arm is turned to carry it there. Any thread.
void ContactWeaponSim(uint32_t key, const ervr::Vec3& grip, const ervr::Vec3& tip, float deviation);

void ContactReset();

}  // namespace kinetic
