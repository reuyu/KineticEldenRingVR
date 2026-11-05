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
    int kind;              // 0 upper arm, 1 forearm, 2 weapon
    ervr::Vec3 a, b;       // capsule segment, world
    float radius;
};

// Game thread, once per simulation frame from the physics layer: rebuild the capsules from the enemies' current
// poses, advance each arm's swing and declare the overrides (between PoseOverrideBegin/End).
void ContactFrame(uint8_t* player, double now);

// The capsules of the last frame (copied out, at most max). Any thread.
int ContactParts(ContactPart* out, int max);

// A contact's impulse on the enemy part (N*s, world) at a world point. Any thread.
void ContactImpulse(uint32_t key, const ervr::Vec3& point, const ervr::Vec3& impulse);

void ContactReset();

}  // namespace kinetic
