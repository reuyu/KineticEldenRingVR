#pragma once
// Kinetic: a character's own body shapes as ELDEN RING has them - the Havok ragdoll capsules its attacks hit and
// are hit with (ChrCtrl+0x28 CSRagdollIns -> hknpRagdoll). Read once per character, then placed every frame on
// the animation bone each ragdoll bone follows (the anim-to-ragdoll skeleton mapper).
#include "vrmath.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kinetic {

struct GameCapsule {
    int animBone = -1;          // bone of the character's animation skeleton (hkbCharacter) it follows
    ervr::Quat q;               // ragdoll body in that bone's model-space frame
    ervr::Vec3 t;
    ervr::Vec3 a, b;            // capsule end points in the body's frame (m)
    float radius = 0.f;
    std::string ragName, animName;
};

// Reads the capsules of a character (game thread). False (and why) when the layout is not what this build knows.
bool GameShapesRead(uint8_t* chr, uint8_t* hkb, std::vector<GameCapsule>& out, std::string& why);

// The ragdoll data object of a character (changes when the character is reloaded), 0 if none.
uintptr_t GameShapesSource(uint8_t* chr);

}  // namespace kinetic
