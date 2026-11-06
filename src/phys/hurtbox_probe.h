#pragma once
// Kinetic Elden Ring VR: finds ELDEN RING's own body shapes for a character (src/phys/hurtbox_probe.*).
// Read-only reconnaissance logged under "[hurt]": the objects reachable from the nearest enemy's ChrCtrl, named by
// RTTI, with the float contents of anything that looks like a shape. [Physics] HurtboxProbe turns it on.
#include <cstdint>

namespace kinetic {

// Game thread, once per simulation frame. Probes at most one new enemy kind per call.
void HurtboxProbeFrame(uint8_t* player);
void HurtboxProbeReset();

// Finds the enemy's model file (FLVER) in memory from its ChrIns and logs the dummy points on its weapon bones
// (the points the game strings its attack spheres along the blade with), under "[dmy]". [Physics] DummyProbe.
void DummyProbeFrame();

}  // namespace kinetic
