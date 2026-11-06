#pragma once
// Kinetic Elden Ring VR: bone overrides on any character (src/phys/pose_override.*).
//
// S1 showed (play test 4, 2026-11-05) that a bone written into a character's live hkbCharacter pose at ERVR's
// pose hook is drawn, and moves the enemy's arm - but only while it idles: during attacks the arm played its
// animation as if nothing had been written. Every override therefore goes in at three points (the pose hook,
// and the character's model-pos update before and after the game's own code), with per-point statistics that
// say where the animation overwrote us.
//
// Each frame the users (the S1 test, enemy contact) declare what they want with Begin/Add/End; the writer keeps
// the animation's own value per bone and re-applies the override on top of it, never on top of itself.
#include "vrmath.h"

#include <cstdint>

namespace kinetic {

// The local pose of `bone` becomes pre * animation * post (pre rotates it in its parent's frame, post in its own).
void PoseOverrideBegin();
void PoseOverrideAdd(uint8_t* chr, uint8_t* hkb, int bone, const ervr::Quat& pre, const ervr::Quat& post);
void PoseOverrideEnd();
void PoseOverrideClear();

// Write points. 0 = pose hook (every character), 1/2 = model-pos update of the character whose ChrCtrl is ctrl,
// before / after the game's own code.
void PoseOverrideWrite(int point, void* ctrl);

// The animation's own local rotation of `bone` if the pose buffer currently holds our override there (so a
// reader can rebuild the un-overridden pose). False if the buffer holds the animation's value.
bool PoseOverrideAnimQ(uint8_t* chr, const float* pose, int bone, float q[4]);

// One log line per overridden character every 5 s.
void PoseOverrideLog();

}  // namespace kinetic
