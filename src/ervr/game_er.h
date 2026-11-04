#pragma once
#include "vrmath.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ervr {

// ELDEN RING integration: first-person camera takeover, head hiding, and
// controller-driven arm bones. RVA maps target exe 2.6.2.0 and 2.7.1.0;
// hooks are only installed when the version matches or the AOB fallbacks hit.
bool GameERInit();
void GameERShutdown();

// Called from the ERVR worker at poll rate: refreshes VR pose snapshot used by the
// game-thread hooks and applies hot-reloaded config.
void GameERTick();

struct GameERStatus {
    bool versionOk = false;
    bool cameraHooked = false;
    bool skeletonHooked = false;
    bool playerFound = false;
    int  boneCount = 0;
    bool armsActive = false;
    long hkCalls = 0, hkSolves = 0, camCalls = 0;   // since last query
    double hkMs = 0, camMs = 0;                     // total ms spent in the hooks since last query
};
GameERStatus GameERGetStatus();
// Cheap "are we actually in the world?" check. Unlike GameERGetStatus it resets no counters,
// so it is safe to call from the input hook every frame.
bool GameERPlayerActive();
bool GameERVersionOk();   // an exact executable-version RVA map is available
bool GameERPlayerDead();        // HP <= 0: a death warp begins THIS frame
bool GameERForeignCamActive();
bool GameERSecondEyePending();  // second-eye stash present (gameplay frame) - menus leave a gap  // menu/cutscene camera owns the FOV (map screen = fast-travel window)
unsigned long long GameERLastStashTick();  // tick of the last stash: fresh = gameplay, quiet = menu (readable mid-frame, unlike Pending)
// Complete the pass's stashed eye report with the FINAL render camera and push it. The
// frame pump calls this right after each pass's queue_tasks returns; without the pump the
// report simply stays unpushed (mono modes never consume reports).
void GameERFinalizePassReport();
bool GameERKillPlayer();   // TEST ONLY: 0 HP -> death warp (see [Experimental] KillPlayerAfter)
// In-game time in ms (0 if unavailable). Advances by the frame dt each sim frame - the
// direct probe for whether pass 2 advances the simulation (see hb "igt" ratio).
uint32_t GameERIgtMs();
// Address of the IGT counter, for the CamWatch writer census ([Experimental] CamWatchIgt).
uint8_t* GameERIgtWatchTarget(uint32_t* outLen);
// True for 4 s after a >100 m camera jump (fast travel / warp): the frame pump suspends
// the second pass while the world is torn down and rebuilt.
bool GameERTeleportGraceActive();
// The game's own transition authority (2026-08-23, replaces the A-press heuristic):
// GameMan.warp_requested (+0x10) - set on the request frame of EVERY warp (map travel
// confirm, death respawn, stakes, emevd) and never by ordinary gameplay A presses.
bool GameERWarpRequested();
// Community loading-screen byte ([[CSEventFlagMan]+0x28]+0x113): TRUE while the loading
// screen is up - the whole teardown/stream/rebuild window, as state instead of a timer.
bool GameERLoadingScreen();
// Latched warp guard for the frame pump: warp_requested seen within the last
// Pass2WarpGraceSeconds (bridges the flag being consumed into the player-null/loading
// holds). Call once per frame - it also edge-logs both signals for postmortems.
bool GameERWarpGuardActive();
// Co-op roster guard for the frame pump: TRUE for graceSeconds after another player's
// character appears, leaves or changes load state (Seamless Co-op connects carry no warp or
// loading signal). *others = other players in the world now. Call once per frame.
bool GameERCoopRoster(float graceSeconds, int* others);
// The game's multiplayer join state machine (CSSessionManagerImp lobby/protocol state) is
// creating, joining or leaving a session, or was within graceSeconds. Any thread.
bool GameERCoopConnecting(float graceSeconds);
bool GameERCoopInSession();   // lobby Host or Client
// Union transition authority for every ERVR path that executes game code or mutates game
// state (combat TAE injection above all): player null/dead, spawn settle, loading screen,
// warp latch. Self-sampling - safe to call from any per-frame path in any session mode.
bool GameERTransitionActive();
// Write-side hazard window: player dead, warp latched, or loading screen up (no arrival
// settle). While TRUE, ERVR's per-frame writes into game objects (camera matrices, pose,
// physics facing) stand down - teardown frees and reuses the memory those writes target.
bool GameERWorldDying();
// Full stereo: write the second eye's camera between the two engine passes of one frame.
// No-op (false) unless UpdateCamera stashed a second-eye request this frame.
bool GameERApplySecondEyeCamera();
// Content-probe experiment: rewrite all camera slots as the last camera pitched by N degrees.
bool GameERDebugPitchCamera(float pitchDeg);
// The render camera's matrix+fov range (CSPersCam via FieldArea->GameRend), for the
// CamWatch guard-page probe. Null until in-game.
uint8_t* GameERCamWatchTarget(uint32_t* outLen);

// ---- inertial arm -------------------------------------------------------------------
// Motion of the weapon pose, with the grip tracked directly and weight resisting bending.
// tipSpeed/handSpeed in m/s, arc = tip path since the tip
// last came to rest. False when the arm is not being driven or the sample is stale.
bool GameERWeaponMotion(int hand, float* tipSpeed, float* handSpeed, float* arc);
// The save's character level (PlayerGameData +0x68), -1 if unavailable. Logged with combat so an
// over-levelled test character is not mistaken for a broken damage model.
int GameERPlayerLevel();
// The player's Strength stat (1..99), or -1.
int GameERPlayerStrength();
// A character's ChrCtrl (what the model-pos update is called with), or null.
uint8_t* GameERChrCtrl(uint8_t* chr);
// The character's live pose buffers through each behavior path that has one (distinct, up to 2). Returns the count.
int GameERChrPoseBuffers(uint8_t* chr, float* out[2]);
// WorldChrMan (null until in-game). Combat's enemy watcher reads the game's own by-distance
// ChrIns list from it (hit / damage ground truth).
uint8_t* GameERWorldChrMan();
// The main player ChrIns (null between worlds). Any thread; every read through it must be a SafeRead.
uint8_t* GameERMainPlayer();
// The save's PlayerGameData (GameDataMan+0x8), null before one is loaded. SafeRead/SafeWrite only.
uint8_t* GameERPlayerGameData();
// On Torrent (mounting, riding or dismounting) with first person running. Any thread.
bool GameERRiding();
// The blade the hand's arm was last solved holding, in the world: grip and tip (grip + weapon reach along the
// controller's blade axis). False when that hand is not tracked or the sample is older than 0.1 s.
bool GameERBladeWorld(int hand, Vec3* grip, Vec3* tip);
// A character's rendered position (its ChrCtrl model matrix), for distances to it. Any thread; SafeRead only.
bool GameERChrPosition(uint8_t* chr, Vec3* out);

// ---- Kinetic physics layer (src/phys) ----------------------------------------------
// Skeleton access for ANY character (enemies included), through the same hkbCharacter path the player's
// arm solve uses. Game thread only (the pose buffer is rewritten by the animation update).
// hkbCharacter of a ChrIns, or null when it has none (yet).
uint8_t* GameERChrHkbCharacter(uint8_t* chr);
// Bone names and parent indices of an hkbCharacter's skeleton. False on a failed or implausible read.
bool GameERHkbSkeleton(uint8_t* hkb, std::vector<std::string>& names, std::vector<int16_t>& parents);
// The live, parent-relative pose buffer: boneCount x hkQsTransform {t[4], q[4] xyzw, s[4]}. Null if unreadable.
float* GameERHkbLivePose(uint8_t* hkb);

// ---- camera ownership -------------------------------------------------------------
// With CameraYawSource=ervr the view yaw is ours, not the game's: virtual_pad feeds turn input in here and
// everything (camera matrix, body facing, movement basis) is derived from it, so nothing the game's own
// follow / lock-on camera does can drag the view around.
void GameERAddCameraYaw(float radians);
void GameERAddBodyYaw(float radians);     // RightStick=body: turn the character body only, the view stays put

struct MoveBasis {
    bool  active = false;    // first person is running and we have a player
    bool  lockedOn = false;  // a real enemy is locked on (the game owns the yaw then)
    bool  riding = false;    // native mount movement needs the game stick even with direct locomotion configured
    float camYaw = 0;        // where the view points
    float chrYaw = 0;        // where the character actually faces (lags camYaw while it turns)
    float bodyYaw = 0;       // where ERVR is turning the body toward (head yaw by default)
    float headYaw = 0;       // HMD yaw relative to the tracking origin, game convention (positive = right)
    bool  headValid = false;
    float viewYaw = 0;       // yaw of the camera matrix the GAME sees - the frame it resolves the move stick in
                             // (camYaw + headYaw with CameraRotation=hmd, camYaw otherwise)
    bool  rideFreeLook = false;  // on Torrent: Forward=head means the free-look direction, not the head itself
    float rideForwardYaw = 0;    // that direction (RideFreeLookDeg of glancing around it leaves the horse alone)
};
MoveBasis GameERGetMoveBasis();
}
