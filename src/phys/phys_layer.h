#pragma once
// Kinetic Elden Ring VR: the physics layer (src/phys/).
// M1: the S1 spike. Track the characters near the player, read their skeletons, and (opt-in test)
// overwrite one bone of the nearest one, to find out whether the game draws it and whether its
// attack hitboxes follow. Later milestones add the Jolt world, pose bridge, ragdolls, interactions.
#include <cstdint>

namespace kinetic {

// Called once from ERVR's worker after the game hooks are installed.
void PhysInit();

// Called when ERVR.ini is reloaded, so [Physics] can be toggled without restarting.
void PhysConfigChanged();

bool PhysEnabled();

// Game thread, from ERVR's per-frame pose hook (CS::NoUpdateInterface vft[10]), after the player's arm
// pose is written. pass = the stereo frame pump's pass (2 = the second eye's re-run at dt 0).
// Called inside an SEH guard; never while the world is being torn down.
void PhysGameFrame(uint8_t* player, int pass);

// The guard caught a fault: the layer stays off until the game restarts.
void PhysTrip();

}  // namespace kinetic
