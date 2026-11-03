#pragma once
// Kinetic Elden Ring VR: the physics layer (src/phys/).
// M0: a switchable skeleton only. Later milestones add the Jolt world, enemy tracking,
// pose bridge, active ragdolls and interactions (see the project's architecture notes).

namespace kinetic {

// Called once from ERVR's worker after the game hooks are installed. Logs whether the
// layer is on; does nothing else yet.
void PhysInit();

// Called when ERVR.ini is reloaded, so [Physics] can be toggled without restarting.
void PhysConfigChanged();

bool PhysEnabled();

}  // namespace kinetic
