#pragma once
// Kinetic Elden Ring VR, M2: the B&S-style sword.
//
// Blade & Sorcery's weapons feel heavy because the weapon is never glued to the controller: the hand and
// the weapon are a rigid body with real mass, pulled toward the tracked controller by a joint whose
// strength is limited. Light weapons follow almost exactly; heavy ones lag, sag under gravity and
// overshoot; a joint that cannot pull hard enough cannot lift them at all. Later (M2.5) the same body
// collides with enemy weapons, so a blade put in the way stops another one by force and weight.
//
// This file runs that model on NVIDIA PhysX 5 (BSD-3, the engine family B&S runs on through Unity),
// one small scene per hand, in the tracking (play-space) frame ERVR builds the hands in.
#include "vrmath.h"

namespace kinetic {

// True when this build has PhysX and [Physics] Enabled + Sword are on (and the layer has not tripped).
bool SwordRigActive();

// Advance hand h's rig by dt toward the controller pose (p, q) and replace (p, q) with the simulated grip.
//   weight     ER weapon weight in that hand (0 = empty hand)
//   reach, com blade length and centre of mass along shaftAxis, metres from the grip
//   strength   multiplier on the arm's force and torque (two-handing)
//   shaftAxis  the blade's direction in the controller's local frame
// Returns false when it did nothing (caller keeps its own model). Game thread only.
bool SwordRigStep(int h, ervr::Vec3& p, ervr::Quat& q, float dt, float weight, float reach, float com,
                  float strength, ervr::Vec3 shaftAxis);

// Strike gate: true while hand h's blade is at an enemy (on its weapon, pushed off its arm, or touching any part)
// without having really struck a body part. ERVR's strike skips the game's attack collision for that frame, so a
// parry or a push is not a hit. Game thread.
bool SwordRigHoldStrike(int h);

// Keep the game from regenerating stamina for SwingRegenDelay after swing stamina was spent. Called from every
// character-update point the physics layer hooks. Any thread.
void SwordRigPinStamina();

// Forget hand h's state (the next step starts exactly at the controller). h < 0: both hands.
void SwordRigReset(int h = -1);

}  // namespace kinetic
