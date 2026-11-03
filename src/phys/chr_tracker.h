#pragma once
// The characters near the player that the physics layer looks at (M1.1 / M1.2).
#include <cstdint>
#include <string>
#include <vector>

namespace kinetic {

struct TrackedChr {
    uint8_t* chr = nullptr;      // ChrIns
    uint8_t* hkb = nullptr;      // its hkbCharacter (null: no skeleton reachable)
    int npcId = 0;               // NpcParam row, for the log
    float distance = 0.f;        // m, from the player's model position
    int hp = -1, maxHp = -1;
    int boneCount = 0;
    bool sameRigAsPlayer = false;   // bone names identical to the player's (the c0000 humanoid rig)
};

// Refreshes the list from WorldChrMan's by-distance list. radius in m. Game thread.
void TrackerUpdate(uint8_t* player, float radius);
const std::vector<TrackedChr>& TrackerList();   // nearest first
void TrackerReset();

// Index of a bone by name in a character's skeleton (cached per hkbCharacter), -1 if absent.
int TrackerBoneIndex(uint8_t* hkb, const std::string& name);

}  // namespace kinetic
