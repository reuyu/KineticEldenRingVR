// ELDEN RING integration (game 1.16.2 / eldenring.exe 2.6.2.0).
//
// Layouts and hook points come from the open-source community work on this exact
// version: erfps2 (Dasaav, camera step hook + head hiding + aim matrix),
// ERSkeletonMan (Dasaav, live Havok pose access via hkbCharacter), fromsoftware-rs
// (struct layouts), FreecamMod / TGA (FieldArea -> GameRend -> CSPersCam), and the
// classic WorldChrMan AOB. Nothing here is guessed from scratch; see README.
#include "game_er.h"
#include "phys_layer.h"
#include "sword_rig.h"
#include "game_version.h"
#include "head_anchor.h"
#include "pose_frame.h"
#include "body_pose.h"
#include "movement_facing.h"
#include "ride_state.h"
#include "mounted_rider.h"
#include "two_hand_aim.h"
#include "game_memory.h"
#include "config.h"
#include "debug_keys.h"
#include "log.h"
#include "vr_backend.h"
#include "midhook.h"
#include "rtti.h"
#include "combat.h"
#include "frame_probe.h"
#include "stereo.h"
#include "belt.h"
#include "weapon_wheel.h"
#include "body_haptics_er.h"
#include <windows.h>
#include <MinHook.h>
#include "virtual_pad.h"
#pragma comment(lib, "version.lib")
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <cstring>

namespace ervr {
namespace {

// ---------------------------------------------------------------- offsets (1.16.2)
namespace off {
    constexpr size_t WCM_MainPlayer   = 0x1E508;   // WorldChrMan -> PlayerIns*
    constexpr size_t WCM_ChrCam       = 0x1ECE0;   // WorldChrMan -> ChrCam* (+0x10 on 2.7.2.0, see GameWorldChrManOffset)
    constexpr size_t WCM_PlayerChrSet = 0x10EE0;   // WorldChrMan -> ChrSet<PlayerIns> (inline), host + other players
    constexpr size_t Chr_ChrCtrl      = 0x58;
    constexpr size_t Chr_IsLockedOn   = 0xC9;
    constexpr size_t Chr_LockOnTarget = 0xD0;      // float4 world position the character faces when locked
    constexpr size_t Ctrl_IsUnlocked  = 0x199;     // ChrCtrl: 1 = free movement (body turns to move dir), 0 = strafe
    constexpr size_t PI_LockedEnemy   = 0x6B0;     // PlayerIns: FieldInsHandle of the locked-on enemy (-1 = none)
    constexpr size_t Chr_AimViewMtx   = 0x120;     // 4x4 float, camera-style rows
    constexpr size_t Chr_Modules      = 0x190;
    constexpr size_t Chr_BaseTransp   = 0x24C;
    constexpr size_t Mod_Behavior     = 0x28;
    constexpr size_t Mod_Physics      = 0x68;
    constexpr size_t Phys_Orientation = 0x50;      // quat xyzw
    constexpr size_t Phys_InterpOrient = 0x60;     // quat the game interpolates toward (write both to pin facing)
    constexpr size_t Phys_Position    = 0x70;      // xyzw
    constexpr size_t Beh_Unk10        = 0x10;      // -> +0x30 hkbCharacter (TGA / ERSkeletonMan)
    constexpr size_t Beh_HkbContext   = 0x60;      // -> +0x30 hkbCharacter (fromsoftware-rs erfps2 branch)
    constexpr size_t Ctx_HkbCharacter = 0x30;
    constexpr size_t Hkb_Setup        = 0x90;      // -> +0x28 hkaSkeleton
    constexpr size_t Setup_Skeleton   = 0x28;
    constexpr size_t Hkb_PoseHolder   = 0x38;      // -> ptr D ; pose = [D] + *(int*)([D]+0x54)
    constexpr size_t Hka_ParentIdx    = 0x20;
    constexpr size_t Hka_BoneCount    = 0x28;
    constexpr size_t Hka_NameLayout   = 0x30;      // char** ; name = [i*2]
    constexpr size_t Hka_RefPose      = 0x40;
    constexpr size_t Ctrl_ModelMatrix = 0x230;     // ChrCtrl model matrix (row 3 = position)
    constexpr size_t PI_AsmModelRes   = 0x640;
    constexpr size_t PI_AsmModelIns   = 0x648;
    constexpr size_t Asm_Parts        = 0x28;      // CSModelIns*[27]
    constexpr size_t MI_DispEntity    = 0x18;
    constexpr size_t Disp_Flags1      = 0x20;
    constexpr size_t Cam_Matrix       = 0x10;      // CSCam: 4 rows right/up/forward/pos
    constexpr size_t Cam_Fov          = 0x50;
    constexpr size_t ChrCam_ExFollowCam = 0x60;    // ChrCam -> ChrExFollowCam* (fromsoftware-rs; 2.7.2.0's chr-cam update calls the follow update with [chrCam+0x60])
    constexpr size_t FA_GameRend      = 0x20;
    // CSCamera (fromsoftware-rs): +0x08 = pers_cam_1, the FINAL render camera; +0x10/+0x18/+0x20 = sources the
    // camera step copies into it per camera_mask (+0x28). Writing only +0x20 (the old GR_PersCam1) meant the true
    // render camera only ever got our matrix through the game's own copy/interpolation - the camera lag.
    constexpr size_t GR_PersCamSlots[4] = {0x08, 0x10, 0x18, 0x20};
}
// RVAs for eldenring.exe 2.6.2.0 (erfps2 src/rva/ww.rs), with AOB fallbacks below.
constexpr uint32_t RVA_CameraStepUpdate = 0x3bc5e0;
constexpr uint32_t RVA_GetDmyPos        = 0x3e94d0;
constexpr uint32_t RVA_MmsUpdateChrCam  = 0xb02390;   // per-frame ChrCam update (erfps2 MMS_UPDATE_CHR_CAM)
constexpr uint32_t RVA_FollowCamFollow  = 0x3b6e20;   // ChrExFollowCam::follow (erfps2 FOLLOW_CAM_FOLLOW)
constexpr uint32_t RVA_UpdateFollowCam  = 0x3b7560;   // outer follow-cam update, fn(ChrExFollowCam*) (erfps2 UPDATE_FOLLOW_CAM)
constexpr uint32_t RVA_UpdateChrModelPos = 0x3ca950;  // per-chr model matrix update, fn(ChrCtrl*) (erfps2 UPDATE_CHR_MODEL_POS) - late in the frame, after the upper-body anim layer
// The game's lock-target update, fn(LockTgtMan*, float dt) (erfps2 UPDATE_LOCK_TGT, 1.16.2 table). It rewrites the
// player's lock state from the real target list every frame, so a lock we force at the camera step is gone by the
// time a cast animation spawns its bullet - erfps2 re-forces its first-person lock AFTER this call, and so does
// cast aim (its ray target + lock flag are what aim an unlocked spell).
constexpr uint32_t RVA_UpdateLockTgt = 0x716260;
constexpr const char* AOB_UpdateLockTgt = "48 8b c4 55 56 57 41 54 41 55 41 56 41 57 48 8d a8 88 fe ff ff 48 81 ec 40 02 00 00 48 c7 45 e0 fe ff ff ff";   // its prologue (unique in 2.6.2.0)
// Havok pose evaluation. Found with the pose scanner: the bulk pose store at +0x24e3 (rva 0x146b793) rewrites the
// live pose AFTER all three of our write points, which is what the weapon attachments then sample - the arms stay
// ours (the mesh consumer at 0x1652f80 reads earlier) while the weapons keep swinging with the animation.
// Function bounds resolved from the exe's .pdata exception table (exact, unlike scanning back for a prologue:
// the earlier 0x14692b0 guess turned out to be an unrelated 45-byte function, so that hook did nothing).
constexpr uint32_t RVA_PoseEval = 0x146b5e0;    // 0x146b5e0..0x146b845, contains the bulk pose store at 0x146b793
// The game rebuilds the camera matrix here (store at 0x68129c) AFTER our model-pos write - camscan phase
// tags put it past every ERVR write point - so its follow-cam-derived, interpolated position is what ends
// up rendered: the view lags the body while moving sideways and converges once you stop.
constexpr uint32_t RVA_CamFinalize = 0x680d70;  // 0x680d70..0x68180c
constexpr uint32_t RVA_CamWallRecovery  = 0x3b15c80;  // global bool: camera wall-recovery pushes (erfps2 clears it per frame in first person)
// Near-camera dither lookup (erfps2 USES_DITHERING): bool fn(mgr, out, id) walks mgr's entry table [+0x1060,+0x1068)
// (24-byte entries {int id; float a0, a1; float nearStart, nearEnd; int x}) and copies the id's entry to *out. Leaf,
// register-only - erfps2 detours it with exactly this signature. The AOB below is the function's own first 36 bytes.
constexpr uint32_t RVA_UsesDithering    = 0x1a679c0;
constexpr size_t   OFF_FollowCam_ResetCamera = 0x315;
constexpr size_t   OFF_FollowCam_AutoRotation = 0x240; // ChrExFollowCam.camera_auto_rotation (fromsoftware-rs; erfps2 clears in first person)
constexpr size_t   OFF_FollowCam_LockedOffset = 0x2E4; // ChrExFollowCam.locked_on_cam_offset (lateral lock-on camera offset; erfps2 zeroes it)

// ---------------------------------------------------------------- game types
struct HkQsTransform { float t[4]; float q[4]; float s[4]; };   // hkQsTransform (parent-relative)

using FnCameraStep = void (__fastcall*)(void* cscamera, void* fd4time);
using FnGetDmyPos  = float* (__fastcall*)(void* chrIns, float* outMtx4x4, const uint32_t* dmyId, int one);

// ---------------------------------------------------------------- state
uint8_t* g_base = nullptr;
uint8_t* g_camWallRecovery = nullptr;    // address of the camera wall-recovery bool global (0 while first person)
uint8_t** g_worldChrManPtr = nullptr;    // address of the WorldChrMan* global
uint8_t** g_fieldAreaPtr = nullptr;      // address of the FieldArea* global
uint8_t** g_gameDataManPtr = nullptr;    // address of the GameDataMan* global (stats cheat)
uint8_t** g_gameManPtr = nullptr;        // address of the GameMan* global (warp_requested)
uint8_t** g_eventFlagManPtr = nullptr;   // address of the CSEventFlagMan* global (loading byte)
std::atomic<ULONGLONG> g_warpReqTick{0}; // last tick GameMan.warp_requested read TRUE
std::atomic<ULONGLONG> g_loadingTick{0}; // last tick the loading-screen event byte read TRUE
std::atomic<ULONGLONG> g_bigJumpTick{0}; // last >100 m camera jump (teleport) tick
std::atomic<ULONGLONG> g_lastStashTick{0}; // last frame a second-eye camera was stashed
                                           // (gameplay heartbeat; goes quiet in menus)

// The pass's eye report, held back until pass end. The game's camera step re-smooths the
// final render camera AFTER our write (yaw in ~100% of frames, position >5 cm in ~half,
// measured 2026-08-22 20:13), so a report pushed at write time describes a camera the image
// was NOT rendered with - the compositor answers with head-motion skew and noisy stereo.
// UpdateCamera / GameERApplySecondEyeCamera stash here (last write of the pass wins, its
// written camera alongside); the frame pump calls GameERFinalizePassReport after each
// pass's queue_tasks returns - every writer done, Flip presented, same thread - which reads
// the camera the renderer REALLY used, folds the written->final delta into the pose, and
// only then pushes the report.
struct PendingReport {
    StereoEyeReport rep;
    Quat qWritten; Vec3 pWritten;   // the camera we wrote, game space
    bool valid = false;
};
std::mutex g_pendingRepMtx;
PendingReport g_pendingRep;

void StashPendingReport(const StereoEyeReport& rep, const Quat& qW, const Vec3& pW) {
    std::lock_guard<std::mutex> g(g_pendingRepMtx);
    g_pendingRep.rep = rep;
    g_pendingRep.qWritten = qW;
    g_pendingRep.pWritten = pW;
    g_pendingRep.valid = true;
}
FnCameraStep g_origCameraStep = nullptr;
using FnMmsUpdate = void (__fastcall*)(void*);
using FnFollow = void (__fastcall*)(void* followCam, float dt, void* c);
using FnUpdFollow = void (__fastcall*)(void* followCam);
using FnUpdModelPos = void (__fastcall*)(void* chrCtrl);
using FnUpdLockTgt = void (__fastcall*)(void* lockTgtMan, float dt);
FnUpdLockTgt g_origUpdLockTgt = nullptr;
using FnPoseEval = uint64_t (__fastcall*)(void*, void*, void*, void*);   // 4 int args + RAX forwarded (still an ABI guess - see fpPoseAfterHavok)
FnMmsUpdate g_origMms = nullptr; FnFollow g_origFollow = nullptr; FnUpdFollow g_origUpdFollow = nullptr;
FnUpdModelPos g_origUpdModelPos = nullptr; FnPoseEval g_origPoseEval = nullptr;
using FnCamFinalize = uint64_t (__fastcall*)(void*, void*, void*, void*);
FnCamFinalize g_origCamFinalize = nullptr;
using FnUsesDithering = bool (__fastcall*)(void* mgr, void* out, uint32_t id);
FnUsesDithering g_origUsesDithering = nullptr;
std::atomic<bool>  g_ditherFp{false};     // first person is running: apply NearDither (refreshed once per camera step)
std::atomic<float> g_ditherScale{0.f};    // Cfg().fpNearDither, cached for the render thread
// The ChrExFollowCam instance, captured from its hooks. Its *own* embedded camera (matrix at +0x10) is the
// source the game's camera step copies/interpolates the render camera from - writing only chr-cam/pers-cam
// left this one holding the game's follow position, and whoever sampled mid-step (R.E.A.L.) rendered a view
// that lagged behind the character and caught up when you stopped. So it gets our matrix at every reassert.
std::atomic<void*> g_followCamPtr{nullptr};
// The last camera matrix ERVR wrote, replayed after the game's own camera finalizer so ours is the one rendered.
float g_lastCamMtx[16];
std::atomic<bool> g_haveCamMtx{false};
std::mutex g_camMtxLock;
FnGetDmyPos  g_getDmyPos = nullptr;
void* g_hkOrig = nullptr; void** g_hkSlot = nullptr;
GameERStatus g_status;
// Foreign-camera-FOV detector. The map screen (the only road to fast travel), cutscenes and
// other full-screen takeovers write their own FOV into the camera we own every frame - the
// 13:18 crash log shows fov 0.838 in the pre-write dumps the whole time the map was open,
// against the 1.938 we keep writing. So "the FOV we wrote last frame is not the FOV we read
// this frame" = some game system has taken the camera = a warp can be requested at any
// moment; the stereo pump uses it to sit out the request window that no other guard covers.
std::atomic<ULONGLONG> g_foreignFovTick{0};
std::atomic<float>     g_lastFovWrite{0.f};
std::mutex g_vrMtx;
VRTracking g_vrTr; VRInputState g_vrIn; bool g_vrValid = false;
// ONE VR sample per render cycle. BuildFrame runs from four write points (pose hook, camera step, model-pos,
// post-havok) spread across the game's worker threads, and each call used to take the newest tracking sample -
// so the body was posed from one head position and the camera placed from another, taken milliseconds later.
// Stick yaw is a single shared number, which is why turning with the stick never separated them; head position
// is sampled per call, so moving your head while the character moves separates body and camera by exactly how
// far your head travelled between the two hooks. The cycle ends at the model-pos hook (our last write point).
double NowSec();
double NowSec();
VRTracking g_cycTr; VRInputState g_cycIn; bool g_cycValid = false;
std::atomic<uint32_t> g_vrCycle{0}, g_vrLatched{0xFFFFFFFFu};
double g_vrLatchTime = 0;

void LatchVRForCycle(VRTracking& tr, VRInputState& in, bool& valid) {
    std::lock_guard<std::mutex> g(g_vrMtx);
    uint32_t cyc = g_vrCycle.load(std::memory_order_relaxed);
    double now = NowSec();
    if (!Cfg().fpLatchVRPerFrame) { tr = g_vrTr; in = g_vrIn; valid = g_vrValid; return; }
    // Re-latch on a new cycle, or if the cycle stopped advancing (model-pos not running: menus, load screens).
    if (g_vrLatched.load(std::memory_order_relaxed) != cyc || now - g_vrLatchTime > 0.1) {
        g_vrLatched.store(cyc, std::memory_order_relaxed);
        g_cycTr = g_vrTr; g_cycIn = g_vrIn; g_cycValid = g_vrValid; g_vrLatchTime = now;
    }
    tr = g_cycTr; in = g_cycIn; valid = g_cycValid;
}
double g_lastDebug = 0;
uint32_t g_frame = 0;
// perf counters (reset by GameERGetStatus)
volatile LONG g_hkCalls = 0, g_hkSolves = 0, g_camCalls = 0;
// Pose-fight probe: counts how often the game overwrote the R_UpperArm local rotation between our writes, split
// by which of our write points ran last (0 = pose hook, 1 = camera-step post pass, 2 = model-pos hook). Locates
// the upper-body animation layer (walk sway, weapon arts) that still shows on the arms. Measured: it evaluates
// every frame AFTER the camera step, which is why the model-pos hook (later still) carries the final write.
Quat g_probeQ; int g_probeLastPoint = -1;
volatile LONG g_poseForeign[4] = {0, 0, 0, 0};
// Which bones the foreign layer touches and how hard (max degrees per 5 s window) - distinguishes a real
// animation layer (tens of degrees on the arm chain) from an aim/twist modifier (a few degrees, twist bones).
std::vector<int> g_probeBones; std::vector<std::string> g_probeNames;
std::vector<Quat> g_probeLastQ; std::vector<uint8_t> g_probeHas; std::vector<float> g_probeMaxDeg;
// Final-render-camera probe: our matrix writes were confirmed clean (foreign yaw 0), yet the view still lagged -
// so also compare the FINAL pers cam (+0x08) against what we last wrote, position included. Nonzero counts here
// mean the game's own step re-smooths the final camera; zero counts mean the smoothing happens beyond memory
// we can see (R.E.A.L.'s own game-camera filter).
Vec3 g_lastWrittenPos{};
volatile LONG g_finalYawForeign = 0, g_finalPosForeign = 0;
// Rig-vs-camera skew probe: distance between the character root the camera write was built on and the model
// matrix the rig actually renders with, same frame. Nonzero while moving = our lag; zero = the lag is R.E.A.L.'s.
Vec3 g_lastCamRoot{}; float g_skewMax = 0.f;
// Rendered head vs that pass's own eye, view axes, [0] = first eye (pass 1), [1] = second eye (pass 2). The two
// used to share one maximum against the FIRST eye, which is how the second eye's rider - re-placed by its own
// pass, up to 0.4 m off on 2026-11-02 - finally showed; now each eye is measured against its own camera.
float g_bodyCamLat[2] = {}, g_bodyCamFwd[2] = {}, g_bodyCamUp[2] = {};
float g_camHMin = 1e9f, g_camHMax = -1e9f;   // camera anchor height above the feet (the template's part of eye height)
volatile LONGLONG g_hkTicks = 0, g_camTicks = 0;
uint32_t g_lastArmFrame = 0xFFFFFFFF;
struct ScopedTicks { volatile LONGLONG* acc; LARGE_INTEGER t0; ScopedTicks(volatile LONGLONG* a) : acc(a) { QueryPerformanceCounter(&t0); } ~ScopedTicks() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); InterlockedExchangeAdd64(acc, t1.QuadPart - t0.QuadPart); } };

// ---------------------------------------------------------------- helpers
template <class T> bool RD(const void* addr, T& out) { return SafeRead(addr, &out, sizeof(T)); }
template <class T> bool WR(void* addr, const T& v) { return SafeWrite(addr, &v, sizeof(T)); }
uint8_t* Deref(const void* addr) { uint8_t* p = nullptr; return (SafeRead(addr, &p, sizeof(p))) ? p : nullptr; }

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}

// VR (right-handed, -Z forward) -> game (left-handed, +Z forward): mirror Z.
// The numpad hotkey wins over the ini so the experiment can be driven from inside the headset.
float DebugPass2PitchDeg() {
    const float k = DebugKeysPass2Pitch();
    return k >= 0.f ? k : Cfg().vrDebugPass2Pitch;
}

Vec3 ToGame(const Vec3& v) { return {v.x, v.y, -v.z}; }
Quat ToGame(const Quat& q) { return {-q.x, -q.y, q.z, q.w}; }

// Camera-style rotation from yaw (about +Y, positive = turn right) and pitch (positive = look down).
void CamBasis(float yaw, float pitch, Vec3& right, Vec3& up, Vec3& fwd) {
    right = {std::cos(yaw), 0.f, -std::sin(yaw)};
    fwd   = {std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    up    = fwd.Cross(right);
}
// Cached "the world is dying" check for the per-frame write paths (evaluated at most every
// 30 ms - the full check walks the HP chain plus two singleton reads). On the RISING edge it
// also drops the cached follow-cam pointer: that cache is the one write target with no
// per-use liveness check, and the 22:21:45 death crash (pass 1 teardown dereferencing a
// FLOAT bit pattern inside CS::CSWorldGeomIns bookkeeping, with our matrix/pose writes
// running at full rate through the whole death window) is exactly what a 64-byte float
// blob landing in freed-and-reused memory looks like.
bool WorldDyingNow() {
    static std::atomic<ULONGLONG> s_next{0};
    static std::atomic<bool> s_val{false};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = s_next.load(std::memory_order_relaxed);
    if (now >= next && s_next.compare_exchange_strong(next, now + 30)) {
        const bool d = GameERWorldDying();
        const bool was = s_val.load(std::memory_order_relaxed);
        if (d && !was) {
            g_followCamPtr.store(nullptr, std::memory_order_relaxed);
            LOGI("[guard] world dying - game-state writes stand down (camera/pose/facing)");
        } else if (!d && was) {
            LOGI("[guard] world stable - game-state writes resume");
        }
        s_val.store(d, std::memory_order_relaxed);
    }
    return s_val.load(std::memory_order_relaxed);
}

void WriteCamMatrix(uint8_t* cam, const Vec3& r, const Vec3& u, const Vec3& f, const Vec3& p) {
    // Single choke point for every camera-matrix write ERVR makes (pers cams, chr cam,
    // follow cam, aim view, second eye, debug pitch): while the world is dying, the game's
    // own camera (death cam included - erfps2 stands down for it too) owns the view.
    if (WorldDyingNow()) return;
    float m[16] = { r.x, r.y, r.z, 0.f, u.x, u.y, u.z, 0.f, f.x, f.y, f.z, 0.f, p.x, p.y, p.z, 1.f };
    SafeWrite(cam, m, sizeof(m));
}
Vec3 QuatRotate(const Quat& q, const Vec3& v) { return q.Rotate(v); }

// ---------------------------------------------------------------- game object access
uint8_t* WorldChrMan() { return g_worldChrManPtr ? Deref(g_worldChrManPtr) : nullptr; }
uint8_t* MainPlayer() { uint8_t* w = WorldChrMan(); return w ? Deref(w + off::WCM_MainPlayer) : nullptr; }
bool LooksLikeCam(uint8_t* p);
// Every chr-cam write is a 64-byte matrix, so only ever hand out a camera: on a game patch whose
// WorldChrMan layout is not mapped, the slot holds some other object.
uint8_t* ChrCam() {
    uint8_t* w = WorldChrMan(); if (!w) return nullptr;
    uint8_t* cc = Deref(w + GameWorldChrManOffset(off::WCM_ChrCam));
    return LooksLikeCam(cc) ? cc : nullptr;
}
// The follow cam the hooks last saw, but only while the live chr cam still owns it. The hooks keep
// capturing it through a death and its loads, so the cached pointer can outlive the object, and a
// 64-byte matrix written into a freed-and-reused follow cam is float data inside whatever lives there
// now (2026-10-31 co-op death: an EnemyIns field read back as two floats, 0x3bb25448'3e59877c).
uint8_t* FollowCam() {
    uint8_t* fc = (uint8_t*)g_followCamPtr.load(std::memory_order_relaxed);
    if (!fc) return nullptr;
    uint8_t* cc = ChrCam();
    uint8_t* owned = cc ? Deref(cc + off::ChrCam_ExFollowCam) : nullptr;
    static std::atomic<ULONGLONG> s_logAt{0};
    static std::atomic<int> s_state{-1};   // last logged: 1 owned, 0 not
    const int st = owned == fc ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    if (st != s_state.load(std::memory_order_relaxed) && now - s_logAt.load(std::memory_order_relaxed) > 30000) {
        s_state.store(st, std::memory_order_relaxed); s_logAt.store(now, std::memory_order_relaxed);
        if (st) LOGI("[cam] follow cam %p is the chr cam's own - follow-cam writes validated", (void*)fc);
        else LOGI("[cam] follow cam %p is not the chr cam's (%p) - follow-cam writes skipped until it is", (void*)fc, (void*)owned);
    }
    return st ? fc : nullptr;
}
uint8_t* GameRend() {
    uint8_t* fa = g_fieldAreaPtr ? Deref(g_fieldAreaPtr) : nullptr; if (!fa) return nullptr;
    return Deref(fa + off::FA_GameRend);
}
// Does this pointer really address a CSCam? A slot that is not a camera must never be written: the pers-cam
// slot layout is version- and build-dependent (the +0x08 "final camera" read back null on 2.6.2.0), and a 64-byte
// matrix written into some other object is silent corruption. Orthonormal basis + a sane fov is proof enough.
bool LooksLikeCam(uint8_t* p) {
    if (!p) return false;
    float m[16]; if (!SafeRead(p + off::Cam_Matrix, m, sizeof(m))) return false;
    for (int r = 0; r < 3; ++r) {
        float L = std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
        if (L < 0.9f || L > 1.1f) return false;
    }
    float fov; if (!SafeRead(p + off::Cam_Fov, &fov, 4)) return false;
    return fov > 0.05f && fov < 3.2f;
}
// All distinct, validated CSPersCam objects hanging off CSCamera. Returns count; logs the layout once.
int AllPersCams(uint8_t* out[4]) {
    uint8_t* gr = GameRend(); if (!gr) return 0;
    int n = 0; uint32_t mask = 0;
    for (int s = 0; s < 4; ++s) {
        uint8_t* p = Deref(gr + off::GR_PersCamSlots[s]);
        if (!LooksLikeCam(p)) continue;
        mask |= 1u << s;
        bool dup = false; for (int i = 0; i < n; ++i) if (out[i] == p) dup = true;
        if (!dup) out[n++] = p;
    }
    static uint32_t s_logged = 0xFFFFFFFF;
    if (mask != s_logged) {
        s_logged = mask;
        LOGI("[cam] CSCamera slots holding real cameras: %s%s%s%s (%d distinct)",
             (mask & 1) ? "+0x08 " : "", (mask & 2) ? "+0x10 " : "", (mask & 4) ? "+0x18 " : "", (mask & 8) ? "+0x20 " : "", n);
    }
    return n;
}
// The camera the game actually renders from: the last validated slot (the classic +0x20 on 2.6.2.0).
uint8_t* PersCam1() {
    uint8_t* cams[4]; int n = AllPersCams(cams);
    return n ? cams[n - 1] : nullptr;
}
uint8_t* Physics(uint8_t* chr) { uint8_t* m = Deref(chr + off::Chr_Modules); return m ? Deref(m + off::Mod_Physics) : nullptr; }
uint8_t* Behavior(uint8_t* chr) { uint8_t* m = Deref(chr + off::Chr_Modules); return m ? Deref(m + off::Mod_Behavior) : nullptr; }

bool PlayerRoot(uint8_t* chr, Vec3& pos, Quat& q) {
    uint8_t* ph = Physics(chr); if (!ph) return false;
    float p[4], r[4];
    if (!SafeRead(ph + off::Phys_Position, p, 16) || !SafeRead(ph + off::Phys_Orientation, r, 16)) return false;
    pos = {p[0], p[1], p[2]}; q = {r[0], r[1], r[2], r[3]};
    return true;
}
bool ModelMatrixPos(uint8_t* chr, Vec3& pos) {
    uint8_t* ctrl = Deref(chr + off::Chr_ChrCtrl); if (!ctrl) return false;
    float p[4]; if (!SafeRead(ctrl + off::Ctrl_ModelMatrix + 48, p, 16)) return false;
    pos = {p[0], p[1], p[2]}; return true;
}
// Full ChrCtrl model matrix (rows 0..2 = basis, row 3 = position). This is the transform the *renderer*
// hangs the skeleton off, so bone world transforms have to be built from it or the arms sit in a slightly
// different place than everything you can see - most visibly while running, when it and the physics
// position are a frame of motion apart.
bool ModelMatrixRoot(uint8_t* chr, Vec3& pos, Quat& q) {
    uint8_t* ctrl = Deref(chr + off::Chr_ChrCtrl); if (!ctrl) return false;
    float m[16]; if (!SafeRead(ctrl + off::Ctrl_ModelMatrix, m, sizeof(m))) return false;
    Vec3 X{m[0], m[1], m[2]}, Y{m[4], m[5], m[6]}, Z{m[8], m[9], m[10]};
    float lx = X.Length(), ly = Y.Length(), lz = Z.Length();
    if (std::fabs(lx - 1.f) > 0.05f || std::fabs(ly - 1.f) > 0.05f || std::fabs(lz - 1.f) > 0.05f) return false;
    pos = {m[12], m[13], m[14]};
    q = Quat::FromBasis(X * (1.f / lx), Y * (1.f / ly), Z * (1.f / lz));
    return true;
}
// One root for the camera anchor and the bone solve, so they never drift apart.
//   model       (default) position from the ChrCtrl model matrix, rotation from the physics module (local -Z =
//               facing, verified; the model matrix rotation was never verified against the physics one)
//   modelmatrix both from the model matrix
//   physics     both from the physics module
//
// The root is YAW ONLY. The physics orientation is not always a pure yaw: locked onto a target above or below the
// character (cast aim's ray target above all) the game pitches it toward the target while the model matrix the
// renderer uses stays level (2026-09-04 log: root and the live head dummy fixed, the template head anchor 1 m off
// sideways and 0.8 m down while the casting hand tilted - the camera "moved with the hand"). Every consumer here
// (camera anchor, rig yaw, calibration) wants the facing, so the tilt is stripped before anyone sees it.
Quat YawOnlyRoot(const Quat& q) {
    const Vec3 f = q.Rotate({0.f, 0.f, -1.f});
    if (f.x * f.x + f.z * f.z < 1e-4f) return q;   // facing straight up/down: no yaw to keep
    static bool s_warned = false;
    if (!s_warned && std::fabs(f.y) > 0.087f) { s_warned = true; LOGI("[fp] physics root is pitched %.0f deg (lock turn toward a high/low target?) - using its yaw only", Rad2Deg(std::asin(Clamp(f.y, -1.f, 1.f)))); }
    return Quat::YawQ(std::atan2(f.x, f.z) + 3.14159265f);   // YawQ(a).Rotate(0,0,-1) = (-sin a, 0, -cos a): a = facing + pi
}
bool SkeletonRoot(uint8_t* chr, Vec3& pos, Quat& q) {
    const std::string& src = Cfg().fpArmRootSource;
    if (src == "modelmatrix" && ModelMatrixRoot(chr, pos, q)) { q = YawOnlyRoot(q); return true; }
    if (!PlayerRoot(chr, pos, q)) return false;
    if (src != "physics") { Vec3 mp; if (ModelMatrixPos(chr, mp)) pos = mp; }
    q = YawOnlyRoot(q);
    return true;
}

// ---------------------------------------------------------------- head hiding
void SetPartVisible(uint8_t* asmIns, int idx, bool visible) {
    uint8_t* part = Deref(asmIns + off::Asm_Parts + idx * 8); if (!part) return;
    uint8_t* disp = Deref(part + off::MI_DispEntity); if (!disp) return;
    uint32_t f; if (!RD(disp + off::Disp_Flags1, f)) return;
    uint32_t nf = (f & ~1u) | (visible ? 1u : 0u);
    if (nf != f) WR(disp + off::Disp_Flags1, nf);
}
void ApplyHeadVisibility(uint8_t* player, bool hideHead, bool hideSheathed) {
    // Model-part flag writes stand down while the world is dying (see WorldDyingNow); the
    // flags keep their last state, so nothing changes visually during the death cam.
    if (WorldDyingNow()) return;
    uint8_t* asmIns = Deref(player + off::PI_AsmModelIns);
    if (asmIns) {
        static const int kHead[] = {0, 2, 6, 21, 22, 23, 24, 25};
        for (int i : kHead) SetPartVisible(asmIns, i, !hideHead);
        // 7 = left weapon, 8 = left sheath, 11 = right weapon, 12 = right sheath. Never touch the weapons.
        static const int kSheath[] = {8, 12};
        for (int i : kSheath) SetPartVisible(asmIns, i, !hideSheathed);
        SetPartVisible(asmIns, 7, true); SetPartVisible(asmIns, 11, true);
    }
    if (uint8_t* res = Deref(player + off::PI_AsmModelRes)) {
        uint8_t b;
        // 1.17's ChrAsmModelRes masks moved by eight bytes (erfps2 1.17 layout).
        if (IsGame117(CurrentGameBuild())) res += 8;
        if (RD(res + 0x58, b)) WR(res + 0x58, (uint8_t)((b & ~8) | (hideHead ? 8 : 0)));
        if (RD(res + 0x49, b)) WR(res + 0x49, (uint8_t)((b & ~8) | (hideHead ? 8 : 0)));
        if (RD(res + 0x50, b)) WR(res + 0x50, (uint8_t)((b & ~32) | (hideHead ? 32 : 0)));
    }
}

MountedTurnQueue g_mountedTurns;
// Once per simulation frame: every frame build and every character's model update asks while riding (~900 calls/s),
// and a summon-set scan per call adds up. Both passes of a frame share the answer.
TorrentLookup CurrentTorrentLookup(uint8_t* player) {
    if (!g_status.versionOk || !player) return {};
    static std::mutex s_mtx; static TorrentLookup s_last; static uint8_t* s_player = nullptr; static uint32_t s_frame = ~0u;
    const uint32_t frame = g_frame;
    {
        std::lock_guard<std::mutex> g(s_mtx);
        if (s_player == player && s_frame == frame) return s_last;
    }
    uint8_t* world = g_worldChrManPtr ? Deref(g_worldChrManPtr) : nullptr;
    const TorrentLookup l = FindMountedTorrent(player, world, SafeRead);
    std::lock_guard<std::mutex> g(s_mtx);
    s_last = l; s_player = player; s_frame = frame;
    return l;
}
uint8_t* CurrentTorrent(uint8_t* player) { return CurrentTorrentLookup(player).mount; }

// Heading of a transform (physics/model forward is local -Z) as a pure yaw, without YawOnlyRoot's lock-on note.
Quat HeadingQ(const Quat& q) {
    const Vec3 f = q.Rotate({0.f, 0.f, -1.f});
    if (f.x * f.x + f.z * f.z < 1e-6f) return q;
    return Quat::YawQ(std::atan2(f.x, f.z) + 3.14159265f);
}

// One line per ride that sits on a horse ERVR cannot find: everything the resolver read, so the next log
// names the field that broke (the 2026-11-02 ride on 1.17.2 resolved nothing and said nothing).
void LogTorrentLookup(bool active, const TorrentLookup& l) {
    static double s_since = 0; static bool s_logged = false, s_found = false;
    const double now = NowSec();
    if (!active) { s_since = 0; s_logged = false; s_found = false; return; }
    if (l.mount) {
        if (!s_found) LOGI("[ride] Torrent %p found by its %s", l.mount, l.how);
        s_found = true; s_since = 0; return;
    }
    if (s_since <= 0) s_since = now;
    // Seated it should resolve at once; the summon and dismount animations take a second or two, so a ride state
    // that never reads as seated is reported after 4 s (a riding state numbered differently would look like that).
    if (s_logged || now - s_since < (l.seated ? 1.5 : 4.0)) return;
    s_logged = true;
    LOGW("[ride] %s, but Torrent was not found: node %d state %u, flags mounting %u mounted %u, rider %llx paired with %llx | "
         "last mount %p npc %d handle %llx ridden %u paired with %llx | %u summon-set entries scanned. Horse turns, Torrent's "
         "visibility and the camera smoothing need the horse; %s",
         l.seated ? "riding" : "4 s in the ride state without a riding state", (int)l.node, l.state, l.mounting, l.mounted,
         (unsigned long long)l.rider, (unsigned long long)l.peer, l.lastMounted, l.lastNpc, (unsigned long long)l.lastHandle,
         l.lastRidden, (unsigned long long)l.lastPeer, l.scanned,
         l.seated ? "the seated pose still applies." : "the seated pose waits for a riding state too.");
}

bool LivingMount(uint8_t* mount) {
    uint8_t flags = 0;
    uint8_t* modules = mount ? Deref(mount + off::Chr_Modules) : nullptr;
    uint8_t* data = modules ? Deref(modules) : nullptr;
    int32_t hp = 0;
    return data && Deref(data + 8) == mount && RD(data + 0x138, hp) && hp > 0 &&
           RD(mount + 0x1C5, flags) && !(flags & 0x80);
}

// The close VR view needs the active horse as well as the rider. NearDither
// only handles a material lookup; it does not restore a culled display entity
// or character opacity. Never touch other summons or a mount/dismount fade.
void ApplyTorrentVisibility(uint8_t* mount) {
    if (!mount || WorldDyingNow() || !LivingMount(mount)) return;
    uint8_t* model = Deref(mount + 0x50);
    uint8_t* display = model ? Deref(model + off::MI_DispEntity) : nullptr;
    if (!display) return;
    MountedVisibility before;
    if (!RD(mount + 0x1C4, before.frustum) || !RD(mount + 0x1C5, before.render) ||
        !RD(display + off::Disp_Flags1, before.display) ||
        !RD(mount + 0x238, before.opacity) || !RD(mount + 0x23C, before.previousOpacity) ||
        !RD(mount + off::Chr_BaseTransp, before.baseOpacity)) return;
    for (float a : {before.opacity, before.previousOpacity, before.baseOpacity})
        if (!std::isfinite(a) || a < 0.f || a > 1.f) return;
    const MountedVisibility visible = VisibleMountedTorrent(before);
    if (before.frustum != visible.frustum) WR(mount + 0x1C4, visible.frustum);
    if (before.render != visible.render) WR(mount + 0x1C5, visible.render);
    if (before.display != visible.display) WR(display + off::Disp_Flags1, visible.display);
    if (before.opacity != visible.opacity) WR(mount + 0x238, visible.opacity);
    if (before.previousOpacity != visible.previousOpacity) WR(mount + 0x23C, visible.previousOpacity);
    if (before.baseOpacity != visible.baseOpacity) WR(mount + off::Chr_BaseTransp, visible.baseOpacity);
    static double s_last = 0;
    const double now = NowSec();
    if (now - s_last > 5.0) {
        s_last = now;
        LOGI("[ride] Torrent %p requested draw state: frustum %02X render %02X display %08X opacity %.3f/%.3f base %.3f (does not confirm a rendered mesh)",
             mount, before.frustum, before.render, before.display, before.opacity, before.previousOpacity, before.baseOpacity);
        uint8_t* ctrl = Deref(mount + off::Chr_ChrCtrl);
        float matrix[16]{}, alpha[3]{}; Vec3 physicsPos; Quat physicsQ;
        if (ctrl && SafeRead(ctrl + off::Ctrl_ModelMatrix, matrix, sizeof(matrix)) && PlayerRoot(mount, physicsPos, physicsQ)) {
            SafeRead(mount + 0x240, alpha, sizeof(alpha));
            LOGI("[ride] Torrent model pos %.3f %.3f %.3f, physics %.3f %.3f %.3f, scale %.3f %.3f %.3f, tint %.3f camouflage %.3f",
                 matrix[12], matrix[13], matrix[14], physicsPos.x, physicsPos.y, physicsPos.z,
                 Vec3{matrix[0], matrix[1], matrix[2]}.Length(), Vec3{matrix[4], matrix[5], matrix[6]}.Length(),
                 Vec3{matrix[8], matrix[9], matrix[10]}.Length(), alpha[0], alpha[2]);
        }
    }
}

// The 5 s "[ride]" summary (UpdateChrModelPosBody): what turned the pair, how far the view sat off the bouncing
// rider, and how far the second eye's pass re-placed the rider and the horse.
struct RideStats {
    std::mutex mtx;
    float stick = 0.f, physical = 0.f, applied = 0.f;   // radians, summed magnitudes
    float camDev = 0.f;                                  // m, smoothed view vs the rider's anchor (max)
    Vec3 riderShift{};                                   // m, pass 2 - pass 1, rider frame (per-axis max magnitude, signed)
    float torrentShift = 0.f;                            // m, pass 2 - pass 1 (max)
    int pass2 = 0;                                       // pass-2 rebuilds of the rider measured
    void Turn(float& sum, float radians) { std::lock_guard<std::mutex> g(mtx); sum += std::fabs(radians); }
} g_rideStats;

void ApplyMountedTurn(uint8_t* player, uint8_t* mount, float radians) {
    if (!mount || radians == 0.f || !std::isfinite(radians) || WorldDyingNow() || !LivingMount(mount)) return;
    g_rideStats.Turn(g_rideStats.applied, radians);
    uint8_t* horsePh = Physics(mount); uint8_t* riderPh = Physics(player);
    if (!horsePh || !riderPh || Deref(horsePh + 8) != mount || Deref(riderPh + 8) != player) return;
    float horse[12], rider[16]; // current/target quaternions, position; rider also previous position
    if (!SafeRead(horsePh + off::Phys_Orientation, horse, sizeof(horse)) ||
        !SafeRead(riderPh + off::Phys_Orientation, rider, sizeof(rider))) return;
    for (float v : horse) if (!std::isfinite(v)) return;
    for (float v : rider) if (!std::isfinite(v)) return;
    Quat horseQ{horse[0], horse[1], horse[2], horse[3]}, riderQ{rider[0], rider[1], rider[2], rider[3]};
    if (std::fabs(horseQ.Norm() - 1.f) > 0.05f || std::fabs(riderQ.Norm() - 1.f) > 0.05f) return;
    const Vec3 horseP{horse[8], horse[9], horse[10]};
    Vec3 riderP{rider[8], rider[9], rider[10]};
    if ((riderP - horseP).Length() > 5.f) return;
    TurnRiderAboutMount(horseP, radians, horseQ, riderP, riderQ);
    const Quat turn = Quat::YawQ(radians);
    const Quat horseTarget = (turn * Quat{horse[4], horse[5], horse[6], horse[7]}).Normalized();
    const Quat riderTarget = (turn * Quat{rider[4], rider[5], rider[6], rider[7]}).Normalized();
    const Vec3 riderPrevious = horseP + turn.Rotate(Vec3{rider[12], rider[13], rider[14]} - horseP);
    const float horseRot[8] = {horseQ.x, horseQ.y, horseQ.z, horseQ.w, horseTarget.x, horseTarget.y, horseTarget.z, horseTarget.w};
    const float riderState[16] = {riderQ.x, riderQ.y, riderQ.z, riderQ.w, riderTarget.x, riderTarget.y, riderTarget.z, riderTarget.w,
        riderP.x, riderP.y, riderP.z, rider[11], riderPrevious.x, riderPrevious.y, riderPrevious.z, rider[15]};
    SafeWrite(horsePh + off::Phys_Orientation, horseRot, sizeof(horseRot));
    SafeWrite(riderPh + off::Phys_Orientation, riderState, sizeof(riderState));
    LOGD("[ride] stick turn %.2f deg applied to Torrent and rider", Rad2Deg(radians));
}

// ---------------------------------------------------------------- skeleton
struct Skeleton {
    uint8_t* chr = nullptr;
    uint8_t* hkb = nullptr;
    int boneCount = 0;
    std::vector<std::string> names;
    std::vector<int16_t> parents;
    std::vector<uint8_t> fingers;
    HkQsTransform* refPose = nullptr;
    std::vector<HkQsTransform> refPoseCopy;
    int L_ua = -1, L_fa = -1, L_hand = -1, R_ua = -1, R_fa = -1, R_hand = -1;
    Vec3 handFwd[2], handThumb[2]; bool handAxesOk[2]{};   // hand-bone local frame, from ref pose finger roots
    int torso = -1, L_clav = -1, R_clav = -1, head = -1, pelvis = -1;
    bool warned = false;

    HkQsTransform* LivePose() const {
        uint8_t* d = Deref(hkb + off::Hkb_PoseHolder); if (!d) return nullptr;
        uint8_t* p = Deref(d); if (!p) return nullptr;
        int o; if (!RD(p + 0x54, o) || o <= 0 || o > 0x100000) return nullptr;
        return (HkQsTransform*)(p + o);
    }
    int Find(const std::string& n) const { for (int i = 0; i < boneCount; ++i) if (names[i] == n) return i; return -1; }
};
Skeleton g_skel;
// PoseMode=vr: the whole rig is rewritten from this captured pose on every Havok tick, so no game animation
// ever shows on the body - rolls, attacks and runs still *displace* the character (root motion goes through
// the physics module, which we do not touch) but the rig itself stays 1:1 with the VR pose. The template is
// captured at skeleton build (whatever the animation held right then) and re-captured whenever you stand
// still for a second, so idle/crouch stances come through as discrete poses.
// Grace/menu poses must never replace this walking template.
std::vector<HkQsTransform> g_poseTemplate;
// Seated on Torrent (see MountedSeatFrame): the rider's own settled seat, its head and pelvis in the rider's model
// frame (camera anchor / upright pivot), the settle candidate, and whose seat it is (kept across reloads like the
// standing template). RiderPassHold keeps the second eye's rider where the first eye drew it.
std::vector<HkQsTransform> g_seatedTemplate, g_seatCand;
MountedSeatFrame g_seatedFrame;
SeatSettle g_seatSettle;
Vec3 g_seatHead{}, g_seatPelvis{};
std::wstring g_seatOwner;
std::mutex g_riderHoldMtx;   // pass 1 writes it from the rider's model-pos hook, pass 2 reads it from every write point
RiderPassHold g_riderHold;
// Stereo pairs, advanced between the two passes (GameERApplySecondEyeCamera): pass 1 of pair k runs while it reads
// k, pass 2 while it reads k+1. g_frame cannot key the hold - it advances at the camera step, AFTER the rider's
// pass-1 rebuild (2026-11-02 20:12 log: not one pass-2 frame matched its pass-1 snapshot).
std::atomic<uint32_t> g_stereoPair{0};
uint32_t RiderHoldPair() { const uint32_t k = g_stereoPair.load(); return FramePumpCurrentPass() == 2 ? k - 1 : k; }
std::mutex g_rideStabMtx;
RideCameraStabilizer g_rideStab;
std::mutex g_rideFollowMtx;
RideHeadFollow g_rideFollow;
bool SeatTemplateReady() { return g_skel.boneCount > 0 && (int)g_seatedTemplate.size() == g_skel.boneCount; }
// The blade each hand's arm was last solved holding (world, pass 1): GameERBladeWorld.
struct BladeSample { bool valid = false; Vec3 grip{}, tip{}; double stamp = 0; };
std::mutex g_bladeMtx;
BladeSample g_blade[2];
// Kinetic: tracking frame -> world for each hand's simulated grip (GameERPlayToWorld), under g_bladeMtx.
struct PlayToWorld { bool valid = false; Vec3 origin{}; Quat rot{}; float scale = 1.f; double stamp = 0; };
PlayToWorld g_playToWorld[2];
BodyPoseCaptureGuard g_poseCaptureGuard;
Vec3 g_statPos{}; double g_statSince = 0; double g_templLastCapture = 0;
// Stance re-capture settle guard: candidate snapshot of a pose that differs from the template, only promoted to
// template once it has held still for a while (see UpdateArms) - a transient animation must never become the template.
std::vector<HkQsTransform> g_templCand; double g_templCandAt = 0, g_templCheckAt = 0;
// Stance bookkeeping (JudgeStance): the camera rides the template's head, so each template is judged by that
// head's height against the skeleton's reference pose. The last standing template is kept apart so a low one
// (crouch) is left the moment the game stands back up, and so a reload of the same character keeps the body it
// had instead of snapshotting whatever the respawn shows.
std::vector<HkQsTransform> g_standTemplate;
float g_refHead = -1.f, g_standHead = -1.f, g_templHead = -1.f;
bool g_templLow = false;
const char* g_templWhy = "";
std::wstring g_templOwner;
double g_firstRejectSince = 0, g_rejectLogAt = 0;
StandUpWatch g_standUp;
bool CaptureTemplate(uint8_t* chr, const char* why, bool firstTemplate);
// Whose templates they are: PlayerGameData::character_name (fromsoftware-rs: u16[17] at +0x9C).
std::wstring PlayerCharacterName() {
    uint8_t* pgd = GameERPlayerGameData(); if (!pgd) return {};
    wchar_t name[17]{};
    if (!SafeRead(pgd + 0x9C, name, sizeof(name))) return {};
    name[16] = 0;
    return name;
}
float TemplateHead(const std::vector<HkQsTransform>& t) {
    if (g_skel.head < 0 || (int)t.size() != g_skel.boneCount) return -1.f;
    return BoneHeight<HkQsTransform>(t, g_skel.parents, g_skel.head);
}
// Replace a template's contents in place when the size allows: the camera hooks read g_poseTemplate from other
// threads, and a move-assign frees the buffer they may be walking.
void SetTemplate(std::vector<HkQsTransform>& dst, const std::vector<HkQsTransform>& src) {
    if (dst.size() == src.size()) std::copy(src.begin(), src.end(), dst.begin()); else dst = src;
}
bool OursBone(int i);
namespace posescan { void Disarm(const char* why); void MarkPhase(int bit); void Drive(void* pose, size_t bytes); }
// Per-root-bone bookkeeping for the BodyFollowsHead shift, so applying it twice in a frame is a no-op.
std::vector<Vec3> g_rootShiftBase, g_rootShiftWritten;
std::vector<Quat> g_rootRotBase, g_rootRotWritten;
std::vector<uint8_t> g_rootShiftHas;
void ResetRecenter();
extern HeadAnchorState g_headAnchor;

uint8_t* FindHkbCharacter(uint8_t* chr) {
    uint8_t* beh = Behavior(chr); if (!beh) return nullptr;
    for (size_t o : { off::Beh_Unk10, off::Beh_HkbContext }) {
        uint8_t* ctx = Deref(beh + o); if (!ctx) continue;
        uint8_t* hkb = Deref(ctx + off::Ctx_HkbCharacter); if (!hkb) continue;
        uint8_t* setup = Deref(hkb + off::Hkb_Setup); if (!setup) continue;
        uint8_t* hka = Deref(setup + off::Setup_Skeleton); if (!hka) continue;
        int count; if (!RD(hka + off::Hka_BoneCount, count) || count <= 0 || count > 1024) continue;
        return hkb;
    }
    return nullptr;
}
// The cached skeleton is `player`'s only while that character still runs the same hkbCharacter. A
// respawn can put the new PlayerIns at the old address, and then the pose buffer reached through the
// dead character's hkbCharacter is freed memory: every bulk pose write is 7 KB of bone floats into
// whatever reuses it (2026-10-31 co-op death: writes resumed between the death load and Seamless
// Co-op's reload with no skeleton rebuild logged, then an EnemyIns held float data and crashed).
// Either behavior path FindHkbCharacter accepts may hold it: a rebuild also resets the VR pose
// template, so a transient read of one path must not count as a new skeleton.
bool SkeletonCurrent(uint8_t* player) {
    if (!player || g_skel.chr != player || !g_skel.hkb) return false;
    uint8_t* beh = Behavior(player); if (!beh) return false;
    for (size_t o : { off::Beh_Unk10, off::Beh_HkbContext }) {
        uint8_t* ctx = Deref(beh + o);
        if (ctx && Deref(ctx + off::Ctx_HkbCharacter) == g_skel.hkb) return true;
    }
    return false;
}

bool BuildSkeleton(uint8_t* chr) {
    Skeleton s; s.chr = chr;
    s.hkb = FindHkbCharacter(chr); if (!s.hkb) return false;
    uint8_t* setup = Deref(s.hkb + off::Hkb_Setup); uint8_t* hka = setup ? Deref(setup + off::Setup_Skeleton) : nullptr;
    if (!hka) return false;
    if (!RD(hka + off::Hka_BoneCount, s.boneCount)) return false;
    char** layout = (char**)Deref(hka + off::Hka_NameLayout);
    int16_t* parents = (int16_t*)Deref(hka + off::Hka_ParentIdx);
    s.refPose = (HkQsTransform*)Deref(hka + off::Hka_RefPose);
    if (!layout || !parents || !s.refPose) return false;
    s.names.resize(s.boneCount); s.parents.resize(s.boneCount); s.refPoseCopy.resize(s.boneCount); s.fingers.resize(s.boneCount);
    if (!SafeRead(s.refPose, s.refPoseCopy.data(), sizeof(HkQsTransform) * s.boneCount)) return false;
    for (int i = 0; i < s.boneCount; ++i) {
        char* nm = nullptr; RD(layout + i * 2, nm);
        char buf[64]{}; if (nm) SafeRead(nm, buf, 63);
        s.names[i] = buf;
        RD(parents + i, s.parents[i]);
        s.fingers[i] = IsFingerBone(s.names[i]) ||
            (s.parents[i] >= 0 && s.parents[i] < i && s.fingers[s.parents[i]]);
    }
    const Config& c = Cfg();
    s.L_ua = s.Find(c.boneLUpperArm); s.L_fa = s.Find(c.boneLForearm); s.L_hand = s.Find(c.boneLHand);
    s.R_ua = s.Find(c.boneRUpperArm); s.R_fa = s.Find(c.boneRForearm); s.R_hand = s.Find(c.boneRHand);
    s.torso = s.Find(c.boneTorso); s.head = s.Find(c.boneHead); s.pelvis = s.Find("Pelvis");
    s.L_clav = s.Find("L_Clavicle"); s.R_clav = s.Find("R_Clavicle");
    if (s.L_clav < 0) s.L_clav = s.L_ua; if (s.R_clav < 0) s.R_clav = s.R_ua;
    // Anatomical hand axes from the reference pose: finger roots = forward, thumb root = thumb side.
    for (int h = 0; h < 2; ++h) {
        const char* pre = h == HandLeft ? "L_" : "R_";
        int hand = h == HandLeft ? s.L_hand : s.R_hand;
        int f2 = s.Find(std::string(pre) + "Finger2"), f1 = s.Find(std::string(pre) + "Finger1"), f3 = s.Find(std::string(pre) + "Finger3"), f0 = s.Find(std::string(pre) + "Finger0");
        if (hand < 0 || f2 < 0 || f0 < 0) continue;
        auto rootOf = [&](int i) {   // accumulate local offsets up to the hand bone
            Vec3 acc{}; int cur = i; int guard = 0;
            while (cur >= 0 && cur != hand && guard++ < 8) { const HkQsTransform& t = s.refPoseCopy[cur]; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]}; acc = lq.Rotate(acc) + Vec3{t.t[0], t.t[1], t.t[2]}; cur = s.parents[cur]; }
            return acc; };
        Vec3 F = rootOf(f2); if (f1 >= 0) F = F + rootOf(f1); if (f3 >= 0) F = F + rootOf(f3);
        Vec3 T = rootOf(f0);
        if (F.Length() < 1e-3f || T.Length() < 1e-3f) continue;
        s.handFwd[h] = F.Normalized(); s.handThumb[h] = T; s.handAxesOk[h] = true;
        LOGI("[skel] %shand axes (local): fwd(%.2f,%.2f,%.2f) thumb(%.2f,%.2f,%.2f)", pre, s.handFwd[h].x, s.handFwd[h].y, s.handFwd[h].z, T.x, T.y, T.z);
    }
    if (c.dumpSkeleton) {
        std::string all;
        for (int i = 0; i < s.boneCount; ++i) { all += std::to_string(i) + ":" + s.names[i] + "(p" + std::to_string(s.parents[i]) + ") "; if (all.size() > 1500) { LOGI("[skel] %s", all.c_str()); all.clear(); } }
        if (!all.empty()) LOGI("[skel] %s", all.c_str());
    }
    LOGI("[skel] player skeleton: %d bones, hkbCharacter %p; arms L(%d,%d,%d) R(%d,%d,%d)", s.boneCount, (void*)s.hkb, s.L_ua, s.L_fa, s.L_hand, s.R_ua, s.R_fa, s.R_hand);
    // A respawn, warp or area load rebuilds the skeleton of the same character. Keep the standing body it had:
    // a fresh snapshot here caught whatever the reload showed - the respawn kneel, a roll, the first steps of a
    // run - and the camera rides that template's head ("dropped to the ground after dying", 2026-11-01).
    const std::wstring owner = PlayerCharacterName();
    const bool sameRig = !owner.empty() && g_skel.boneCount == s.boneCount && g_skel.names == s.names && g_skel.parents == s.parents;
    const bool carry = !g_standTemplate.empty() && owner == g_templOwner && sameRig;
    const bool carrySeat = !g_seatedTemplate.empty() && owner == g_seatOwner && sameRig;   // the seat waits for an idle moment on the horse
    g_skel = std::move(s);
    g_rootShiftBase.clear(); g_rootShiftWritten.clear(); g_rootShiftHas.clear(); g_rootRotBase.clear(); g_rootRotWritten.clear();
    ResetRecenter();   // new character / area load: start centred on wherever you are standing now
    g_headAnchor.Reset();
    g_seatedFrame = {}; g_seatSettle.Reset(); g_seatCand.clear(); g_riderHold.Reset();
    if (!carrySeat) g_seatedTemplate.clear();
    g_poseTemplate.clear(); g_statPos = {}; g_statSince = 0; g_templLastCapture = 0;
    g_templCand.clear(); g_templCandAt = 0; g_templCheckAt = 0;
    posescan::Disarm("skeleton rebuild");   // the watched buffer may be gone
    // Pose-fight probe bones: right arm chain + torso, plus twist helpers if the model has them.
    g_probeBones.clear(); g_probeNames.clear();
    auto addProbe = [&](int i) { if (i >= 0) { g_probeBones.push_back(i); g_probeNames.push_back(g_skel.names[i]); } };
    addProbe(g_skel.R_clav); addProbe(g_skel.R_ua); addProbe(g_skel.R_fa); addProbe(g_skel.R_hand); addProbe(g_skel.torso);
    int twists = 0;
    for (int i = 0; i < g_skel.boneCount && twists < 2; ++i)
        if (g_skel.names[i].find("Twist") != std::string::npos) { addProbe(i); ++twists; }
    g_probeLastQ.assign(g_probeBones.size(), {}); g_probeHas.assign(g_probeBones.size(), 0); g_probeMaxDeg.assign(g_probeBones.size(), 0.f);
    g_probeLastPoint = -1;
    g_refHead = g_skel.head >= 0 ? BoneHeight<HkQsTransform>(g_skel.refPoseCopy, g_skel.parents, g_skel.head) : -1.f;
    if (StanceReferencePlausible(g_refHead))
        LOGI("[skel] reference pose head %.2f m above the feet: a template reads standing from %.2f m, on the ground under %.2f m",
             g_refHead, g_refHead * kStanceStandingRatio, g_refHead * kStanceDownRatio);
    else
        LOGW("[skel] reference pose head reads %.2f m above the feet - implausible, so templates are not judged by height", g_refHead);
    g_standUp.Reset(); g_firstRejectSince = 0;
    if (carry) {
        g_poseTemplate = g_standTemplate; g_templHead = g_standHead; g_templLow = false; g_templWhy = "kept across a reload";
        LOGI("[skel] pose template kept across the reload (same character): standing, head %.2f m above the feet", g_standHead);
    } else {
        g_standTemplate.clear(); g_standHead = g_templHead = -1.f; g_templLow = false; g_templWhy = "";
        CaptureTemplate(chr, "skeleton build", true);   // pose buffer is untouched by us at this point
    }
    return true;
}

// World transforms for all bones from a parent-relative local pose + character root.
struct WorldPose { std::vector<Vec3> pos; std::vector<Quat> q; };
void WorldFromLocal(const Skeleton& s, const std::vector<HkQsTransform>& local, const Vec3& rootPos, const Quat& rootQ, WorldPose& out) {
    out.pos.assign(s.boneCount, {}); out.q.assign(s.boneCount, {});
    for (int i = 0; i < s.boneCount; ++i) {
        const HkQsTransform& t = local[i];
        Vec3 lt{t.t[0], t.t[1], t.t[2]}; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]};
        int p = s.parents[i];
        if (p < 0 || p >= i) { out.q[i] = rootQ * lq; out.pos[i] = rootPos + rootQ.Rotate(lt); }
        else { out.q[i] = out.q[p] * lq; out.pos[i] = out.pos[p] + out.q[p].Rotate(lt); }
    }
}
bool ComputeWorld(const Skeleton& s, HkQsTransform* pose, const Vec3& rootPos, const Quat& rootQ, WorldPose& out, std::vector<HkQsTransform>& local) {
    local.resize(s.boneCount);
    if (!SafeRead(pose, local.data(), sizeof(HkQsTransform) * s.boneCount)) return false;
    WorldFromLocal(s, local, rootPos, rootQ, out);
    return true;
}

// World position of the head bone from the live pose, walking only the parent chain (cheap enough to run from the
// camera hooks). Uses the *live* pose - with CameraAnchor=head nothing of ours moves the root bones any more, so
// what we read is the animation's own head placement, which is exactly what the camera should sit on.
bool HeadBoneWorld(uint8_t* player, const Vec3& rootPos, const Quat& rootQ, Vec3& out) {
    if (!SkeletonCurrent(player)) { if (!BuildSkeleton(player)) return false; }
    if (g_skel.head < 0) return false;
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return false;
    int chain[64]; int n = 0;
    for (int i = g_skel.head; i >= 0 && n < 64; i = (g_skel.parents[i] < i ? g_skel.parents[i] : -1)) chain[n++] = i;
    Vec3 p = rootPos; Quat q = rootQ;
    for (int k = n - 1; k >= 0; --k) {
        HkQsTransform t; if (!SafeRead(&pose[chain[k]], &t, sizeof(t))) return false;
        Vec3 lt{t.t[0], t.t[1], t.t[2]}; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]};
        p = p + q.Rotate(lt); q = (q * lq).Normalized();
    }
    out = p; return true;
}
// Head bone in the TEMPLATE pose, body yaw applied, WITHOUT the body-follows-head shift. PoseMode=vr rewrites the
// live pose every frame including that shift, so anchoring the camera to the live head bone and then moving the
// body onto the camera is a feedback loop that walks the character away a headset-offset per frame. Measuring on
// the template breaks it: this is where the head would be if the body were not following you.
bool HeadBoneTemplateWorld(const Vec3& rootPos, const Quat& rootQ, float bodyYaw, Vec3& out) {
    if ((int)g_poseTemplate.size() != g_skel.boneCount || g_skel.head < 0) return false;
    Vec3 f = rootQ.Rotate({0.f, 0.f, -1.f});
    float facing = WrapPi(std::atan2(f.x, f.z) + Deg2Rad(Cfg().bodyFacingOffsetDeg));
    Quat rotL = Cfg().bodyRotateToHead ? (rootQ.Conj() * Quat::YawQ(WrapPi(bodyYaw - facing)) * rootQ).Normalized() : Quat{};
    int chain[64]; int n = 0;
    for (int i = g_skel.head; i >= 0 && n < 64; i = (g_skel.parents[i] < i ? g_skel.parents[i] : -1)) chain[n++] = i;
    Vec3 p{}; Quat q{};
    for (int k = n - 1; k >= 0; --k) {
        int idx = chain[k];
        const HkQsTransform& t = g_poseTemplate[idx];
        Vec3 lt{t.t[0], t.t[1], t.t[2]}; Quat lq{t.q[0], t.q[1], t.q[2], t.q[3]};
        int pa = g_skel.parents[idx];
        if (pa < 0 || pa >= idx) { lt = rotL.Rotate(lt); lq = (rotL * lq).Normalized(); }
        p = p + q.Rotate(lt); q = (q * lq).Normalized();
    }
    out = rootPos + rootQ.Rotate(p);
    return true;
}
HeadAnchorState g_headAnchor;

// ---------------------------------------------------------------- pose access scan (diagnostic)
// One-shot memory watch: page-protect the live Havok pose buffer and record the RIP of every read/write for
// ~1 second, then restore and log the unique code addresses (module-relative). Finds the upper-body animation
// writer AND the renderer-side consumer that our timed writes keep missing. Armed by PoseWriterScan=true.
namespace posescan {
    std::atomic<bool> g_armed{false};
    uint8_t* g_pageLo = nullptr; size_t g_pageLen = 0;
    uint8_t* g_watchLo = nullptr; uint8_t* g_watchHi = nullptr;   // only accesses in here are recorded
    const char* g_what = "pose";
    DWORD g_oldProt = PAGE_READWRITE;
    struct Hit { void* rip; int isWrite; volatile LONG count; volatile LONG phaseMask; };
    Hit g_hitsArr[64]; volatile LONG g_hitN = 0;
    // Our three write points stamp this each time they run: bit0 pose-hook, bit1 camera-step, bit2 model-pos.
    // Each recorded access ORs in the phases seen since the last write point, so the log shows whether the
    // consumer/writer runs before or after each of our writes - that is the whole ordering question.
    volatile LONG g_phase = 0;
    volatile LONG g_events = 0;
    double g_armedAt = 0;
    bool g_done = false;          // latch: one scan per false->true flip of the config flag
    thread_local bool t_step = false;

    void SetProt(DWORD p) { DWORD old; VirtualProtect(g_pageLo, g_pageLen, p, &old); }
    void Record(void* rip, int isWrite) {
        long ph = g_phase;
        long n = g_hitN;
        for (long i = 0; i < n && i < 64; ++i)
            if (g_hitsArr[i].rip == rip && g_hitsArr[i].isWrite == isWrite) { InterlockedIncrement(&g_hitsArr[i].count); InterlockedOr(&g_hitsArr[i].phaseMask, ph); return; }
        long i = InterlockedIncrement(&g_hitN) - 1;
        if (i < 64) { g_hitsArr[i].rip = rip; g_hitsArr[i].isWrite = isWrite; g_hitsArr[i].count = 1; g_hitsArr[i].phaseMask = ph; }
    }
    // Mark that one of our write points ran. The first pose-hook mark after a camera/model mark begins a new
    // frame, so reset there - that keeps the mask meaning "which of our writes preceded this access, this frame".
    void MarkPhase(int bit) { if (bit == 0 && (g_phase & 6)) g_phase = 1; else InterlockedOr(&g_phase, 1 << bit); }
    LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_SINGLE_STEP) {
            if (!t_step) return EXCEPTION_CONTINUE_SEARCH;
            t_step = false;
            if (g_armed.load(std::memory_order_relaxed)) SetProt(PAGE_NOACCESS);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (code != EXCEPTION_ACCESS_VIOLATION || !g_armed.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
        uint8_t* addr = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        if (addr < g_pageLo || addr >= g_pageLo + g_pageLen) return EXCEPTION_CONTINUE_SEARCH;
        // Only accesses to the bytes we actually care about are recorded; the rest of the page still faults
        // (page granularity) but is let through silently - otherwise a busy page fills the cap instantly.
        if (addr >= g_watchLo && addr < g_watchHi) {
            Record((void*)ep->ContextRecord->Rip, (int)ep->ExceptionRecord->ExceptionInformation[0]);
            InterlockedIncrement(&g_events);
        }
        SetProt(g_oldProt);
        ep->ContextRecord->EFlags |= 0x100;   // trap flag: re-protect right after this one access
        t_step = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    void Install() { AddVectoredExceptionHandler(1, Veh); }
    // Which module an address belongs to, e.g. "RealVR64.dll+0x1234" - the whole point when hunting who reads
    // the camera: a read from R.E.A.L. tells us exactly when it samples, which no game-side RVA can.
    void DescribeAddr(uint8_t* rip, char* out, size_t n) {
        if (rip >= g_base && rip < g_base + 0x4000000) { snprintf(out, n, "eldenring.exe+0x%llx", (unsigned long long)(rip - g_base)); return; }
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &mod) && mod) {
            char path[MAX_PATH]{}; GetModuleFileNameA(mod, path, MAX_PATH);
            const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
            snprintf(out, n, "%s+0x%llx", base, (unsigned long long)(rip - (uint8_t*)mod));
        } else snprintf(out, n, "%p (unknown module)", (void*)rip);
    }
    void Disarm(const char* why) {
        if (!g_armed.exchange(false)) return;
        SetProt(g_oldProt);
        LOGI("[%sscan] done (%s): %ld accesses, %ld unique sites:", g_what, why, g_events, (long)g_hitN);
        for (long i = 0; i < g_hitN && i < 64; ++i) {
            long pm = g_hitsArr[i].phaseMask;
            char ph[16]; snprintf(ph, 16, "%s%s%s", (pm & 1) ? "P" : "-", (pm & 2) ? "C" : "-", (pm & 4) ? "M" : "-");
            char who[160]; DescribeAddr((uint8_t*)g_hitsArr[i].rip, who, sizeof(who));
            LOGI("[%sscan]   %s %s hits=%ld phases[%s]", g_what, g_hitsArr[i].isWrite ? "WRITE" : "READ ", who, g_hitsArr[i].count, ph);
        }
    }
    // Called once per frame with the current pose buffer. Fires automatically ~60 s into gameplay (once per
    // launch), and additionally on every false->true flip of the PoseWriterScan ini flag.
    void Drive(void* pose, size_t bytes) {
        double now = NowSec();
        static double s_first = 0; if (s_first == 0) s_first = now;
        static bool s_autoFired = false;
        if (g_armed.load()) {
            if (g_events >= 400 || now - g_armedAt > 1.0) Disarm(g_events >= 400 ? "event cap" : "time cap");
            return;
        }
        bool fire = false;
        (void)s_autoFired;   // auto-scans retired: they answered the ordering questions and each one is a brief VEH fault storm
        if (Cfg().fpPoseScan) { if (!g_done) fire = true; } else g_done = false;
        if (!fire) return;
        // Alternate targets: the pose buffer (who animates the rig) and the final render camera matrix (who
        // reads the camera, and when - the module names in the report identify R.E.A.L.'s own sample point).
        static int s_target = 0;
        void* watch = pose; size_t wbytes = bytes; const char* what = "pose";
        if (s_target == 1) {
            uint8_t* pc = PersCam1(); if (!pc) { s_target = 0; return; }
            watch = pc + off::Cam_Matrix; wbytes = 64; what = "cam";
        }
        s_target ^= 1;
        if (!watch || !wbytes) return;
        g_hitN = 0; g_events = 0; g_armedAt = now; g_done = true; g_what = what;
        g_watchLo = (uint8_t*)watch; g_watchHi = g_watchLo + wbytes;
        uintptr_t lo = (uintptr_t)watch & ~(uintptr_t)0xFFF;
        uintptr_t hi = ((uintptr_t)watch + wbytes + 0xFFF) & ~(uintptr_t)0xFFF;
        g_pageLo = (uint8_t*)lo; g_pageLen = hi - lo;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(g_pageLo, &mbi, sizeof(mbi))) g_oldProt = mbi.Protect ? mbi.Protect : PAGE_READWRITE;
        LOGI("[%sscan] armed on %p..%p (watching %p..%p)", what, (void*)lo, (void*)hi, (void*)g_watchLo, (void*)g_watchHi);
        g_armed.store(true);
        SetProt(PAGE_NOACCESS);
    }
}

// ---------------------------------------------------------------- VR -> game frame
// ---------------------------------------------------------------- weapon handling
// One pose per rendered frame, shared by repeated hooks and both eyes. The main
// grip stays on the tracked controller; weight is angular resistance about it.
struct InertiaState {
    bool init = false;
    uint32_t frame = 0xFFFFFFFFu;
    Vec3 pos; Quat q;
    double t = 0;
    TwoHandAimState twoHandAim;
    float arc = 0.f, tipSpeed = 0.f, handSpeed = 0.f;
    double stamp = 0;
};
InertiaState g_inertia[2];
std::mutex g_inertiaMtx;

void InertiaStep(int h, Vec3& p, Quat& q, double now,
                 bool twoHandAim, const Vec3& handLine, Vec3 shaftAxis) {
    const Config& c = Cfg();
    std::lock_guard<std::mutex> lock(g_inertiaMtx);
    InertiaState& s = g_inertia[h];
    const uint32_t fr = g_frame;
    if (s.init && s.frame == fr) { p = s.pos; q = s.q; return; }
    s.frame = fr;
    const float dt = s.init ? (float)(now - s.t) : 0.f;
    const bool goodDt = dt >= 0.001f && dt <= 0.1f && (p - s.pos).Length() <= 1.5f;
    s.t = now;
    const Quat wrist = q;
    q = s.twoHandAim.Update(q, handLine, twoHandAim, goodDt ? dt : 0.f, shaftAxis);
    const float reach = CombatHandReach(h);
    const Vec3 previousTip = s.pos * c.handScale + s.q.Rotate(shaftAxis) * reach;
    // Kinetic M2: the B&S-style sword. The grip itself is simulated (mass, limited arm strength), so it
    // moves the position as well as the angle; when it is off or unavailable ERVR's angular lag runs as before.
    bool simulated = false;
    if (goodDt && kinetic::SwordRigActive()) {
        const bool twoArms = CombatTwoHandWeaponHand() == h;
        simulated = kinetic::SwordRigStep(h, p, q, dt, CombatHandWeight(h), reach, CombatHandCom(h),
                                          twoArms ? c.inertiaTwoHandTorque : 1.f, shaftAxis);
    } else if (!goodDt) {
        kinetic::SwordRigReset(h);
    }
    if (!simulated && c.kineticInertia && goodDt) {
        const bool twoArms = CombatTwoHandWeaponHand() == h;
        q = StepWeaponOrientation(s.q, q, shaftAxis, dt, CombatHandWeight(h), reach,
                                  CombatHandCom(h), c.kineticAccelWeight, c.inertiaComRef,
                                  c.inertiaSpringHz, c.inertiaAngAccel,
                                  twoArms ? c.inertiaTwoHandTorque : 1.f,
                                  c.inertiaBrake, c.inertiaMaxLagAngle);
    }
    // Composing two bending corrections can introduce a twist even if each
    // individually has none. Keep the final rendered edge tied to the wrist.
    q = PreserveWeaponWristTwist(q, wrist, shaftAxis);
    // Use the displacement actually rendered, including positional two-hand
    // steering. An internal spring velocity (or raw controller forward) did not
    // describe the visible blade after lag clamps and could miss real contacts.
    const Vec3 tip = p * c.handScale + q.Rotate(shaftAxis) * reach;
    s.tipSpeed = goodDt ? (tip - previousTip).Length() / dt : 0.f;
    s.handSpeed = goodDt ? (p - s.pos).Length() * c.handScale / dt : 0.f;
    const float offSpeed = c.kineticEnabled && c.kineticContinuous ? c.contactOffSpeed : c.swingOffSpeed;
    if (goodDt && s.tipSpeed >= offSpeed) s.arc += s.tipSpeed * dt;
    else s.arc = 0.f;
    s.pos = p; s.q = q; s.init = true; s.stamp = now;
}

struct FrameInfo {
    bool valid = false;
    bool lockedOn = false; // a real enemy is locked on
    bool riding = false;  // mounting/riding/dismounting: native seated body and physics
    bool seated = false;  // riding proper: the mount animation is over, the dismount has not begun
    bool seatPose = false;// seated and the rider is the captured seated pose (PoseMode=vr), upright in its own frame
    bool latePost = false;// built in the player's model-pos POST of pass 1: the frame's final camera write
    Vec3 rideCamShift{};  // seated: how far the smoothed view sits off the rider's own anchor - the body goes with it
    bool capturePose = false; // menu/exit poses cannot replace the template; this never gates rendering
    uint8_t* mount = nullptr; // validated, fully mounted Torrent (null during transitions)
    Vec3 root; Quat rootQ; // character root the whole frame is built on (model matrix, physics fallback)
    Vec3 anchor;         // eye anchor in game world (no HMD offset)
    float baseYaw = 0;   // game camera yaw the HMD is relative to
    float basePitch = 0; // game camera pitch (from chr cam)
    bool  headValid = false; // HMD tracking is live (otherwise eye == anchor and there is no stereo)
    Quat  headQ;         // HMD orientation in game frame (relative to tracking origin)
    Vec3  headP;         // HMD position in game frame (relative to the *tracking* origin)
    Vec3  headRel;       // HMD position relative to our own play-space origin (drift removed)
    Vec3  eye;           // where the eye is rendered: anchor + R(baseYaw)*headRel
    float chrYaw = 0;    // physics facing of the character (what the body actually points at)
    float headYaw = 0;   // HMD yaw relative to the tracking origin (game convention, positive = right)
    float bodyYaw = 0;   // absolute yaw the body is driven toward: facing, torso twist, elbow poles
    Vec3  handP[2]; Quat handQ[2]; bool handValid[2]{};
};

// ---------------------------------------------------------------- cast aim
// The ray a casting hand aims along, in the game world: origin = the controller (placed exactly as the arm
// solver places the hand, so it matches the rendered weapon), direction = the controller's pointing axis
// (CastAimAxis=aim) or up its handle (=handle, the staff's own direction), tilted by CastAimPitchOffsetDeg.
bool CastAimRay(const FrameInfo& f, int h, Vec3& origin, Vec3& dir) {
    const Config& c = Cfg();
    if (h < 0 || h > 1 || !f.handValid[h]) return false;
    const Quat baseQ = Quat::YawQ(f.baseYaw);
    float handRotX = c.handRotOffX;
    DebugKeysHandRotX(&handRotX);
    const Quat calib = Quat::FromEulerYXZ(Deg2Rad(c.handRotOffY), Deg2Rad(handRotX), Deg2Rad(c.handRotOffZ));
    const Quat cq = (baseQ * f.handQ[h] * calib).Normalized();
    const Vec3 cf = cq.Rotate({0, 0, 1}), cu = cq.Rotate({0, 1, 0});   // controller forward / up (game frame)
    Vec3 d = c.castAimAxis == "handle" ? cu : cf;
    if (c.castAimPitchOffsetDeg != 0.f) {
        const Vec3 axis = d.Cross(Vec3{0, 1, 0});   // rotating about (dir x up) by a positive angle tilts the ray UP
        if (axis.Length() > 1e-3f) d = Quat::AxisAngle(axis, Deg2Rad(c.castAimPitchOffsetDeg)).Rotate(d);
    }
    Vec3 p = f.anchor + baseQ.Rotate(f.handP[h] * c.handScale) - cf * c.handBackOffset;
    if (c.handOrigin == "head") p = p + (f.eye - f.anchor);   // hands are head-relative: re-add where the view actually is
    origin = p; dir = d.Normalized();
    return dir.Length() > 0.5f;
}
// Camera-style basis (right, up) around a forward vector, same handedness as CamBasis.
void AimBasis(const Vec3& fwd, float fallbackYaw, Vec3& right, Vec3& up) {
    right = Vec3{0, 1, 0}.Cross(fwd);
    if (right.Length() < 1e-3f) right = {std::cos(fallbackYaw), 0.f, -std::sin(fallbackYaw)};   // straight up/down: keep the body's right
    right = right.Normalized();
    up = fwd.Cross(right).Normalized();
}

// ---------------------------------------------------------------- play-space origin
// The runtime hands us head/hand positions relative to *its* origin (wherever you were standing when the
// runtime last recentred). Everything downstream - the camera, the arm targets, the skeleton shift - is then
// rotated by the camera yaw, so a metre of "I am standing off to the side of my play space" turns into the
// camera swinging around the character every time you turn with the stick, and into a body that is
// permanently shoved sideways. We therefore keep our own origin that slowly follows your head: fast motion
// (leaning, ducking, a step) still comes through, slow drift is absorbed.
Vec3 g_psOrigin{};
bool g_psInit = false;
double g_psLast = 0;
std::mutex g_psMtx;
std::atomic<uint32_t> g_psFrame{0xFFFFFFFFu};

void ResetRecenter() { std::lock_guard<std::mutex> g(g_psMtx); g_psInit = false; g_psOrigin = {}; }

// Returns the play-space origin to measure the head against.
Vec3 UpdateRecenter(const Vec3& headP, double now) {
    const Config& c = Cfg();
    std::lock_guard<std::mutex> g(g_psMtx);
    if (!c.fpRecenter) { g_psOrigin = {}; g_psInit = false; return g_psOrigin; }
    if (!g_psInit) { g_psOrigin = headP; g_psInit = true; g_psLast = now; return g_psOrigin; }
    Vec3 d = headP - g_psOrigin;
    float horiz = std::sqrt(d.x * d.x + d.z * d.z);
    // A large residual means the runtime's origin is nowhere near where you actually stand - a room-scale setup
    // recentred at the other end of the room, say (measured: 1.2 m). Easing that away over RecenterSeconds is
    // what put the body a clamped half-metre from the camera and then slid it back for seconds afterwards, and
    // it is why recentring in the runtime "fixed" everything. Snap instead: the same thing the runtime's own
    // recenter does, and after it the residual is only ever your own leaning.
    if (c.fpRecenterSnapRadius > 1e-3f && (horiz > c.fpRecenterSnapRadius || std::fabs(d.y) > c.fpRecenterSnapRadius)) {
        g_psOrigin.x = headP.x; g_psOrigin.z = headP.z;
        if (c.fpRecenterHeight) g_psOrigin.y = headP.y;
        g_psLast = now;
        LOGD("[fp] play-space origin snapped to the head (residual was %.2f m horizontal, %.2f m vertical)", horiz, d.y);
        return g_psOrigin;
    }
    // Advance the smoothing exactly once per rendered frame. BuildFrame runs from several hooks and the game
    // calls those from a pool of worker threads (8 seen in one session), so letting every call apply its own
    // exponential step made the origin converge at a rate that depended on how many threads happened to call in.
    uint32_t fr = g_frame;
    if (g_psFrame.exchange(fr) != fr) {
        float dt = Clamp((float)(now - g_psLast), 0.f, 0.25f);
        g_psLast = now;
        float k = c.fpRecenterSeconds > 1e-3f ? 1.f - std::exp(-dt / c.fpRecenterSeconds) : 1.f;
        g_psOrigin.x += d.x * k; g_psOrigin.z += d.z * k;
        if (c.fpRecenterHeight) g_psOrigin.y += d.y * k;
        // Hard limit: never let the residual grow past the allowed radius (walking across the room).
        d = headP - g_psOrigin;
        horiz = std::sqrt(d.x * d.x + d.z * d.z);
        if (c.fpRecenterMaxRadius > 1e-3f && horiz > c.fpRecenterMaxRadius) {
            float pull = 1.f - c.fpRecenterMaxRadius / horiz;
            g_psOrigin.x += d.x * pull; g_psOrigin.z += d.z * pull;
        }
        if (c.fpRecenterHeight && c.fpRecenterMaxRadius > 1e-3f) g_psOrigin.y += d.y - Clamp(d.y, -c.fpRecenterMaxRadius, c.fpRecenterMaxRadius);
    }
    return g_psOrigin;
}

// ---------------------------------------------------------------- view yaw
// CameraYawSource=ervr: the view yaw is ERVR's own state, integrated from the turn stick by virtual_pad.
// Reading it back out of the game's chr-cam (the "game" setting) means inheriting everything that moves that
// camera - the follow cam drifting behind you, the lock-on logic framing a target, R.E.A.L. touching it - and
// every one of those shows up as the view sliding off the character.
std::atomic<float> g_camYaw{0.f};
std::atomic<float> g_chrYaw{0.f};
// Yaw applied by the stick since the last frame, still to be pivoted about the head: rotating the view must
// spin the world around YOUR HEAD, not around the body anchor - otherwise standing anywhere but dead centre
// of the play space makes the eye swing sideways on every turn (needs Recenter=true to persist the shift).
std::atomic<float> g_pendingYawPivot{0.f};
// Body yaw: where the character body is turned toward. BodyYaw=head makes it chase the headset yaw (with a time
// constant / deadzone), so shoulders and torso stay under and behind your head when you turn your real body;
// BodyYaw=camera keeps it on the stick yaw (the old behaviour: only the head moves).
std::atomic<float> g_bodyYaw{0.f}, g_headYaw{0.f};
std::atomic<float> g_bodyStickYaw{0.f};   // BodyYaw=stick: integrated from the right stick, absolute
std::atomic<bool>  g_headYawValid{false};
bool g_bodyYawInit = false; double g_bodyYawLast = 0;

float UpdateBodyYaw(float target, double now, bool instant) {
    const Config& c = Cfg();
    if (!g_bodyYawInit) { g_bodyYaw.store(target); g_bodyYawInit = true; g_bodyYawLast = now; return target; }
    if (instant) { g_bodyYaw.store(target); g_bodyYawLast = now; return target; }   // BodyYaw=camera: no lag behind the view
    float dt = Clamp((float)(now - g_bodyYawLast), 0.f, 0.25f); g_bodyYawLast = now;
    float cur = g_bodyYaw.load();
    float d = WrapPi(target - cur);
    float dz = Deg2Rad(std::fmax(0.f, c.bodyYawDeadzoneDeg));
    if (std::fabs(d) <= dz) return cur;
    d -= (d > 0 ? dz : -dz);                     // only the part outside the deadzone pulls
    float k = c.bodyYawFollowSeconds > 1e-3f ? 1.f - std::exp(-dt / c.bodyYawFollowSeconds) : 1.f;
    cur = WrapPi(cur + d * k);
    g_bodyYaw.store(cur);
    return cur;
}
std::atomic<bool>  g_moveActive{false}, g_moveLocked{false}, g_moveRiding{false};
bool g_camYawInit = false;

float GameCamYaw() {
    uint8_t* cc = ChrCam();
    float fwd[4] = {0, 0, 1, 0};
    if (cc) SafeRead(cc + off::Cam_Matrix + 32, fwd, 16);
    return std::atan2(fwd[0], fwd[2]);
}

// ERVR now owns rendering, so it is unconditionally the sole owner of where the eye sits.
// (This used to negotiate with R.E.A.L. over who applied the HMD offset.)

void StabilizeRideCamera(FrameInfo& f);
void BuildCameraAnchor(uint8_t* player, FrameInfo& f) {
    const Config& c = Cfg();
    const Vec3 root = f.root; const Quat rootQ = f.rootQ;
    f.anchor = root + Vec3{0.f, c.fpEyeHeight, 0.f};
    // CameraAnchor=head: the camera rides on the animation's head bone (what a first-person mod does), so the neck,
    // shoulders and arms are under the view whatever the animation does - run lean, bob, turn - instead of us
    // dragging the skeleton back under a root-anchored camera every tick and losing that fight on some frames.
    // Horizontal: the head bone (low-passed by CameraAnchorSmoothing). Vertical: EyeHeight above the root plus
    // CameraBob of the head bone's own vertical excursion (0 = no bob at all).
    if (f.riding || c.fpCameraAnchor == "head") {
        Vec3 hb;
        // PoseMode=vr: measure on the template (see HeadBoneTemplateWorld) so the anchor is independent of the
        // shift we are about to apply; otherwise fall back to the live pose. Seated on Torrent: the seated
        // template's head in the upright seat frame - never the gallop or jump animation's head.
        bool haveHb = false;
        if (f.seatPose) { hb = root + rootQ.Rotate(g_seatHead); haveHb = true; }
        if (!f.riding && c.fpPoseMode == "vr" && SkeletonCurrent(player))
            haveHb = HeadBoneTemplateWorld(root, rootQ, g_bodyYaw.load(), hb);
        const bool templatePose = haveHb;
        if (!haveHb) haveHb = HeadBoneWorld(player, root, rootQ, hb);
        if (haveHb) {
            // EyeHeight belongs to the STANDING body. After a reset the anchor would otherwise calibrate from the
            // first head it sees - a live pose mid-reload, or a crouch template - and every later standing frame
            // would sit that difference too high or too low.
            if (!f.riding && c.fpPoseMode == "vr" && g_headAnchor.restHeight < 0.f && g_standHead > 0.f)
                g_headAnchor.restHeight = g_standHead;
            f.anchor = g_headAnchor.Update(root, hb, c.fpEyeHeight, c.fpCameraBob,
                                          c.fpCameraAnchorSmoothing, templatePose, NowSec(), g_frame, f.riding);
        }
        if (f.riding) StabilizeRideCamera(f);
    }
}

// Gallop bounce out of the view (RideCameraStabilizer): the anchor is smoothed in Torrent's own frame. Only the
// frame's final camera write moves the smoothing - the model-pos post with CameraAfterModelPos - so the earlier
// writes of a frame, built on last frame's rider matrix, cannot drag it. The seated body is moved by the same
// offset (rideCamShift): a view smoothed over a body that kept the saddle's bounce let the body rise into it on
// every jump and stride ("horse animations affect the body but not the camera", 2026-11-02). Now the horse
// moves under a rider and view that move together.
void StabilizeRideCamera(FrameInfo& f) {
    const Config& c = Cfg();
    Vec3 mp; Quat mq;
    f.rideCamShift = {};
    std::lock_guard<std::mutex> g(g_rideStabMtx);
    if (!f.mount || c.rideCameraSmoothing <= 0.f || !PlayerRoot(f.mount, mp, mq)) { g_rideStab.Reset(); return; }
    const bool advance = FramePumpCurrentPass() != 2 && (f.latePost || !c.fpCameraAfterModelPos);
    const Vec3 raw = f.anchor;
    f.anchor = g_rideStab.Apply(f.anchor, mp, HeadingQ(mq), NowSec(), advance, c.rideCameraSmoothing, c.rideCameraMaxDeviation);
    if (f.seatPose) f.rideCamShift = f.anchor - raw;
    if (advance) { std::lock_guard<std::mutex> s(g_rideStats.mtx); g_rideStats.camDev = std::max(g_rideStats.camDev, g_rideStab.deviation); }
}

// Free look on horseback (RideHeadFollow): within RideFreeLookDeg of the way you face, looking around neither steers
// Torrent nor turns it. Beyond it the excess moves that forward direction - the move stick steers along it
// (GameERGetMoveBasis), not along the head - and with BodyYaw=head turns Torrent and the rider like the right stick
// does, at BodyYawFollowSeconds. With Forward=head the stick used to follow the head 1:1, so every glance steered
// the horse (2026-11-02). Locked on, the game aims; the second eye never turns anything.
std::atomic<float> g_rideForward{0.f};      // the followed head yaw: forward on the horse, head-yaw convention
std::atomic<bool>  g_rideForwardValid{false};
void RideFollowHead(const FrameInfo& f, bool headValid) {
    const Config& c = Cfg();
    std::lock_guard<std::mutex> g(g_rideFollowMtx);
    if (!headValid || !f.riding || f.lockedOn || c.rideFreeLookDeg < 0.f) {
        g_rideFollow.Reset(); g_rideForwardValid.store(false);
        return;
    }
    if (FramePumpCurrentPass() == 2) return;
    const float turn = g_rideFollow.Step(f.headYaw, NowSec(), g_frame, Deg2Rad(c.rideFreeLookDeg), c.bodyYawFollowSeconds);
    g_rideForward.store(g_rideFollow.followed); g_rideForwardValid.store(g_rideFollow.init);
    if (turn == 0.f || !f.mount || !f.seated || !c.rideTurnsWithHead || c.bodyYawSource != "head") return;
    g_mountedTurns.Add(turn);
    g_rideStats.Turn(g_rideStats.physical, turn);
}

bool BuildFrame(uint8_t* player, FrameInfo& f, bool latePost = false) {
    const Config& c = Cfg();
    Vec3 root; Quat rootQ;
    const int pass = FramePumpCurrentPass();
    const RideState ride = ReadRideState(player, SafeRead);
    f.riding = ride.NativeBody();
    f.seated = ride.valid && ride.seated;
    f.latePost = latePost && pass != 2;
    const uintptr_t poseOwner = reinterpret_cast<uintptr_t>(player);
    g_poseCaptureGuard.Update(poseOwner, CombatMenuActive(), NowSec(), pass != 2);
    f.capturePose = !f.riding && g_poseCaptureGuard.CanCapture(poseOwner);
    TorrentLookup torrent;
    if (ride.valid && ride.active) torrent = CurrentTorrentLookup(player);
    f.mount = torrent.mount;
    if (pass != 2) { LogTorrentLookup(ride.valid && ride.active, torrent); g_mountedTurns.SetMount(f.mount); }
    g_seatedFrame.Observe(player, f.seated, NowSec(), pass != 2);
    static std::atomic<int> s_rideMode{-1};
    const int rideMode = !ride.valid ? 2 : (ride.active ? 1 : 0);
    if (s_rideMode.exchange(rideMode) != rideMode)
        LOGI("[ride] %s", rideMode == 2 ? "state unavailable; preserving native body/facing" :
             (rideMode == 1 ? "mount/riding/dismount: native pair facing; the seated pose takes over once seated" : "on foot: configured VR body/facing restored"));
    f.seatPose = f.riding && f.seated && c.fpPoseMode == "vr" && g_seatedFrame.Seated(player) && SeatTemplateReady();
    if (f.riding) {
        // The attached rider's rendered transform can differ from its physics position AND rotation: the rider is
        // built on its model matrix. Seated, upright about the pelvis (UprightSeatRoot). The second eye's pass solves
        // in the first pass's placement, which it then re-expresses in its own rebuild (RiderPassHold).
        Vec3 mPos; Quat mRot;
        bool held = false;
        if (f.seatPose && pass == 2) {
            std::lock_guard<std::mutex> g(g_riderHoldMtx);
            if ((held = g_riderHold.Held(RiderHoldPair(), NowSec()))) { mPos = g_riderHold.pos; mRot = g_riderHold.rot; }
        }
        if (!held && !ModelMatrixRoot(player, mPos, mRot) && !PlayerRoot(player, mPos, mRot)) return false;
        if (f.seatPose) UprightSeatRoot(mPos, mRot, g_seatPelvis, root, rootQ);
        else { root = mPos; rootQ = mRot; }
    } else if (!SkeletonRoot(player, root, rootQ)) return false;
    f.root = root; f.rootQ = rootQ;
    BuildCameraAnchor(player, f);

    float fwd[4] = {0, 0, 1, 0};
    if (uint8_t* cc = ChrCam()) SafeRead(cc + off::Cam_Matrix + 32, fwd, 16);
    f.basePitch = std::asin(Clamp(-fwd[1], -1.f, 1.f));

    int64_t enemy = -1; RD(player + off::PI_LockedEnemy, enemy);
    f.lockedOn = enemy != -1 && enemy != 0;

    // Which way the character actually faces right now. Read from the physics module: the model matrix carries
    // the animation's root rotation on top and wobbles while moving.
    // (physics forward is local -Z: with the character facing our lock-on point, +Z read back 180 degrees off)
    { Vec3 pp; Quat pq; Quat fq = PlayerRoot(player, pp, pq) ? pq : rootQ; Vec3 chrF = fq.Rotate({0, 0, -1}); f.chrYaw = std::atan2(chrF.x, chrF.z); }

    if (c.fpCameraYawSource == "game") {
        f.baseYaw = std::atan2(fwd[0], fwd[2]);
    } else {
        // First frame: start the view where the character faces. The game's chr-cam is not usable for this - during
        // loading it is an identity matrix (yaw 0), and initialising from it snapped the view to world north and
        // then FaceCamera turned the whole character to match ("body starts rotated").
        if (!g_camYawInit) { g_camYaw.store(f.chrYaw); g_camYawInit = true; }
        // Locked on: the game picked the direction, follow the point it makes the character face.
        if (f.lockedOn) {
            float t[4];
            if (SafeRead(player + off::Chr_LockOnTarget, t, 16)) {
                float dx = t[0] - root.x, dz = t[2] - root.z;
                if (dx * dx + dz * dz > 0.01f) g_camYaw.store(std::atan2(dx, dz));
            }
        }
        f.baseYaw = g_camYaw.load();
    }
    g_chrYaw.store(f.chrYaw);
    g_moveLocked.store(f.lockedOn);
    g_moveRiding.store(f.riding);
    g_moveActive.store(true);

    VRTracking tr; VRInputState in; bool vr;
    LatchVRForCycle(tr, in, vr);
    f.bodyYaw = f.riding ? f.chrYaw : f.baseYaw;
    if (vr && tr.head.valid) {
        Quat hq = ToGame(tr.head.rot); Vec3 hp = ToGame(tr.head.pos);
        // Pivot stick turns about the head: baseYaw already contains the new yaw, so shift the play-space origin
        // such that R(baseYaw)*headRel - and with it the eye - is unchanged by the turn itself.
        float dy = g_pendingYawPivot.exchange(0.f);
        if (dy != 0.f) { g_psOrigin = hp - Quat::YawQ(-dy).Rotate(hp - g_psOrigin); g_psInit = true; }
        Vec3 psOrigin = UpdateRecenter(hp, NowSec());
        f.headValid = true;
        f.headQ = hq; f.headP = hp; f.headRel = hp - psOrigin;
        f.eye = f.anchor + Quat::YawQ(f.baseYaw).Rotate(f.headRel);
        // Pitch-robust head yaw: atan2 of the forward vector alone collapses when you pitch the head toward
        // straight down/up (the horizontal projection shrinks to noise and the yaw swings wildly - with
        // BodyYaw=head that spun the whole rig while you looked at your own body). Blend in the head's up
        // vector scaled by -forward.y: level it contributes nothing, pitched down the up vector's horizontal
        // part points where the face points (pitched up, its negation does), so the sum stays a full-length
        // facing vector at every pitch.
        {
            Vec3 hf = hq.Rotate({0, 0, 1}), hu = hq.Rotate({0, 1, 0});
            f.headYaw = std::atan2(hf.x - hf.y * hu.x, hf.z - hf.y * hu.z);
        }
        g_headYaw.store(f.headYaw); g_headYawValid.store(true);
        RideFollowHead(f, true);
        for (int h = 0; h < 2; ++h) {
            const Pose& hand = tr.hand[h];
            f.handValid[h] = hand.valid && in.controllerConnected[h];
            if (!f.handValid[h]) continue;
            Vec3 p = ToGame(hand.pos); Quat q = ToGame(hand.rot);
            float handRotX = c.handRotOffX;
            DebugKeysHandRotX(&handRotX);
            const Quat calibration = Quat::FromEulerYXZ(Deg2Rad(c.handRotOffY), Deg2Rad(handRotX), Deg2Rad(c.handRotOffZ));
            const Vec3 shaftAxis = WeaponShaftAxis(calibration, c.twoHandAxis != "forward");
            bool twoHandAim = false; Vec3 handLine;
            if (c.twoHandAim) {
                int pin = 0, wep = 0; float along = 0.f; bool handleAxis = true;
                const Pose& other = tr.hand[1 - h];
                if (CombatTwoHandPin(&pin, &wep, &along, &handleAxis) && wep == h && other.valid && in.controllerConnected[1 - h]) {
                    twoHandAim = true; handLine = p - ToGame(other.pos);
                }
            }
            { Vec3 pPlay = p - psOrigin; InertiaStep(h, pPlay, q, NowSec(), twoHandAim, handLine, shaftAxis); p = pPlay + psOrigin; }
            // All three origins end up drift-free: "head" is head-relative by construction (the head offset is
            // added back at the target), the other two are measured from our own play-space origin.
            if (c.handOrigin == "head") p = p - hp;
            else if (c.handOrigin == "floor") p = p - Vec3{psOrigin.x, c.fpEyeHeight, psOrigin.z};
            else p = p - psOrigin;
            f.handP[h] = p; f.handQ[h] = q;
        }
        // Body: chase the head yaw (default) so shoulders/torso follow your real body; locked on, the game aims it.
        // A live cast (CastAimTurnBody) turns it toward where the casting hand points instead - the game's own
        // turn-to-target, done by us because the physics facing is pinned to BodyYaw.
        float want; bool instant = f.riding || f.lockedOn || c.bodyYawSource == "camera";
        if (!g_bodyYawInit) g_bodyStickYaw.store(f.baseYaw);   // start facing the view, not world yaw 0
        if (f.riding) want = f.chrYaw;
        else if (instant) want = f.baseYaw;
        else if (c.bodyYawSource == "head") want = WrapPi(f.baseYaw + f.headYaw);
        else want = g_bodyStickYaw.load();                       // stick: absolute, right stick turns it
        if (!f.riding && c.castAim && c.castAimTurnBody && !f.lockedOn) {
            bool casting = false; const int ah = CombatCastAimHand(&casting); Vec3 o, d;
            if (casting && CastAimRay(f, ah, o, d) && d.x * d.x + d.z * d.z > 0.05f) { want = std::atan2(d.x, d.z); instant = false; }
        }
        f.bodyYaw = UpdateBodyYaw(want, NowSec(), instant);
    }
    if (!(vr && tr.head.valid)) { f.eye = f.anchor; g_headYawValid.store(false); RideFollowHead(f, false); }
    f.valid = true;
    return true;
}

MovementFacingState ReadMovementFacing(uint8_t* player) {
    MovementFacingState s;
    uint8_t* mods = Deref(player + off::Chr_Modules); if (!mods) return s;
    uint8_t* ar = Deref(mods + 0x80);   // CSChrActionRequestModule
    uint8_t* af = Deref(mods + 0x08);   // CSChrActionFlagModule
    if (!ar || !af || Deref(ar + 8) != player || Deref(af + 8) != player || Deref(ar + 0x48) != ar) return s;
    uint64_t ready = 0, queued = 0;
    uint32_t tae = 0; uint8_t queueMode = 0;
    if (!SafeRead(ar + 0x10, &s.requests, 8) || !SafeRead(ar + 0x30, &ready, 8) ||
        !SafeRead(ar + 0x38, &queued, 8) || !SafeRead(ar + 0xFC, &s.movement, 4) ||
        !SafeRead(ar + 0x100, &tae, 4) || !SafeRead(ar + 0x138, &queueMode, 1) ||
        !SafeRead(af + 0x40, &s.modifiers, 8) || queueMode > 1) return s;
    s.requests |= ready | queued;
    ActionQueueSnapshot cancels;
    int32_t currentTae = -1, overrideTae = -1;
    if (queueMode) {
        if (!SafeRead(ar + 0x90, &currentTae, 4) || !SafeRead(ar + 0x134, &overrideTae, 4)) return s;
        if (overrideTae >= 0) currentTae = overrideTae;
        cancels = ReadActionQueue(ar + 0x70, SafeRead);
        if (!cancels.valid) return s;
    }
    s.movementCancelable = MovementCancelOpen(tae, queueMode != 0, currentTae, cancels);
    s.valid = true;
    return s;
}

bool CanPinPlayerFacing(uint8_t* player) {
    const MoveIntent mi = VirtualPadGetMove();
    return !CombatMenuOpen() && CanPinMovementFacing(ReadMovementFacing(player), mi.valid && mi.sprint);
}

// ---------------------------------------------------------------- camera
// Keep the body facing the camera direction (strafe/lock-on mode) unless a real enemy is locked on,
// otherwise walking backwards/sideways turns the torso away and the arms cannot reach the hands.
//
// The fake lock-on point is measured from the character root, flat, and far away on purpose. The game's
// lock-on camera keeps nudging its own yaw to frame this point, and that yaw is what we read back as
// baseYaw - so any lateral error between the game's camera position and ours feeds straight back into our
// facing. At the old 10 m the loop had enough gain that simply strafing (the game's follow cam lags behind
// you) rotated you a little further every frame; at fpLockTargetDistance the same lag is a rounding error.
//
// Cast aim: with a casting hand (aimO/aimD = its ray) the fake target sits down THAT ray - pitch included, so
// an unlocked bullet aimed at the target flies where the hand points - and, in strafe mode with
// CastAimForceLock, the lock-on state is forced for the press window (`casting`) like erfps2 does.
void ApplyFaceCamera(uint8_t* player, const Vec3& root, float bodyYaw, const Vec3* aimO, const Vec3* aimD, bool casting, bool riding) {
    const Config& c = Cfg();
    if (riding) return;   // Torrent owns rider facing, movement mode and lock flags
    if (!c.fpFaceCamera || c.fpFaceCameraMode == "off") return;
    // Facing/lock-state choke point: while the world is dying, do not pin a dying body's
    // physics orientation or write lock flags - the ragdoll/death anim owns the character.
    if (WorldDyingNow()) return;
    int64_t enemy = -1; RD(player + off::PI_LockedEnemy, enemy);
    bool realLock = enemy != -1 && enemy != 0;
    if (uint8_t* ctrl = Deref(player + off::Chr_ChrCtrl)) WR(ctrl + off::Ctrl_IsUnlocked, (uint8_t)0);
    const float d = std::fmax(1.f, c.fpLockTargetDistance);
    // The ray steers the target only while a cast is LIVE: in lock mode the game turns the character toward the
    // target while moving, and a target that follows a seal held at your side spun the body around (2026-09-04).
    const bool handAim = aimO && aimD && casting;
    Vec3 tgt;
    if (handAim) tgt = *aimO + *aimD * d;
    else { Vec3 flat{std::sin(bodyYaw), 0.f, std::cos(bodyYaw)}; tgt = root + Vec3{0.f, c.fpEyeHeight, 0.f} + flat * d; }
    const float t4[4] = {tgt.x, tgt.y, tgt.z, 1.f};
    if (c.fpFaceCameraMode == "strafe") {
        // Strafe movement WITHOUT the game's lock-on state. Forcing Chr_IsLockedOn=1 (the old "lock" mode)
        // engages the game's lock-on *camera* - a camera writer we do not hook - and that camera orbits the
        // character while strafing and swallows our stick turns. Ctrl_IsUnlocked=0 alone keeps the movement in
        // strafe animations; the rig facing is ours anyway (BodyRotate / PoseMode=vr).
        if (!realLock) {
            const bool forceLock = handAim && casting && c.castAimForceLock;
            WR(player + off::Chr_IsLockedOn, (uint8_t)(forceLock ? 1 : 0));   // clear a flag a previous "lock" mode may have left (or force it for the cast)
            if (handAim) SafeWrite(player + off::Chr_LockOnTarget, t4, 16);   // the ray's far point: only consumed while locked, harmless otherwise
            // Pin the physics facing to BodyYaw. The game decides forward-run vs strafe from the angle between
            // the move direction and this facing: without the pin, physically turning your head while walking
            // (Forward=head rotates the stick) reads as a sideways stick and you side-step at strafe speed
            // instead of just walking the new way. Dash and unlocked rolls need the game's own physics
            // turn toward the movement stick. Do not overwrite it, including after B is released and after
            // invulnerability ends. The rendered skeleton still follows BodyYaw independently.
            if (c.fpPinBodyFacing && CanPinPlayerFacing(player)) {
                if (uint8_t* ph = Physics(player)) {
                    Quat q = Quat::YawQ(WrapPi(bodyYaw + 3.14159265f));   // physics forward is local -Z
                    float r4[4] = {q.x, q.y, q.z, q.w};
                    SafeWrite(ph + off::Phys_Orientation, r4, 16);
                    SafeWrite(ph + off::Phys_InterpOrient, r4, 16);
                }
            }
        }
        return;
    }
    WR(player + off::Chr_IsLockedOn, (uint8_t)1);
    if (realLock) return;
    SafeWrite(player + off::Chr_LockOnTarget, t4, 16);
}
// The re-assert sites (model-pos hook, Havok body): same facing, same cast-aim ray as the camera step, so the
// lock target and flag never flip between a hand ray and the flat point within one frame.
void ReassertFaceCamera(uint8_t* player, const FrameInfo& f) {
    const Config& c = Cfg();
    Vec3 aimO, aimD; bool casting = false, handAim = false;
    if (c.castAim && !f.lockedOn) { const int ah = CombatCastAimHand(&casting); handAim = ah >= 0 && CastAimRay(f, ah, aimO, aimD); }
    ApplyFaceCamera(player, f.root, f.bodyYaw, handAim ? &aimO : nullptr, handAim ? &aimD : nullptr, handAim && casting, f.riding);
}

// One eye per game frame, latched once so that the several UpdateCamera calls a frame makes
// (camera step, camera finalize, post-model-pos) all agree on which eye they are drawing.
struct EyeLatch { bool active = false; StereoEyeRequest req; };
EyeLatch g_eyeLatch;
std::atomic<uint32_t> g_eyeLatchFrame{0xFFFFFFFFu};

const EyeLatch& LatchEyeForFrame() {
    uint32_t fr = g_frame;
    if (g_eyeLatchFrame.exchange(fr) != fr) {
        StereoEyeRequest r;
        g_eyeLatch.active = StereoTakeEyeRequest(r);
        g_eyeLatch.req = r;
    }
    return g_eyeLatch;
}

// Full stereo: everything the draw-only second pass needs to place the OTHER eye's camera.
// Written by UpdateCamera during the real pass; consumed between passes by the frame pump
// (the camera step is gated in pass 2, so the pump's write is what that pass renders from).
struct SecondEyeStash {
    std::mutex mtx;
    bool  valid = false;
    bool  haveHeadBasis = false;
    Quat  qHead;                 // head orientation, game convention (basis when haveHeadBasis)
    Vec3  r, u, fw;              // fallback basis when !haveHeadBasis
    Vec3  posBase;               // the head-centre eye position before any eye offset
    Vec3  offset;                // other eye, head-frame, game convention
    Quat  rot;                   // other eye's cant
    int   eye = 1;
    uint64_t pair = 0;
    float fovY = 0;              // what pass 1 read back from Cam_Fov (pass 2 renders the same)
    Vec3  headPos; Quat headRot; // XR convention, for the report
    // The finished pass-2 camera basis, computed by GameERApplySecondEyeCamera and re-applied
    // by UpdateCamera on every camera-step call DURING pass 2. Writing the matrices once
    // between the passes proved insufficient (DebugPass2Pitch showed in NEITHER eye): the
    // renderer picks the camera up inside the camera-step machinery, so pass 2 must re-run
    // group 106 (Pass2Keep) and the hook must feed it the other eye - the same write path
    // pass 1 provably renders from.
    bool  camValid = false;
    Vec3  camR, camU, camF, camPos;
};
SecondEyeStash g_secondEye;

void UpdateCamera(uint8_t* player, const FrameInfo& f) {
    const Config& c = Cfg();
    // Draw-only second pass: the camera step re-runs (Pass2Keep includes 106) purely so the
    // engine transfers the camera into its render-side state, and the camera it must carry
    // is the OTHER eye's. No latch, no report, no stash: pass 2 renders, it does not simulate.
    if (FramePumpCurrentPass() == 2) {
        Vec3 r2, u2, f2, p2; float fov2;
        bool have = false;
        {
            std::lock_guard<std::mutex> g(g_secondEye.mtx);
            if (g_secondEye.camValid) {
                r2 = g_secondEye.camR; u2 = g_secondEye.camU;
                f2 = g_secondEye.camF; p2 = g_secondEye.camPos;
                fov2 = g_secondEye.fovY;
                have = true;
            }
        }
        // Flat experiments (pump mode 1, no headset): no second-eye stash exists, but the
        // pitch diagnostic still needs a distinguishable pass-2 camera on the monitor.
        if (!have) {
            const float pd = DebugPass2PitchDeg();
            if (pd == 0.f || !g_haveCamMtx.load(std::memory_order_relaxed)) return;
            float m[16];
            { std::lock_guard<std::mutex> g(g_camMtxLock); memcpy(m, g_lastCamMtx, sizeof(m)); }
            r2 = {m[0], m[1], m[2]}; u2 = {m[4], m[5], m[6]};
            f2 = {m[8], m[9], m[10]}; p2 = {m[12], m[13], m[14]};
            const Quat pq = Quat::AxisAngle(r2, Deg2Rad(pd));
            u2 = pq.Rotate(u2); f2 = pq.Rotate(f2);
            fov2 = 0.f;
        }
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            LOGI("[cam] pass-2 camera path ACTIVE: second-eye basis re-applied at camera-step time");
        }
        uint8_t* pcs2[4]; const int npc2 = AllPersCams(pcs2);
        for (int i = 0; i < npc2; ++i) {
            WriteCamMatrix(pcs2[i] + off::Cam_Matrix, r2, u2, f2, p2);
            if (fov2 > 0.05f) SafeWrite(pcs2[i] + off::Cam_Fov, &fov2, 4);
        }
        if (uint8_t* cc = ChrCam()) {
            WriteCamMatrix(cc + off::Cam_Matrix, r2, u2, f2, p2);
            if (fov2 > 0.05f) SafeWrite(cc + off::Cam_Fov, &fov2, 4);
        }
        if (uint8_t* fc = FollowCam())
            WriteCamMatrix(fc + off::Cam_Matrix, r2, u2, f2, p2);
        // Keep the reassert hooks (follow cam, CamFinalize) replaying THIS eye for the rest
        // of the pass.
        std::lock_guard<std::mutex> g(g_camMtxLock);
        float m[16] = { r2.x, r2.y, r2.z, 0.f, u2.x, u2.y, u2.z, 0.f,
                        f2.x, f2.y, f2.z, 0.f, p2.x, p2.y, p2.z, 1.f };
        memcpy(g_lastCamMtx, m, sizeof(m));
        g_haveCamMtx.store(true, std::memory_order_relaxed);
        return;
    }
    float yaw = f.baseYaw, pitch = 0.f;
    Vec3 r, u, fw;
    // Which eye this frame renders, if the render thread is driving stereo.
    const EyeLatch& el = LatchEyeForFrame();
    const bool stereo = el.active && f.headValid;
    if (c.fpCameraRotation == "game") pitch = f.basePitch;
    bool haveHeadBasis = false;
    Quat qHead;
    if (c.fpCameraRotation == "hmd") {
        // full HMD orientation composed onto the base yaw
        qHead = Quat::YawQ(f.baseYaw) * f.headQ;
        haveHeadBasis = true;
        Quat q = stereo ? qHead * el.req.rot : qHead;   // per-eye cant; identity on most headsets
        fw = q.Rotate({0, 0, 1}); r = q.Rotate({1, 0, 0}); u = q.Rotate({0, 1, 0});
    } else CamBasis(yaw, pitch, r, u, fw);
    // Capture probe (Numpad 3, third state): tilt the REAL pass's camera. If both eyes show
    // the tilt, the renderer captures its camera once per sim frame from pass 1 - which is
    // why nothing written for pass 2 has ever reached pass 2's image. One tilted eye instead
    // names the eye that receives pass 1's content.
    const float p1p = DebugKeysPass1Pitch();
    if (p1p != 0.f) {
        const Quat pq = Quat::AxisAngle(r, Deg2Rad(p1p));
        u = pq.Rotate(u); fw = pq.Rotate(fw);
    }
    // Positional head tracking: nothing downstream adds an HMD offset any more, so the eye
    // we compute is the eye that gets rendered.
    Vec3 pos = f.eye;
    // Stereo: displace to this frame's eye. The offset is measured in the HEAD's frame, so it
    // is rotated by the head basis - not by the per-eye canted one, which would shear the two
    // eyes apart by the cant angle.
    if (stereo) {
        const Vec3& o = el.req.offset;
        pos = pos + (haveHeadBasis ? qHead.Rotate(o) : (r * o.x + u * o.y + fw * o.z));
    }

    uint8_t* pcs[4]; int npc = AllPersCams(pcs);
    for (int i = 0; i < npc; ++i) WriteCamMatrix(pcs[i] + off::Cam_Matrix, r, u, fw, pos);
    // Widen the frustum so one rendered image can contain a whole eye's field of view. The
    // value comes from the render thread, which is the only side that knows what the headset
    // needs; it then crops back down to the runtime's exact asymmetric FOV, so too wide only
    // costs resolution while too narrow costs field of view.
    // [FirstPerson] ForceFovDeg: write Cam_Fov WITHOUT a VR session, so the fov-semantics
    // A/B (which height does a WRITTEN fov apply across - boot or current?) can run in the
    // flat harness. The stereo path never sets it; 0 = off.
    const bool fovOn = (stereo && el.req.fovWrite > 0.01f) || Cfg().fpForceFovDeg > 1.f;
    if (fovOn) {
        float fov = Cfg().fpForceFovDeg > 1.f ? Deg2Rad(Cfg().fpForceFovDeg) : el.req.fovWrite;
        // Before overwriting: did the game change the FOV back since OUR last write?
        // A single foreign read is NOT enough: the game resets the FOV occasionally in
        // ordinary play too (measured 2026-08-22: 9% of pre-write samples were the game's
        // 0.838 during pure gameplay - and those isolated blips kept the guard's 3 s tail
        // refreshed for a whole session, which is the "always in cinema mode" bug). What
        // separates a takeover from a blip is PERSISTENCE: with one FOV-write call per
        // frame, the map/cutscene camera wins the write race every consecutive frame
        // (16 consecutive in the map burst at 14:41), while gameplay blips are isolated.
        // So arm only on a streak - ~20 frames = ~300 ms of map, far less time than it
        // takes to pick a destination, and a length ordinary play never strings together.
        const float lastW = g_lastFovWrite.load(std::memory_order_relaxed);
        float cur = 0.f;
        if (npc > 0 && lastW > 0.01f && SafeRead(pcs[0] + off::Cam_Fov, &cur, 4)) {
            // Majority over a sliding window, NOT a consecutive streak. With stereo live
            // this code runs twice per frame, and only PASS 1 sees the menu camera's value
            // (pass 2 pre-reads our own pass-1 write) - so a takeover reads foreign, ours,
            // foreign, ours... and a consecutive streak NEVER arms. That is exactly how the
            // 14:53 fast-travel crash got through: map open, guard silent. Rates measured
            // 2026-08-22: map/menu >= 50% of reads foreign (100% in mono), ordinary
            // gameplay <= 9% in isolated blips. 16-of-40 (40%) splits those cleanly and
            // arms within ~13 frames of the map opening.
            static uint8_t s_win[40] = {};
            static int s_head = 0, s_sum = 0;
            const uint8_t f = std::fabs(cur - lastW) > 0.05f ? 1 : 0;
            s_sum += f - s_win[s_head];
            s_win[s_head] = f;
            s_head = (s_head + 1) % 40;
            if (s_sum >= 16) {
                g_foreignFovTick.store(GetTickCount64(), std::memory_order_relaxed);
                static long s_logged = 0;
                if (s_logged < 16 && ++s_logged)
                    LOGI("[cam] foreign FOV %.3f (ours %.3f), %d of last 40 reads - "
                         "menu/cutscene camera owns the view", cur, lastW, s_sum);
            }
        }
        if (!WorldDyingNow()) {   // fov writes stand down with the matrix writes
            for (int i = 0; i < npc; ++i) SafeWrite(pcs[i] + off::Cam_Fov, &fov, 4);
            if (uint8_t* cc = ChrCam()) SafeWrite(cc + off::Cam_Fov, &fov, 4);
            g_lastFovWrite.store(fov, std::memory_order_relaxed);
        }
    }
    // World-origin-shift telemetry: an origin rebase (BeginShiftWorldPosition, group 160)
    // moves every world coordinate at once, so the camera jumps farther in one frame than any
    // real motion could. Logged so in-headset glitch reports can be lined up against shifts -
    // the 2026-08-20 "geometry shifting/teleport ~1 min in" started at the first rebase.
    { static Vec3 s_prevPos{}; static bool s_havePrev = false;
      if (s_havePrev) {
          const float jump = (pos - s_prevPos).Length();
          if (jump > 20.f) {
              // Rebases move ~32 m; anything much larger is a teleport (fast travel, grace
              // warp) - suspend the second pass while the world is torn down and rebuilt
              // around it (fast travel's fade fired pass-2 DL_PANICs on 2026-08-21).
              if (jump > 100.f) g_bigJumpTick.store(GetTickCount64(), std::memory_order_relaxed);
              LOGI("[shift] camera jumped %.1f m in one frame - %s", jump,
                   jump > 100.f ? "TELEPORT (pass 2 suspended)" : "world-origin rebase");
          }
      }
      s_prevPos = pos; s_havePrev = true; }
    g_lastWrittenPos = pos; g_lastCamRoot = f.root;
    { std::lock_guard<std::mutex> g(g_camMtxLock);
      float m[16] = { r.x, r.y, r.z, 0.f, u.x, u.y, u.z, 0.f, fw.x, fw.y, fw.z, 0.f, pos.x, pos.y, pos.z, 1.f };
      memcpy(g_lastCamMtx, m, sizeof(m)); g_haveCamMtx.store(true, std::memory_order_relaxed); }
    if (uint8_t* cc = ChrCam()) WriteCamMatrix(cc + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* fc = FollowCam()) WriteCamMatrix(fc + off::Cam_Matrix, r, u, fw, pos);
    // Cast aim: with a casting hand the ray the game aims unlocked bullets from is the controller's, not the
    // head's - the fake lock target goes down it and the aim view matrix looks along it from the hand.
    Vec3 aimO, aimD; bool casting = false, handAim = false;
    if (c.castAim && !f.lockedOn) { const int ah = CombatCastAimHand(&casting); handAim = ah >= 0 && CastAimRay(f, ah, aimO, aimD); }
    ApplyFaceCamera(player, f.root, f.bodyYaw, handAim ? &aimO : nullptr, handAim ? &aimD : nullptr, handAim && casting, f.riding);
    if (c.fpWriteAim) {
        if (handAim) {
            Vec3 ar, au; AimBasis(aimD, f.bodyYaw, ar, au);
            WriteCamMatrix(player + off::Chr_AimViewMtx, ar, au, aimD, aimO);
        } else {
            Quat q = Quat::YawQ(f.baseYaw) * f.headQ;
            Vec3 ar = q.Rotate({1, 0, 0}), au = q.Rotate({0, 1, 0}), af = q.Rotate({0, 0, 1});
            WriteCamMatrix(player + off::Chr_AimViewMtx, ar, au, af, f.eye);   // nobody adds to this one: use the real eye
        }
    }
    // Tell the render thread what this frame actually is. The FOV is READ BACK rather than
    // assumed: whatever the game (or the override above) left in Cam_Fov is what the image
    // gets rasterised with, and the crop has to match it or the world comes out the wrong
    // size. The head pose goes back in XR/stage convention - ToGame is its own inverse -
    // because that is the frame the compositor reprojects in.
    if (stereo) {
        float fov = 0.f;
        if (npc > 0) SafeRead(pcs[0] + off::Cam_Fov, &fov, 4);
        StereoEyeReport rep;
        rep.eye = el.req.eye;
        rep.pair = el.req.pair;
        rep.fovY = (fov > 0.05f && fov < 3.2f) ? fov : 0.f;
        rep.headPos = ToGame(f.headP);
        rep.headRot = ToGame(f.headQ);
        // The report is NOT pushed here: the game's own step re-smooths the final camera
        // after this write (measured 20:13 session: yaw off in 3270 of 3270 checks,
        // position off >5 cm in half), so the image is rendered with a camera this pose
        // does not describe - a standing image/pose mismatch the compositor turns into
        // head-motion skew. The report is STASHED and completed at pass end by
        // GameERFinalizePassReport, which reads the camera the renderer really used.
        // Pass 2's re-assert calls come through here too with the frame's PASS-1 latch -
        // they must not overwrite pass 2's own pending report (stashed by
        // GameERApplySecondEyeCamera between the passes).
        if (FramePumpCurrentPass() != 2)
            StashPendingReport(rep, Quat::FromBasis(r, u, fw), pos);
        // Full mode: stash the second eye for the pump. Overwritten by every UpdateCamera
        // call in the frame, so the pump gets the LAST (freshest) camera state, exactly
        // like the report queue does.
        std::lock_guard<std::mutex> g(g_secondEye.mtx);
        g_secondEye.valid = el.req.haveOther;
        if (el.req.haveOther) {
            g_lastStashTick.store(GetTickCount64(), std::memory_order_relaxed);
            g_secondEye.haveHeadBasis = haveHeadBasis;
            g_secondEye.qHead = qHead;
            g_secondEye.r = r; g_secondEye.u = u; g_secondEye.fw = fw;
            g_secondEye.posBase = f.eye;
            g_secondEye.offset = el.req.otherOffset;
            g_secondEye.rot = el.req.otherRot;
            g_secondEye.eye = el.req.otherEye;
            g_secondEye.pair = el.req.pair;
            g_secondEye.fovY = rep.fovY;
            g_secondEye.headPos = rep.headPos;
            g_secondEye.headRot = rep.headRot;
        }
    }
}

// ---------------------------------------------------------------- arms
void SolveArm(const Skeleton& s, HkQsTransform* pose, std::vector<HkQsTransform>& local, WorldPose& w, int iUA, int iFA, int iHand,
              const Vec3& target, const Quat& targetQ, const Vec3& pole) {
    if (iUA < 0 || iFA < 0 || iHand < 0) return;
    if (s.parents[iFA] != iUA || s.parents[iHand] != iFA) return;
    const HkQsTransform& rFA = s.refPoseCopy[iFA]; const HkQsTransform& rHand = s.refPoseCopy[iHand];
    Vec3 axisUA{rFA.t[0], rFA.t[1], rFA.t[2]}, axisFA{rHand.t[0], rHand.t[1], rHand.t[2]};
    float L1 = axisUA.Length(), L2 = axisFA.Length();
    if (L1 < 0.05f || L2 < 0.05f) return;
    axisUA = axisUA * (1.f / L1); axisFA = axisFA * (1.f / L2);

    Vec3 S = w.pos[iUA];
    Vec3 toT = target - S; float d = toT.Length();
    if (d < 1e-4f) return;
    float maxReach = (L1 + L2) * 0.999f;
    float stretch = 1.f;
    if (d > maxReach) {
        float want = d / maxReach;
        stretch = std::fmin(want, std::fmax(1.f, Cfg().armStretch));
        L1 *= stretch; L2 *= stretch; maxReach *= stretch;
        if (d > maxReach) { toT = toT * (maxReach / d); d = maxReach; }
    }
    Vec3 dir = toT * (1.f / d);
    float cosA = Clamp((L1 * L1 + d * d - L2 * L2) / (2.f * L1 * d), -1.f, 1.f);
    float sinA = std::sqrt(std::fmax(0.f, 1.f - cosA * cosA));
    Vec3 perp = (pole - dir * pole.Dot(dir)).Normalized();
    if (perp.Length() < 1e-3f) perp = Vec3{0, -1, 0};
    Vec3 E = S + dir * (L1 * cosA) + perp * (L1 * sinA);
    Vec3 T = S + toT;

    // Upper arm: rotate so its child axis points at the elbow.
    int pUA = s.parents[iUA];
    Quat parentQ = pUA >= 0 ? w.q[pUA] : Quat{};
    Vec3 curAxis = w.q[iUA].Rotate(axisUA);
    Quat qUA = (Quat::FromTo(curAxis, (E - S).Normalized()) * w.q[iUA]).Normalized();
    Quat lUA = (parentQ.Conj() * qUA).Normalized();
    // Forearm: child axis points at the wrist.
    const HkQsTransform& curFA = local[iFA];
    Quat lFAcur{curFA.q[0], curFA.q[1], curFA.q[2], curFA.q[3]};
    Quat qFAcur = qUA * lFAcur;
    Vec3 curAxisFA = qFAcur.Rotate(axisFA);
    Quat qFA = (Quat::FromTo(curAxisFA, (T - E).Normalized()) * qFAcur).Normalized();
    Quat lFA = (qUA.Conj() * qFA).Normalized();
    // Hand: absolute orientation from the controller.
    Quat lHand = (qFA.Conj() * targetQ).Normalized();

    auto writeQ = [&](int i, const Quat& q) {
        HkQsTransform& t = local[i]; t.q[0] = q.x; t.q[1] = q.y; t.q[2] = q.z; t.q[3] = q.w;
        SafeWrite(&pose[i].q[0], t.q, 16);
    };
    auto writeT = [&](int i, float scale) {   // stretch bone length along its (reference) offset
        const HkQsTransform& r = s.refPoseCopy[i]; HkQsTransform& t = local[i];
        t.t[0] = r.t[0] * scale; t.t[1] = r.t[1] * scale; t.t[2] = r.t[2] * scale;
        SafeWrite(&pose[i].t[0], t.t, 16);
    };
    writeQ(iUA, lUA); writeQ(iFA, lFA); writeQ(iHand, lHand);
    if (stretch != 1.f) { writeT(iFA, stretch); writeT(iHand, stretch); }
    w.q[iUA] = qUA; w.q[iFA] = qFA; w.q[iHand] = targetQ; w.pos[iFA] = E; w.pos[iHand] = T;
}

// Yaw the character root faces: local -Z of the (physics) root, the convention verified against the lock-on
// point. Deliberately *not* built from the Master bone: for c0000 that bone's own orientation is not identity, so
// its -Z is not the body's forward, and aligning it to the view spun the whole body ~90 degrees (log: "rendered
// facing-cam 0.0, shoulder-line-cam -98.5").
float BodyFacingYaw(const Quat& rootQ, const WorldPose&) {
    Vec3 fwd = rootQ.Rotate({0, 0, -1});
    return WrapPi(std::atan2(fwd.x, fwd.z) + Deg2Rad(Cfg().bodyFacingOffsetDeg));
}
// Yaw the rendered shoulders face (perpendicular to the L/R clavicle line). Which clavicle is on which side is
// model-authoring dependent, so the 180-degree ambiguity is resolved at runtime against the root facing, measured
// on the animation's own (unrotated) pose where the two must agree to within a few tens of degrees.
bool g_shoulderFlip = false;
float ShoulderLineYawRaw(const WorldPose& w) {
    if (g_skel.L_clav < 0 || g_skel.R_clav < 0) return 0.f;
    Vec3 right = w.pos[g_skel.R_clav] - w.pos[g_skel.L_clav]; right.y = 0.f;
    return std::atan2(-right.z, right.x);
}
float ShoulderLineYaw(const WorldPose& w) { return WrapPi(ShoulderLineYawRaw(w) + (g_shoulderFlip ? 3.14159265f : 0.f)); }
void CalibrateShoulderFlip(const Quat& rootQ, const WorldPose& w) {
    if (g_skel.L_clav < 0 || g_skel.R_clav < 0) return;
    float d = std::fabs(WrapPi(ShoulderLineYawRaw(w) - BodyFacingYaw(rootQ, w)));
    bool was = g_shoulderFlip;
    if (d < Deg2Rad(60.f)) g_shoulderFlip = false; else if (d > Deg2Rad(120.f)) g_shoulderFlip = true;
    if (was != g_shoulderFlip) LOGI("[fp] shoulder line reads %s the root facing (%.0f deg off); %s", g_shoulderFlip ? "opposite to" : "along", Rad2Deg(d), g_shoulderFlip ? "flipping it" : "using it as is");
}

// Bones PoseMode=vr writes every frame: the root bones (yawed to BodyYaw + follow shift), the torso twist and
// the whole arm chain. Everything else in the buffer is the game's own animation output.
bool OursBone(int i) {
    if (i == g_skel.torso || i == g_skel.L_ua || i == g_skel.L_fa || i == g_skel.L_hand ||
        i == g_skel.R_ua || i == g_skel.R_fa || i == g_skel.R_hand) return true;
    int p = g_skel.parents[i]; return p < 0 || p >= i;
}
// Snapshot the animation's current pose as the PoseMode=vr template. Only ever called when the pose buffer
// holds the game's own output (skeleton build, or a fresh buffer while standing still), never our writes.
// `firstTemplate`: the body has no template yet (skeleton build, post-load retry), so only a standing pose will do.
bool CaptureTemplate(uint8_t* chr, const char* why, bool firstTemplate) {
    if (FramePumpCurrentPass() == 2 || !g_poseCaptureGuard.CanCapture(reinterpret_cast<uintptr_t>(chr)) ||
        ReadRideState(chr, SafeRead).NativeBody()) return false;
    if (g_skel.boneCount <= 0 || !SkeletonCurrent(chr)) return false;
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return false;
    std::vector<HkQsTransform> t(g_skel.boneCount);
    if (!SafeRead(pose, t.data(), sizeof(HkQsTransform) * g_skel.boneCount)) return false;
    // Re-capture: keep the bones WE write from the previous template. The game's layered animation does not
    // reliably rewrite the root bones between our last write and this read, so capturing them takes our own
    // output - body yaw and follow shift baked into the template, the shift then applied a second time on top,
    // and the rig parks half a shift off to one side of the camera until the next capture ("camera on the
    // shoulder after a snapback"). The bones we write are re-derived every frame anyway, so keeping the old
    // template's values loses nothing.
    if ((int)g_poseTemplate.size() == g_skel.boneCount)
        for (int i = 0; i < g_skel.boneCount; ++i) if (OursBone(i)) t[i] = g_poseTemplate[i];
    // Judge it by the head height first: the camera rides this template's head until the next one.
    const float head = TemplateHead(t);
    const double now = NowSec();
    const bool overdue = firstTemplate && g_firstRejectSince > 0 && now - g_firstRejectSince > 15.0;
    const BodyStance stance = JudgeStance(head, g_refHead);
    const TemplateVerdict verdict = JudgeTemplate(stance, firstTemplate, overdue);
    const float pct = StanceReferencePlausible(g_refHead) ? head / g_refHead * 100.f : 0.f;
    if (verdict == TemplateVerdict::Rejected) {
        if (firstTemplate && g_firstRejectSince <= 0) g_firstRejectSince = now;
        if (now - g_rejectLogAt > 5.0) {
            g_rejectLogAt = now;
            LOGI("[skel] pose not taken as the template (%s): head %.2f m above the feet = %.0f%% of the reference pose - %s", why, head, pct,
                 stance == BodyStance::Down ? "a body on the ground (knockdown, roll) is never a stance"
                                            : "the first template has to be a standing one, retrying");
        }
        return false;
    }
    SetTemplate(g_poseTemplate, t);
    g_templHead = head; g_templLow = verdict == TemplateVerdict::Low; g_templWhy = why;
    g_firstRejectSince = 0; g_standUp.Reset();
    if (!g_templLow) { SetTemplate(g_standTemplate, g_poseTemplate); g_standHead = head; }
    g_templOwner = PlayerCharacterName();
    Vec3 rp; Quat rq;   // shoulder flip must be calibrated on the game's own (facing-aligned) pose
    if (PlayerRoot(chr, rp, rq)) { WorldPose w; WorldFromLocal(g_skel, g_poseTemplate, rp, rq, w); CalibrateShoulderFlip(rq, w); }
    // Keep the initial eye-to-head calibration. Re-basing it here pins the camera to
    // EyeHeight while the new template moves the body up/down, leaving a permanent gap.
    LOGI("[skel] pose template captured (%s): head %.2f m above the feet = %.0f%% of the reference pose - %s%s", why, head, pct,
         g_templLow ? "low stance, the camera goes down with it until the game stands back up" : "standing",
         overdue ? " (nothing standing for 15 s, taken anyway)" : "");
    return true;
}

// The seated pose: the rider's own native seat, judged upright against the reference pose (head over pelvis) -
// a forward-leaning seat would put the camera back over Torrent's neck. Head and pelvis are kept in the rider's
// model frame: the camera anchor and the upright pivot.
bool SetSeatTemplate(const std::vector<HkQsTransform>& t) {
    if (g_skel.head < 0 || (int)t.size() != g_skel.boneCount) return false;
    WorldPose w; WorldFromLocal(g_skel, t, Vec3{}, Quat{}, w);
    const Vec3 head = w.pos[g_skel.head], pelvis = g_skel.pelvis >= 0 ? w.pos[g_skel.pelvis] : Vec3{};
    if (g_skel.pelvis >= 0) {
        const float above = head.y - pelvis.y;
        const float ref = BoneHeight<HkQsTransform>(g_skel.refPoseCopy, g_skel.parents, g_skel.head) -
                          BoneHeight<HkQsTransform>(g_skel.refPoseCopy, g_skel.parents, g_skel.pelvis);
        if (ref > 0.3f && above < 0.7f * ref) {
            static double s_last = 0;
            if (NowSec() - s_last > 5.0) {
                s_last = NowSec();
                LOGI("[ride] seat not taken: head %.2f m over the pelvis = %.0f%% of standing - leaning, waiting for an upright seat",
                     above, above / ref * 100.f);
            }
            return false;
        }
    }
    g_seatedTemplate = t; g_seatHead = head; g_seatPelvis = pelvis; g_seatOwner = PlayerCharacterName();
    LOGI("[ride] seated pose captured: head %.2f m over the pelvis, %.2f m over the rider's root - the camera rides this "
         "seat, upright, instead of the gallop and jump animations", head.y - pelvis.y, head.y);
    return true;
}
void ObserveSeat(const std::vector<HkQsTransform>& local, double now) {
    for (const auto& t : local)
        for (int k = 0; k < 4; ++k)
            if (!std::isfinite(t.t[k]) || !std::isfinite(t.q[k]) || !std::isfinite(t.s[k])) return;
    float diff = 0.f;
    if ((int)g_seatCand.size() == g_skel.boneCount)
        for (int i = 0; i < g_skel.boneCount; ++i) {
            if (OursBone(i) || g_skel.fingers[i]) continue;
            for (int k = 0; k < 4; ++k) diff += std::fabs(local[i].q[k] - g_seatCand[i].q[k]);
        }
    switch (g_seatSettle.Step(diff, 0.12f, now, 0.3)) {
    case SeatStep::NewCandidate: g_seatCand = local; break;
    case SeatStep::Take: SetSeatTemplate(g_seatCand); break;
    case SeatStep::Wait: break;
    }
}

void UpdateArms(uint8_t* player, FrameInfo& f, int writePoint = 0) {
    const Config& c = Cfg();
    // Pose-write choke point: while the world is dying (death anim, warp, load) the game's
    // own animation (ragdoll, death anim) owns the body, and our bulk pose write is 720
    // bytes of floats going into memory teardown is about to free - stand down.
    if (WorldDyingNow()) return;
    if (!SkeletonCurrent(player)) { if (!BuildSkeleton(player)) return; }
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return;
    // Same root the camera anchor was built from - the bones must be solved in the frame they are drawn in.
    Vec3 rootPos = f.root; Quat rootQ = f.rootQ;
    std::vector<HkQsTransform> local;
    WorldPose w; if (!ComputeWorld(g_skel, pose, rootPos, rootQ, w, local)) return;
    // Pose-fight probe: was our last R_UpperArm write replaced by the game before this call?
    // Doubles as the buffer-freshness answer: the game's full-body evaluation rewrites R_UpperArm every tick,
    // so ours-still-there means the whole buffer is our own echo, not the animation's output.
    bool poseForeign = false;
    int probe = g_skel.R_ua;
    if (probe >= 0 && g_probeLastPoint >= 0) {
        const HkQsTransform& t = local[probe];
        float d = std::fabs(t.q[0] - g_probeQ.x) + std::fabs(t.q[1] - g_probeQ.y) + std::fabs(t.q[2] - g_probeQ.z) + std::fabs(t.q[3] - g_probeQ.w);
        if (d > 1e-3f) { poseForeign = true; InterlockedIncrement(&g_poseForeign[g_probeLastPoint]); }
        // Per-bone magnitude: how hard does the foreign layer move each tracked bone?
        for (size_t k = 0; k < g_probeBones.size(); ++k) {
            if (!g_probeHas[k]) continue;
            const HkQsTransform& b = local[g_probeBones[k]];
            float dot = std::fabs(b.q[0] * g_probeLastQ[k].x + b.q[1] * g_probeLastQ[k].y + b.q[2] * g_probeLastQ[k].z + b.q[3] * g_probeLastQ[k].w);
            float ang = Rad2Deg(2.f * std::acos(Clamp(dot, 0.f, 1.f)));
            if (ang > g_probeMaxDeg[k]) g_probeMaxDeg[k] = ang;
        }
    }

    Quat baseQ = Quat::YawQ(f.baseYaw);
    // Body follows head: shift the whole skeleton (root bones) so the character hangs under your view instead of
    // under its own root. The physics root does not move (no collision cheating) and the shift is clamped.
    //
    // BodyAnchor=head puts the character's head bone *at* the eye position, whatever the animation is doing -
    // running leans the body forward, a crouch drops it, the eye height differs from the model's. Measuring the
    // correction against the bone closes all of that, where shifting by the raw headset offset (BodyAnchor=root)
    // only ever corrects for you moving in the play space.
    Vec3 eyeW = f.eye;
    Vec3 headOff = eyeW - f.anchor, bodyOff{};
    // PoseMode=vr: the rig is the captured template, yawed to BodyYaw, every tick - the animation system can
    // write whatever it wants into the pose buffer, we replace all of it. Root motion (the character's actual
    // displacement, physics module) is untouched, so rolls and attacks still move you - they just don't pose you.
    if (f.capturePose && FramePumpCurrentPass() != 2 && c.fpPoseMode == "vr" && (int)g_poseTemplate.size() != g_skel.boneCount) {
        // Build-time capture failed (pose buffer not readable yet): retry, else vr mode would silently never start.
        double nowT = NowSec();
        if (nowT - g_templLastCapture > 2.0) {
            g_templLastCapture = nowT;
            if (CaptureTemplate(player, "retry", true)) {
                const Vec3 oldAnchor = f.anchor;
                BuildCameraAnchor(player, f);
                f.eye = f.eye + (f.anchor - oldAnchor);
                eyeW = f.eye; headOff = eyeW - f.anchor;
            }
        }
    }
    // Seated on Torrent: the rider's own seat becomes the seated pose once it holds still (ObserveSeat) - an idle
    // moment in the saddle, never a gallop or jump frame - and later rides reuse it (BuildSkeleton carries it).
    if (c.fpPoseMode == "vr" && f.riding && f.seated && g_skel.head >= 0 && !SeatTemplateReady() &&
        g_seatedFrame.CanCapture(NowSec(), poseForeign, FramePumpCurrentPass() != 2))
        ObserveSeat(local, NowSec());
    const bool seatedPose = f.seatPose;
    bool vrPose = !f.riding && c.fpPoseMode == "vr" && (int)g_poseTemplate.size() == g_skel.boneCount;
    bool followsHead = !f.riding && !vrPose && c.bodyFollowsHead && c.fpCameraAnchor != "head";   // head anchor: the camera goes to the body, not the body to the camera
    if (f.riding) {
        // Keep the saved walking stance, but discard a pending capture from before mounting.
        g_templCandAt = 0; g_statPos = f.root; g_statSince = NowSec();
        for (auto& has : g_rootShiftHas) has = 0;
    }
    if (!vrPose && !seatedPose && !followsHead) CalibrateShoulderFlip(rootQ, w);
    if (seatedPose) {
        ApplyBodyPoseTemplate<HkQsTransform>(local, g_seatedTemplate, g_skel.fingers);
        if (c.bodyFollowsHead) {
            bodyOff = baseQ.Rotate(f.headRel);
            const float horiz = std::sqrt(bodyOff.x * bodyOff.x + bodyOff.z * bodyOff.z);
            if (horiz > c.bodyFollowMaxHorizontal && horiz > 1e-4f) {
                const float k = c.bodyFollowMaxHorizontal / horiz; bodyOff.x *= k; bodyOff.z *= k;
            }
            bodyOff.y = Clamp(bodyOff.y, -c.bodyFollowMaxDown, c.bodyFollowMaxUp);
            bodyOff.x -= std::sin(f.bodyYaw) * c.bodyBackOffset;
            bodyOff.z -= std::cos(f.bodyYaw) * c.bodyBackOffset;
        }
        // The rider moves with the smoothed view, so the saddle's bounce cannot lift the body into it.
        bodyOff = bodyOff + f.rideCamShift;
        if (bodyOff.Length() > 1e-5f) {
            const Vec3 lo = rootQ.Conj().Rotate(bodyOff);
            for (int i = 0; i < g_skel.boneCount; ++i) {
                if (g_skel.parents[i] >= 0 && g_skel.parents[i] < i) continue;
                local[i].t[0] += lo.x; local[i].t[1] += lo.y; local[i].t[2] += lo.z;
            }
        }
        // Only the rider is stabilized. Torrent's own bones keep their native animation.
        SafeWrite(pose, local.data(), sizeof(HkQsTransform) * g_skel.boneCount);
        WorldFromLocal(g_skel, local, rootPos, rootQ, w);
    }
    else if (vrPose) {
        // Stance re-capture: standing still for a second, take the animation's own pose as the new template
        // (idle after a run capture, crouch). "The animation's own" = the buffer differs from the
        // template on bones we never write; comparing bones we do write (roots, torso, arms) would see our own
        // previous output as "fresh animation" and bake our body-yaw rotation into the template (spinning rig).
        //
        // Settle guard: a stance is a pose the animation HOLDS. A transient with the root planted - an attack
        // swing, an item pickup, an idle fidget - also differs from the template, and capturing it mid-motion
        // is the "body drops into a crouch, snaps back seconds later" bug. So a differing pose first becomes a
        // candidate, and only a candidate that is still the same pose 0.4 s later is captured; anything still
        // moving keeps replacing its own candidate and never lands.
        // The check only runs on a game-fresh buffer (poseForeign): on an echo frame the non-ours bones read
        // back as the template we wrote, which looked like "stance returned to normal" and kept clearing the
        // candidate a real stance change had just planted.
        double nowT = NowSec();
        // Leaving a low stance does not wait for standing still: once the game's own pose reads standing again
        // (fresh frames for 0.25 s - you stood up from the crouch, the kneel or the held wind-up ended), the
        // standing template comes back, walking or not. Before, a low template stayed until you happened to
        // stand still for 1.4 s, and the camera rode it ("stuck crouching", "became really short").
        if (g_templLow && (int)g_standTemplate.size() == g_skel.boneCount && FramePumpCurrentPass() != 2) {
            // The game's pose, with the template's root bones: ours carry the body-follow shift, vertical included.
            std::vector<HkQsTransform> probe = local;
            for (int i = 0; i < g_skel.boneCount; ++i) { int p = g_skel.parents[i]; if (p < 0 || p >= i) probe[i] = g_poseTemplate[i]; }
            const float liveHead = TemplateHead(probe);
            if (g_standUp.Update(poseForeign, JudgeStance(liveHead, g_refHead) == BodyStance::Standing, nowT)) {
                LOGI("[skel] the game stood back up (head %.2f m above the feet) - standing template restored, the low one from \"%s\" dropped", liveHead, g_templWhy);
                SetTemplate(g_poseTemplate, g_standTemplate);
                g_templHead = g_standHead; g_templLow = false; g_templWhy = "restored on standing up";
                g_templCandAt = 0; g_standUp.Reset();
                const Vec3 oldAnchor = f.anchor;
                BuildCameraAnchor(player, f);
                f.eye = f.eye + (f.anchor - oldAnchor);
                eyeW = f.eye; headOff = eyeW - f.anchor;
            }
        }
        // A planted root also occurs during rest/stand-up. Discard any candidate
        // through the menu and exit cooldown, without changing the pose we render.
        if (!f.capturePose || (f.root - g_statPos).Length() > 0.03f) {
            g_statPos = f.root; g_statSince = nowT; g_templCandAt = 0;
        }
        // A new stance belongs to the simulation pass; both eyes must render that same template.
        else if (FramePumpCurrentPass() != 2 && poseForeign && nowT - g_statSince > 1.0 && nowT - g_templLastCapture > 3.0 && nowT - g_templCheckAt > 0.25) {
            g_templCheckAt = nowT;
            bool haveCand = g_templCandAt > 0 && (int)g_templCand.size() == g_skel.boneCount;
            float diff = 0.f, cand = 0.f;
            for (int i = 0; i < g_skel.boneCount; ++i) {
                if (OursBone(i) || g_skel.fingers[i]) continue;
                for (int k = 0; k < 4; ++k) {
                    diff += std::fabs(local[i].q[k] - g_poseTemplate[i].q[k]);
                    if (haveCand) cand += std::fabs(local[i].q[k] - g_templCand[i].q[k]);
                }
            }
            if (diff <= 0.2f) g_templCandAt = 0;                                       // still the template's stance
            else if (!haveCand || cand > 0.1f) { g_templCand = local; g_templCandAt = nowT; }   // new or still-moving pose: wait
            else if (nowT - g_templCandAt > 0.4) {
                g_templLastCapture = nowT; g_templCandAt = 0;
                if (CaptureTemplate(player, "standing still, pose settled", false)) {
                    // BuildFrame measured the old template. Move the eye and hand anchor
                    // together before solving the new pose and publishing this frame's camera.
                    const Vec3 oldAnchor = f.anchor;
                    BuildCameraAnchor(player, f);
                    f.eye = f.eye + (f.anchor - oldAnchor);
                    eyeW = f.eye; headOff = eyeW - f.anchor;
                }
            }
        }
        ApplyBodyPoseTemplate<HkQsTransform>(local, g_poseTemplate, g_skel.fingers);
        // Yaw the whole rig (root bones, in the model-root frame) so the body faces BodyYaw regardless of
        // where the physics character points - same math as the BodyRotate step of the animated path.
        float facing = BodyFacingYaw(rootQ, w);
        Quat rotL = c.bodyRotateToHead ? (rootQ.Conj() * Quat::YawQ(WrapPi(f.bodyYaw - facing)) * rootQ).Normalized() : Quat{};
        for (int i = 0; i < g_skel.boneCount; ++i) {
            int p = g_skel.parents[i];
            if (!(p < 0 || p >= i)) continue;
            HkQsTransform& t = local[i];
            Vec3 tv = rotL.Rotate(Vec3{t.t[0], t.t[1], t.t[2]});
            Quat q = (rotL * Quat{t.q[0], t.q[1], t.q[2], t.q[3]}).Normalized();
            t.t[0] = tv.x; t.t[1] = tv.y; t.t[2] = tv.z; t.q[0] = q.x; t.q[1] = q.y; t.q[2] = q.z; t.q[3] = q.w;
        }
        // Body follows head: put the rig's head bone under your eye, so physically stepping/leaning moves the
        // whole character with you instead of sliding the camera off a body that stays behind. The template is
        // re-applied from scratch every frame, so this is idempotent by construction - none of the per-bone
        // "what did we write last time" bookkeeping the animated path needs. Horizontal comes from the head
        // bone (measured on the template, no feedback); vertical is only your own crouch, so the rig can never
        // float off the ground trying to match a configured eye height.
        if (c.bodyFollowsHead) {
            Vec3 headTpl;
            if (HeadBoneTemplateWorld(rootPos, rootQ, f.bodyYaw, headTpl)) {
                Vec3 off{eyeW.x - headTpl.x, 0.f, eyeW.z - headTpl.z};
                float horiz = std::sqrt(off.x * off.x + off.z * off.z);
                if (horiz > c.bodyFollowMaxHorizontal && horiz > 1e-4f) { float k = c.bodyFollowMaxHorizontal / horiz; off.x *= k; off.z *= k; }
                off.y = Clamp(f.headRel.y, -c.bodyFollowMaxDown, c.bodyFollowMaxUp);
                // Park the rig a little behind the camera: the eye sits inside the head bone, so with the head
                // exactly under it the chest and pauldrons hang in the bottom of the view. After the clamp on
                // purpose - this is a deliberate offset, not follow error.
                off.x -= std::sin(f.bodyYaw) * c.bodyBackOffset; off.z -= std::cos(f.bodyYaw) * c.bodyBackOffset;
                bodyOff = off;
                // Keep the shift in the same frame as the template and the IK solve.
                // The finished root pose is converted to the native render frame below.
                Vec3 lo = rootQ.Conj().Rotate(off);
                for (int i = 0; i < g_skel.boneCount; ++i) {
                    int p = g_skel.parents[i];
                    if (!(p < 0 || p >= i)) continue;
                    HkQsTransform& t = local[i];
                    t.t[0] += lo.x; t.t[1] += lo.y; t.t[2] += lo.z;
                }
            }
        }
        SafeWrite(pose, local.data(), sizeof(HkQsTransform) * g_skel.boneCount);
        WorldFromLocal(g_skel, local, rootPos, rootQ, w);
    }
    else if (followsHead) {
        if ((int)g_rootShiftWritten.size() != g_skel.boneCount) {
            g_rootShiftWritten.assign(g_skel.boneCount, {}); g_rootShiftBase.assign(g_skel.boneCount, {}); g_rootShiftHas.assign(g_skel.boneCount, 0);
            g_rootRotBase.assign(g_skel.boneCount, {}); g_rootRotWritten.assign(g_skel.boneCount, {});
        }
        auto recomputeWorld = [&]() {
            for (int k = 0; k < g_skel.boneCount; ++k) {
                const HkQsTransform& lt = local[k]; Vec3 v{lt.t[0], lt.t[1], lt.t[2]}; Quat q{lt.q[0], lt.q[1], lt.q[2], lt.q[3]};
                int pk = g_skel.parents[k];
                if (pk < 0 || pk >= k) { w.q[k] = rootQ * q; w.pos[k] = rootPos + rootQ.Rotate(v); }
                else { w.q[k] = w.q[pk] * q; w.pos[k] = w.pos[pk] + w.q[pk].Rotate(v); }
            }
        };
        auto isRoot = [&](int i) { int p = g_skel.parents[i]; return p < 0 || p >= i; };
        // Step 1: recover the *animation's* root transforms. This runs more than once per frame and the pose
        // buffer is not always refreshed in between, so if a root bone still holds exactly what we wrote last time
        // the untouched value is the base we remembered, not the buffer contents - otherwise every call would walk
        // (and spin) the skeleton a little further. Put the bases back into the local pose and rebuild the world
        // pose from them, so everything measured below is measured on the animation's own skeleton.
        bool anyRestored = false;
        for (int i = 0; i < g_skel.boneCount; ++i) {
            if (!isRoot(i)) continue;
            HkQsTransform& t = local[i];
            Vec3 cur{t.t[0], t.t[1], t.t[2]}, base = cur;
            Quat curQ{t.q[0], t.q[1], t.q[2], t.q[3]}, baseQ_ = curQ;
            if (g_rootShiftHas[i]) {
                Vec3 d = cur - g_rootShiftWritten[i];
                const Quat& wq = g_rootRotWritten[i];
                float dq = std::fabs(curQ.x - wq.x) + std::fabs(curQ.y - wq.y) + std::fabs(curQ.z - wq.z) + std::fabs(curQ.w - wq.w);
                if (std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z) < 1e-5f && dq < 1e-5f) { base = g_rootShiftBase[i]; baseQ_ = g_rootRotBase[i]; anyRestored = true; }
            }
            g_rootShiftBase[i] = base; g_rootRotBase[i] = baseQ_; g_rootShiftHas[i] = 1;
            t.t[0] = base.x; t.t[1] = base.y; t.t[2] = base.z;
            t.q[0] = baseQ_.x; t.q[1] = baseQ_.y; t.q[2] = baseQ_.z; t.q[3] = baseQ_.w;
        }
        if (anyRestored) recomputeWorld();
        CalibrateShoulderFlip(rootQ, w);   // on the animation's own pose, before we turn anything
        // Step 2: yaw the whole skeleton (about the vertical axis through the character root) so the shoulder line
        // faces BodyYaw *now*. The game only turns the physics character toward our lock-on point when moving or
        // when the error is large; idle it happily leaves the body 30-40 degrees (or, mid-turn, 180) off your head.
        // Measured on the base pose, so it is idempotent across repeated calls.
        Quat rotL{};   // extra rotation applied to every root bone, in the model-root frame
        if (c.bodyRotateToHead) {
            // Facing of the rendered body = the model root's forward. The physics/model root maps local -Z onto
            // the direction the character faces (verified: with the character facing our lock-on point, +Z read
            // back 180 degrees off). BodyFacingOffsetDeg is the manual override if a model disagrees.
            float facing = BodyFacingYaw(rootQ, w);
            {
                Quat Yw = Quat::YawQ(WrapPi(f.bodyYaw - facing));
                rotL = (rootQ.Conj() * Yw * rootQ).Normalized();
                for (int i = 0; i < g_skel.boneCount; ++i) {
                    if (!isRoot(i)) continue;
                    HkQsTransform& t = local[i];
                    Vec3 tv = rotL.Rotate(Vec3{t.t[0], t.t[1], t.t[2]});
                    Quat q = (rotL * Quat{t.q[0], t.q[1], t.q[2], t.q[3]}).Normalized();
                    t.t[0] = tv.x; t.t[1] = tv.y; t.t[2] = tv.z; t.q[0] = q.x; t.q[1] = q.y; t.q[2] = q.z; t.q[3] = q.w;
                }
                recomputeWorld();
            }
        }
        // Step 3: how far the (rotated, unshifted) skeleton has to move so it hangs under the view.
        bodyOff = headOff;
        if (c.bodyAnchor == "head" && g_skel.head >= 0) bodyOff = eyeW - w.pos[g_skel.head];
        float horiz = std::sqrt(bodyOff.x * bodyOff.x + bodyOff.z * bodyOff.z);
        if (horiz > c.bodyFollowMaxHorizontal && horiz > 1e-4f) { float k = c.bodyFollowMaxHorizontal / horiz; bodyOff.x *= k; bodyOff.z *= k; }
        bodyOff.y = Clamp(bodyOff.y, -c.bodyFollowMaxDown, c.bodyFollowMaxUp);
        bodyOff.x -= std::sin(f.bodyYaw) * c.bodyBackOffset; bodyOff.z -= std::cos(f.bodyYaw) * c.bodyBackOffset;   // see the PoseMode=vr comment
        Vec3 lo = rootQ.Conj().Rotate(bodyOff);
        // Step 4: write rotation + shift and propagate.
        for (int i = 0; i < g_skel.boneCount; ++i) {
            if (!isRoot(i)) continue;
            HkQsTransform& t = local[i];
            Vec3 nw = Vec3{t.t[0], t.t[1], t.t[2]} + lo;
            Quat nq{t.q[0], t.q[1], t.q[2], t.q[3]};
            g_rootShiftWritten[i] = nw; g_rootRotWritten[i] = nq;
            t.t[0] = nw.x; t.t[1] = nw.y; t.t[2] = nw.z;
            SafeWrite(&pose[i].t[0], t.t, 16);
            SafeWrite(&pose[i].q[0], t.q, 16);
        }
        recomputeWorld();
    }
    // Torso twist: rotate the Spine bone about world Y so the shoulders face the body yaw. This is the residual
    // after BodyRotate (which only knows the root facing): whatever yaw the animation puts into the root/spine
    // bones. Clamped, so a mis-read shoulder line can never fold the torso backwards.
    if (!f.riding && c.torsoFaceCamera && g_skel.torso >= 0 && g_skel.L_clav >= 0 && g_skel.R_clav >= 0 && (f.handValid[0] || f.handValid[1])) {
        {
            float shoulderYaw = ShoulderLineYaw(w);
            float delta = Clamp(WrapPi(f.bodyYaw - shoulderYaw), -Deg2Rad(60.f), Deg2Rad(60.f));
            int i = g_skel.torso, p = g_skel.parents[i];
            Quat parentQ = p >= 0 ? w.q[p] : Quat{};
            Quat newW = (Quat::YawQ(delta) * w.q[i]).Normalized();
            Quat lq = (parentQ.Conj() * newW).Normalized();
            HkQsTransform& t = local[i]; t.q[0] = lq.x; t.q[1] = lq.y; t.q[2] = lq.z; t.q[3] = lq.w;
            SafeWrite(&pose[i].q[0], t.q, 16);
            // recompute world transforms from the modified local pose (cheap: local copy already updated)
            for (int k = 0; k < g_skel.boneCount; ++k) {
                const HkQsTransform& lt = local[k]; Vec3 v{lt.t[0], lt.t[1], lt.t[2]}; Quat q{lt.q[0], lt.q[1], lt.q[2], lt.q[3]};
                int pk = g_skel.parents[k];
                if (pk < 0 || pk >= k) { w.q[k] = rootQ * q; w.pos[k] = rootPos + rootQ.Rotate(v); }
                else { w.q[k] = w.q[pk] * q; w.pos[k] = w.pos[pk] + w.q[pk].Rotate(v); }
            }
        }
    }
    float handRotX = c.handRotOffX;
    DebugKeysHandRotX(&handRotX);   // Numpad 6/9 live calibration wins while active
    Quat calib = Quat::FromEulerYXZ(Deg2Rad(c.handRotOffY), Deg2Rad(handRotX), Deg2Rad(c.handRotOffZ));
    bool any = false;
    Vec3 tp[2]; Quat tq[2]; Vec3 tcf[2], tcu[2]; bool tok[2] = {false, false};
    for (int h = 0; h < 2; ++h) {
        if (!f.handValid[h]) continue;
        Quat cq = (baseQ * f.handQ[h] * calib).Normalized();          // controller orientation in game world
        Vec3 cf = cq.Rotate({0, 0, 1}), cu = cq.Rotate({0, 1, 0});     // controller forward / up (game frame)
        Quat q = cq;
        if (g_skel.handAxesOk[h]) q = Quat::MapPairs(g_skel.handFwd[h], g_skel.handThumb[h], cf, cu);   // hand-local -> world
        Vec3 p = f.anchor + baseQ.Rotate(f.handP[h] * c.handScale) - cf * c.handBackOffset;
        if (c.handOrigin == "head") p = p + headOff;   // hands are head-relative: re-add where the view actually is
        tp[h] = p; tq[h] = q; tcf[h] = cf; tcu[h] = cu; tok[h] = true;
    }
    // Two-handing: the free hand rides the weapon hand's hilt where it took hold - toward the pommel,
    // i.e. down the controller's handle (-up) or back along its aim (-forward) - so it follows the
    // (inertial, lagging) weapon instead of drifting off the shaft. Its orientation keeps the
    // wrist's angle relative to the weapon when the grab registered, mapped through its own
    // hand's bone axes. It sits TwoHandPinSideOffset m off the hilt axis so the wrist clears it.
    {
        int pin = -1, wep = -1; float along = 0.f; bool handle = true;
        TwoHandGripOrientation grip;
        if (CombatTwoHandPin(&pin, &wep, &along, &handle, &grip) && pin >= 0 && wep >= 0 && tok[pin] && tok[wep]) {
            Vec3 axis = handle ? tcu[wep] : tcf[wep];                            // up the blade; pommel = -axis
            Vec3 side = (handle ? tcf[wep] : tcu[wep]).Cross(axis).Normalized();   // lateral, perpendicular to the hilt (sign verified in-game 2026-08-31)
            tp[pin] = tp[wep] + axis * -along + side * c.twoHandPinSideOffset;
            if (c.twoHandPinOrient) {
                const Quat cq = grip.InGame(baseQ * f.handQ[wep], calib);
                tq[pin] = g_skel.handAxesOk[pin] ? Quat::MapPairs(g_skel.handFwd[pin], g_skel.handThumb[pin],
                                                               cq.Rotate({0, 0, 1}), cq.Rotate({0, 1, 0})) : cq;
            }
        }
    }
    for (int h = 0; h < 2; ++h) {
        if (!tok[h]) continue;
        float side = h == HandLeft ? -1.f : 1.f;
        Vec3 pole = Quat::YawQ(f.bodyYaw).Rotate(Vec3{side * c.elbowPoleOut, -c.elbowPoleDown, -c.elbowPoleBack});
        if (h == HandLeft) SolveArm(g_skel, pose, local, w, g_skel.L_ua, g_skel.L_fa, g_skel.L_hand, tp[h], tq[h], pole);
        else               SolveArm(g_skel, pose, local, w, g_skel.R_ua, g_skel.R_fa, g_skel.R_hand, tp[h], tq[h], pole);
        any = true;
    }
    // The blade each hand holds, in the world (GameERBladeWorld): combat measures how close a strike came to an
    // enemy - on horseback the difference between "never reached" and "reached and did nothing".
    if (FramePumpCurrentPass() != 2) {
        std::lock_guard<std::mutex> g(g_bladeMtx);
        for (int h = 0; h < 2; ++h) {
            g_blade[h].valid = tok[h];
            if (!tok[h]) continue;
            const Vec3 axis = c.twoHandAxis != "forward" ? tcu[h] : tcf[h];
            g_blade[h].grip = tp[h]; g_blade[h].tip = tp[h] + axis * CombatHandReach(h); g_blade[h].stamp = NowSec();
            // tp = anchor + baseQ * (handP * scale) + offsets, and handP is the simulated grip plus a constant:
            // whatever is left over is the origin of the grip frame in the world.
            Vec3 grip; { std::lock_guard<std::mutex> li(g_inertiaMtx); grip = g_inertia[h].pos; }
            PlayToWorld& m = g_playToWorld[h];
            m.rot = baseQ; m.scale = c.handScale; m.origin = tp[h] - baseQ.Rotate(grip * c.handScale);
            m.valid = g_inertia[h].init; m.stamp = g_blade[h].stamp;
        }
    }
    // Publish the entire solved root in the native model frame. Mixing a render-frame
    // shift with physics-frame rotations reversed the lean when the frames differed.
    // Editing the model matrix itself also changed axes the game uses for movement,
    // and left its derived/propagated matrices disagreeing with the one we had flipped.
    if (vrPose || seatedPose || followsHead) {
        Vec3 renderPos; Quat renderQ;
        if (ModelMatrixRoot(player, renderPos, renderQ)) {
            // Second eye, seated: until pass 2 has re-placed the rider this frame, its rebuild is predicted, so the
            // pose written now already lands where pass 1 drew the rider (RiderPassHold).
            if (seatedPose && FramePumpCurrentPass() == 2) {
                std::lock_guard<std::mutex> g(g_riderHoldMtx);
                const uint32_t pair = RiderHoldPair();
                if (g_riderHold.Held(pair, NowSec())) renderPos = g_riderHold.Render(renderPos, HeadingQ(renderQ), pair);
            }
            for (int i = 0; i < g_skel.boneCount; ++i) {
                const int parent = g_skel.parents[i];
                if (!(parent < 0 || parent >= i)) continue;
                HkQsTransform& t = local[i];
                Vec3 translation{t.t[0], t.t[1], t.t[2]}; Quat rotation{t.q[0], t.q[1], t.q[2], t.q[3]};
                RebasePoseRoot(rootPos, rootQ, renderPos, renderQ, translation, rotation);
                t.t[0] = translation.x; t.t[1] = translation.y; t.t[2] = translation.z;
                t.q[0] = rotation.x; t.q[1] = rotation.y; t.q[2] = rotation.z; t.q[3] = rotation.w;
                SafeWrite(&pose[i], &t, sizeof(t));
                if (followsHead) {   // next call must recognize and undo this exact output
                    g_rootShiftWritten[i] = translation; g_rootRotWritten[i] = rotation;
                }
            }
        }
    }
    g_status.armsActive = any;
    // Record what we left in the probe bone this pass, and report the fight once per 5 s.
    bool vrWrote = vrPose || seatedPose;
    if (probe >= 0 && (any || vrWrote)) {
        const HkQsTransform& t = local[probe];
        g_probeQ = {t.q[0], t.q[1], t.q[2], t.q[3]}; g_probeLastPoint = writePoint;
        for (size_t k = 0; k < g_probeBones.size(); ++k) {
            const HkQsTransform& b = local[g_probeBones[k]];
            g_probeLastQ[k] = {b.q[0], b.q[1], b.q[2], b.q[3]}; g_probeHas[k] = 1;
        }
    }
    posescan::MarkPhase(writePoint);
    if (writePoint == 0) posescan::Drive(pose, sizeof(HkQsTransform) * (size_t)g_skel.boneCount);
    {
        static double s_pfLast = 0; double nowP = NowSec();
        if (nowP - s_pfLast > 5.0) {
            s_pfLast = nowP;
            long a = InterlockedExchange(&g_poseForeign[0], 0), b = InterlockedExchange(&g_poseForeign[1], 0), d = InterlockedExchange(&g_poseForeign[2], 0), e = InterlockedExchange(&g_poseForeign[3], 0);
            if (a + b + d + e) {
                std::string mags;
                for (size_t k = 0; k < g_probeBones.size(); ++k) {
                    if (g_probeMaxDeg[k] > 0.5f) { char buf[64]; snprintf(buf, 64, "%s %.1f ", g_probeNames[k].c_str(), g_probeMaxDeg[k]); mags += buf; }
                    g_probeMaxDeg[k] = 0.f;
                }
                LOGI("[pose] game re-wrote the arm pose %ld/%ld/%ld/%ld times after the pose hook / camera step / model-pos / post-havok write (5 s); max foreign deltas (deg): %s", a, b, d, e, mags.empty() ? "(none >0.5)" : mags.c_str());
            }
        }
    }
    if (c.fpDebug) {
        double now = NowSec();
        if (now - g_lastDebug > 1.0) {
            g_lastDebug = now;
            int hb = g_skel.Find("Head");
            Vec3 headW = hb >= 0 ? w.pos[hb] : Vec3{};
            float dmy[16]{}; Vec3 dmyP{};
            if (g_getDmyPos) { uint32_t id = 907; g_getDmyPos(player, dmy, &id, 1); dmyP = {dmy[12], dmy[13], dmy[14]}; }
            Vec3 phys; Quat physQ; Vec3 dRoot{};
            if (PlayerRoot(player, phys, physQ)) dRoot = rootPos - phys;
            LOGD("[fp] root(%.2f,%.2f,%.2f) model-phys(%.3f,%.3f,%.3f) anchor(%.2f,%.2f,%.2f) headBone(%.2f,%.2f,%.2f) anchor-head(%.2f,%.2f,%.2f) dmy907(%.2f,%.2f,%.2f) baseYaw %.1f | R hand tgt(%.2f,%.2f,%.2f) valid L%d R%d",
                 rootPos.x, rootPos.y, rootPos.z, dRoot.x, dRoot.y, dRoot.z, f.anchor.x, f.anchor.y, f.anchor.z, headW.x, headW.y, headW.z, f.anchor.x - headW.x, f.anchor.y - headW.y, f.anchor.z - headW.z, dmyP.x, dmyP.y, dmyP.z,
                 Rad2Deg(f.baseYaw), (f.anchor + headOff + baseQ.Rotate(f.handP[1])).x, (f.anchor + headOff + baseQ.Rotate(f.handP[1])).y, (f.anchor + headOff + baseQ.Rotate(f.handP[1])).z,
                 (int)f.handValid[0], (int)f.handValid[1]);
            LOGD("[fp] hmd raw(%.2f,%.2f,%.2f) origin(%.2f,%.2f,%.2f) rel(%.2f,%.2f,%.2f) bodyShift(%.2f,%.2f,%.2f) | chr-cam yaw %.1f, body-cam yaw %.1f, head-cam yaw %.1f deg | root facing-cam %.1f, shoulder-line-cam %.1f (flip %d) | locked %d",
                 f.headP.x, f.headP.y, f.headP.z, g_psOrigin.x, g_psOrigin.y, g_psOrigin.z, f.headRel.x, f.headRel.y, f.headRel.z, bodyOff.x, bodyOff.y, bodyOff.z,
                 Rad2Deg(WrapPi(f.chrYaw - f.baseYaw)), Rad2Deg(WrapPi(f.bodyYaw - f.baseYaw)), Rad2Deg(f.headYaw),
                 Rad2Deg(WrapPi(BodyFacingYaw(rootQ, w) - f.baseYaw)), Rad2Deg(WrapPi(ShoulderLineYaw(w) - f.baseYaw)), (int)g_shoulderFlip, (int)f.lockedOn);
        }
    }
}

// ---------------------------------------------------------------- hooks
bool FirstPersonActive() { return Cfg().fpEnabled && MainPlayer() != nullptr; }

int SehFilter(unsigned code) { LOGE("[game] exception 0x%08X inside hook; disabling first person for safety", code); return EXCEPTION_EXECUTE_HANDLER; }
bool g_tripped = false;

void DumpMatrix(const char* tag, uint8_t* cam) {
    float m[16]; if (!cam || !SafeRead(cam + off::Cam_Matrix, m, sizeof(m))) { LOGD("[cam] %s: unreadable", tag); return; }
    LOGD("[cam] %s r(%.2f,%.2f,%.2f,%.2f) u(%.2f,%.2f,%.2f,%.2f) f(%.2f,%.2f,%.2f,%.2f) p(%.2f,%.2f,%.2f,%.2f) fov %.3f", tag,
         m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], *(float*)(cam + off::Cam_Fov));
}

double g_lastCamDump = 0;
float g_lastWrittenYaw = 0.f; bool g_haveWrittenYaw = false;
long g_camForeign = 0, g_camChecks = 0;   // how often the chr-cam held a yaw other than ours when we came to write

// Which camera machinery re-runs in the draw-only second pass. The renderer captures its
// camera SOMEWHERE the pass-2 keep set does not cover (writing every camera object at
// camera-step time in pass 2 changes nothing on screen); a hook that fires in pass 1 but
// never in pass 2 marks gated machinery - the capture point is downstream of one of those.
struct PassCount { volatile long p1 = 0, p2 = 0; };
PassCount g_cntFinalize, g_cntFollow, g_cntUpdFollow, g_cntMms, g_cntModelPos, g_cntPoseEval;
void CountPass(PassCount& c) {
    InterlockedIncrement(FramePumpCurrentPass() == 2 ? &c.p2 : &c.p1);
}
// Returns true when our camera was written.
bool CameraStepBody(bool& skipOriginal, bool afterOriginal) {
    const Config& c = Cfg();
    uint8_t* player = MainPlayer(); if (!player) return false;
    FrameInfo f; if (!BuildFrame(player, f)) return false;
    if (g_camWallRecovery && c.fpCameraMode != "passthrough") WR(g_camWallRecovery, (uint8_t)0);   // the game refreshes this global; keep it off
    if (g_haveWrittenYaw) {
        float y = GameCamYaw(); ++g_camChecks;
        const bool foreignYaw = std::fabs(WrapPi(y - g_lastWrittenYaw)) > Deg2Rad(1.f);
        if (foreignYaw) ++g_camForeign;
        // Takeover detector, yaw edition (2026-08-31): the 14:31 session's map takeover wrote
        // its own YAW every frame but left our FOV alone, so the FOV majority below never
        // armed and the menu gate missed the whole map. LOG-ONLY (2026-08-31 16:38 session):
        // yaw cannot reuse the FOV detector's 16-of-40 majority, because in ordinary play the
        // game's final-camera smoother re-writes yaw AFTER us every frame (the long-standing
        // "foreign camera yaw in N of N writes" warning) - the window saturates the moment the
        // world loads and GameERForeignCamActive() never decays: pass 2 permanently held
        // ("stays in cinema mode"), combat gated all session. FOV separates cleanly (gameplay
        // blips <= 9% of reads); yaw is 100% foreign in gameplay, so majority-of-writes has no
        // discriminating power. This block only samples the delta magnitudes so a usable yaw
        // signal (if one exists) can be tuned from logs; it must NOT feed g_foreignFovTick.
        {
            static float s_maxDelta = 0.f; static long s_n = 0; static ULONGLONG s_lastLog = 0;
            if (foreignYaw && !f.lockedOn) {
                const float d = Rad2Deg(std::fabs(WrapPi(y - g_lastWrittenYaw)));
                if (d > s_maxDelta) s_maxDelta = d;
                ++s_n;
                const ULONGLONG t = GetTickCount64();
                if (t - s_lastLog > 30000) {
                    s_lastLog = t;
                    LOGI("[cam] yaw-takeover probe: %ld foreign-yaw reads (not locked on), max delta %.1f deg since last report", s_n, s_maxDelta);
                    s_maxDelta = 0.f; s_n = 0;
                }
            }
        }
        // The FINAL render camera, position included - the chr-cam yaw check cannot see position smoothing there.
        if (uint8_t* pc = PersCam1()) {
            float m[16];
            if (SafeRead(pc + off::Cam_Matrix, m, sizeof(m))) {
                if (std::fabs(WrapPi(std::atan2(m[8], m[10]) - g_lastWrittenYaw)) > Deg2Rad(1.f)) InterlockedIncrement(&g_finalYawForeign);
                Vec3 dp{m[12] - g_lastWrittenPos.x, m[13] - g_lastWrittenPos.y, m[14] - g_lastWrittenPos.z};
                if (dp.Length() > 0.05f) InterlockedIncrement(&g_finalPosForeign);
            }
        }
    }
    // Always-on watchdog: if the chr-cam regularly holds someone else's yaw when we come to write, an unhooked
    // camera writer is still active - that is exactly what "the view orbits while strafing / the turn stick does
    // nothing" looks like, so say it loudly instead of hiding it behind Debug=true.
    {
        static double s_win = 0; double nw = NowSec();
        if (nw - s_win > 5.0) {
            if (g_camChecks > 50 && g_camForeign * 10 >= g_camChecks * 3)
                LOGW("[cam] foreign camera yaw in %ld of %ld writes in the last 5 s - an unhooked writer (lock-on camera?) is moving the view. FaceCameraMode=strafe avoids the game's lock-on camera.", g_camForeign, g_camChecks);
            long fy = InterlockedExchange(&g_finalYawForeign, 0), fp = InterlockedExchange(&g_finalPosForeign, 0);
            if (fy + fp) LOGI("[cam] final render camera differed from our write: yaw off in %ld, position off (>5 cm) in %ld of %ld checks (5 s) - the game step re-smooths the final camera", fy, fp, g_camChecks);
            // Per-pass camera-machinery census (only meaningful while the pump runs).
            {
                long f1 = InterlockedExchange(&g_cntFinalize.p1, 0), f2 = InterlockedExchange(&g_cntFinalize.p2, 0);
                long o1 = InterlockedExchange(&g_cntFollow.p1, 0),   o2 = InterlockedExchange(&g_cntFollow.p2, 0);
                long u1 = InterlockedExchange(&g_cntUpdFollow.p1, 0),u2 = InterlockedExchange(&g_cntUpdFollow.p2, 0);
                long m1 = InterlockedExchange(&g_cntMms.p1, 0),      m2 = InterlockedExchange(&g_cntMms.p2, 0);
                long d1 = InterlockedExchange(&g_cntModelPos.p1, 0), d2 = InterlockedExchange(&g_cntModelPos.p2, 0);
                long e1 = InterlockedExchange(&g_cntPoseEval.p1, 0), e2 = InterlockedExchange(&g_cntPoseEval.p2, 0);
                if (f2 + o2 + u2 + m2 + d2 + e2 + f1 + o1 + u1 + m1 + d1 + e1)
                    LOGI("[cam] 5s hook calls pass1/pass2: finalize %ld/%ld follow %ld/%ld updFollow %ld/%ld mms %ld/%ld modelPos %ld/%ld poseEval %ld/%ld",
                         f1, f2, o1, o2, u1, u2, m1, m2, d1, d2, e1, e2);
            }
            g_camForeign = 0; g_camChecks = 0; s_win = nw;
        }
    }
    bool dump = c.fpDebug && (NowSec() - g_lastCamDump > 3.0);
    if (dump && !afterOriginal) {
        g_lastCamDump = NowSec(); DumpMatrix("game chr_cam (before write)", ChrCam()); DumpMatrix("game pers_cam_1 (before write)", PersCam1());
        LOGD("[cam] foreign yaw found before %ld of %ld writes in this window; our yaw %.1f", g_camForeign, g_camChecks, Rad2Deg(f.baseYaw));
    }
    // fov-semantics probe: with ForceFovDeg armed, dump the pers-cam struct as floats a few
    // times, ~10 s in (after the late resize AND with our fov write long since applied). The
    // projection terms live somewhere in here - m11 = 1/tan(vFov/2) and m00 = m11/aspect -
    // and WHICH aspect appears (buffer 0.94 / 1.78 vs boot 1.78 vs desktop 2.39) answers,
    // from memory instead of screenshots, which height a WRITTEN fov applies across.
    if (c.fpForceFovDeg > 1.f) {
        static int s_dumps = 0; static double s_lastStructDump = 0;
        if (s_dumps < 3 && NowSec() - s_lastStructDump > 12.0) {
            ++s_dumps; s_lastStructDump = NowSec();
            if (uint8_t* pc = PersCam1()) {
                float v[80];
                if (SafeRead(pc, v, sizeof(v))) {
                    char line[1024]; int p = 0;
                    for (int i = 0; i < 80 && p < (int)sizeof(line) - 16; ++i)
                        p += snprintf(line + p, sizeof(line) - p, "%.4g ", v[i]);
                    LOGI("[cam] pers_cam_1 floats +0x00..+0x140: %s", line);
                }
            }
        }
    }
    ApplyHeadVisibility(player, c.fpHideHead, c.fpHideSheathed);
    ApplyTorrentVisibility(f.mount);
    if (c.fpCameraMode == "passthrough") { skipOriginal = false; return false; }
    if (c.fpCameraMode == "post" && !afterOriginal) { skipOriginal = false; return false; }   // come back after the original
    if (afterOriginal) {
        // Final word on the pose. The camera step runs after all Havok evaluation in the frame (libER taskgroup
        // order: HavokBehavior .. HavokClothUpdate -> CameraStep -> render), so a write here beats the upper-body
        // animation layer (attacks, aim) that evaluates after the pose hook and was still showing on arms/shoulders.
        if (c.handTracking) UpdateArms(player, f, 1);
        // UpdateArms may capture a stance and refresh f's anchor; publish that same height.
        UpdateCamera(player, f); g_lastWrittenYaw = f.baseYaw; g_haveWrittenYaw = true;
        return true;
    }
    UpdateCamera(player, f);
    // fov-semantics probe, part 2 ([FirstPerson] ForcePitchDeg): pitch the written camera a
    // KNOWN angle; a landmark's on-screen displacement divided by it measures px/deg
    // directly, immune to camera position. Flat harness only; hot-reloads for one-session
    // before/after shots.
    if (c.fpForcePitchDeg != 0.f) GameERDebugPitchCamera(c.fpForcePitchDeg);
    g_lastWrittenYaw = f.baseYaw; g_haveWrittenYaw = true;
    if (dump) { DumpMatrix("ervr chr_cam (after write)", ChrCam()); DumpMatrix("ervr pers_cam_1 (after write)", PersCam1()); }
    skipOriginal = (c.fpCameraMode == "block");
    g_status.playerFound = true;
    return true;
}
// The game's follow cam has yaw state of its own (it turns behind a moving character) and writes it into the
// chr-cam from more than one place per frame: skipping the chr-cam update alone left the follow-cam output in
// the matrix at camera-step time, and the view flip-flopped between the game's yaw and ours every frame ("camera
// spinning like crazy"). So the rule is simple: after *every* place the game writes the camera that we know of
// (follow-cam function, chr-cam update, camera step), ours goes straight back in, and the dump counts how often
// a foreign matrix was still found before a write (a non-zero count = a writer we do not hook yet).
void ReassertCamera() {
    if (g_tripped || !FirstPersonActive() || Cfg().fpCameraMode != "pre") return;
    __try { bool d; CameraStepBody(d, false); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
}
void __fastcall Hook_MmsUpdateChrCam(void* a) {
    CountPass(g_cntMms);
    g_origMms(a);
    ReassertCamera();
}
void __fastcall Hook_FollowCamFollow(void* fc, float dt, void* c) {
    CountPass(g_cntFollow);
    g_followCamPtr.store(fc, std::memory_order_relaxed);
    // Setting reset_camera disables the follow cam's position interpolation (erfps2 trick) so it stops fighting us.
    uint8_t saved = 0; bool have = false;
    if (!g_tripped && FirstPersonActive() && Cfg().fpCameraMode == "pre") {
        have = RD(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, saved); if (have) WR(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, (uint8_t)1);
        // The rest of erfps2's first-person follow-cam neutralization: no auto-rotation behind a moving
        // character, no lateral lock-on camera offset, no wall-recovery pushes. All three are camera writers
        // that run *after* our matrix writes and used to show up as the view orbiting / sliding on its own.
        WR(((uint8_t*)fc) + OFF_FollowCam_AutoRotation, (uint8_t)0);
        float zero = 0.f; SafeWrite(((uint8_t*)fc) + OFF_FollowCam_LockedOffset, &zero, 4);
        if (g_camWallRecovery) WR(g_camWallRecovery, (uint8_t)0);
    }
    g_origFollow(fc, dt, c);
    if (have) WR(((uint8_t*)fc) + OFF_FollowCam_ResetCamera, saved);
    if (Cfg().fpOwnChrCam) ReassertCamera();   // replace its output at the source
}
// Outer follow-cam update (erfps2's UPDATE_FOLLOW_CAM): it keeps writing camera output *after*
// ChrExFollowCam::follow returns, which was the writer the watchdog kept counting while moving.
void __fastcall Hook_UpdateFollowCam(void* fc) {
    CountPass(g_cntUpdFollow);
    g_followCamPtr.store(fc, std::memory_order_relaxed);
    g_origUpdFollow(fc);
    if (Cfg().fpOwnChrCam) ReassertCamera();
}
// After the game's lock-target update: re-force the cast-aim lock state (flag + ray target) it just rewrote.
// Only while a cast is live - outside one the lock mode's flat target is re-asserted by the other hooks as before.
void __fastcall Hook_UpdateLockTgt(void* man, float dt) {
    g_origUpdLockTgt(man, dt);
    if (!Cfg().castAim) return;
    bool casting = false;
    if (CombatCastAimHand(&casting) < 0 || !casting) return;
    uint8_t* player = MainPlayer(); if (!player) return;
    FrameInfo f; if (!BuildFrame(player, f)) return;
    ReassertFaceCamera(player, f);
}
// Replay our camera after the game's own camera finalizer. Cheap by design: no frame rebuild, no bone solve -
// just the matrix we already computed, written back into every camera object we know of.
void ReassertCameraMatrix() {
    if (WorldDyingNow()) return;   // stand down with every other camera write
    if (!g_haveCamMtx.load(std::memory_order_relaxed)) return;
    float m[16];
    { std::lock_guard<std::mutex> g(g_camMtxLock); memcpy(m, g_lastCamMtx, sizeof(m)); }
    uint8_t* pcs[4]; int n = AllPersCams(pcs);
    for (int i = 0; i < n; ++i) SafeWrite(pcs[i] + off::Cam_Matrix, m, sizeof(m));
    if (uint8_t* cc = ChrCam()) SafeWrite(cc + off::Cam_Matrix, m, sizeof(m));
    if (uint8_t* fc = FollowCam()) SafeWrite(fc + off::Cam_Matrix, m, sizeof(m));
}
// Pass-1 finalize call, captured fresh every frame for the pass-2 replay below. Pass 1
// always runs the full camera chain natively (our pass-2 gating never touches pass 1), so
// these are at most one frame old and their objects are the live camera system.
struct { std::atomic<void*> a{nullptr}, b{nullptr}, c{nullptr}, d{nullptr};
         std::atomic<ULONGLONG> tick{0}; } g_finArgs;

uint64_t __fastcall Hook_CamFinalize(void* a, void* b, void* c, void* d) {
    CountPass(g_cntFinalize);
    if (FramePumpCurrentPass() != 2) {
        g_finArgs.a.store(a, std::memory_order_relaxed);
        g_finArgs.b.store(b, std::memory_order_relaxed);
        g_finArgs.c.store(c, std::memory_order_relaxed);
        g_finArgs.d.store(d, std::memory_order_relaxed);
        g_finArgs.tick.store(GetTickCount64(), std::memory_order_release);
    }
    uint64_t rv = g_origCamFinalize(a, b, c, d);
    if (g_tripped || !FirstPersonActive() || !Cfg().fpCameraFinalWrite) return rv;
    __try { ReassertCameraMatrix(); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    return rv;
}

// The pair's rebuilt model matrices, per pass: pass 1's rider placement is what both eyes draw (RiderPassHold);
// how far pass 2 re-places the rider and the horse goes into the "[ride]" summary.
void NoteRidePass(uint8_t* chr, bool isMount) {
    Vec3 p; Quat q;
    if (!chr || !ModelMatrixRoot(chr, p, q)) return;
    const bool pass2 = FramePumpCurrentPass() == 2;
    static Vec3 s_torrent{}; static uint32_t s_torrentFrame = ~0u;
    if (isMount) {
        if (!pass2) { s_torrent = p; s_torrentFrame = g_frame; }
        else if (s_torrentFrame == g_frame) {
            std::lock_guard<std::mutex> g(g_rideStats.mtx);
            g_rideStats.torrentShift = std::max(g_rideStats.torrentShift, (p - s_torrent).Length());
        }
        return;
    }
    const Quat yaw = HeadingQ(q);
    Vec3 d; bool held;
    {
        std::lock_guard<std::mutex> g(g_riderHoldMtx);
        const uint32_t pair = RiderHoldPair();
        if (!pass2) { g_riderHold.Pass1(p, q, pair, NowSec()); return; }
        held = g_riderHold.Held(pair, NowSec());
        if (!held) return;
        d = g_riderHold.Pass2(p, yaw, pair);
    }
    const Vec3 l = yaw.Conj().Rotate(d);   // rider frame: x = side, y = up, -z = forward
    const float axis[3] = {-l.z, l.y, l.x};
    std::lock_guard<std::mutex> g(g_rideStats.mtx);
    float* keep[3] = {&g_rideStats.riderShift.z, &g_rideStats.riderShift.y, &g_rideStats.riderShift.x};
    for (int i = 0; i < 3; ++i) if (std::fabs(axis[i]) > std::fabs(*keep[i])) *keep[i] = axis[i];
    ++g_rideStats.pass2;
}

// Every 5 s on horseback: whether the horse was found, which body rides, how far the smoothed view sat off the
// bouncing rider, how far the second eye's pass re-placed rider and horse, and what turned the pair.
void LogRideSummary(const FrameInfo& f) {
    static double s_last = 0;
    const double now = NowSec();
    if (!f.riding) { s_last = now; return; }
    if (now - s_last < 5.0) return;
    s_last = now;
    float stick, physical, applied, camDev, torrentShift; Vec3 shift; int pass2;
    {
        std::lock_guard<std::mutex> g(g_rideStats.mtx);
        stick = g_rideStats.stick; physical = g_rideStats.physical; applied = g_rideStats.applied;
        camDev = g_rideStats.camDev; torrentShift = g_rideStats.torrentShift; shift = g_rideStats.riderShift; pass2 = g_rideStats.pass2;
        g_rideStats.stick = g_rideStats.physical = g_rideStats.applied = g_rideStats.camDev = g_rideStats.torrentShift = 0.f;
        g_rideStats.riderShift = {}; g_rideStats.pass2 = 0;
    }
    LOGI("[ride] 5 s: Torrent %s | %s | view smoothed up to %.0f mm off the bouncing rider | second eye re-placed the rider up to "
         "forward %+.0f up %+.0f side %+.0f mm (%d frames)%s, Torrent %.0f mm | turns: stick %.0f deg, physical %.0f deg, applied to the pair %.0f deg",
         f.mount ? "found" : "NOT FOUND",
         f.seatPose ? "seated pose" : f.seated ? "seated, live animation until a still moment gives the seated pose" : "mount/dismount animation",
         camDev * 1000.f, shift.z * 1000.f, shift.y * 1000.f, shift.x * 1000.f, pass2,
         f.seatPose ? " - drawn where the first eye drew it" : "", torrentShift * 1000.f,
         Rad2Deg(stick), Rad2Deg(physical), Rad2Deg(applied));
}

// Per-character model-matrix update, late in the frame. The pose probe measured the upper-body animation layer
// (walk sway, weapon arts) evaluating every frame AFTER the camera step - this hook runs later still, so the
// pose written here is what the renderer actually skins.
void UpdateChrModelPosBody(void* ctrl, bool post) {
    uint8_t* player = MainPlayer();
    if (!player) return;
    const bool isPlayer = Deref(player + off::Chr_ChrCtrl) == (uint8_t*)ctrl;
    uint8_t* mount = g_moveRiding.load() ? CurrentTorrent(player) : nullptr;
    const bool isMount = mount && Deref(mount + off::Chr_ChrCtrl) == (uint8_t*)ctrl;
    if (!isPlayer && !isMount) return;
    // Whichever member of the pair rebuilds first consumes the stick turn. Both
    // native model updates then see the new physics transforms in the same frame.
    if (!post)
        ApplyMountedTurn(player, mount, g_mountedTurns.Take(mount, g_frame, FramePumpCurrentPass() != 2));
    ApplyTorrentVisibility(mount);
    if (post && g_moveRiding.load()) NoteRidePass(isMount ? mount : player, isMount);
    if (!isPlayer) return;
    if (post) {
        // Skew probe: the model matrix the rig renders with this frame vs the root our camera write used.
        float p[4];
        if (SafeRead((uint8_t*)ctrl + off::Ctrl_ModelMatrix + 48, p, 16)) {
            Vec3 d{p[0] - g_lastCamRoot.x, 0.f, p[2] - g_lastCamRoot.z};
            float h = d.Length();
            if (h > g_skewMax) g_skewMax = h;
        }
        static double s_last = 0; double nw = NowSec();
        if (nw - s_last > 5.0) {
            s_last = nw;
            if (g_skewMax > 0.005f) LOGI("[cam] rig-root vs camera-root horizontal skew max %.0f mm (5 s) - nonzero while moving means the camera write used a stale root", g_skewMax * 1000.f);
            g_skewMax = 0.f;
        }
    }
    FrameInfo f; if (!BuildFrame(player, f, post)) return;
    // Last word on the facing before the original rebuilds the model matrix FROM the physics orientation.
    // The pin written at the pose hook / camera step can be overwritten by the game's own turn-toward-move
    // logic later in the frame (it only runs while moving), and then the matrix - the rotation the renderer
    // skins the rig with - carries the game's facing while the pose was solved in ours: the whole rig renders
    // rotated by the difference, but only while moving. Re-pinning here makes the rebuild read our facing.
    if (!post && Cfg().fpFaceCameraMode == "strafe") ReassertFaceCamera(player, f);
    // Keep the rebuilt model matrix native. UpdateArms expresses its solved pose in
    // that frame, so visual facing no longer changes the game's movement axes.
    if (Cfg().handTracking) UpdateArms(player, f, 2);
    // Camera last, from the model matrix that was just rebuilt - the very transform the rig renders with. The
    // camera step runs earlier in the frame, so a camera written there sits on a root that is up to a frame of
    // motion away from where the body ends up: the body then appears to slide toward a camera that looks static
    // while moving, and to snap into place when you stop. Writing here makes the two exact by construction.
    if (post && Cfg().fpCameraAfterModelPos) UpdateCamera(player, f);
    // Body-vs-camera probe: where the RENDERED head bone actually ends up versus where we put the eye, in the
    // view's own axes. "Disconnected when strafing" is a lateral number; if this reads near zero while the
    // disconnect is still visible, our placement is correct and the separation is produced downstream (R.E.A.L.
    // renders one eye per frame, and lateral motion is exactly where that shows).
    if (post) {
        // Each eye against its own camera: pass 2 renders the second eye's.
        const int eye = FramePumpCurrentPass() == 2 ? 1 : 0;
        Vec3 eyePos = g_lastWrittenPos; bool haveEye = true;
        if (eye == 1) { std::lock_guard<std::mutex> g(g_secondEye.mtx); haveEye = g_secondEye.camValid; eyePos = g_secondEye.camPos; }
        Vec3 hw;
        Vec3 renderPos = f.root; Quat renderQ = f.rootQ;
        ModelMatrixRoot(player, renderPos, renderQ);
        if (haveEye && HeadBoneWorld(player, renderPos, renderQ, hw)) {
            Vec3 d = hw - eyePos;
            float cy = std::cos(f.baseYaw), sy = std::sin(f.baseYaw);
            float lat = d.x * cy - d.z * sy, fwd = d.x * sy + d.z * cy;
            if (std::fabs(lat) > std::fabs(g_bodyCamLat[eye])) g_bodyCamLat[eye] = lat;
            if (std::fabs(fwd) > std::fabs(g_bodyCamFwd[eye])) g_bodyCamFwd[eye] = fwd;
            if (std::fabs(d.y) > std::fabs(g_bodyCamUp[eye])) g_bodyCamUp[eye] = d.y;
        }
        // Eye height is the camera anchor over the feet: the probe above cannot see a template that took the camera
        // down WITH the body (it stays a constant few cm under the eye), this can.
        const float camH = f.anchor.y - f.root.y;
        if (camH < g_camHMin) g_camHMin = camH;
        if (camH > g_camHMax) g_camHMax = camH;
        static double s_bc = 0; double nb = NowSec();
        if (nb - s_bc > 5.0) {
            s_bc = nb;
            const bool lowCam = !f.riding && g_camHMin < Cfg().fpEyeHeight - 0.15f;
            bool off = lowCam;
            for (int e = 0; e < 2; ++e) off = off || std::fabs(g_bodyCamLat[e]) > 0.02f || std::fabs(g_bodyCamFwd[e]) > 0.02f || std::fabs(g_bodyCamUp[e]) > 0.02f;
            if (off)
                LOGI("[body] rendered head vs camera eye (5 s max): lateral %+.0f mm, forward %+.0f mm, vertical %+.0f mm | second eye %+.0f / %+.0f / %+.0f mm | camera %.2f-%.2f m above the %s, template %s (head %.2f m, %s)",
                     g_bodyCamLat[0] * 1000.f, g_bodyCamFwd[0] * 1000.f, g_bodyCamUp[0] * 1000.f,
                     g_bodyCamLat[1] * 1000.f, g_bodyCamFwd[1] * 1000.f, g_bodyCamUp[1] * 1000.f, g_camHMin, g_camHMax,
                     f.seatPose ? "seated pose's root" : f.riding ? "rider's root" : "feet",
                     f.seatPose ? "seated" : f.riding ? "unused (riding, live animation)" : g_templLow ? "LOW" : "standing", g_templHead, g_templWhy);
            for (int e = 0; e < 2; ++e) g_bodyCamLat[e] = g_bodyCamFwd[e] = g_bodyCamUp[e] = 0.f;
            g_camHMin = 1e9f; g_camHMax = -1e9f;
        }
        if (eye == 0) LogRideSummary(f);
        g_vrCycle.fetch_add(1, std::memory_order_relaxed);   // next cycle takes a fresh head sample
    }
}
// Post-Havok pose reassert. This function runs for every character, so the common case must be nearly free:
// read one bone, and only if it is no longer what we wrote does the full solve run (once per frame, player only).
void RewriteIfClobbered() {
    if (g_skel.boneCount <= 0 || g_probeLastPoint < 0) return;
    int probe = g_skel.R_ua; if (probe < 0) return;
    HkQsTransform* pose = g_skel.LivePose(); if (!pose) return;
    float q[4]; if (!SafeRead(&pose[probe].q[0], q, 16)) return;
    float d = std::fabs(q[0] - g_probeQ.x) + std::fabs(q[1] - g_probeQ.y) + std::fabs(q[2] - g_probeQ.z) + std::fabs(q[3] - g_probeQ.w);
    if (d < 1e-4f) return;   // still ours: the animation has not touched the player this call
    uint8_t* player = MainPlayer(); if (!player) return;
    FrameInfo f; if (!BuildFrame(player, f)) return;
    UpdateArms(player, f, 3);
}
uint64_t __fastcall Hook_PoseEval(void* a, void* b, void* c, void* d) {
    CountPass(g_cntPoseEval);
    uint64_t rv = g_origPoseEval(a, b, c, d);
    if (g_tripped || !FirstPersonActive() || !Cfg().fpPoseAfterHavok || !Cfg().handTracking) return rv;
    __try { RewriteIfClobbered(); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    return rv;
}
void KineticModelPosGuarded(void* ctrl, bool post);
void __fastcall Hook_UpdateChrModelPos(void* ctrl) {
    CountPass(g_cntModelPos);
    // Kinetic: enemy bone overrides go in here, before the renderer consumes this character's pose.
    if (kinetic::PhysEnabled() && !GameERWorldDying()) KineticModelPosGuarded(ctrl, false);
    // Pre-write as well: the buffer measured OURS after this hook still rendered the layer's animation, so the
    // renderer's pose consumption happens INSIDE this function - the pose present at entry is what gets skinned.
    // The upper-body layer ran just before us; put ours back first, then let the original consume it.
    bool act = !g_tripped && FirstPersonActive() && (Cfg().handTracking || g_moveRiding.load());
    if (act) { __try { UpdateChrModelPosBody(ctrl, false); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; act = false; } }
    g_origUpdModelPos(ctrl);
    if (kinetic::PhysEnabled() && !GameERWorldDying()) KineticModelPosGuarded(ctrl, true);
    if (act) { __try { UpdateChrModelPosBody(ctrl, true); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; } }
}
void HookCameraStepImpl(void* a, void* b);
// Near-camera dither fade. The renderer asks, per drawn part, for the part's dither entry and dithers it
// when entry.nearEnd exceeds the part's distance from the camera; the pixel shader then gets
// 1/(nearEnd-nearStart) and fades the part across that band. Tuned for a third-person camera metres
// behind the character, the band reaches well past a head-mounted one: walls, doorways and anything you
// lean toward go see-through long before you touch them. NearDither=0 answers "no entry" (erfps2's
// first-person behaviour, no fade at all); 0<s<1 scales the band on the copy the game just made - *out
// is the caller's stack local, so the game's own table is never modified. Render-thread hot path: only
// atomics are read here.
bool __fastcall Hook_UsesDithering(void* mgr, void* out, uint32_t id) {
    if (!g_ditherFp.load(std::memory_order_relaxed)) return g_origUsesDithering(mgr, out, id);
    const float scale = g_ditherScale.load(std::memory_order_relaxed);
    if (scale <= 0.f) return false;
    const bool hit = g_origUsesDithering(mgr, out, id);
    if (hit && out) {
        float* f = (float*)out;
        static std::atomic<int> s_logged{0};   // confirm the entry layout from live data, a few times
        if (s_logged.load(std::memory_order_relaxed) < 4 && s_logged.fetch_add(1) < 4)
            LOGI("[dither] entry id %u: a0 %.3f a1 %.3f near %.3f..%.3f x 0x%08X (NearDither %.2f)",
                 id, f[1], f[2], f[3], f[4], ((uint32_t*)out)[5], scale);
        if (scale < 1.f) { f[3] *= scale; f[4] *= scale; }
    }
    return hit;
}
void __fastcall Hook_CameraStep(void* a, void* b) {
    InterlockedIncrement(&g_camCalls);
    ScopedTicks st(&g_camTicks);
    HookCameraStepImpl(a, b);
}
void HookCameraStepImpl(void* a, void* b) {
    // Pass 2 is the SAME sim frame drawn again - keep every per-frame cache (BuildFrame,
    // eye latch) on pass 1's values by not advancing the frame counter.
    if (FramePumpCurrentPass() != 2) ++g_frame;
    bool skip = false;
    const bool fpOn = !g_tripped && FirstPersonActive();
    g_ditherFp.store(fpOn, std::memory_order_relaxed);
    g_ditherScale.store(Cfg().fpNearDither, std::memory_order_relaxed);
    if (fpOn) {
        __try { CameraStepBody(skip, false); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; skip = false; }
    }
    if (skip) return;
    g_origCameraStep(a, b);
    if (!g_tripped && FirstPersonActive() && (Cfg().fpCameraMode == "post" || Cfg().fpCameraMode == "pre")) {
        __try { bool dummy; CameraStepBody(dummy, true); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    }
    // FINALIZE REPLAY: under the SAFE keep set the camera finalizer's native caller (the
    // GameFlowInGame_MoveMap_PostPhysics family) is gated out of pass 2, so the second-eye
    // camera written above never reaches the renderer's view state - the "no depth, same
    // eye twice" failure. Pass 1 captured the finalizer's arguments this very frame; calling
    // the game's own finalizer here, after the second-eye write, propagates eye 2 without
    // running ANY of the MoveMap family's world/grid walking - which is the entire reason
    // that family is gated. Only when the SAFE set is live (native finalize covers FULL).
    if (FramePumpCurrentPass() == 2 && Cfg().vrPass2FinalizeReplay && FramePumpSafeSetActive() &&
        g_origCamFinalize && !g_tripped) {
        const ULONGLONG t = g_finArgs.tick.load(std::memory_order_acquire);
        if (t && GetTickCount64() - t < 200) {
            __try {
                g_origCamFinalize(g_finArgs.a.load(std::memory_order_relaxed),
                                  g_finArgs.b.load(std::memory_order_relaxed),
                                  g_finArgs.c.load(std::memory_order_relaxed),
                                  g_finArgs.d.load(std::memory_order_relaxed));
                static std::atomic<bool> s_logged{false};
                if (!s_logged.exchange(true))
                    LOGI("[stereo] pass-2 finalize replay ACTIVE (SAFE set): second eye reaches "
                         "the renderer without the MoveMap family");
            } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
        }
    }
}

// Locomotion=direct: ERVR moves the character. Horizontal position integrated from the processed stick in the view
// frame; height left to the game; facing = view yaw. Only while the stick is pushed - idle, the game owns the
// character again so its own animations (roll, attack, ladder, mount) still displace it. No collision from us.
Vec3 g_locoPos{}; bool g_locoActive = false; double g_locoLast = 0; unsigned g_locoFrame = 0;
void DirectLocomotion(uint8_t* player, const FrameInfo& f) {
    const Config& c = Cfg();
    if (f.riding || c.locomotion != "direct") { g_locoActive = false; return; }
    // Do not move or re-orient the character while the world is dying (see WorldDyingNow).
    if (WorldDyingNow()) { g_locoActive = false; return; }
    if (g_locoFrame == g_frame) return; g_locoFrame = g_frame;   // once per rendered frame
    MoveIntent mi = VirtualPadGetMove();
    double now = NowSec(); float dt = Clamp((float)(now - g_locoLast), 0.f, 0.05f); g_locoLast = now;
    uint8_t* ph = Physics(player); if (!ph) return;
    float p[4]; if (!SafeRead(ph + off::Phys_Position, p, 16)) return;
    float mag = std::sqrt(mi.x * mi.x + mi.y * mi.y);
    if (!mi.valid || mag < 0.15f) { g_locoActive = false; return; }
    if (!g_locoActive) { g_locoPos = {p[0], p[1], p[2]}; g_locoActive = true; return; }
    if (mag > 1.f) { mi.x /= mag; mi.y /= mag; mag = 1.f; }
    float speed = (mi.sprint ? c.locoSprintSpeed : c.locoRunSpeed) * mag;
    // The stick arrives in the frame of the camera matrix the game sees (VirtualPadUpdate rotates it into that
    // frame), which includes the HMD yaw when CameraRotation=hmd - same rule as MoveBasis::viewYaw.
    float frameYaw = (f.headValid && c.fpCameraRotation == "hmd") ? WrapPi(f.baseYaw + f.headYaw) : f.baseYaw;
    Vec3 dir = Quat::YawQ(frameYaw).Rotate(Vec3{mi.x, 0.f, mi.y}); dir.y = 0.f;
    if (dir.Length() > 1e-4f) dir = dir.Normalized();
    g_locoPos = g_locoPos + dir * (speed * dt);
    // resync if the game moved us a long way (fall, teleport, ladder) so we never drag the character back
    Vec3 gp{p[0], p[1], p[2]};
    if ((Vec3{gp.x - g_locoPos.x, 0.f, gp.z - g_locoPos.z}).Length() > 1.5f) g_locoPos = gp;
    p[0] = g_locoPos.x; p[2] = g_locoPos.z; g_locoPos.y = p[1];
    SafeWrite(ph + off::Phys_Position, p, 16);
    // As in ApplyFaceCamera, dash/roll physics facing belongs to the game.
    if (!CanPinPlayerFacing(player)) return;
    // face the view (physics forward is local -Z); also pin the interpolation target or the game turns it back
    Quat q = Quat::YawQ(WrapPi(f.bodyYaw + 3.14159265f));
    float r[4] = {q.x, q.y, q.z, q.w};
    SafeWrite(ph + off::Phys_Orientation, r, 16);
    SafeWrite(ph + off::Phys_InterpOrient, r, 16);
}

void HkBody() {
    // The Havok update fires several times per rendered frame. Solving on every call is what keeps the arms
    // from being lost to a later animation evaluation (most visible while running); ArmSolveEveryCall=false
    // goes back to solving once per frame, which is cheaper but can drop a write.
    if (!Cfg().fpArmSolveEveryCall && g_status.cameraHooked && g_lastArmFrame == g_frame) return;
    g_lastArmFrame = g_frame;
    InterlockedIncrement(&g_hkSolves);
    ScopedTicks st(&g_hkTicks);
    uint8_t* player = MainPlayer(); if (!player) return;
    FrameInfo f; if (!BuildFrame(player, f)) return;
    // Re-assert the strafe flags here too: the camera hook runs once, at one point in the frame, and the game
    // is free to put the character back into free-turn before it reads them.
    ReassertFaceCamera(player, f);
    // Full-frame pass 2 re-runs the behavior/pose machinery at dt~0: re-applying the ARMS
    // is wanted (pass 2's render must show the VR pose too), but movement and combat are
    // once-per-SIM-frame actions and must not double-fire.
    if (FramePumpCurrentPass() != 2) DirectLocomotion(player, f);
    UpdateArms(player, f);
}
// Kinetic physics layer, once per pose-hook call. Its own SEH guard: a fault in the experimental layer
// turns the layer off for the session instead of taking first person down with it.
int KineticSehFilter(unsigned code) { LOGE("[phys] exception 0x%08X in the physics layer; turning it off", code); return EXCEPTION_EXECUTE_HANDLER; }
void KineticFrameGuarded(uint8_t* player, int pass) {
    __try { kinetic::PhysGameFrame(player, pass); }
    __except (KineticSehFilter(GetExceptionCode())) { kinetic::PhysTrip(); }
}
void KineticModelPosGuarded(void* ctrl, bool post) {
    __try { if (post) kinetic::PhysAfterModelPos(ctrl); else kinetic::PhysBeforeModelPos(ctrl); }
    __except (KineticSehFilter(GetExceptionCode())) { kinetic::PhysTrip(); }
}
void HkHandler(MidHookRegs*, void*) {
    InterlockedIncrement(&g_hkCalls);
    if (g_tripped || !Cfg().fpEnabled) return;
    if (Cfg().handTracking) {
        __try { HkBody(); }
        __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
    }
    // Physical combat: inject the AttackBehavior event once per frame after the arm pose is written.
    if (g_status.cameraHooked) {
        const int pass = FramePumpCurrentPass();
        uint8_t* player = MainPlayer();
        if (pass != 2) CombatGameFrame(player, g_frame);
        if (pass != 2) WeaponWheelGameFrame(player);   // the slots and icons the weapon wheel shows
        if (pass != 2) BeltGameFrame(player);          // the pouch items the belt shows, and what a use changed
        if (pass != 2) BodyHapticsGameFrame(player, g_frame);   // HP, the attacker's side, falls: the bHaptics vest
        CombatActionDiag(player, pass, g_frame);   // both passes: the press-vs-pass picture is the point
    }
    if (kinetic::PhysEnabled() && !GameERWorldDying()) KineticFrameGuarded(MainPlayer(), FramePumpCurrentPass());
}

// ---------------------------------------------------------------- init
// erfps2 binary-mapper patterns: '$' marks a rel32 whose target is the function.
uint8_t* ResolveCallTarget(const char* pattern, const char* name) {
    std::string pat(pattern); size_t dollar = pat.find('$');
    if (dollar == std::string::npos) return nullptr;
    // count bytes before '$'
    int idx = 0; { std::string pre = pat.substr(0, dollar); size_t i = 0; while (i < pre.size()) { while (i < pre.size() && pre[i] == ' ') ++i; if (i >= pre.size()) break; ++idx; while (i < pre.size() && pre[i] != ' ') ++i; } }
    std::string scan = pat; scan.replace(dollar, 1, "?? ?? ?? ??");
    uint8_t* hit = AobScan(scan);
    if (!hit) { LOGW("[game] AOB for %s not found", name); return nullptr; }
    int32_t rel; memcpy(&rel, hit + idx, 4);
    return hit + idx + 4 + rel;
}

} // namespace

bool GameERWeaponMotion(int hand, float* tipSpeed, float* handSpeed, float* arc) {
    if (hand != HandLeft && hand != HandRight) return false;
    std::lock_guard<std::mutex> g(g_inertiaMtx);
    const InertiaState& s = g_inertia[hand];
    if (!s.init || NowSec() - s.stamp > 0.1) return false;
    if (tipSpeed) *tipSpeed = s.tipSpeed;
    if (handSpeed) *handSpeed = s.handSpeed;
    if (arc) *arc = s.arc;
    return true;
}

int GameERPlayerLevel() {
    if (!g_gameDataManPtr) return -1;
    uint8_t* gdm = Deref(g_gameDataManPtr); if (!gdm) return -1;
    uint8_t* pgd = Deref(gdm + 0x8); if (!pgd) return -1;
    int level = -1; SafeRead(pgd + 0x68, &level, 4);
    return level;
}

// PlayerGameData stats are vigor, mind, endurance, strength... from +0x3C (see SetAllStats below).
int GameERChrPoseBuffers(uint8_t* chr, float* out[2]) {
    int n = 0;
    uint8_t* beh = chr ? Behavior(chr) : nullptr; if (!beh) return 0;
    for (size_t o : { off::Beh_Unk10, off::Beh_HkbContext }) {
        uint8_t* ctx = Deref(beh + o); if (!ctx) continue;
        uint8_t* hkb = Deref(ctx + off::Ctx_HkbCharacter); if (!hkb) continue;
        Skeleton tmp; tmp.hkb = hkb;
        float* pose = (float*)tmp.LivePose(); if (!pose) continue;
        if (n == 1 && out[0] == pose) continue;
        out[n++] = pose;
    }
    return n;
}

uint8_t* GameERChrCtrl(uint8_t* chr) { return chr ? Deref(chr + off::Chr_ChrCtrl) : nullptr; }

int GameERPlayerStrength() {
    uint8_t* pgd = GameERPlayerGameData(); if (!pgd) return -1;
    int str = -1; SafeRead(pgd + 0x48, &str, 4);
    return str;
}

uint8_t* GameERChrHkbCharacter(uint8_t* chr) { return chr ? FindHkbCharacter(chr) : nullptr; }

bool GameERHkbSkeleton(uint8_t* hkb, std::vector<std::string>& names, std::vector<int16_t>& parents) {
    if (!hkb) return false;
    uint8_t* setup = Deref(hkb + off::Hkb_Setup); uint8_t* hka = setup ? Deref(setup + off::Setup_Skeleton) : nullptr;
    int count = 0;
    if (!hka || !RD(hka + off::Hka_BoneCount, count) || count <= 0 || count > 1024) return false;
    char** layout = (char**)Deref(hka + off::Hka_NameLayout);
    int16_t* par = (int16_t*)Deref(hka + off::Hka_ParentIdx);
    if (!layout || !par) return false;
    names.assign(count, std::string()); parents.assign(count, -1);
    for (int i = 0; i < count; ++i) {
        char* nm = nullptr; RD(layout + i * 2, nm);
        char buf[64]{}; if (nm) SafeRead(nm, buf, 63);
        names[i] = buf;
        RD(par + i, parents[i]);
    }
    return true;
}

float* GameERHkbLivePose(uint8_t* hkb) {
    if (!hkb) return nullptr;
    Skeleton tmp; tmp.hkb = hkb;
    return (float*)tmp.LivePose();
}

uint8_t* GameERWorldChrMan() { return WorldChrMan(); }
uint8_t* GameERMainPlayer() { return MainPlayer(); }

bool GameERRiding() { return g_moveActive.load() && g_moveRiding.load(); }

bool GameERChrModelRoot(uint8_t* chr, Vec3* pos, Quat* rot) {
    Vec3 p; Quat q;
    if (!chr || !pos || !rot || !ModelMatrixRoot(chr, p, q)) return false;
    *pos = p; *rot = q;
    return true;
}

bool GameERPlayToWorld(int hand, Vec3* origin, Quat* rot, float* scale) {
    if (hand != HandLeft && hand != HandRight) return false;
    std::lock_guard<std::mutex> g(g_bladeMtx);
    const PlayToWorld& m = g_playToWorld[hand];
    if (!m.valid || NowSec() - m.stamp > 0.1) return false;
    *origin = m.origin; *rot = m.rot; *scale = m.scale;
    return true;
}

bool GameERBladeWorld(int hand, Vec3* grip, Vec3* tip) {
    if (hand != HandLeft && hand != HandRight) return false;
    std::lock_guard<std::mutex> g(g_bladeMtx);
    const BladeSample& b = g_blade[hand];
    if (!b.valid || NowSec() - b.stamp > 0.1) return false;
    if (grip) *grip = b.grip;
    if (tip) *tip = b.tip;
    return true;
}

bool GameERChrPosition(uint8_t* chr, Vec3* out) {
    Vec3 p;
    if (!chr || !out || !ModelMatrixPos(chr, p)) return false;
    *out = p;
    return true;
}

// Full stereo, pass 2: write the OTHER eye's camera into every slot the real pass wrote.
// Called by the frame pump between the two engine passes, on the main-loop thread, while
// every camera writer is quiescent (the camera step is gated for the pass that follows).
// Peek: is a second-eye camera stashed for this frame? Non-consuming - the frame pump uses
// it to tell "gameplay frame" (stash present every frame) from "menu/loading frame" (the
// game never stashes one there, by design). The stash gap IS the menu detector: the map,
// where fast travel is confirmed, produces a multi-second gap ending the instant the
// confirm closes it.
bool GameERSecondEyePending() {
    std::lock_guard<std::mutex> g(g_secondEye.mtx);
    return g_secondEye.valid;
}

// Tick of the last stash - unlike GameERSecondEyePending (consumed between the passes, so
// false through most of every frame) this reads the same from any thread at any point in
// the frame: fresh = gameplay, quiet for ~0.7 s+ = menu/loading/cinema.
unsigned long long GameERLastStashTick() {
    return g_lastStashTick.load(std::memory_order_relaxed);
}

bool GameERApplySecondEyeCamera() {
    bool haveHeadBasis; Quat qHead, rot; Vec3 br, bu, bf, posBase, offset;
    StereoEyeReport rep;
    {
        std::lock_guard<std::mutex> g(g_secondEye.mtx);
        if (!g_secondEye.valid) return false;
        g_secondEye.valid = false;   // consumed; UpdateCamera refills next frame
        g_stereoPair.fetch_add(1);   // pass 2 follows: it belongs to the pass 1 that just ran (RiderHoldPair)
        haveHeadBasis = g_secondEye.haveHeadBasis;
        qHead = g_secondEye.qHead; rot = g_secondEye.rot;
        br = g_secondEye.r; bu = g_secondEye.u; bf = g_secondEye.fw;
        posBase = g_secondEye.posBase; offset = g_secondEye.offset;
        rep.eye = g_secondEye.eye; rep.pair = g_secondEye.pair;
        rep.fovY = g_secondEye.fovY;
        rep.headPos = g_secondEye.headPos; rep.headRot = g_secondEye.headRot;
    }
    Vec3 r, u, fw, pos;
    if (haveHeadBasis) {
        Quat q = qHead * rot;   // per-eye cant, identity on most headsets
        // Diagnostic: tilt ONLY the second pass's camera. Whichever headset eye shows the
        // tilted world is the eye that receives pass 2's CONTENT - the 30-second test for
        // whether presents carry their own pass's render or lag one behind ([VR] PresentLag).
        if (DebugPass2PitchDeg() != 0.f)
            q = q * Quat::AxisAngle({1, 0, 0}, Deg2Rad(DebugPass2PitchDeg()));
        r = q.Rotate({1, 0, 0}); u = q.Rotate({0, 1, 0}); fw = q.Rotate({0, 0, 1});
        pos = posBase + qHead.Rotate(offset);   // offset lives in the HEAD frame, not the canted one
    } else {
        r = br; u = bu; fw = bf;
        if (DebugPass2PitchDeg() != 0.f) {
            const Quat pq = Quat::AxisAngle(r, Deg2Rad(DebugPass2PitchDeg()));
            u = pq.Rotate(u); fw = pq.Rotate(fw);
        }
        pos = posBase + r * offset.x + u * offset.y + fw * offset.z;
    }
    uint8_t* pcs[4]; const int npc = AllPersCams(pcs);
    for (int i = 0; i < npc; ++i) WriteCamMatrix(pcs[i] + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* cc = ChrCam()) WriteCamMatrix(cc + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* fc = FollowCam()) WriteCamMatrix(fc + off::Cam_Matrix, r, u, fw, pos);
    // Retain the finished basis: UpdateCamera re-applies it on every camera-step call during
    // pass 2 (the write above, alone, never reached the renderer - see SecondEyeStash).
    {
        std::lock_guard<std::mutex> g(g_secondEye.mtx);
        g_secondEye.camValid = true;
        g_secondEye.camR = r; g_secondEye.camU = u;
        g_secondEye.camF = fw; g_secondEye.camPos = pos;
    }
    // Held back until pass 2 ends - see PendingReport. The pump finalizes it right after
    // pass 2's queue_tasks returns, before the present that consumes it.
    StashPendingReport(rep, Quat::FromBasis(r, u, fw), pos);
    return true;
}

// Complete the pass's stashed report with the camera the renderer REALLY used and push it.
// Called by the frame pump on the frame thread right after each pass's queue_tasks returns
// (all camera writers and the smoother are done, the pass's Flip has presented, and the
// present that will consume this report has not run yet).
void GameERFinalizePassReport() {
    PendingReport pr;
    {
        std::lock_guard<std::mutex> g(g_pendingRepMtx);
        if (!g_pendingRep.valid) return;
        pr = g_pendingRep;
        g_pendingRep.valid = false;
    }
    static long s_corr = 0, s_skipped = 0; static float s_sumYawDeg = 0.f, s_sumPosCm = 0.f;
    if (Cfg().vrReportFinalCamera) {
        float m[16];
        uint8_t* pc = PersCam1();
        if (pc && SafeRead(pc + off::Cam_Matrix, m, sizeof(m))) {
            const Quat qFinal = Quat::FromBasis({m[0], m[1], m[2]}, {m[4], m[5], m[6]},
                                                {m[8], m[9], m[10]}).Normalized();
            const Vec3 pFinal{m[12], m[13], m[14]};
            Quat dq = (qFinal * pr.qWritten.Conj()).Normalized();   // world-frame delta, game space
            if (dq.w < 0.f) dq = {-dq.x, -dq.y, -dq.z, -dq.w};
            const Vec3 dp = pFinal - pr.pWritten;
            const float angDeg = Rad2Deg(2.f * std::acos(std::fmin(1.f, dq.w)));
            // A cut/rebase/teleport moves the camera wildly WITH the world - folding that
            // into the head pose would slam the layer. Correct only plausible smoothing.
            if (angDeg < 25.f && dp.Length() < 2.f) {
                const float ws = Clamp(Cfg().vrWorldScale, 0.25f, 4.f);
                const Quat dqXr = ToGame(dq).Normalized();
                const Vec3 dpXr = ToGame(dp) * (1.f / ws);
                pr.rep.headRot = (dqXr * pr.rep.headRot).Normalized();
                pr.rep.headPos = pr.rep.headPos + dpXr;
                ++s_corr; s_sumYawDeg += angDeg; s_sumPosCm += dp.Length() * 100.f;
            } else {
                ++s_skipped;
            }
        }
    }
    // 5 s digest: how far the game's smoother really moves the render camera off our write.
    static double s_win = 0; const double nw = NowSec();
    if (nw - s_win > 5.0) {
        if (s_corr || s_skipped)
            LOGD("[cam] report pose corrected to the FINAL render camera in %ld passes "
                 "(avg delta %.2f deg / %.1f cm), %ld skipped as cuts (5 s)",
                 s_corr, s_corr ? s_sumYawDeg / s_corr : 0.f, s_corr ? s_sumPosCm / s_corr : 0.f,
                 s_skipped);
        s_corr = s_skipped = 0; s_sumYawDeg = s_sumPosCm = 0.f; s_win = nw;
    }
    StereoReportEye(pr.rep);
}

// Content-probe experiment: rewrite every camera slot as the LAST written camera pitched by
// `pitchDeg` about its own right axis. Called between engine passes (same safe point as the
// second-eye write) to make pass 2's render visually distinguishable from pass 1's without
// any VR session - the flat harness then identifies which Present carries which pass.
bool GameERDebugPitchCamera(float pitchDeg) {
    if (!g_haveCamMtx.load(std::memory_order_relaxed)) return false;
    float m[16];
    { std::lock_guard<std::mutex> g(g_camMtxLock); memcpy(m, g_lastCamMtx, sizeof(m)); }
    Vec3 r{m[0], m[1], m[2]}, u{m[4], m[5], m[6]}, fw{m[8], m[9], m[10]}, pos{m[12], m[13], m[14]};
    const Quat pq = Quat::AxisAngle(r, Deg2Rad(pitchDeg));
    u = pq.Rotate(u); fw = pq.Rotate(fw);
    uint8_t* pcs[4]; const int npc = AllPersCams(pcs);
    if (npc == 0) return false;
    for (int i = 0; i < npc; ++i) WriteCamMatrix(pcs[i] + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* cc = ChrCam()) WriteCamMatrix(cc + off::Cam_Matrix, r, u, fw, pos);
    if (uint8_t* fc = FollowCam()) WriteCamMatrix(fc + off::Cam_Matrix, r, u, fw, pos);
    return true;
}

// CamWatch target: the first perspective camera - the chain TGA's freecam writes and the one
// whose matrix provably steers the render each frame, matrix through fov (0x10..0x54).
uint8_t* GameERCamWatchTarget(uint32_t* outLen) {
    uint8_t* pc = PersCam1();
    if (!pc) return nullptr;
    if (outLen) *outLen = (uint32_t)(off::Cam_Fov + 4 - off::Cam_Matrix);
    return pc + off::Cam_Matrix;
}

bool GameERInit() {
    g_base = (uint8_t*)GetModuleHandleW(nullptr);
    g_status.versionOk = CurrentGameBuild() != GameBuild::Unknown;
    posescan::Install();

    // Globals via classic AOBs.
    if (uint8_t* hit = AobScan("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 0F 48 39 88")) g_worldChrManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    if (uint8_t* hit = AobScan("48 8B 3D ?? ?? ?? ?? 49 8B D8 48 8B F2 4C 8B F1 48 85 FF")) g_fieldAreaPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    if (uint8_t* hit = AobScan("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3")) g_gameDataManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    // GameMan + CSEventFlagMan: the transition-authority pair (2026-08-23). GameMan holds
    // warp_requested (+0x10, fromsoftware-rs GameMan - the byte EVERY warp source sets on
    // its request frame); CSEventFlagMan's virtual-memory-flag block holds the community
    // "loading screen up" byte ([+0x28]+0x113, the one TGA's loading-screen tools poll).
    // AOBs are johndisandonato's practice-tool scans (aob_indirect_twice, rip at +3 len 7),
    // same shape as the WorldChrMan classic above.
    if (uint8_t* hit = AobScan("48 8B 1D ?? ?? ?? ?? 48 8B F8 48 85 DB 74 18 4C 8B 03")) g_gameManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    if (uint8_t* hit = AobScan("48 8B 3D ?? ?? ?? ?? 48 85 FF 74 ?? 48 8B 49")) g_eventFlagManPtr = (uint8_t**)ResolveRipRelative(hit, 3, 7);
    LOGI("[game] WorldChrMan* @ %p, FieldArea* @ %p, GameMan* @ %p, CSEventFlagMan* @ %p",
         (void*)g_worldChrManPtr, (void*)g_fieldAreaPtr, (void*)g_gameManPtr, (void*)g_eventFlagManPtr);
    if (!g_gameManPtr) LOGW("[game] GameMan AOB missed - warp-request guard degraded to heuristics (consider Pass2SafeOnA)");
    if (!g_worldChrManPtr || !g_fieldAreaPtr) { LOGW("[game] core globals not found; first person disabled"); return false; }

    // Functions: RVA when the version matches, else erfps2's AOBs.
    uint8_t* camStep = nullptr; uint8_t* getDmy = nullptr;
    if (g_status.versionOk) { camStep = g_base + GameRva(RVA_CameraStepUpdate); getDmy = g_base + GameRva(RVA_GetDmyPos); }
    if (!camStep) camStep = ResolveCallTarget("48 8d 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8d 05 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? 48 8d 05 $", "CameraStepUpdate");
    if (!getDmy)  getDmy  = ResolveCallTarget("48 8d 44 24 40 48 89 44 24 28 41 8b 00 89 44 24 40 4c 8d 44 24 40 49 8b ca e8 $", "GetDmyPos");
    g_getDmyPos = (FnGetDmyPos)getDmy;
    LOGI("[game] CameraStepUpdate @ %p, GetDmyPos @ %p", (void*)camStep, (void*)getDmy);

    if (camStep) {
        if (MH_CreateHook(camStep, (void*)&Hook_CameraStep, (void**)&g_origCameraStep) == MH_OK && MH_EnableHook(camStep) == MH_OK) {
            g_status.cameraHooked = true; LOGI("[game] camera step hooked");
        } else LOGW("[game] failed to hook camera step");
    }
    // Camera wall recovery: a global bool the follow cam consults to push the camera out of geometry. The game
    // refreshes it every frame, so the hooks above clear it per frame while first person runs (erfps2 pattern:
    // "0F B6 43 0D 88 05 <rel32>" = the instruction pair that refreshes it).
    if (g_status.versionOk) g_camWallRecovery = g_base + GameRva(RVA_CamWallRecovery);
    else if (uint8_t* hit = AobScan("0F B6 43 0D 88 05 ?? ?? ?? ?? 0F B6 43 10")) g_camWallRecovery = ResolveRipRelative(hit + 4, 2, 6);
    LOGI("[game] cam wall-recovery flag @ %p", (void*)g_camWallRecovery);

    uint8_t* mms = g_status.versionOk ? g_base + GameRva(RVA_MmsUpdateChrCam) : ResolveCallTarget("48 8b cb e8 $ f3 0f 10 4f 08 48 8b cb e8 ?? ?? ?? ?? f3 0f 10 4f 08 48 8b cb e8 ?? ?? ?? ?? 48 8b cb e8", "MmsUpdateChrCam");
    uint8_t* fol = g_status.versionOk ? g_base + GameRva(RVA_FollowCamFollow) : ResolveCallTarget("4d 8b c6 41 0f 28 cf 48 8b ce e8 $ 0f b6 86 10 03 00 00", "FollowCamFollow");
    if (mms && MH_CreateHook(mms, (void*)&Hook_MmsUpdateChrCam, (void**)&g_origMms) == MH_OK && MH_EnableHook(mms) == MH_OK) LOGI("[game] chr-cam update hooked @ %p", (void*)mms); else LOGW("[game] chr-cam update hook unavailable");
    if (fol && MH_CreateHook(fol, (void*)&Hook_FollowCamFollow, (void**)&g_origFollow) == MH_OK && MH_EnableHook(fol) == MH_OK) LOGI("[game] follow-cam hooked @ %p", (void*)fol); else LOGW("[game] follow-cam hook unavailable");
    uint8_t* ufol = g_status.versionOk ? g_base + GameRva(RVA_UpdateFollowCam) : nullptr;
    if (ufol && MH_CreateHook(ufol, (void*)&Hook_UpdateFollowCam, (void**)&g_origUpdFollow) == MH_OK && MH_EnableHook(ufol) == MH_OK) LOGI("[game] follow-cam outer update hooked @ %p", (void*)ufol); else LOGW("[game] follow-cam outer update hook unavailable");
    uint8_t* umod = g_status.versionOk ? g_base + GameRva(RVA_UpdateChrModelPos) : nullptr;
    if (umod && MH_CreateHook(umod, (void*)&Hook_UpdateChrModelPos, (void**)&g_origUpdModelPos) == MH_OK && MH_EnableHook(umod) == MH_OK) LOGI("[game] chr model-pos hooked @ %p (final pose write point)", (void*)umod); else LOGW("[game] chr model-pos hook unavailable");
    uint8_t* ulock = g_status.versionOk ? g_base + GameRva(RVA_UpdateLockTgt) : AobScan(AOB_UpdateLockTgt);
    if (ulock && MH_CreateHook(ulock, (void*)&Hook_UpdateLockTgt, (void**)&g_origUpdLockTgt) == MH_OK && MH_EnableHook(ulock) == MH_OK) LOGI("[game] lock-target update hooked @ %p (cast aim re-forces its lock after it)", (void*)ulock); else LOGW("[game] lock-target update hook unavailable: cast aim cannot hold the lock the game rewrites");
    uint8_t* cfin = (g_status.versionOk && Cfg().fpCameraFinalWrite) ? g_base + GameRva(RVA_CamFinalize) : nullptr;
    if (cfin && MH_CreateHook(cfin, (void*)&Hook_CamFinalize, (void**)&g_origCamFinalize) == MH_OK && MH_EnableHook(cfin) == MH_OK) LOGI("[game] camera finalizer hooked @ %p (our matrix goes back after it)", (void*)cfin); else if (Cfg().fpCameraFinalWrite) LOGW("[game] camera finalizer hook unavailable");
    uint8_t* peval = (g_status.versionOk && Cfg().fpPoseAfterHavok) ? g_base + GameRva(RVA_PoseEval) : nullptr;
    if (peval && MH_CreateHook(peval, (void*)&Hook_PoseEval, (void**)&g_origPoseEval) == MH_OK && MH_EnableHook(peval) == MH_OK) LOGI("[game] havok pose eval hooked @ %p (post-animation pose reassert)", (void*)peval); else if (Cfg().fpPoseAfterHavok) LOGW("[game] havok pose eval hook unavailable");

    // Near-camera dither lookup: RVA on 2.6.2.0, else the function's own bytes (the hit IS the function).
    uint8_t* udit = g_status.versionOk ? g_base + GameRva(RVA_UsesDithering)
                  : AobScan("48 8B 81 60 10 00 00 48 8B 89 68 10 00 00 48 3B C1 74 0E 44 39 00 74 0C 48 83 C0 18 48 3B C1 75 F2 32 C0 C3");
    if (udit && MH_CreateHook(udit, (void*)&Hook_UsesDithering, (void**)&g_origUsesDithering) == MH_OK && MH_EnableHook(udit) == MH_OK)
        LOGI("[game] near-camera dither lookup hooked @ %p (NearDither=%.2f)", (void*)udit, Cfg().fpNearDither);
    else LOGW("[game] near-camera dither hook unavailable - NearDither has no effect");

    // Per-frame pose hook: CS::NoUpdateInterface vft[10] (ERSkeletonMan's timing point).
    if (void** vt = FindVTable("CS::NoUpdateInterface")) {
        void* fn = vt[10];
        void* stub = MidHookMakeStub(fn, &HkHandler, nullptr);
        if (stub) {
            DWORD old;
            if (VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &old)) {
                g_hkOrig = fn; g_hkSlot = &vt[10];
                vt[10] = stub;
                VirtualProtect(&vt[10], sizeof(void*), old, &old);
                g_status.skeletonHooked = true;
            }
        }
        LOGI("[game] NoUpdateInterface[10] = %p, vtable hook %s", fn, g_status.skeletonHooked ? "installed" : "FAILED");
    }
    if (Cfg().combatEnabled) CombatInit(g_base, CurrentGameBuild() == GameBuild::V262);
    return g_status.cameraHooked;
}

// [Cheats] SetAllStats: PlayerGameData (GameDataMan+0x8) stats at +0x3C..+0x58 (vigor..arcane), level +0x68
// (fromsoftware-rs PlayerGameData). Applied once per configured value, only raising stats; level = sum - 79.
void ApplyStatCheat() {
    static int applied = 0;
    int want = Cfg().cheatSetAllStats;
    if (want <= 0 || want == applied || !g_gameDataManPtr || !MainPlayer()) return;
    uint8_t* gdm = Deref(g_gameDataManPtr); if (!gdm) return;
    uint8_t* pgd = Deref(gdm + 0x8); if (!pgd) return;
    int stats[8]; if (!SafeRead(pgd + 0x3C, stats, sizeof(stats))) return;
    for (int& s : stats) if (s < 1 || s > 99) return;   // sanity: not a PlayerGameData we understand
    // Sets every stat to EXACTLY the value (2026-08-24: it used to only raise, which left a
    // test character at 50-everything with no way back for balance testing).
    int sum = 0, changed = 0;
    for (int& s : stats) { if (s != want) { s = want; ++changed; } sum += s; }
    if (changed) {
        SafeWrite(pgd + 0x3C, stats, sizeof(stats));
        int level = sum - 79; SafeWrite(pgd + 0x68, &level, 4);
        LOGI("[cheat] set %d stats to %d (level -> %d)", changed, want, level);
    }
    applied = want;
}

// [Cheats] SetLevel: a level no single value reaches (158 = 237 points, not a multiple of 8). The
// difference comes off the highest stats (or goes onto the lowest), one point at a time, so the
// build keeps its shape until a respec; then the key is set back to 0 in ERVR.ini, so it applies
// exactly once and a later level-up is never undone.
void ApplyLevelCheat() {
    static int applied = 0;
    const int want = Cfg().cheatSetLevel;
    if (want <= 0 || want == applied || !g_gameDataManPtr || !MainPlayer()) return;
    uint8_t* gdm = Deref(g_gameDataManPtr); if (!gdm) return;
    uint8_t* pgd = Deref(gdm + 0x8); if (!pgd) return;
    int stats[8]; if (!SafeRead(pgd + 0x3C, stats, sizeof(stats))) return;
    for (int& s : stats) if (s < 1 || s > 99) return;   // sanity: not a PlayerGameData we understand
    applied = want;
    int sum = 0; for (int s : stats) sum += s;
    const int from = sum - 79, target = want + 79;
    if (target < 8 || target > 8 * 99) { LOGW("[cheat] SetLevel=%d is out of range (level 1..713)", want); return; }
    while (sum > target) {   // highest first; ties from arcane back towards vigor
        int i = 7; for (int k = 6; k >= 0; --k) if (stats[k] > stats[i]) i = k;
        if (stats[i] <= 1) break;
        --stats[i]; --sum;
    }
    while (sum < target) {   // lowest first; ties from vigor on
        int i = 0; for (int k = 1; k < 8; ++k) if (stats[k] < stats[i]) i = k;
        if (stats[i] >= 99) break;
        ++stats[i]; ++sum;
    }
    const int level = sum - 79;
    if (level != from) {
        SafeWrite(pgd + 0x3C, stats, sizeof(stats));
        SafeWrite(pgd + 0x68, &level, 4);
    }
    LOGI("[cheat] level %d -> %d (vigor %d mind %d endurance %d strength %d dexterity %d intelligence %d faith %d arcane %d)",
         from, level, stats[0], stats[1], stats[2], stats[3], stats[4], stats[5], stats[6], stats[7]);
    const std::wstring& ini = Cfg().iniPath;
    if (!ini.empty() && WritePrivateProfileStringW(L"Cheats", L"SetLevel", L"0", ini.c_str()))
        LOGI("[cheat] [Cheats] SetLevel set back to 0 in ERVR.ini (applied once)");
}

uint8_t* GameERPlayerGameData() {
    uint8_t* gdm = g_gameDataManPtr ? Deref(g_gameDataManPtr) : nullptr;
    return gdm ? Deref(gdm + 0x8) : nullptr;
}

// In-game time in ms (GameDataMan+0xA0, the community IGT offset speedrun timers use). IGT
// advances by the frame dt each sim frame, which makes it the perfect probe for the pass-2
// dt swap: a full-dt second pass doubles its rate, a dt~0 one leaves it at 1.0x realtime.
uint32_t GameERIgtMs() {
    if (!g_gameDataManPtr) return 0;
    uint8_t* gdm = Deref(g_gameDataManPtr);
    if (!gdm) return 0;
    uint32_t igt = 0;
    if (!SafeRead(gdm + 0xA0, &igt, 4)) return 0;
    return igt;
}

bool GameERForeignCamActive() {
    const ULONGLONG t = g_foreignFovTick.load(std::memory_order_relaxed);
    // 1.2 s tail (2026-08-24, was 3 s): the tail's old job - bridging "map closed by the
    // travel confirm" into the teardown - belongs to GameMan.warp_requested now, which
    // fires on the confirm frame itself. The tail only needs to outlive the detector's
    // own sampling window while a takeover camera is still fading out.
    return t != 0 && GetTickCount64() - t < 1200;
}

// The game's OWN warp commitment: GameMan.warp_requested (+0x10). Set on the request frame
// of every warp - map-screen travel confirm, death respawn, stake of Marika, emevd
// WarpPlayer, coop return - and never by an ordinary interact/jump A press. This is the
// authoritative signal the Pass2SafeOnA heuristic approximated: the 2026-08-23 fast-travel
// crash fired 160 ms after the confirm with no guard holding; this byte is already set on
// that frame. Offset from fromsoftware-rs's GameMan (erfps2 branch, ER 1.16.2 - our exe).
bool GameERWarpRequested() {
    if (!g_gameManPtr) return false;
    uint8_t* gm = Deref(g_gameManPtr); if (!gm) return false;
    uint8_t b = 0;
    return RD(gm + 0x10, b) && b != 0;
}

// The community "loading screen up" byte: [[CSEventFlagMan]+0x28] + 0x113 (flag_blocks of
// the CSFD4VirtualMemoryFlag that heads CSEventFlagMan; the exact read TGA's Seamless-Coop
// loading-screen fix polls at 100 ms until it clears). TRUE for the whole teardown/stream/
// rebuild window between a warp request and arrival - state, not a timer.
bool GameERLoadingScreen() {
    if (!g_eventFlagManPtr) return false;
    uint8_t* efm = Deref(g_eventFlagManPtr); if (!efm) return false;
    uint8_t* blocks = Deref(efm + 0x28); if (!blocks) return false;
    uint8_t b = 0;
    return RD(blocks + 0x113, b) && b != 0;
}

// Co-op roster. Seamless Co-op adds and removes other players' characters with no warp,
// loading screen or player teardown - the signals every pump guard keys on - and that
// rebuilds the character grids the pass-2 MoveMap family walks (2026-10-31: a connect
// crashed at the transition null-call site, task 0x90000052, with every guard quiet).
// WorldChrMan.player_chr_set (+0x10EE0, fromsoftware-rs; unmoved from 2.6.2 to 2.7.2):
// ChrSet {vft, index, capacity u32 @+0x10, entries @+0x18}, entries 16 bytes
// {ChrIns* @0, u8 load_status @8}. The main player is skipped, so solo play never changes it.
bool GameERCoopRoster(float graceSeconds, int* others) {
    static ULONGLONG s_changeTick = 0;
    static int s_others = 0;
    static uint64_t s_sig = 0;
    static ULONGLONG s_loadLogTick = 0;
    const ULONGLONG now = GetTickCount64();
    const bool holding = s_changeTick && now - s_changeTick < (ULONGLONG)(graceSeconds * 1000.f);
    // No main player (loading, title) = nothing to tell ourselves apart from: keep the last
    // roster rather than count our own entry as somebody else.
    uint8_t* me = MainPlayer();
    if (!me) { if (others) *others = s_others; return holding; }
    int n = 0; uint64_t sig = 0;
    uint8_t* set = WorldChrMan() ? WorldChrMan() + off::WCM_PlayerChrSet : nullptr;
    uint32_t cap = 0; uint8_t* entries = nullptr;
    if (set && RD(set + 0x10, cap) && RD(set + 0x18, entries) && entries && cap <= 64) {
        for (uint32_t i = 0; i < cap; ++i) {
            uint8_t* chr = nullptr; uint8_t status = 0;
            if (!RD(entries + i * 16, chr) || !chr || chr == me) continue;
            RD(entries + i * 16 + 8, status);
            ++n;
            sig = (sig ^ ((uint64_t)(uintptr_t)chr ^ ((uint64_t)status << 56))) * 1099511628211ull;
        }
    }
    if (n != s_others) {
        LOGI("[coop] other players in the world: %d -> %d (pass 2 holds %.1f s%s)",
             s_others, n, graceSeconds, Cfg().vrPass2CoopSafe ? "; SAFE keep set while anyone is here" : "");
        s_changeTick = now;
    } else if (sig != s_sig) {
        if (now - s_loadLogTick > 2000) { LOGI("[coop] a player's character changed load state; pass 2 holds"); s_loadLogTick = now; }
        s_changeTick = now;
    }
    s_others = n; s_sig = sig;
    if (others) *others = n;
    return s_changeTick && now - s_changeTick < (ULONGLONG)(graceSeconds * 1000.f);
}

// Multiplayer session state: the game's own join state machine, which Seamless Co-op drives
// (fromsoftware-rs session_manager.rs). CSSessionManagerImp {vft, u32, lobby_state u32 @+0xC,
// protocol_state u32 @+0x10}; protocol_state walks JoinCheck -> WaitInitData -> WaitReloadWait
// -> WaitReload ("reload start to character initialization") -> WaitReload2 -> Ingame.
// 2026-10-31: two connects crashed 11-12 s after the item was used with no warp, loading or
// roster signal first - this machine is what runs in that window.
namespace {
std::atomic<uint8_t*> g_sessionMan{nullptr};
std::atomic<void**> g_sessionVt{nullptr};
std::atomic<bool> g_sessionLookup{false};
std::atomic<uint32_t> g_sessLobby{0}, g_sessProto{0};
std::atomic<ULONGLONG> g_connectSince{0}, g_connectLast{0};
const char* const kLobbyNames[] = {"None", "TryToCreateSession", "FailedToCreateSession", "Host", "TryToJoinSession",
                                   "FailedToJoinSession", "Client", "OnLeaveSession", "FailedToLeaveSession"};
const char* const kProtocolNames[] = {"None", "JoinCheck", "WaitInitData", "WaitReloadWait", "WaitReload",
                                      "WaitReload2", "Ingame", "WaitReentryToMap"};
// Creating, joining or leaving a session, the join-only protocol steps, or a reload while a
// session exists. Seamless Co-op runs WaitReload2 -> Ingame on every SOLO world load too
// (measured 2026-10-31, lobby None), which the loading guards already cover.
bool SessionConnecting(uint32_t lobby, uint32_t proto) {
    return lobby == 1 || lobby == 4 || lobby == 7 || (proto >= 1 && proto <= 3) ||
           (lobby != 0 && (proto == 4 || proto == 5 || proto == 7));
}
// Worker thread: find the manager once (module scan, so on its own thread), then sample and
// log every transition - the timeline a co-op crash needs, in any stereo mode.
void SessionWatchTick() {
    uint8_t* sm = g_sessionMan.load();
    if (!sm) {
        if (!MainPlayer() || g_sessionLookup.exchange(true)) return;
        std::thread([] {
            uint8_t* p = nullptr; void** vt = nullptr;
            for (int i = 0; i < 10 && !p; ++i) {
                p = FindSingletonInstance("CSSessionManager", "CS::CSSessionManagerImp", i == 9, &vt);
                if (!p) Sleep(3000);
            }
            if (p) { g_sessionVt.store(vt); g_sessionMan.store(p); LOGI("[coop] CSSessionManagerImp at %p - multiplayer join state watched", (void*)p); }
            else LOGW("[coop] CSSessionManagerImp not found - co-op connects are guarded by the player roster only");
        }).detach();
        return;
    }
    uint32_t lobby = 0, proto = 0; void** vt = nullptr;
    if (!RD(sm, vt) || vt != g_sessionVt.load() || !RD(sm + 0xC, lobby) || !RD(sm + 0x10, proto) || lobby > 8 || proto > 7) {
        // Freed (process exit, or re-created elsewhere): drop it and find it again.
        LOGI("[coop] session manager %p went away - looking it up again", (void*)sm);
        g_sessionMan.store(nullptr); g_sessLobby.store(0); g_sessProto.store(0); g_connectSince.store(0);
        g_sessionLookup.store(false);
        return;
    }
    static bool s_first = true;
    if (s_first) { s_first = false; LOGI("[coop] session lobby %s, protocol %s (first read)", kLobbyNames[lobby], kProtocolNames[proto]); }
    const uint32_t pl = g_sessLobby.exchange(lobby), pp = g_sessProto.exchange(proto);
    if (lobby != pl || proto != pp)
        LOGI("[coop] session lobby %s -> %s, protocol %s -> %s", kLobbyNames[pl], kLobbyNames[lobby], kProtocolNames[pp], kProtocolNames[proto]);
    const ULONGLONG now = GetTickCount64();
    if (SessionConnecting(lobby, proto)) {
        // The 30 s cap runs per state: a long search must not use it up before the join
        // steps that follow it.
        if (!g_connectSince.load() || lobby != pl || proto != pp) g_connectSince.store(now);
        g_connectLast.store(now);
    } else g_connectSince.store(0);
}
}

bool GameERCoopConnecting(float graceSeconds) {
    const ULONGLONG now = GetTickCount64(), since = g_connectSince.load(), last = g_connectLast.load();
    if (since) return now - since < 30000;   // a state that never settles must not hold all session
    return last && now - last < (ULONGLONG)(graceSeconds * 1000.f);
}
bool GameERCoopInSession() { const uint32_t l = g_sessLobby.load(); return l == 3 || l == 6; }

// Latched warp guard for the frame pump: TRUE from the first frame warp_requested reads set
// until Pass2WarpGraceSeconds after it last read set. The tail bridges the gap between the
// flag being consumed by the transfer machinery and the player-null/loading-screen guards
// picking the hold up - without it a one-frame flag would let pass 2 back in mid-teardown.
// Also logs both authoritative signals' edges so any future transition crash can be timed
// against them from the log alone.
bool GameERWarpGuardActive() {
    const ULONGLONG now = GetTickCount64();
    static bool s_warpWas = false, s_loadWas = false;
    const bool warp = GameERWarpRequested();
    const bool load = GameERLoadingScreen();
    if (warp) g_warpReqTick.store(now, std::memory_order_relaxed);
    if (load) g_loadingTick.store(now, std::memory_order_relaxed);
    if (warp != s_warpWas) { LOGI("[guard] GameMan.warp_requested -> %d", (int)warp); s_warpWas = warp; }
    if (load != s_loadWas) { LOGI("[guard] loading-screen byte -> %d", (int)load); s_loadWas = load; }
    const ULONGLONG t = g_warpReqTick.load(std::memory_order_relaxed);
    return t != 0 && now - t < (ULONGLONG)(Cfg().vrPass2WarpGraceSeconds * 1000.f);
}

// "The world is being torn down or is about to be": player dead (death anim + death warp
// follow), warp latched, or the loading screen up. This is the write-side hazard window -
// while it holds, ERVR must not write into game objects or execute game code, because
// teardown frees and reuses the memory our chains and caches point at. Distinct from
// GameERTransitionActive below only in that it has NO arrival settle: writes into a built
// world (player active, load done) are legitimate the moment the world exists.
// Self-sampling (warp flag read here too) so it works in sessions without the frame pump.
bool GameERWorldDying() {
    if (GameERPlayerDead()) return true;
    if (GameERLoadingScreen()) return true;
    // A co-op join re-initializes characters under us like a load does (WaitReload*).
    if (GameERCoopConnecting(Cfg().vrPass2CoopGraceSeconds)) return true;
    const ULONGLONG now = GetTickCount64();
    if (GameERWarpRequested()) g_warpReqTick.store(now, std::memory_order_relaxed);
    const ULONGLONG w = g_warpReqTick.load(std::memory_order_relaxed);
    return w != 0 && now - w < (ULONGLONG)(Cfg().vrPass2WarpGraceSeconds * 1000.f);
}

// The transition authority for every ERVR path that EXECUTES GAME CODE or mutates game
// state outside our own pass-2 pump - combat TAE injection above all. GameERWorldDying
// plus player-null and an arrival settle (the world keeps streaming in for seconds after
// the player appears; injecting behavior events into that is the suspected death-arrival
// crash class).
//
// Why this exists (2026-08-23 evening): both post-warp-guard crashes correlate with the
// physical-combat swing detector firing DURING a transition window - the arm motion of
// pressing A on the travel confirm reads as a ~4 m/s swing, so EVERY fast travel injected
// an attack-behavior event into the player 200-400 ms into world teardown (travel 1
// survived the race, travel 2 crashed 238 ms after the injection, CS::ChrIns + garbage
// index in the fault). Executing the game's behavior dispatch mid-teardown is a coin flip;
// the erfps2/practice-tool rule is that EVERYTHING gates on world validity, not just the
// renderer.
bool GameERTransitionActive() {
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG s_lastInactive = now;
    if (!GameERPlayerActive()) { s_lastInactive = now; return true; }
    // Spawn settle: the world streams in around the player for seconds after arrival.
    if (now - s_lastInactive < (ULONGLONG)(Cfg().vrPass2SpawnGraceSeconds * 1000.f)) return true;
    return GameERWorldDying();
}

bool GameERTeleportGraceActive() {
    const ULONGLONG t = g_bigJumpTick.load(std::memory_order_relaxed);
    // Fixed 1 s tail since 2026-08-24: the >100 m jump was the ARRIVAL heuristic of the
    // pre-signal era, and the last speed pass showed it had become the slowest link in
    // the resume chain (the jump fires as the destination places the camera, so its tail
    // ran on after the loading byte cleared). Warp latch + loading byte + spawn grace now
    // cover arrival authoritatively; this stays only as a last-resort catch for a
    // world-moving jump that arrives with no warp flag and no load screen.
    return t != 0 && GetTickCount64() - t < 1000;
}

uint8_t* GameERIgtWatchTarget(uint32_t* outLen) {
    if (!g_gameDataManPtr) return nullptr;
    uint8_t* gdm = Deref(g_gameDataManPtr);
    if (!gdm) return nullptr;
    if (outLen) *outLen = 4;
    return gdm + 0xA0;
}

void GameERTick() {
    VRTracking tr; VRInputState in;
    bool ok = GetLatestVRState(tr, in);
    { std::lock_guard<std::mutex> g(g_vrMtx); g_vrValid = ok; if (ok) { g_vrTr = tr; g_vrIn = in; } }
    if (!MainPlayer() || !Cfg().fpEnabled) { g_moveActive.store(false); g_camYawInit = false; g_bodyYawInit = false; g_mountedTurns.SetMount(nullptr); }
    ApplyStatCheat();
    ApplyLevelCheat();
    SessionWatchTick();
}

void GameERAddCameraYaw(float radians) {
    if (radians == 0.f || !g_camYawInit) return;
    if (g_moveLocked.load()) return;   // locked on: the game aims the view, our turn input would fight it
    g_camYaw.store(WrapPi(g_camYaw.load() + radians));
    if (g_moveRiding.load()) { g_mountedTurns.Add(radians); g_rideStats.Turn(g_rideStats.stick, radians); }
    float e = g_pendingYawPivot.load();
    while (!g_pendingYawPivot.compare_exchange_weak(e, e + radians)) {}
}

void GameERAddBodyYaw(float radians) {
    if (radians == 0.f || !g_bodyYawInit) return;
    if (g_moveLocked.load()) return;   // locked on: the game aims the body
    if (g_moveRiding.load()) { GameERAddCameraYaw(radians); return; } // keep the saddle and view together
    g_bodyStickYaw.store(WrapPi(g_bodyStickYaw.load() + radians));
}

MoveBasis GameERGetMoveBasis() {
    MoveBasis m;
    m.active = g_moveActive.load() && Cfg().fpEnabled && Cfg().fpCameraYawSource != "game";
    m.lockedOn = g_moveLocked.load();
    m.riding = g_moveActive.load() && Cfg().fpEnabled && g_moveRiding.load();
    m.camYaw = g_camYaw.load();
    m.chrYaw = g_chrYaw.load();
    m.bodyYaw = g_bodyYaw.load();
    m.headYaw = g_headYaw.load();
    m.headValid = g_headYawValid.load();
    // With CameraRotation=hmd the matrix we write into ChrCam is YawQ(camYaw) * headQ (UpdateCamera), and the game
    // derives its stick frame from that matrix - so physically turning your head already turns "stick forward".
    m.viewYaw = (m.headValid && Cfg().fpCameraRotation == "hmd") ? WrapPi(m.camYaw + m.headYaw) : m.camYaw;
    // On Torrent: forward is the free-look direction (RideFollowHead), so a glance does not steer the horse.
    m.rideFreeLook = m.riding && m.headValid && g_rideForwardValid.load();
    m.rideForwardYaw = WrapPi(m.camYaw + g_rideForward.load());
    return m;
}

GameERStatus GameERGetStatus() {
    GameERStatus s = g_status; s.boneCount = g_skel.boneCount;
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    s.hkCalls = InterlockedExchange(&g_hkCalls, 0); s.hkSolves = InterlockedExchange(&g_hkSolves, 0); s.camCalls = InterlockedExchange(&g_camCalls, 0);
    s.hkMs = (double)InterlockedExchange64(&g_hkTicks, 0) * 1000.0 / (double)f.QuadPart;
    s.camMs = (double)InterlockedExchange64(&g_camTicks, 0) * 1000.0 / (double)f.QuadPart;
    return s;
}

// Live check, NOT the g_status latch: playerFound is set once when the player is first
// resolved and never cleared, so keying the pass-2 grace off it left that guard permanently
// TRUE - i.e. dead - for the rest of the session. WorldChrMan->MainPlayer goes null while a
// warp (fast travel, death) tears the world down, and stays null until the destination map's
// player is built: exactly the window the inactive-grace exists to cover.
// TEST HARNESS ONLY: drop the player to 0 HP to fire the DEATH WARP. That warp is one of the
// two crash paths, and it is the only one reachable unattended - a fast travel needs map-screen
// navigation and would move the character to another site of grace. Dying costs the held runes
// (recoverable at the death spot) and respawns at the last grace; nothing else in the save moves.
//
// CSChrDataModule layout is anchored by the stamina pair combat.cpp already relies on
// (0x154/0x158); HP/MaxHP sit at 0x138/0x13C in the same block. Both are range-checked before
// the write, so a wrong offset logs and refuses instead of corrupting whatever it landed on.
// Same-frame death detector. HP reaches 0 on the REQUEST frame of a death warp - before
// the fade, before any teardown, before every other signal this file has. The 2026-08-22
// crash hunt showed the existing guards all fire too late (the real crash landed 200 ms
// after the warp was requested, with no guard holding); this one cannot be late, because
// the thing that starts the warp IS the value it reads. Chain and offsets are the ones
// GameERKillPlayer validated in-game (hp 1878/1878 next to the known stamina pair).
bool GameERPlayerDead() {
    uint8_t* player = MainPlayer(); if (!player) return false;
    uint8_t* mods = Deref(player + off::Chr_Modules); if (!mods) return false;
    uint8_t* data = Deref(mods + 0x00); if (!data) return false;   // CSChrDataModule
    int32_t hp = -1, maxHp = -1;
    if (!RD(data + 0x138, hp) || !RD(data + 0x13C, maxHp)) return false;
    return maxHp > 0 && maxHp < 100000 && hp <= 0;
}

bool GameERKillPlayer() {
    uint8_t* player = MainPlayer();
    if (!player) { LOGW("[kill] no player"); return false; }
    uint8_t* mods = Deref(player + off::Chr_Modules);
    if (!mods) { LOGW("[kill] no module container"); return false; }
    uint8_t* data = Deref(mods + 0x00);            // CSChrDataModule
    if (!data) { LOGW("[kill] no data module"); return false; }
    int32_t hp = -1, maxHp = -1, stam = -1, maxStam = -1;
    RD(data + 0x138, hp); RD(data + 0x13C, maxHp);
    RD(data + 0x154, stam); RD(data + 0x158, maxStam);
    // Sanity: the stamina pair is KNOWN good, so it validates that we are looking at the right
    // struct at all; the HP pair then has to look like HP.
    const bool stamOk = maxStam > 0 && maxStam < 100000 && stam >= 0 && stam <= maxStam;
    const bool hpOk = maxHp > 0 && maxHp < 100000 && hp > 0 && hp <= maxHp;
    LOGI("[kill] data module %p: hp %d/%d, stamina %d/%d", (void*)data, hp, maxHp, stam, maxStam);
    if (!stamOk || !hpOk) {
        LOGE("[kill] REFUSING to write - values do not look like HP/stamina (stamOk %d hpOk %d); "
             "the CSChrDataModule offsets are wrong for this build",
             (int)stamOk, (int)hpOk);
        return false;
    }
    const int32_t zero = 0;
    if (!WR(data + 0x138, zero)) { LOGE("[kill] HP write faulted"); return false; }
    LOGW("[kill] player HP %d -> 0; death warp incoming (test harness)", hp);
    return true;
}

// Exe version check result (2.6.2.0 pinned at startup). The crash-fix VEH gates its
// RVA-pinned instruction skips on this when Arxan's code-page scrambling makes byte
// verification unreliable inside the exception dispatch (measured 16:52: raw AND guarded
// reads of the faulting code page can disagree with the executing bytes).
bool GameERVersionOk() { return g_status.versionOk; }

bool GameERPlayerActive() { return g_status.playerFound && MainPlayer() != nullptr; }

void GameERShutdown() {
    if (g_hkSlot && g_hkOrig) {
        DWORD old;
        if (VirtualProtect(g_hkSlot, sizeof(void*), PAGE_READWRITE, &old)) { *g_hkSlot = g_hkOrig; VirtualProtect(g_hkSlot, sizeof(void*), old, &old); }
        g_hkSlot = nullptr;
    }
}
}
