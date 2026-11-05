#pragma once
// Kinetic Elden Ring VR: whether an enemy's melee blow really reached you (src/phys/hit_judge.*).
//
// No guard button: a blow is judged by where the enemy's weapon went. Every frame each nearby enemy weapon
// capsule (enemy_contact) is measured against your body (a capsule from your feet to your eyes) and, with a
// shield in the left hand, against the shield. When the game takes HP from you and an enemy weapon was close:
//   - it never touched your body (your blade held it off, or it missed): the HP comes back;
//   - it touched the shield first: the HP comes back and the shield costs stamina by the blow's damage;
//   - it touched your body: you keep the damage in proportion to how fast the weapon was moving (a graze that
//     slipped past a weak block hurts little, a full-speed hit hurts fully).
// Damage the game deals with no enemy weapon nearby (spells, arrows, falls) is left alone.
#include <cstdint>

namespace kinetic {

// Game thread, once per simulation frame after ContactFrame.
void HitJudgeFrame(uint8_t* player, double now);
void HitJudgeReset();

// The enemy weapon's blade axis learned from the blows you took: 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z; -1 not yet.
int HitJudgeWeaponAxis();

}  // namespace kinetic
