#pragma once
#include "vr_backend.h"
#include "two_hand_grip.h"
#include <cstdint>

namespace ervr {

// Physical melee combat: instead of pressing R1/R2 (which plays an attack animation
// with animation-timed hitboxes), ERVR invokes the game's own TAE "AttackBehavior"
// event (event 1) on the player every frame while a controller swing is in progress.
// The game then does what it does for a real attack: looks up BehaviorParam_PC ->
// AtkParam_Pc for the current weapon, sweeps the weapon dummy-poly capsule (which
// follows the controller-driven arm bones), applies damage/poise/status, hitstop,
// hit-list dedupe and stamina cost. No attack animation plays.
//
// Judge IDs (which BehaviorParam row = which attack) are learned by watching the
// events the game fires for the player when ERVR presses the light/heavy/left
// buttons in "buttons" mode, or configured in [Combat].

bool CombatInit(uint8_t* moduleBase, bool versionOk);   // installs the TAE event hook
void CombatShutdown();

// Worker thread (poll rate): feed controller poses; runs the swing state machine.
void CombatPollHands(const VRTracking& tr, const VRInputState& in, double now);
// True while physical combat is active: virtual_pad must not pulse attack buttons for swings.
bool CombatHandlesSwings();
// Called by virtual_pad whenever it pulses a button (used to learn judge IDs).
void CombatNotifyButton(uint16_t xinputMask, double now);
// XInput button mask virtual_pad must not emit while physical combat is active (attack buttons -> animations).
uint16_t CombatBlockedButtons();
// XInput buttons virtual_pad must HOLD: the guard button while the left-hand shield is raised.
uint16_t CombatHeldButtons();
// Pad worker: publish whether any game menu is open (game state or menu-nav mode). While true,
// physical combat stands down completely - no swings, no injection, no guard hold, no chords,
// param edits restored - so the game never sees combat state under a menu (the equipment menu
// refuses weapon changes while it does). CombatMenuOpen returns the gate's current state.
void CombatSetMenuOpen(bool open);
bool CombatMenuOpen();
bool CombatMenuActive(); // published menu state, independent of the combat MenuGate option
// Buttons combat FORCES on the pad this tick - the game's own chords (Y + RB/LB two-hand toggle, the
// skill button, a learn press) - applied AFTER the attack-button block. Worker thread; drives the
// chord timing, so call it exactly once per pad update.
uint16_t CombatForcedButtons(double now);
// Off-hand ride: while two-handing, the free hand's arm target sits on the weapon hand's hilt axis,
// `alongToPommel` metres from that controller toward the pommel - down the controller's handle
// (-Y) when `handleAxis`, else back along its aim axis (+Z). Optional `grip` receives
// the controller orientation captured at the grab. game_er applies it. False otherwise.
bool CombatTwoHandPin(int* pinHand, int* weaponHand, float* alongToPommel, bool* handleAxis,
                      TwoHandGripOrientation* grip = nullptr);
// The weapon wheel's gate, worker thread, after CombatPollHands: this hand is at the other weapon's hilt
// (a grip closed there is the two-hand grab), and *holding = it has taken that hilt.
bool CombatTwoHandGrip(int hand, bool* holding);
// Weight the hand is carrying (EquipParamWeapon weight; both hands share it when two-handing;
// 0 = empty/unknown). Drives the inertial arm in game_er.
float CombatHandWeight(int hand);
// The hand holding a two-handed weapon per the game's arm style, else -1 (both arms drive that weapon).
int CombatTwoHandWeaponHand();
// Geometry of the weapon in the hand: grip->tip length (the tip-speed lever arm; ini WeaponReach when unknown)
// and how far out along it the mass sits (rotational inertia about the grip goes with its square).
float CombatHandReach(int hand);
float CombatHandCom(int hand);
// The main player's stamina (CSChrDataModule) and its maximum; -1 when unreadable. Game thread.
int CombatPlayerStamina(int* maxOut);
// Sets the main player's stamina (clamped at 0). Game thread.
bool CombatSetPlayerStamina(int value);
// The main player's HP and its maximum; -1 when unreadable. Game thread.
int CombatPlayerHp(int* maxOut);
// Sets the main player's HP (clamped to 1..max). Game thread.
bool CombatSetPlayerHp(int value);
// A shield in the left hand (any [Combat] Block setting), not two-handing the right weapon. Game thread.
bool CombatLeftIsShield();
// Cast aim: ranged spells / skills / bows aim where the CASTING HAND points. The pad worker feeds every
// button the GAME is actually handed (final mask, VP_LT/VP_RT for the triggers; 0 while a menu owns the
// pad) so the pressing hand becomes the aim hand for CastAimSeconds. Call once per pad update.
void CombatNotifyPad(uint16_t gameMask, double now);
// Which hand aims ranged effects right now: -1 = none (the head aims, as before), HandLeft/HandRight.
// *casting = a press made it so within CastAimSeconds (the body turns to the ray, the lock target may be
// forced, only then); a hand that merely holds a catalyst/bow aims passively. Any thread, atomics only.
int CombatCastAimHand(bool* casting);
// Haptic requests raised by the game-thread side (stamina empty, swing armed); worker drains them.
// Returns a bitmask: bit0 = left hand, bit1 = right hand, bit2/3 = "fail" flavour for left/right,
// bit4 = blocked hit (left), bit5 = deflect (left).
uint32_t CombatTakeHapticRequests();

// Game thread, once per rendered frame with the main player ChrIns.
void CombatGameFrame(uint8_t* player, uint32_t frame);
// Action-request diagnostic (Combat Debug): for a short window after any press handed to the game, log the
// player's CSChrActionRequestModule + TimeAct state once per stereo pass (pass = FramePumpCurrentPass()).
// Any pass, game thread. Cheap when not armed.
void CombatActionDiag(uint8_t* player, int pass, uint32_t frame);
// Zero-step full-frame stereo: preserve the player's input buffers, including queue-mode entries,
// unless an action was consumed or their ownership changed ([VR] Pass2KeepActionRequest). Frame-pump thread.
void CombatPass2Begin(uint8_t* player);
void CombatPass2End(uint8_t* player);

struct CombatStatus {
    bool taeHooked = false;
    bool physical = false;          // physical mode currently active
    bool learnedLight = false, learnedHeavy = false, learnedLeft = false;
    long injected = 0;              // synthetic AttackBehavior calls since last query
    long swings = 0;                // swings started since last query
    int  stamina = -1;
    bool paramsReady = false;       // SoloParamRepository located (kinetic scaling possible)
    float lastQuality = 0.f, lastMult = 0.f;   // last strike's quality and damage multiplier
    bool shield = false, guarding = false;     // shield in the left hand / guard currently raised
    float blockQuality = 0.f;
    int  armStyle = 0;              // ChrAsm arm style: 0 empty, 1 one-handed, 2 left two-handed, 3 right two-handed
    bool twoHandWanted = false;     // the grab says two-hand
    int  fp = -1;
    const char* skill = "idle";     // skill state: idle | recording | replaying | window
    int  skillsRecorded = 0;        // ash-of-war recordings held this session
    int  castAimHand = -1;          // hand aiming ranged effects (-1 = head)
    bool casting = false;           // inside the press window (body turns to the ray)
};
CombatStatus CombatGetStatus();
}
