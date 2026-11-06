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

// A node of the character's model that a weapon or shield mesh hangs on (its name has "Weapon" in it), with the
// box its mesh fills in the node's own frame (m). From CSFD4FlverModelData (ChrIns+0x50 -> +0x10 -> +0x68): +0x18
// node count, +0x20 a 0x30-byte entry per node (box min, box max as float4), +0x30 the node names (UTF-16, in
// order). A node with no mesh has an empty box (min > max) and is left out.
struct WeaponNode {
    std::string name;
    ervr::Vec3 min, max;
};
bool GameWeaponNodesRead(uint8_t* chr, std::vector<WeaponNode>& out, std::string& why);

// The ragdoll data object of a character (changes when the character is reloaded), 0 if none.
uintptr_t GameShapesSource(uint8_t* chr);

}  // namespace kinetic
