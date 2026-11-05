// Physical melee combat via the game's own TAE AttackBehavior event.
//
// Sources: fromsoftware-rs (CSChrTaeAnimEvent / CSTaeAnimEventArgs layouts, event ids,
// ChrIns module container + CSChrDataModule stamina offsets, PlayerIns/ChrAsm equipment
// slots, EQUIP_PARAM_WEAPON_ST / BEHAVIOR_PARAM_ST / ATK_PARAM_ST field layouts), erfps2
// (hooking CSChrTaeAnimEvent vtable RVA 0x2a34280 for 2.6.2.0), and our own disassembly of
// that vtable: slot 1 = run slot 2 then slot 3; slot 2 handles the "generic" events and is a
// NO-OP for events 1/2/5; slot 3 dispatches the behavior events, event 1 -> handler at RVA
// 0x4266d0. That handler reads args+0x1a (u16, must be >= 0x1a for the Source byte to be
// honoured), args+0x20 (float* animation time, dereferenced on first execution) and passes
// the AttackBehaviorArgs pointer into the damage module (so it must stay stable per attack).
// DSAnimStudio TAE.Template.ER.xml (AttackBehavior args: AttackType s32, AttackIndex s32,
// BehaviorJudgeID s32, DirectionType u8, Source u8 (0 default/1 right/2 left), StateInfo s16),
// Muffin's DS3 moveset notes (BehaviorParam_PC row = 1_0000_0000 + variation*1000 + judgeId).
//
// Kinetic layer ("damage is physics"): a strike's quality - tip speed at impact relative to a
// reference speed, squared like kinetic energy, times how much arc the tip travelled - scales
// the motion values of the AtkParam_Pc row the strike resolves to (BehaviorParam_PC row ->
// refId), live in the loaded params, and is restored when the strike ends. Stamina is fatigue:
// each strike drains by energy spent (scaled by the weapon's weight), and low stamina weakens
// strikes. Shields: the left hand's pose against the head yields a block quality that holds
// the guard button and scales the shield's guard cut rates / stability the same way; a hit
// taken while the shield moves INTO the attack is a deflect (stamina refunded).
//
// Two-handing (2026-08-25): the free hand closing on the weapon hand's weapon shaft IS the
// two-hand command. ERVR does not write ChrAsm.arm_style - it sends the game's own chord
// (Y + RB for the right weapon, Y + LB for the left; the same chord toggles back) and reads
// arm_style back as the authority, so HKS, the weapon's two-hand attack rows and the strength
// bonus all come from the game. The free hand's arm then rides the weapon shaft.
// Skill (Ash of War) while two-handed: the skill's effect lives in TAE events the skill
// animation carries - AddSpEffect_WeaponArts (331: {SpEffect if enough FP, SpEffect if not}),
// AddSpEffect (67/66), BulletBehavior (2), FFX spawns - so "the effect without the animation"
// is those events replayed through the same CSChrTaeAnimEvent slots physical melee already
// uses. They are recorded the first time a weapon's skill is played the game's way (forced
// skill button, the hooks capture every event the player runs for SkillRecordSeconds) and
// replayed on the trigger from then on; the skill's own AttackBehavior becomes the row the
// next swing uses. FP/stamina costs come from SwordArtsParam (the applied gem's arts id via
// CSGaitem, else the weapon's own). Event ids: DSAnimStudio TAE.Template.ER.xml.
#include "combat.h"
#include "body_haptics_er.h"
#include "config.h"
#include "equipment_layout.h"
#include "game_er.h"
#include "game_memory.h"
#include "game_version.h"
#include "action_queue_snapshot.h"
#include "weapon_motion.h"
#include "weapon_contact.h"
#include "mounted_rider.h"
#include "debug_keys.h"
#include "params_er.h"
#include "log.h"
#include "rtti.h"
#include "belt.h"
#include "weapon_wheel.h"
#include "sword_rig.h"
#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ervr {
namespace {

// ---------------------------------------------------------------- game layouts
constexpr uint32_t RVA_ChrTaeAnimEventVft = 0x2a34280;   // CS::CSChrTaeAnimEvent vtable (2.6.2.0)
constexpr size_t Chr_Modules       = 0x190;
constexpr size_t Mod_Data          = 0x00;   // CSChrDataModule
constexpr size_t Mod_TimeAct       = 0x18;   // CSChrTimeActModule
constexpr size_t Data_Fp           = 0x148;  // CSChrDataModule: hp 0x138, fp 0x148/max 0x14C, stamina 0x154/max 0x158 (fromsoftware-rs)
constexpr size_t Data_MaxFp        = 0x14C;
constexpr size_t Data_Stamina      = 0x154;
constexpr size_t Data_MaxStamina   = 0x158;
constexpr size_t TA_ChrTaeAnimEvent = 0x18; // CSChrTimeActModule -> CSChrTaeAnimEvent*
// Module container slots and the action-request module (fromsoftware-rs chr_ins/module.rs, action_request.rs).
constexpr size_t Mod_ActionFlag    = 0x08;   // CSChrActionFlagModule: +0x10 animation_action_flags (bit0 stay_state)
constexpr size_t Mod_Event         = 0x58;   // CSChrEventModule: +0x18 request_animation_id, +0x1C idle_anim_id
constexpr size_t Mod_ActionRequest = 0x80;   // CSChrActionRequestModule (0x140 bytes)
constexpr size_t AR_Requests       = 0x10;   // ChrActions raw from the pad manipulator
constexpr size_t AR_NewPresses     = 0x20;   // pressed this frame
constexpr size_t AR_CancelReady    = 0x30;   // queued & possible cancels
constexpr size_t AR_Queued         = 0x38;   // the input buffer: presses made while allowed
constexpr size_t AR_Disabled       = 0x40;
constexpr size_t AR_InputQueue     = 0x50;   // DLVector<ActionQueueEntry>
constexpr size_t AR_CancelQueue    = 0x70;
constexpr size_t AR_QueueTae       = 0x90;
constexpr size_t AR_PossibleInputs = 0x98;   // what the current animation lets you queue
constexpr size_t AR_PossibleCancels= 0xA0;   // what may interrupt it
constexpr size_t AR_PrevPossibleIn = 0xA8;
constexpr size_t AR_MoveFlags      = 0xFC;   // u32
constexpr size_t AR_TaeCancels     = 0x100;  // u32, TAE-driven (transient) cancel flags
constexpr size_t AR_Readback       = 0x110;  // four cancel/queue bitfields; preserve the new-press readback at 0x108
constexpr size_t AR_QueueMode      = 0x138;  // u8
constexpr size_t TA_AnimQueue      = 0x20;   // CSChrTimeActModuleAnim[10] {anim_id, play_time, play_time2, anim_length}
constexpr size_t TA_ReadIdx        = 0xC4;   // index of the animation last played / updated
constexpr uint32_t EV_AttackBehavior = 1;
constexpr uint32_t EV_BulletBehavior = 2, EV_CommonBehavior = 5;   // the other slot-3 (behavior) events; everything else is slot 2
constexpr uint32_t EV_AddSpEffectWeaponArts = 331;                 // {SpEffectID_EnoughFP, SpEffectID_NotEnoughFP, 0, 0}: an Ash of War's effect

// PlayerIns -> ChrAsm and the EQUIP_PARAM_WEAPON_ST fields the weapon wheel shares: equipment_layout.h.
// EQUIP_PARAM_WEAPON_ST
constexpr size_t   Wep_VariationId   = 0x04;   // i32 behaviorVariationId
constexpr size_t   Wep_Weight        = 0x10;   // f32
constexpr size_t   Wep_GuardCut      = 0x34;   // f32 x4: phys, mag, fire, thun (% of damage blocked)
constexpr size_t   Wep_StaminaGuardDef = 0xD8; // i16 stability
constexpr size_t   Wep_DarkGuardCut  = 0x188;  // f32
constexpr size_t   Wep_SwordArtsId   = 0x198;  // i32 swordArtsParamId: the weapon's own skill (an applied Ash of War overrides it through the gem)
constexpr uint32_t Wep_RowSize       = 0x298;
// EQUIP_PARAM_GEM_ST (the applied Ash of War) and SWORD_ARTS_PARAM_ST (its per-button costs)
constexpr size_t   Gem_SwordArtsId   = 0x18;   // i32
constexpr uint32_t Gem_RowSize       = 0x60;
constexpr size_t   Arts_UsePointL2   = 0x09;   // i8  stamina cost of the skill button (L2)
constexpr size_t   Arts_UseMpL2      = 0x12;   // i16 FP cost of the skill button
constexpr uint32_t Arts_RowSize      = 0x20;
// CSGaitemImp (singleton "CSGaitem"): gaitems[5120] OwnedPtr<CSGaitemIns> @+8; CSGaitemIns item_id @0xC;
// CSWepGaitemIns: gem slot table @0x18 -> slot 0's gaitem handle @0x28 (fromsoftware-rs gaitem.rs).
constexpr size_t   Gaitem_Table = 0x08, Gaitem_ItemId = 0x0C, WepGaitem_GemHandle = 0x28;
constexpr uint32_t Gaitem_Count = 5120;
// BEHAVIOR_PARAM_ST
constexpr size_t   Bhv_VariationId = 0x00, Bhv_JudgeId = 0x04, Bhv_RefType = 0x09, Bhv_RefId = 0x0C, Bhv_Stamina = 0x14;
constexpr uint32_t Bhv_RowSize     = 0x20;
// ATK_PARAM_ST (player rows: the *_correction u16s are the motion values, in percent)
constexpr size_t   Atk_HitRadius = 0x00;         // f32 x4: hit0..hit3 capsule radius (fromsoftware-rs generated.rs)
constexpr size_t   Atk_HitStopTime = 0x14;       // f32
constexpr size_t   Atk_PhysCorr = 0x3E, Atk_MagCorr = 0x40, Atk_FireCorr = 0x42, Atk_ThunCorr = 0x44, Atk_StamCorr = 0x46;
constexpr size_t   Atk_DmgLevel = 0x72;          // u8 hit-reaction level (0 = no reaction anim)
constexpr size_t   Atk_DarkCorr = 0x186;
constexpr size_t   Atk_SuperArmorCorr = 0x1B0;   // f32 poise-damage motion value
constexpr uint32_t Atk_RowSize  = 0x1C8;
constexpr uint32_t BehaviorPcBase = 100000000u;
// WorldChrMan: the game's own list of nearby ChrIns sorted by distance to the main player,
// rebuilt every frame for the update-omission system (fromsoftware-rs world_chr_man.rs; the
// struct's own arithmetic reproduces our in-game-verified main_player 0x1E508 / chr_cam
// 0x1ECE0, so the neighbouring offsets are trusted). DLVector = {allocator, first, last, end}.
// 2.7.2.0 moved both 0x10 later (GameWorldChrManOffset).
constexpr size_t WCM_ChrByDistFirst = 0x1F1D8;
constexpr size_t WCM_ChrByDistLast  = 0x1F1E0;
constexpr size_t ChrDist_Stride = 0x10;          // {ChrIns* @0, f32 distance @8, pad}
// ChrIns (fromsoftware-rs chr_ins.rs; modules @0x190 = the same container stamina already uses)
constexpr size_t Chr_Handle   = 0x08;            // FieldInsHandle {u32 selector, i32 block}: the character itself
constexpr size_t Chr_NpcId    = 0x64;            // i32, the 4-digit character id (e.g. 3070)
constexpr size_t Chr_LastHitBy = 0x180;          // FieldInsHandle of whoever hit it last (selector ~0 = none)
constexpr size_t Data_Hp      = 0x138;
constexpr size_t Data_MaxHp   = 0x13C;

struct CSChrTaeAnimEvent { void** vft; uint64_t unk08; uint32_t animId; uint32_t pad; uint8_t* owner; };
struct CSTaeAnimEventArgs {
    uint32_t eventId; uint32_t pad04;
    void* args;
    float from, to;
    uint8_t isFirstExecution;
    uint8_t pad19;
    uint16_t taeVersion;      // +0x1a: handler honours Source only if >= 0x1a
    uint32_t pad1c;
    const float* animTime;    // +0x20: current animation time (read on first execution)
    uint8_t unk28[0x18];
};
static_assert(sizeof(CSTaeAnimEventArgs) == 0x40, "CSTaeAnimEventArgs layout");
struct AttackBehaviorArgs { int32_t attackType; int32_t attackIndex; int32_t judgeId; uint8_t dirType; uint8_t source; int16_t stateInfo; };
static_assert(sizeof(AttackBehaviorArgs) == 16, "AttackBehaviorArgs layout");

using FnExecEvent = void (__fastcall*)(CSChrTaeAnimEvent* self, CSTaeAnimEventArgs* args);
FnExecEvent g_origExec = nullptr;      // slot 3 (behavior events) trampoline: used for injection
FnExecEvent g_origGeneric = nullptr;   // slot 2 (generic events: SpEffect, FFX, sound...) trampoline: skill replay
constexpr int kExecSlot = 3, kGenericSlot = 2;
void** g_taeVft = nullptr;

// ---------------------------------------------------------------- state
// Two-handed attacks have rows of their own (learned into the 2H slots while the game reports
// arm style 2/3, or configured); a missing 2H row falls back to the one-handed one.
enum Slot { SlotLight = 0, SlotHeavy = 1, SlotLeft = 2, SlotLight2H = 3, SlotHeavy2H = 4, SlotCount = 5 };
struct Template { bool valid = false; AttackBehaviorArgs a{}; };
std::mutex g_mtx;                 // guards templates + swing commands + block state + two-hand state (worker <-> game thread)
Template g_tpl[SlotCount];
double g_lastBtn[3] = {-1e9, -1e9, -1e9};   // button presses map to light/heavy/left; the 2H slots learn from those while two-handed
uint8_t* g_player = nullptr;      // last main player seen by the game thread (for owner match in the hook)

// Cast aim (ranged spells / skills / bows aimed by the casting hand). The worker owns the press
// window, the game thread the passive hand (from the equipment); CombatCastAimHand joins them.
std::atomic<int>    g_castPressHand{-1};   // hand whose attack/skill button fired last...
std::atomic<double> g_castPressAt{-1e9};   // ...and when (NowSec clock): the hand a cast's aim events pick, if recent
std::atomic<double> g_castUntil{0.0};      // the aim is live until then - armed by the game's own aim events, never by a press (2026-09-04: pressing guard / a grip forced the lock and the game turned the body after the hand)
std::atomic<int>    g_castWepHand{-1};     // hand holding a catalyst/bow: the passive aim hand
std::atomic<int>    g_wepRanged[2]{};      // per hand: the weapon is a CastAimWeaponTypes class
std::atomic<int>    g_wepShield[2]{};      // per hand: the weapon is a shield (any combat mode, unlike g_leftShield)
uint16_t g_padPrev = 0;                    // worker only: last game-facing pad mask, for press edges
// The cast animation's own aim events, relaxed for the player while a casting hand is live.
constexpr uint32_t EV_SetBulletAimAngle = 238;    // {up,down,right,left limit; up,down,right,left deadzone} i16 degrees (fromsoftware-rs tae.rs)
constexpr uint32_t EV_EnableTwistModifier = 700;  // upper-body twist toward the aim: limits f32 x4, modifier id, target type, minimum angles f32 x4
struct BulletAimAngleArgs { int16_t upLimit, downLimit, rightLimit, leftLimit, upDead, downDead, rightDead, leftDead; };
static_assert(sizeof(BulletAimAngleArgs) == 16, "SetBulletAimAngle args layout");
struct TwistModifierArgs { float upLimit, downLimit, rightLimit, leftLimit; int32_t modifierId; uint8_t targetType; uint8_t pad15[3]; float upMin, downMin, rightMin, leftMin; uint8_t unk28[12]; };
static_assert(sizeof(TwistModifierArgs) == 0x34, "EnableTwistModifier args layout");
// The game keeps a pointer to the twist event's args (CSChrBehaviorDataModule.twist_modifiers[].tae_event),
// so the relaxed copies live in statics, never on the hook's stack. Events run on the game thread, serially.
BulletAimAngleArgs g_aimArgsCopy{}; TwistModifierArgs g_twistArgsCopy{};

struct SwingCmd : WeaponContactLifetime { // worker -> game thread
    int  slot = SlotLight;
    double start = 0;
    uint32_t id = 0;
    bool staminaFail = false;     // game thread -> worker: rejected for lack of stamina
    float quality = 1.f;          // kinetic: live strike quality (1 = vanilla)
    float qualityCommit = 1.f;    // quality when the strike committed (fatigue drain)
    float arc = 0.f, tipSpeed = 0.f;
    float tipLive = 0.f;          // the weapon tip's speed right now (continuous model), no peak floor
};
SwingCmd g_cmd[2];                // per hand

struct HandSM {
    enum { Idle, Armed, Swinging, Cooldown } st = Idle;
    double t = 0;                 // state entry time
    float peak = 0;
    float prevTip = 0;
    uint32_t nextId = 1;
    // kinetic tracking
    Vec3 lastTip; bool tipValid = false;
    float arc = 0;                // m of tip travel since the tip last came to rest
    float vSmooth = 0;
    // Inertia: the "virtual weapon" tip speed, which can only change as fast as the weapon's
    // weight allows. A wrist flick never gets an ultra greatsword up to speed; a committed
    // swing does. Damage and light/heavy follow this, hit detection follows the real tip.
    float vVirt = 0, vVirtPeak = 0;
    double lastPoll = 0;
    double spentAt = 0;           // when the game thread retired this strike (budget spent), 0 = live
    float restSpeed = 0;          // continuous cooldown: tip speed under which the hand may arm again (0 = ContactOffSpeed)
    float rearmMin = 0;           // after a valley end: lowest tip speed since; a new strike must rise above it (RearmReady)
} g_sm[2];
std::atomic<int> g_spentValleyEnds{0}, g_spentTimeoutEnds{0};   // worker -> 5 s combat summary

// Shield / block (worker writes under g_mtx, game thread reads)
struct BlockState {
    bool raised = false;
    float quality = 0.f;
    float into = 0.f;             // shield speed along the look direction, recent max (deflect)
    double intoT = 0;
} g_block;
std::atomic<int> g_leftShield{0};    // game thread -> worker: 1 while a shield is in use in the left hand

CombatStatus g_status;
volatile LONG g_injected = 0, g_swings = 0;
std::atomic<uint32_t> g_haptic{0};
uint32_t g_lastFrame = 0xFFFFFFFF;
double g_lastEvLog = 0;
bool g_tripped = false;

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
uint8_t* Deref(const void* addr) { uint8_t* p = nullptr; return SafeRead(addr, &p, sizeof(p)) ? p : nullptr; }
const char* SlotName(int s) { switch (s) { case SlotLight: return "light"; case SlotHeavy: return "heavy"; case SlotLeft: return "left"; case SlotLight2H: return "light-2h"; case SlotHeavy2H: return "heavy-2h"; } return "?"; }
const wchar_t* SlotIniKey(int s) { switch (s) { case SlotLight: return L"LightJudgeId"; case SlotHeavy: return L"HeavyJudgeId"; case SlotLeft: return L"LeftJudgeId"; case SlotLight2H: return L"TwoHandLightJudgeId"; case SlotHeavy2H: return L"TwoHandHeavyJudgeId"; } return L"?"; }
const char* HandName(int h) { return h == HandRight ? "right" : "left"; }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

int ReadStamina(uint8_t* player, int* maxOut = nullptr) {
    uint8_t* mods = Deref(player + Chr_Modules); if (!mods) return -1;
    uint8_t* data = Deref(mods + Mod_Data); if (!data) return -1;
    int s = -1, m = -1; SafeRead(data + Data_Stamina, &s, 4); SafeRead(data + Data_MaxStamina, &m, 4);
    if (maxOut) *maxOut = m;
    return s;
}
bool WriteStamina(uint8_t* player, int value) {
    uint8_t* mods = Deref(player + Chr_Modules); uint8_t* data = mods ? Deref(mods + Mod_Data) : nullptr;
    if (!data) return false;
    if (value < 0) value = 0;
    return SafeWrite(data + Data_Stamina, &value, 4);
}
int ReadFp(uint8_t* player, int* maxOut = nullptr) {
    uint8_t* mods = Deref(player + Chr_Modules); if (!mods) return -1;
    uint8_t* data = Deref(mods + Mod_Data); if (!data) return -1;
    int f = -1, m = -1; SafeRead(data + Data_Fp, &f, 4); SafeRead(data + Data_MaxFp, &m, 4);
    if (maxOut) *maxOut = m;
    return f;
}
bool WriteFp(uint8_t* player, int value) {
    uint8_t* mods = Deref(player + Chr_Modules); uint8_t* data = mods ? Deref(mods + Mod_Data) : nullptr;
    if (!data) return false;
    if (value < 0) value = 0;
    return SafeWrite(data + Data_Fp, &value, 4);
}
CSChrTaeAnimEvent* PlayerTaeEvent(uint8_t* player) {
    uint8_t* mods = Deref(player + Chr_Modules); if (!mods) return nullptr;
    uint8_t* ta = Deref(mods + Mod_TimeAct); if (!ta) return nullptr;
    auto* ev = (CSChrTaeAnimEvent*)Deref(ta + TA_ChrTaeAnimEvent); if (!ev) return nullptr;
    void** vft = nullptr; if (!SafeRead(&ev->vft, &vft, sizeof(vft))) return nullptr;
    if (g_taeVft && vft != g_taeVft) return nullptr;   // not the object we expect: refuse to call into it
    return ev;
}

// Templates from config unless learned ones exist. Source is always forced to the swinging hand:
// the game's "default" source follows the hand HKS last attacked with, which goes stale after
// weapon swaps / left-hand actions and then resolves the wrong weapon's BehaviorParam row.
void ApplyConfigTemplates() {
    const Config& c = Cfg();
    static int lastCfg[SlotCount] = {-2, -2, -2, -2, -2};   // ini hot-reload with new ids overrides learned templates
    int cur[SlotCount] = {c.lightJudgeId, c.heavyJudgeId, c.leftJudgeId, c.twoHandLightJudgeId, c.twoHandHeavyJudgeId};
    for (int i = 0; i < SlotCount; ++i) if (cur[i] != lastCfg[i]) { if (lastCfg[i] != -2) g_tpl[i].valid = false; lastCfg[i] = cur[i]; }
    auto set = [&](int slot, int judge, uint8_t source) {
        if (judge < 0 || g_tpl[slot].valid) return;
        Template t; t.valid = true; t.a = {0, 0, judge, (uint8_t)0xFF, source, 0};
        g_tpl[slot] = t;
        LOGI("[combat] %s attack from config: judgeId %d source %u", SlotName(slot), judge, source);
    };
    set(SlotLight, c.lightJudgeId, 1); set(SlotHeavy, c.heavyJudgeId, 1); set(SlotLeft, c.leftJudgeId, 2);
    set(SlotLight2H, c.twoHandLightJudgeId, 1); set(SlotHeavy2H, c.twoHandHeavyJudgeId, 1);
}
void SaveLearned(int slot, int judgeId) {
    const Config& c = Cfg();
    if (!c.combatSaveLearned || c.iniPath.empty()) return;
    const wchar_t* key = SlotIniKey(slot);
    wchar_t v[16]; swprintf_s(v, L"%d", judgeId);
    if (WritePrivateProfileStringW(L"Combat", key, v, c.iniPath.c_str())) LOGI("[combat] saved [Combat] %S=%d to ERVR.ini", key, judgeId);
}
bool PhysicalActiveLocked() {
    const Config& c = Cfg();
    if (!c.fpEnabled || !c.combatEnabled) return false;
    if (c.combatMode == "buttons") return false;
    if (c.combatMode == "physical") return true;
    return g_tpl[SlotLight].valid;   // auto
}

// ---------------------------------------------------------------- two-handing + skill state
// The worker owns the grab detection and the skill trigger; the game thread reads
// ChrAsm.arm_style (the authority) and asks for the game's own chord when the two disagree. The
// chord itself is played on the worker (CombatForcedButtons), where the pad is built.
struct TwoHandState {
    bool want = false;               // the grab says: two-hand
    int  weaponHand = HandRight;     // whose weapon is held...
    int  pinHand = HandLeft;         // ...by which free hand
    float along = 0.f;               // m along the weapon axis (from the weapon hand's controller) where it was grabbed
    TwoHandGripOrientation grip;     // supporting wrist's angle relative to the weapon at that grab
    bool inZone = false;             // the free hand is on the shaft right now (its grip is the grab, not the skill button)
    bool owned = false;              // ERVR put the game into two-hand; only then does it take it back out
} g_two;                             // under g_mtx
std::atomic<int> g_menuOpen{0};            // pad worker -> everyone: a game menu is open (see CombatSetMenuOpen)
int g_armStyle = 0;                        // game thread: ChrAsm.arm_style as last read (0 empty, 1 one-handed, 2 left both, 3 right both)
std::atomic<int> g_armStyleShared{0};      // game thread -> worker: the same
std::atomic<int> g_wepPresent[2]{0, 0};    // game thread -> worker: a (non-shield) weapon sits in that hand
// Chord requests, game thread -> worker: 1 = two-hand toggle of the right weapon (modifier + light),
// 2 = of the left weapon (modifier + guard), 3 = the skill button, 4/5 = a learn press (light/heavy)
// so the game teaches its two-handed attack rows.
std::atomic<int> g_chordReq{0};
struct ChordRun { int phase = 0; double t = 0; uint16_t lead = 0, key = 0; int kind = 0; } g_chord;   // worker only

// Skill (Ash of War): trigger on the worker, recording and replay on the game thread.
std::atomic<int> g_skillReq{0};            // worker -> game thread: 1 = effect (replay; record if unknown), 2 = the game's way (+ record)
struct RecEvent {
    uint32_t id = 0, animId = 0;
    uint8_t args[64]{}; size_t argLen = 0; // the game's TAE args, copied (plain values per the TAE templates)
    float from = 0, to = 0;
    float firstT = 0, lastT = 0;           // s after the press: first / last frame this instance executed
    float animTime0 = 0;                   // *animTime at its first execution
    int count = 0;
    const void* inst = nullptr;            // the game's args pointer = the instance's identity while recording
};
struct SkillRecording {
    bool valid = false;
    int weaponId = 0, arts = -1;           // key: the weapon (affinity + level) and the SwordArtsParam it carried
    std::vector<RecEvent> ev;
    int fpObserved = 0;                    // FP the game took while recording (cost fallback)
    bool hasAttack = false; AttackBehaviorArgs attack{};   // the skill's first AttackBehavior: the next swing's row
    float length = 0.f;
};
std::vector<SkillRecording> g_skillLib;    // game thread only
struct SkillRec { bool active = false; double start = 0; int fpStart = 0, fpMin = 0; SkillRecording rec; } g_rec;
struct SkillPlay {
    bool active = false; double start = 0; int libIndex = -1;
    struct St { bool started = false, done = false; CSTaeAnimEventArgs args{}; float time = 0.f; uint8_t argBuf[64]{}; };
    std::vector<St> st;                    // sized once at start: the game keys instances on the args pointer
    bool costCharged = false; int fpCost = 0, stamCost = 0;
} g_play;
struct SkillSwing { bool armed = false; double until = 0; AttackBehaviorArgs a{}; } g_skillSwing;   // game thread
struct SkillInfo { int weaponId = 0; int arts = -1, gem = -1; int fpCost = 0, stamCost = 0; bool logged = false; } g_skillInfo[2];   // per hand, game thread

uint32_t ParseVRButton(const std::string& n) {
    if (n == "RTrigger") return VRB_RTrigger; if (n == "LTrigger") return VRB_LTrigger;
    if (n == "RGrip") return VRB_RGrip;       if (n == "LGrip") return VRB_LGrip;
    if (n == "A") return VRB_A; if (n == "B") return VRB_B; if (n == "X") return VRB_X; if (n == "Y") return VRB_Y;
    if (n == "RThumb") return VRB_RThumb;     if (n == "LThumb") return VRB_LThumb;
    return 0;
}
// The XInput button a VR control is mapped to in [Buttons] (what a two-handed skill trigger must not also send).
uint16_t MappedButtonOf(const std::string& n, const Config& c) {
    if (n == "RTrigger") return ParseXInputButton(c.btnRTrigger); if (n == "LTrigger") return ParseXInputButton(c.btnLTrigger);
    if (n == "RGrip") return ParseXInputButton(c.btnRGrip);       if (n == "LGrip") return ParseXInputButton(c.btnLGrip);
    if (n == "A") return ParseXInputButton(c.btnA); if (n == "B") return ParseXInputButton(c.btnB);
    if (n == "X") return ParseXInputButton(c.btnX); if (n == "Y") return ParseXInputButton(c.btnY);
    if (n == "RThumb") return ParseXInputButton(c.btnRThumb);     if (n == "LThumb") return ParseXInputButton(c.btnLThumb);
    return 0;
}
bool ReplayAllowed(uint32_t id) {   // game thread
    static std::string s_src; static std::vector<uint32_t> s_ids;
    const std::string& s = Cfg().skillReplayEvents;
    if (s != s_src) {
        s_src = s; s_ids.clear(); size_t p = 0;
        while (p < s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); if (q > p) s_ids.push_back((uint32_t)atoi(s.substr(p, q - p).c_str())); p = q + 1; }
    }
    for (uint32_t x : s_ids) if (x == id) return true;
    return false;
}
bool TwoHandedStyle(int style) { return style == 2 || style == 3; }
int  TwoHandedWeaponHand(int style) { return style == 2 ? HandLeft : HandRight; }

// Skill recording (game thread, from both TAE hooks): every event the game runs on the player
// while the window is open is kept - id, a copy of its args, when it started, how long it kept
// executing - keyed by the game's own args pointer, which identifies the instance across frames.
void RecordEvent(CSChrTaeAnimEvent* self, CSTaeAnimEventArgs* args, uint32_t id) {
    SkillRec& r = g_rec;
    const float t = (float)(NowSec() - r.start);
    void* a = nullptr; float from = 0, to = 0; const float* at = nullptr; uint32_t animId = 0;
    SafeRead(&args->args, &a, sizeof(a)); SafeRead(&args->from, &from, 4); SafeRead(&args->to, &to, 4);
    SafeRead(&args->animTime, &at, sizeof(at)); SafeRead(&self->animId, &animId, 4);
    for (RecEvent& e : r.rec.ev) if (e.inst == a && e.id == id) { e.lastT = t; ++e.count; return; }
    if (r.rec.ev.size() >= 64) return;
    RecEvent e; e.id = id; e.animId = animId; e.inst = a; e.from = from; e.to = to; e.firstT = e.lastT = t; e.count = 1;
    if (at) SafeRead(at, &e.animTime0, 4);
    if (a) { for (size_t n = sizeof(e.args); n >= 8; n /= 2) if (SafeRead(a, e.args, n)) { e.argLen = n; break; } }
    if (id == EV_AttackBehavior && !r.rec.hasAttack && e.argLen >= sizeof(AttackBehaviorArgs)) { memcpy(&r.rec.attack, e.args, sizeof(AttackBehaviorArgs)); r.rec.hasAttack = true; }
    r.rec.ev.push_back(e);
    if (Cfg().combatDebug) {
        int32_t w[4] = {0, 0, 0, 0}; memcpy(w, e.args, e.argLen < 16 ? e.argLen : 16);
        LOGD("[combat] skill rec: t %.2f event %u anim %u args %d %d %d %d (%zu B) %.2f..%.2f", t, id, animId, w[0], w[1], w[2], w[3], e.argLen, from, to);
    }
}

// ---------------------------------------------------------------- cast aim (worker + game thread)
// A wepType listed in CastAimWeaponTypes: the hand holding it aims ranged effects even without a press.
bool IsRangedWepType(uint16_t t) {
    const std::string& s = Cfg().castAimWeaponTypes;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || s[i] == ' ')) ++i;
        if (i >= s.size()) break;
        char* end = nullptr; const long v = strtol(s.c_str() + i, &end, 10);
        if (end == s.c_str() + i) break;
        if (v == (long)t) return true;
        i = (size_t)(end - s.c_str());
    }
    return false;
}
// A press (or a held charge) makes `hand` the aim hand for CastAimSeconds from now.
void ArmCastAim(int hand, double now, const char* why) {
    const Config& c = Cfg();
    if (!c.castAim || hand < 0) return;
    const bool wasLive = g_castUntil.load() > now;
    const int prev = g_castPressHand.exchange(hand);
    g_castUntil.store(now + (c.castAimSeconds > 0.f ? c.castAimSeconds : 0.f));
    if (!wasLive || prev != hand) LOGD("[combat] cast aim: %s hand aims (%s)", HandName(hand), why);
}
// Arm from the game's OWN aim events - 238 SetBulletAimAngle at a cast's start, 2 BulletBehavior at each bullet -
// so only an animation that actually aims something engages the lock. The hand: the one whose attack/skill
// button was pressed within the last kCastPressMemory s, else the one holding a ranged weapon, else the right.
// Game thread (TAE hooks).
constexpr double kCastPressMemory = 3.0;
int CastEventHand(double now) {
    int hand = now - g_castPressAt.load() < kCastPressMemory ? g_castPressHand.load() : -1;
    if (hand < 0) hand = g_castWepHand.load();
    return hand < 0 ? HandRight : hand;
}
void ArmCastAimFromEvent(const char* why) {
    const double now = NowSec();
    ArmCastAim(CastEventHand(now), now, why);
}
// The cast animation's SetBulletAimAngle (238) clamps the bullet's aim with limits and deadzones sized
// for a camera a few degrees off the body; EnableTwistModifier (700) does the same for the upper-body
// twist. With a casting hand the player's copies are relaxed: deadzones / minimum angles zeroed (what
// erfps2 does for its first-person camera), limits raised to CastAimLimitDeg. Returns true when the
// event was run with the relaxed copy.
bool RunRelaxedAimEvent(CSChrTaeAnimEvent* self, CSTaeAnimEventArgs* args, uint32_t id) {
    const Config& c = Cfg();
    uint8_t* owner = nullptr; void* a = nullptr;
    if (!SafeRead(&self->owner, &owner, sizeof(owner)) || !owner || owner != g_player) return false;
    if (!SafeRead(&args->args, &a, sizeof(a)) || !a) return false;
    if (id == EV_SetBulletAimAngle) ArmCastAimFromEvent("SetBulletAimAngle");   // a cast is aiming: this is what makes the aim live
    if (CombatCastAimHand(nullptr) < 0) return false;
    if (id == EV_SetBulletAimAngle) {
        BulletAimAngleArgs b{};
        if (!SafeRead(a, &b, sizeof(b))) return false;
        const BulletAimAngleArgs o = b;
        const int16_t lim = (int16_t)Clamp(c.castAimLimitDeg, 0.f, 180.f);
        b.upDead = b.downDead = b.rightDead = b.leftDead = 0;
        if (lim > 0) {   // only widen a limit the animation set (0 / negative may mean "no limit": left alone)
            if (b.upLimit > 0 && b.upLimit < lim) b.upLimit = lim;
            if (b.downLimit > 0 && b.downLimit < lim) b.downLimit = lim;
            if (b.rightLimit > 0 && b.rightLimit < lim) b.rightLimit = lim;
            if (b.leftLimit > 0 && b.leftLimit < lim) b.leftLimit = lim;
        }
        g_aimArgsCopy = b;
        args->args = &g_aimArgsCopy; g_origGeneric(self, args); args->args = a;
        if (c.combatDebug) {
            static double s_last = 0; const double n = NowSec();
            if (n - s_last > 0.5) { s_last = n; LOGD("[combat] cast aim: SetBulletAimAngle limits %d/%d/%d/%d deadzones %d/%d/%d/%d -> limits %d/%d/%d/%d deadzones 0 (anim %u)", o.upLimit, o.downLimit, o.rightLimit, o.leftLimit, o.upDead, o.downDead, o.rightDead, o.leftDead, b.upLimit, b.downLimit, b.rightLimit, b.leftLimit, self->animId); }
        }
        return true;
    }
    TwistModifierArgs t{};
    if (!SafeRead(a, &t, sizeof(t))) return false;
    t.upMin = t.downMin = t.rightMin = t.leftMin = 0.f;
    g_twistArgsCopy = t;
    args->args = &g_twistArgsCopy; g_origGeneric(self, args); args->args = a;
    return true;
}

// ---------------------------------------------------------------- TAE hooks (game thread)
// Slot 2: the generic events (SpEffect, FFX, sound, ...). Watched while a skill is being recorded;
// the cast animation's aim events are relaxed for the player while a casting hand aims.
void __fastcall Hook_GenericEvent(CSChrTaeAnimEvent* self, CSTaeAnimEventArgs* args) {
    uint32_t id = 0;
    const bool haveId = !g_tripped && SafeRead(&args->eventId, &id, 4);
    if (haveId && g_rec.active) {
        uint8_t* owner = nullptr;
        if (id != EV_AttackBehavior && id != EV_BulletBehavior && id != EV_CommonBehavior   // those pass through slot 3 too
            && SafeRead(&self->owner, &owner, sizeof(owner)) && owner && owner == g_player) RecordEvent(self, args, id);
    }
    if (haveId && (id == EV_SetBulletAimAngle || id == EV_EnableTwistModifier) && Cfg().castAim && RunRelaxedAimEvent(self, args, id)) return;
    g_origGeneric(self, args);
}
// Slot 3: the behavior events (attack / bullet / common).
void __fastcall Hook_ExecEvent(CSChrTaeAnimEvent* self, CSTaeAnimEventArgs* args) {
    uint32_t id = 0; uint8_t* owner = nullptr; void* a = nullptr; uint8_t first = 0;
    bool isPlayer = false;
    // Recording: this slot sees every event too, but only dispatches the behavior ones (slot 2 records the rest).
    if (!g_tripped && g_rec.active && SafeRead(&args->eventId, &id, 4) && (id == EV_AttackBehavior || id == EV_BulletBehavior || id == EV_CommonBehavior)
        && SafeRead(&self->owner, &owner, sizeof(owner)) && owner && owner == g_player) RecordEvent(self, args, id);
    // Cast aim: a bullet leaving the player (spells without a SetBulletAimAngle, ranged skills, bows) arms the aim.
    if (!g_tripped && Cfg().castAim && SafeRead(&args->eventId, &id, 4) && id == EV_BulletBehavior && SafeRead(&self->owner, &owner, sizeof(owner)) && owner && owner == g_player)
        ArmCastAimFromEvent("BulletBehavior");
    // Body haptics: the bullet leaves the casting hand - once, on the event's first execution (it runs every
    // frame of its window), and once per volley.
    if (!g_tripped && SafeRead(&args->eventId, &id, 4) && id == EV_BulletBehavior && SafeRead(&self->owner, &owner, sizeof(owner)) && owner && owner == g_player &&
        SafeRead(&args->isFirstExecution, &first, 1) && first) {
        static double s_lastCast = 0; const double now = NowSec();
        if (now - s_lastCast > 0.08) { s_lastCast = now; BodyHapticsPost(BodyKind::Cast, CastEventHand(now)); }
    }
    if (!g_tripped && SafeRead(&args->eventId, &id, 4) && id == EV_AttackBehavior && SafeRead(&self->owner, &owner, sizeof(owner)) && owner && owner == g_player) {
        isPlayer = true;
        SafeRead(&args->args, &a, sizeof(a)); SafeRead(&args->isFirstExecution, &first, 1);
        AttackBehaviorArgs ab{};
        if (a && SafeRead(a, &ab, sizeof(ab))) {
            const Config& c = Cfg();
            double now = NowSec();
            if (c.combatDebug && first && now - g_lastEvLog > 0.05) {
                g_lastEvLog = now;
                LOGD("[combat] game AttackBehavior: type %d index %d judgeId %d dir %u source %u state %d anim %u", ab.attackType, ab.attackIndex, ab.judgeId, ab.dirType, ab.source, ab.stateInfo, self->animId);
            }
            // Learn: first execution of the first hit (attackIndex 0) shortly after we pulsed an attack button.
            if (c.combatLearn && first && ab.attackIndex == 0) {
                int learnedSlot = -1, learnedJudge = 0;
                {
                    std::lock_guard<std::mutex> g(g_mtx);
                    int best = -1; double bt = -1e9;
                    for (int s = 0; s < 3; ++s) if (now - g_lastBtn[s] < 1.0 && g_lastBtn[s] > bt) { bt = g_lastBtn[s]; best = s; }
                    if (best >= 0) {
                        // While the game reports a two-handed arm style the press taught the 2H row.
                        int slot = best;
                        if (TwoHandedStyle(g_armStyle) && best != SlotLeft) slot = best == SlotLight ? SlotLight2H : SlotHeavy2H;
                        AttackBehaviorArgs t = ab; t.source = best == SlotLeft ? 2 : 1; t.attackIndex = 0;
                        bool changed = !g_tpl[slot].valid || memcmp(&g_tpl[slot].a, &t, sizeof(t)) != 0;
                        g_tpl[slot].valid = true; g_tpl[slot].a = t; g_lastBtn[best] = -1e9;
                        if (changed) { learnedSlot = slot; learnedJudge = ab.judgeId; }
                    }
                }
                if (learnedSlot >= 0) {
                    LOGI("[combat] learned %s attack: judgeId %d (type %d dir %u state %d, anim %u) -> [Combat] %S=%d",
                         SlotName(learnedSlot), learnedJudge, ab.attackType, ab.dirType, ab.stateInfo, self->animId, SlotIniKey(learnedSlot), learnedJudge);
                    SaveLearned(learnedSlot, learnedJudge);
                }
            }
            if (c.combatSuppressAnimAttacks) { std::lock_guard<std::mutex> g(g_mtx); if (PhysicalActiveLocked()) return; }
        }
    }
    (void)isPlayer;
    g_origExec(self, args);
}

int SehFilter(unsigned code) { LOGE("[combat] exception 0x%08X; physical combat disabled", code); return EXCEPTION_EXECUTE_HANDLER; }

// ---------------------------------------------------------------- kinetic: equipment + params (game thread)
struct HandWeapon {
    bool valid = false;
    int  paramId = 0;         // equipped item's EquipParamWeapon id (upgrade level included)
    int  variation = 0;       // behaviorVariationId
    float weight = 0.f;
    uint16_t wepType = 0;
    uint8_t* row = nullptr;   // EQUIP_PARAM_WEAPON_ST base row
    bool isShield = false;
    bool isRanged = false;    // a CastAimWeaponTypes class (staff, seal, bow, ...): the hand aims, its buttons cast
};
HandWeapon g_wep[2];
std::atomic<float> g_wepWeight[2]{0.f, 0.f};   // game thread -> worker: equipped weight per hand (inertia)
std::atomic<float> g_wepReach[2]{0.f, 0.f};    // game thread -> worker/game_er: grip->tip length per hand (0 = unknown)
std::atomic<float> g_wepCom[2]{0.f, 0.f};      // ... and the centre-of-mass distance from the grip (0 = unknown)

// Weapon geometry per category (EquipParamWeapon wepType = WEP_TYPE, values per Paramdex): grip->tip
// length in m and the mass centre as a fraction of it. No param carries either, so this is a table
// by category (the game's own hit capsule dummy polys could replace it later). Blades balance about a
// third of the way out, hafted heads sit at the end, shields and fists hug the hand. Twinblades are
// gripped in the middle: half the shaft is the lever, the mass is at the hand.
struct WeaponGeom { float length, com; };
WeaponGeom GeomForType(uint16_t t) {
    switch (t) {
    case 1:  return {0.35f, 0.40f};   // dagger
    case 3:  return {0.95f, 0.35f};   // straight sword
    case 5:  return {1.25f, 0.40f};   // greatsword
    case 7:  return {1.60f, 0.45f};   // colossal sword
    case 9:  return {0.85f, 0.40f};   // curved sword
    case 11: return {1.20f, 0.45f};   // curved greatsword
    case 13: return {1.00f, 0.35f};   // katana
    case 14: return {0.70f, 0.10f};   // twinblade (centre grip)
    case 15: return {1.00f, 0.30f};   // thrusting sword
    case 16: return {1.20f, 0.40f};   // heavy thrusting sword
    case 17: return {0.70f, 0.75f};   // axe
    case 19: return {1.30f, 0.75f};   // greataxe
    case 21: return {0.70f, 0.80f};   // hammer
    case 23: return {1.30f, 0.85f};   // great hammer
    case 24: return {0.80f, 0.85f};   // flail
    case 25: return {1.60f, 0.40f};   // spear
    case 28: return {1.90f, 0.45f};   // heavy spear
    case 29: return {1.80f, 0.60f};   // halberd
    case 31: return {1.70f, 0.65f};   // scythe / reaper
    case 35: return {0.15f, 0.30f};   // fist
    case 37: return {0.30f, 0.30f};   // claw
    case 39: return {1.50f, 0.30f};   // whip
    case 41: return {1.60f, 0.75f};   // colossal weapon
    case 50: case 51: case 53: case 55: case 56: return {1.00f, 0.30f};   // bows, ballista
    case 57: return {1.20f, 0.50f};   // glintstone staff
    case 61: return {0.20f, 0.30f};   // sacred seal
    case 65: return {0.30f, 0.15f};   // small shield
    case 67: return {0.45f, 0.20f};   // medium shield
    case 69: return {0.80f, 0.35f};   // greatshield
    case 87: return {0.60f, 0.60f};   // torch
    case 88: return {0.15f, 0.30f};   // hand-to-hand
    case 89: return {0.30f, 0.50f};   // perfume bottle
    case 90: return {0.70f, 0.40f};   // thrusting shield
    case 91: return {0.40f, 0.50f};   // throwing blade
    case 92: return {0.70f, 0.40f};   // backhand blade
    case 93: return {1.15f, 0.35f};   // light greatsword
    case 94: return {1.40f, 0.40f};   // great katana
    case 95: return {0.40f, 0.40f};   // beast claw
    }
    return {0.f, 0.f};   // unknown category: the ini WeaponReach stands in
}
// ini override "type:length:comFraction,..." (WeaponGeometry) wins over the table.
bool GeomOverride(uint16_t t, WeaponGeom* out) {
    const std::string& s = Cfg().weaponGeometry;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(',', i); if (e == std::string::npos) e = s.size();
        int ty = 0; float len = 0.f, com = 0.f;
        if (sscanf_s(s.c_str() + i, "%d:%f:%f", &ty, &len, &com) == 3 && ty == (int)t && len > 0.05f) { *out = {len, com}; return true; }
        i = e + 1;
    }
    return false;
}
WeaponGeom GeomFor(const HandWeapon& w) {
    if (!w.valid || w.paramId <= 0) return {0.f, 0.f};
    WeaponGeom g; if (!GeomOverride(w.wepType, &g)) g = GeomForType(w.wepType);
    return g;
}
bool g_layoutBad = false;      // a param row size disagreed with the layout we compiled against: never write
bool g_loggedLayout = false;

bool IsShieldType(uint16_t t) {
    const std::string& s = Cfg().shieldWepTypes;
    size_t p = 0;
    while (p < s.size()) {
        size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
        if (q > p && (uint16_t)atoi(s.substr(p, q - p).c_str()) == t) return true;
        p = q + 1;
    }
    return false;
}

bool CheckRowSize(const char* what, uint32_t got, uint32_t expected) {
    if (got == 0 || got == expected) return true;
    if (!g_loggedLayout) { g_loggedLayout = true; LOGW("[combat] %s row size 0x%X != expected 0x%X: kinetic param edits DISABLED (layout changed)", what, got, expected); }
    g_layoutBad = true;
    return false;
}

void ReadEquipment(uint8_t* player) {
    uint8_t* asmp = Deref(player + Player_ChrAsm);
    if (!asmp) { g_wep[0].valid = g_wep[1].valid = false; g_wepWeight[0].store(0.f); g_wepWeight[1].store(0.f); return; }
    int style = 0; uint32_t slotL = 0, slotR = 0;
    SafeRead(asmp + Asm_ArmStyle, &style, 4); SafeRead(asmp + Asm_LeftSlot, &slotL, 4); SafeRead(asmp + Asm_RightSlot, &slotR, 4);
    if (slotL > 2) slotL = 0; if (slotR > 2) slotR = 0;
    g_armStyle = style;
    const uint32_t idx[2] = {slotL * 2 + 0, slotR * 2 + 1};
    for (int h = 0; h < 2; ++h) {
        int id = 0; SafeRead(asmp + Asm_ParamIds + idx[h] * 4, &id, 4);
        HandWeapon& w = g_wep[h];
        if (id <= 0 || id >= 100000000) { w.valid = false; continue; }
        if (w.valid && w.paramId == id) continue;   // unchanged: keep the resolved row
        w = HandWeapon{}; w.paramId = id;
        if (!ParamsReady()) continue;
        uint32_t sz = 0;
        uint8_t* row = ParamsRow(PI_EquipParamWeapon, (uint32_t)(id / 100) * 100u, &sz);
        if (!row) { LOGW("[combat] %s weapon %d: no EquipParamWeapon row", HandName(h), id); w.valid = true; continue; }
        CheckRowSize("EQUIP_PARAM_WEAPON_ST", sz, Wep_RowSize);
        SafeRead(row + Wep_VariationId, &w.variation, 4); SafeRead(row + Wep_Weight, &w.weight, 4); SafeRead(row + Wep_Type, &w.wepType, 2);
        w.row = row; w.isShield = IsShieldType(w.wepType); w.isRanged = !w.isShield && IsRangedWepType(w.wepType); w.valid = true;
        const WeaponGeom g = GeomFor(w);
        LOGI("[combat] %s hand: weapon %d variation %d weight %.1f wepType %u%s length %.2f m mass at %.2f m (player level %d)", HandName(h), id, w.variation, w.weight, w.wepType, w.isShield ? " (shield)" : w.isRanged ? " (ranged: cast aim)" : "", g.length, g.length * g.com, GameERPlayerLevel());
    }
    // Cast aim: publish the weapon classes for the worker (button routing, the physical block) and the
    // passive aim hand - the one holding a catalyst/bow (two-handed: that weapon's hand; both hands: the right).
    for (int h = 0; h < 2; ++h) {
        g_wepRanged[h].store(g_wep[h].valid && g_wep[h].isRanged ? 1 : 0);
        g_wepShield[h].store(g_wep[h].valid && g_wep[h].isShield ? 1 : 0);
    }
    {
        int wepHand = -1;
        if (TwoHandedStyle(style)) { const int th = TwoHandedWeaponHand(style); if (g_wep[th].valid && g_wep[th].isRanged) wepHand = th; }
        else if (g_wep[HandRight].valid && g_wep[HandRight].isRanged) wepHand = HandRight;
        else if (g_wep[HandLeft].valid && g_wep[HandLeft].isRanged) wepHand = HandLeft;
        if (g_castWepHand.exchange(wepHand) != wepHand && Cfg().castAim)
            LOGI("[combat] cast aim: %s", wepHand < 0 ? "no ranged weapon in hand - the head aims until an attack/skill button is pressed"
                                                     : wepHand == HandRight ? "the right hand holds a ranged weapon and aims" : "the left hand holds a ranged weapon and aims");
    }
    // Carried weight per hand. Two-handing puts the one weapon in both hands (each arm carries
    // it, so each lags with it); the free hand of a two-hander carries nothing extra.
    float wl = g_wep[HandLeft].valid ? g_wep[HandLeft].weight : 0.f, wr = g_wep[HandRight].valid ? g_wep[HandRight].weight : 0.f;
    if (style == 3) wl = wr; else if (style == 2) wr = wl;
    g_wepWeight[HandLeft].store(wl); g_wepWeight[HandRight].store(wr);
    WeaponGeom gl = GeomFor(g_wep[HandLeft]), gr = GeomFor(g_wep[HandRight]);
    if (style == 3) gl = gr; else if (style == 2) gr = gl;
    g_wepReach[HandLeft].store(gl.length); g_wepCom[HandLeft].store(gl.length * gl.com);
    g_wepReach[HandRight].store(gr.length); g_wepCom[HandRight].store(gr.length * gr.com);
}

// Distinct behaviorVariationIds that actually have BehaviorParam_PC rows (built once, ~14k rows).
// Unique-moveset weapons carry a variation of their own in EquipParamWeapon (Sword of Night and
// Flame = 214) that has NO rows of its own for the basic attacks - the game falls back to the
// class base. We do the same: exact, then the tens, then the hundreds, then the nearest lower.
const std::vector<int>& BehaviorVariations() {
    static std::vector<int> v; static bool built = false;
    if (built) return v;
    const uint32_t n = ParamsRowCount(PI_BehaviorParam_PC);
    if (n == 0) return v;
    v.reserve(256);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = 0; uint8_t* row = ParamsRowAt(PI_BehaviorParam_PC, i, &id); int var = -1;
        if (row && SafeRead(row + Bhv_VariationId, &var, 4) && var >= 0) v.push_back(var);
    }
    std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
    built = true;
    LOGI("[combat] BehaviorParam_PC: %u rows, %zu distinct variations", n, v.size());
    return v;
}
// Candidate variations for a weapon, best first. A unique-moveset weapon's own variation (804)
// often has rows for its weapon art ONLY, so the fallback has to be tried per judge id, not
// just when the variation is absent altogether.
int VariationCandidates(int var, int out[4]) {
    const std::vector<int>& v = BehaviorVariations();
    auto has = [&](int x) { return std::binary_search(v.begin(), v.end(), x); };
    int n = 0;
    auto add = [&](int x) { if (x < 0) return; for (int i = 0; i < n; ++i) if (out[i] == x) return; if (n < 4) out[n++] = x; };
    add(var);
    if (v.empty()) return n;
    const int c10 = var - (var % 10), c100 = var - (var % 100);
    if (has(c10)) add(c10);
    if (has(c100)) add(c100);
    auto it = std::lower_bound(v.begin(), v.end(), var);
    if (it != v.begin()) add(*(it - 1));
    return n;
}
// The BehaviorParam_PC row for (variation, judge): id formula verified by reading the fields back,
// then a field scan.
uint8_t* FindBehaviorRow(int variation, int judge, uint32_t* idOut) {
    uint32_t bhvId = BehaviorPcBase + (uint32_t)variation * 1000u + (uint32_t)judge;
    uint8_t* b = ParamsRow(PI_BehaviorParam_PC, bhvId);
    int var = -1, jd = -1;
    if (b) { SafeRead(b + Bhv_VariationId, &var, 4); SafeRead(b + Bhv_JudgeId, &jd, 4); if (var != variation || jd != judge) b = nullptr; }
    if (!b) {
        const uint32_t n = ParamsRowCount(PI_BehaviorParam_PC);
        for (uint32_t i = 0; i < n && !b; ++i) {
            uint32_t id = 0; uint8_t* row = ParamsRowAt(PI_BehaviorParam_PC, i, &id);
            if (!row) continue;
            if (SafeRead(row + Bhv_VariationId, &var, 4) && var == variation && SafeRead(row + Bhv_JudgeId, &jd, 4) && jd == judge) { b = row; bhvId = id; }
        }
    }
    if (b && idOut) *idOut = bhvId;
    return b;
}

// The AtkParam_Pc rows a (weapon variation, judge id) pair can resolve to. The BehaviorParam_PC
// row id formula (1_0000_0000 + variation*1000 + judge, verified in ER 1.16 for 700/800/2400) is
// checked by reading the row's own variation/judge fields back, with a field scan as fallback.
// ALL candidate variations that carry the judge are collected and edited together (2026-08-31):
// the game runs its own variation fallback for a unique-moveset weapon (var 214 has no basic
// rows), and editing only OUR best candidate left the row the game actually used untouched -
// full vanilla damage on every real hit while the strike log printed a mult.
struct AtkResolve {
    bool valid = false; int variation = -1, judge = -1;
    int n = 0;
    uint8_t* atkRow[4] = {}; uint8_t* bhvRow[4] = {};
    uint32_t bhvId[4] = {}; uint32_t atkId[4] = {};
    int resolvedVar = -1;    // best candidate variation (first with a row) - the alignment target
    int bhvStamina = 0;      // of the best candidate
};
AtkResolve g_resolve[2];
AtkResolve* ResolveAttack(int h, int wepVariation, int judge) {
    AtkResolve& r = g_resolve[h];
    if (r.variation == wepVariation && r.judge == judge) return r.valid ? &r : nullptr;
    r = AtkResolve{}; r.variation = wepVariation; r.judge = judge;
    if (!ParamsReady() || judge < 0) return nullptr;
    int cands[4]; const int nc = VariationCandidates(wepVariation, cands);
    for (int i = 0; i < nc && r.n < 4; ++i) {
        uint32_t bhvId = 0;
        uint8_t* b = FindBehaviorRow(cands[i], judge, &bhvId);
        if (!b) continue;
        { uint32_t sz = 0; ParamsRow(PI_BehaviorParam_PC, bhvId, &sz); CheckRowSize("BEHAVIOR_PARAM_ST", sz, Bhv_RowSize); }
        int refId = -1; uint8_t refType = 0xFF; int stam = 0;
        SafeRead(b + Bhv_RefType, &refType, 1); SafeRead(b + Bhv_RefId, &refId, 4); SafeRead(b + Bhv_Stamina, &stam, 4);
        if (refType != 0 || refId < 0) { LOGW("[combat] BehaviorParam_PC %u refType %u refId %d is not an attack: skipped", bhvId, refType, refId); continue; }
        uint32_t asz = 0;
        uint8_t* a = ParamsRow(PI_AtkParam_Pc, (uint32_t)refId, &asz);
        if (!a) { LOGW("[combat] no AtkParam_Pc row %d for BehaviorParam_PC %u", refId, bhvId); continue; }
        CheckRowSize("ATK_PARAM_ST", asz, Atk_RowSize);
        if (r.n == 0) { r.resolvedVar = cands[i]; r.bhvStamina = stam; }
        r.bhvRow[r.n] = b; r.atkRow[r.n] = a; r.bhvId[r.n] = bhvId; r.atkId[r.n] = (uint32_t)refId; ++r.n;
    }
    if (r.n == 0) { LOGW("[combat] no BehaviorParam_PC row for judge %d under variation %d or its class bases: strikes keep vanilla damage", judge, wepVariation); return nullptr; }
    r.valid = true;
    if (r.resolvedVar != wepVariation) LOGI("[combat] %s hand: judge %d not under variation %d; editing %d candidate row(s), best class variation %d", HandName(h), judge, wepVariation, r.n, r.resolvedVar);
    uint16_t mv = 0; SafeRead(r.atkRow[0] + Atk_PhysCorr, &mv, 2);
    LOGI("[combat] %s hand judge %d -> BehaviorParam_PC %u (stamina %d) -> AtkParam_Pc %u (phys MV %u%%), %d row(s) edited per strike", HandName(h), judge, r.bhvId[0], r.bhvStamina, r.atkId[0], mv, r.n);
    return &r;
}

// Live edit of the resolved AtkParam_Pc rows' motion values (plus hit-reaction level, hitstop,
// poise and capsule radii); originals captured once, restored when the strike ends.
struct AtkRowEdit { uint8_t* row = nullptr; uint16_t orig[6]{}; float origSA = 0.f; float origRad[4]{}; float origHitStop = 0.f; uint8_t origDmg = 0; };
struct AtkEdit { bool active = false; int n = 0; AtkRowEdit e[4]; float mult = -1.f; bool suppress = false; };
AtkEdit g_atk[2];
const size_t kAtkCorr[6] = {Atk_PhysCorr, Atk_MagCorr, Atk_FireCorr, Atk_ThunCorr, Atk_DarkCorr, Atk_StamCorr};

// Stamina-off mode: the game charges a behavior's stamina cost on every hit (and with continuous
// hits, every frame of contact), so the only real disconnect is to make that cost 1 at the source
// - the BehaviorParam_PC row's stamina field - for the strike's duration. The 1 that is still
// charged is the hit signal (haptics) and is refunded.
struct BhvEdit { bool active = false; int n = 0; uint8_t* row[4] = {}; int orig[4] = {}; };
BhvEdit g_bhv[2];
bool OtherHandOwnsRow(const BhvEdit& o, uint8_t* row, int* origOut) {
    if (!o.active) return false;
    for (int i = 0; i < o.n; ++i) if (o.row[i] == row) { if (origOut) *origOut = o.orig[i]; return true; }
    return false;
}
void RestoreBhv(int h) {
    BhvEdit& e = g_bhv[h];
    if (!e.active) return;
    for (int i = 0; i < e.n; ++i)
        if (!OtherHandOwnsRow(g_bhv[1 - h], e.row[i], nullptr)) SafeWrite(e.row[i] + Bhv_Stamina, &e.orig[i], 4);
    e = BhvEdit{};
}
void ApplyBhvStamina(int h, const AtkResolve* r, int value) {
    if (g_layoutBad || !r || r->n == 0) return;
    BhvEdit& e = g_bhv[h];
    if (e.active && e.row[0] != r->bhvRow[0]) RestoreBhv(h);
    if (!e.active) {
        e.n = r->n;
        for (int i = 0; i < e.n; ++i) {
            e.row[i] = r->bhvRow[i];
            if (!OtherHandOwnsRow(g_bhv[1 - h], e.row[i], &e.orig[i])) SafeRead(e.row[i] + Bhv_Stamina, &e.orig[i], 4);
        }
        e.active = true;
    }
    for (int i = 0; i < e.n; ++i) {
        int cur = -1; SafeRead(e.row[i] + Bhv_Stamina, &cur, 4);
        if (cur != value) SafeWrite(e.row[i] + Bhv_Stamina, &value, 4);
    }
}

const AtkRowEdit* OtherHandAtkRow(const AtkEdit& o, uint8_t* row) {
    if (!o.active) return nullptr;
    for (int i = 0; i < o.n; ++i) if (o.e[i].row == row) return &o.e[i];
    return nullptr;
}
void RestoreAtk(int h);
void ApplyWepVarRestore(int h);
void RestoreAtk(int h) {
    RestoreBhv(h);
    ApplyWepVarRestore(h);
    AtkEdit& e = g_atk[h];
    if (!e.active) return;
    for (int i = 0; i < e.n; ++i) {
        AtkRowEdit& re = e.e[i];
        // Both hands on the same row: the other edit still owns it, leave its values in place.
        if (OtherHandAtkRow(g_atk[1 - h], re.row)) continue;
        for (int j = 0; j < 6; ++j) SafeWrite(re.row + kAtkCorr[j], &re.orig[j], 2);
        SafeWrite(re.row + Atk_SuperArmorCorr, &re.origSA, 4);
        for (int j = 0; j < 4; ++j) SafeWrite(re.row + Atk_HitRadius + j * 4, &re.origRad[j], 4);
        SafeWrite(re.row + Atk_HitStopTime, &re.origHitStop, 4);
        SafeWrite(re.row + Atk_DmgLevel, &re.origDmg, 1);
    }
    e = AtkEdit{};
}
// mult scales the motion values; suppressReaction (after the first landed hit of a strike) takes
// the hit reaction out: dmg_level 0, no hitstop, no poise damage - contact keeps hurting but a
// swing staggers an enemy at most once, so continuous contact cannot stunlock.
void ApplyAtk(int h, const AtkResolve* r, float mult, bool suppressReaction) {
    if (g_layoutBad || !r || r->n == 0) return;
    AtkEdit& e = g_atk[h];
    if (e.active && e.e[0].row != r->atkRow[0]) RestoreAtk(h);
    if (!e.active) {
        e.n = r->n;
        for (int i = 0; i < e.n; ++i) {
            AtkRowEdit& re = e.e[i]; re.row = r->atkRow[i];
            if (const AtkRowEdit* o = OtherHandAtkRow(g_atk[1 - h], re.row)) { re = *o; }
            else {
                for (int j = 0; j < 6; ++j) SafeRead(re.row + kAtkCorr[j], &re.orig[j], 2);
                SafeRead(re.row + Atk_SuperArmorCorr, &re.origSA, 4);
                for (int j = 0; j < 4; ++j) SafeRead(re.row + Atk_HitRadius + j * 4, &re.origRad[j], 4);
                SafeRead(re.row + Atk_HitStopTime, &re.origHitStop, 4);
                SafeRead(re.row + Atk_DmgLevel, &re.origDmg, 1);
            }
        }
        e.active = true; e.mult = -1.f; e.suppress = false;
    }
    if (std::fabs(mult - e.mult) < 0.002f && suppressReaction == e.suppress) return;
    e.mult = mult; e.suppress = suppressReaction;
    const Config& c = Cfg();
    const float radScale = c.kineticHitRadiusScale > 0.f ? c.kineticHitRadiusScale : 1.f;
    for (int i = 0; i < e.n; ++i) {
        AtkRowEdit& re = e.e[i];
        for (int j = 0; j < 6; ++j) {
            if (j == 5 && !c.kineticStaminaDamageScales) continue;
            float v = (float)re.orig[j] * mult; uint16_t w = (uint16_t)(v < 0 ? 0 : v > 65535.f ? 65535.f : v + 0.5f);
            SafeWrite(re.row + kAtkCorr[j], &w, 2);
        }
        float sa = suppressReaction ? 0.f : (c.kineticPoiseScales ? re.origSA * mult : re.origSA);
        SafeWrite(re.row + Atk_SuperArmorCorr, &sa, 4);
        for (int j = 0; j < 4; ++j) { if (re.origRad[j] > 0.f) { float rr = re.origRad[j] * radScale; SafeWrite(re.row + Atk_HitRadius + j * 4, &rr, 4); } }
        float hs = suppressReaction ? 0.f : re.origHitStop; SafeWrite(re.row + Atk_HitStopTime, &hs, 4);
        uint8_t dl = suppressReaction ? 0 : re.origDmg; SafeWrite(re.row + Atk_DmgLevel, &dl, 1);
    }
}

// Variation alignment (2026-08-31): a unique-moveset weapon's behaviorVariationId (SoNaF = 214)
// has no rows for the basic judges, so what the GAME resolves for our injected AttackBehavior is
// its own affair - possibly nothing (whiffed strikes: 0 hits all session in the 13:50 log) or a
// row we did not edit. For the strike's duration the weapon row's behaviorVariationId is pointed
// at the best candidate variation that actually has the judge, so the game and ERVR provably
// agree on the row. Never done for a skill strike (the skill's judges live under the weapon's
// own variation).
struct WepVarEdit { bool active = false; uint8_t* row = nullptr; int orig = 0; };
WepVarEdit g_wepVar[2];
void ApplyWepVarRestore(int h) {
    WepVarEdit& e = g_wepVar[h];
    if (!e.active) return;
    if (!(g_wepVar[1 - h].active && g_wepVar[1 - h].row == e.row)) SafeWrite(e.row + Wep_VariationId, &e.orig, 4);
    e = WepVarEdit{};
}
void ApplyWepVariation(int h, uint8_t* row, int variation) {
    if (g_layoutBad || !row) return;
    WepVarEdit& e = g_wepVar[h];
    if (e.active && e.row != row) ApplyWepVarRestore(h);
    if (!e.active) {
        const WepVarEdit& o = g_wepVar[1 - h];
        if (o.active && o.row == row) e.orig = o.orig; else SafeRead(row + Wep_VariationId, &e.orig, 4);
        e.active = true; e.row = row;
    }
    int cur = -1; SafeRead(row + Wep_VariationId, &cur, 4);
    if (cur != variation) SafeWrite(row + Wep_VariationId, &variation, 4);
}

// Shield guard edit (EQUIP_PARAM_WEAPON_ST of the left-hand shield).
struct GuardEdit { bool active = false; uint8_t* row = nullptr; float origCut[4]{}; float origDark = 0.f; int16_t origDef = 0; float q = -1.f; } g_guard;
void RestoreGuard() {
    GuardEdit& e = g_guard;
    if (!e.active) return;
    for (int i = 0; i < 4; ++i) SafeWrite(e.row + Wep_GuardCut + i * 4, &e.origCut[i], 4);
    SafeWrite(e.row + Wep_DarkGuardCut, &e.origDark, 4);
    SafeWrite(e.row + Wep_StaminaGuardDef, &e.origDef, 2);
    e.active = false; e.row = nullptr; e.q = -1.f;
}
void ApplyGuard(uint8_t* row, float q) {
    if (g_layoutBad || !row) return;
    GuardEdit& e = g_guard;
    if (e.active && e.row != row) RestoreGuard();
    if (!e.active) {
        for (int i = 0; i < 4; ++i) SafeRead(row + Wep_GuardCut + i * 4, &e.origCut[i], 4);
        SafeRead(row + Wep_DarkGuardCut, &e.origDark, 4); SafeRead(row + Wep_StaminaGuardDef, &e.origDef, 2);
        e.active = true; e.row = row; e.q = -1.f;
    }
    if (std::fabs(q - e.q) < 0.01f) return;
    e.q = q;
    const Config& c = Cfg();
    const float cutMul = Lerp(c.blockMinCut, 1.f, q), defMul = Lerp(c.blockMinStability, 1.f, q);
    for (int i = 0; i < 4; ++i) { float v = e.origCut[i] * cutMul; SafeWrite(row + Wep_GuardCut + i * 4, &v, 4); }
    float d = e.origDark * cutMul; SafeWrite(row + Wep_DarkGuardCut, &d, 4);
    int16_t def = (int16_t)((float)e.origDef * defMul + 0.5f); SafeWrite(row + Wep_StaminaGuardDef, &def, 2);
}
void RestoreAll() { RestoreAtk(HandLeft); RestoreAtk(HandRight); RestoreBhv(HandLeft); RestoreBhv(HandRight); RestoreGuard(); }

float QualityToMult(float q, const Config& c) {
    if (q <= 1.f) return Lerp(c.kineticMinMult, 1.f, q < 0 ? 0 : q);
    float t = (q - 1.f) / 0.5f; if (t > 1.f) t = 1.f;
    return Lerp(1.f, c.kineticMaxMult, t);
}
float Fatigue(int stam, int maxStam, const Config& c) {
    if (maxStam <= 0 || stam < 0 || c.kineticFatigueStart <= 0.f) return 1.f;
    float f = (float)stam / (float)maxStam;
    if (f >= c.kineticFatigueStart) return 1.f;
    return Lerp(c.kineticFatigueFloor, 1.f, f / c.kineticFatigueStart);
}

int g_prevStam = INT_MIN;   // stamina at the end of the previous frame body (hit / block impact detection); INT_MIN = unknown
constexpr int kHitMarkStamina = 8;   // stamina off, one hit per swing: the strike's on-hit behavior cost (refunded) - clear of a frame's regen

// End of the per-frame body. [Cheats] InfiniteStamina refills the bar to max here - AFTER the
// hit / blocked-hit detection above has seen this frame's one-frame drop - so the game charges its
// attack, block and sprint costs against a bar that is full again before its next update: no
// guard break, no exhaustion, no gate, while the drop-based haptics keep working.
void EndFrameStamina(uint8_t* player, int maxStam) {
    if (Cfg().cheatInfiniteStamina && maxStam > 0) {
        int cur = ReadStamina(player);
        if (cur != maxStam) WriteStamina(player, maxStam);
    }
    g_prevStam = ReadStamina(player);
}

// ---------------------------------------------------------------- two-handing (game thread)
// ChrAsm.arm_style is the authority. When the grab disagrees with it the game's own chord is
// requested (modifier + light for the right weapon, modifier + guard for the left; the same
// chord toggles back), at most 3 tries per wish; a wish the game keeps refusing (nothing in
// that hand, a two-hand-only weapon) is logged once and left alone until the wish changes.
void UpdateTwoHand(double now) {
    bool want, owned; int wh;
    { std::lock_guard<std::mutex> g(g_mtx); want = g_two.want; owned = g_two.owned; wh = g_two.weaponHand; }
    g_armStyleShared.store(g_armStyle);
    for (int h = 0; h < 2; ++h) g_wepPresent[h].store(g_wep[h].valid && g_wep[h].row && !g_wep[h].isShield ? 1 : 0);
    static int s_lastStyle = -1, s_desired = -1, s_attempts = 0, s_chordStyle = 0; static double s_lastReq = 0; static bool s_gaveUp = false;
    if (g_armStyle != s_lastStyle) {
        if (s_lastStyle >= 0) {
            LOGI("[combat] arm style %d -> %d (%s)", s_lastStyle, g_armStyle, g_armStyle == 3 ? "right weapon two-handed" : g_armStyle == 2 ? "left weapon two-handed" : g_armStyle == 1 ? "one-handed" : "empty-handed");
            // Felt, not just logged: both hands tick when the grip closes on the weapon, once when it opens.
            g_haptic.fetch_or(TwoHandedStyle(g_armStyle) ? 256u : 512u);
            BodyHapticsPost(TwoHandedStyle(g_armStyle) ? BodyKind::TwoHand : BodyKind::OneHand);
            // Ours if it followed our chord: then letting go takes it back out, even if the grab had
            // already been released before the game got round to switching (2026-08-25 17:51 log).
            if (g_armStyle == s_chordStyle && now - s_lastReq < 2.0) { std::lock_guard<std::mutex> g(g_mtx); g_two.owned = TwoHandedStyle(g_armStyle); owned = g_two.owned; }
        }
        s_lastStyle = g_armStyle;
    }
    if (g_armStyle == 0 || Cfg().twoHandMode != "grab") return;
    // On Torrent the chord's attack button is a mounted attack, and the saddle does not change grips: a grab
    // there sends nothing (the attempts start fresh once you are on foot).
    if (GameERRiding()) { s_desired = -1; return; }
    const int desired = want ? (wh == HandRight ? 3 : 2) : (owned ? 1 : g_armStyle);
    if (desired != s_desired) { s_desired = desired; s_attempts = 0; s_gaveUp = false; }
    if (g_armStyle == desired) {
        if (want && !owned) { std::lock_guard<std::mutex> g(g_mtx); if (g_two.want) g_two.owned = true; }
        if (!want && owned) { std::lock_guard<std::mutex> g(g_mtx); g_two.owned = false; }
        return;
    }
    if (s_attempts >= 3) {
        if (!s_gaveUp) {
            s_gaveUp = true;
            g_haptic.fetch_or(4u | 8u);   // fail buzz in both hands: the game refused the switch
            LOGW("[combat] two-hand: the game stayed at arm style %d after 3 chords (wanted %d): nothing to two-hand in that hand, the chord is not %s + %s/%s, or the game is running too slow for the chord timing (raise TwoHandChordLead/Press at low fps)", g_armStyle, desired, Cfg().twoHandModifierButton.c_str(), Cfg().lightAttackButton.c_str(), Cfg().guardButton.c_str());
        }
        return;
    }
    if (now - s_lastReq < 0.9 || g_chordReq.load() != 0) return;
    const int req = (desired == 2 || (desired == 1 && g_armStyle == 2)) ? 2 : 1;   // only the two-handed hand's chord toggles it
    g_chordReq.store(req); s_lastReq = now; ++s_attempts; s_chordStyle = desired;
    LOGI("[combat] two-hand: %s -> chord %s + %s (arm style %d -> %d, try %d)", want ? "grabbed" : "let go", Cfg().twoHandModifierButton.c_str(), req == 1 ? Cfg().lightAttackButton.c_str() : Cfg().guardButton.c_str(), g_armStyle, desired, s_attempts);
}

// ---------------------------------------------------------------- skill / Ash of War (game thread)
uint8_t* GaitemByHandle(uint8_t* imp, uint32_t handle) {
    if (!imp || !(handle & (1u << 23))) return nullptr;
    const uint32_t index = handle & 0xFFFF; if (index >= Gaitem_Count) return nullptr;
    return Deref(imp + Gaitem_Table + (size_t)index * 8);
}
// The skill on a hand's weapon: the applied Ash of War (the weapon's gaitem -> gem slot -> gem
// item -> EquipParamGem.swordArtsParamId) else the weapon's own swordArtsParamId; SwordArtsParam
// then gives the skill button's FP / stamina cost. Re-resolved when the weapon changes.
void ResolveSkill(uint8_t* player, int h) {
    SkillInfo& si = g_skillInfo[h]; const HandWeapon& w = g_wep[h];
    if (!w.valid || !w.row) { si = SkillInfo{}; return; }
    if (si.weaponId == w.paramId && si.logged) return;
    si = SkillInfo{}; si.weaponId = w.paramId;
    static uint8_t* s_slot = nullptr; static bool s_tried = false;
    if (!s_tried) { s_tried = true; s_slot = FindSingletonSlot("CSGaitem"); }
    uint8_t* asmp = Deref(player + Player_ChrAsm);
    uint8_t* imp = s_slot ? Deref(s_slot) : nullptr;
    if (asmp && imp) {
        uint32_t slot = 0; SafeRead(asmp + (h == HandLeft ? Asm_LeftSlot : Asm_RightSlot), &slot, 4); if (slot > 2) slot = 0;
        uint32_t hdl = 0; SafeRead(asmp + Asm_GaitemHandles + (slot * 2 + (h == HandRight ? 1 : 0)) * 4, &hdl, 4);
        uint8_t* wg = GaitemByHandle(imp, hdl); uint32_t item = 0;
        if (wg && SafeRead(wg + Gaitem_ItemId, &item, 4) && (int)(item & 0x0FFFFFFF) == w.paramId) {   // layout check: the entry names our weapon
            uint32_t gh = 0; SafeRead(wg + WepGaitem_GemHandle, &gh, 4);
            uint8_t* gg = GaitemByHandle(imp, gh); uint32_t gid = 0;
            if (gg && SafeRead(gg + Gaitem_ItemId, &gid, 4) && gid != 0xFFFFFFFF) {
                si.gem = (int)(gid & 0x0FFFFFFF);
                uint32_t sz = 0; uint8_t* row = ParamsRow(PI_EquipParamGem, (uint32_t)si.gem, &sz);
                if (row && (sz == 0 || sz == Gem_RowSize)) SafeRead(row + Gem_SwordArtsId, &si.arts, 4);
                else if (row) LOGW("[combat] EQUIP_PARAM_GEM_ST row size 0x%X != 0x%X: gem ignored", sz, Gem_RowSize);
            }
        } else if (wg) { static bool s_warned = false; if (!s_warned) { s_warned = true; LOGW("[combat] CSGaitem entry item 0x%08X != equipped weapon %d: applied Ash of War not readable (layout?)", item, w.paramId); } }
    }
    if (si.arts < 0) SafeRead(w.row + Wep_SwordArtsId, &si.arts, 4);
    if (si.arts >= 0) {
        uint32_t sz = 0; uint8_t* row = ParamsRow(PI_SwordArtsParam, (uint32_t)si.arts, &sz);
        if (row && (sz == 0 || sz == Arts_RowSize)) { int16_t mp = 0; int8_t sp = 0; SafeRead(row + Arts_UseMpL2, &mp, 2); SafeRead(row + Arts_UsePointL2, &sp, 1); si.fpCost = mp; si.stamCost = sp; }
        else if (row) LOGW("[combat] SWORD_ARTS_PARAM_ST row size 0x%X != 0x%X: skill costs unknown", sz, Arts_RowSize);
    }
    si.logged = true;
    LOGI("[combat] %s hand skill: SwordArtsParam %d%s (FP %d, stamina %d)", HandName(h), si.arts, si.gem >= 0 ? " from the applied Ash of War" : " (the weapon's own)", si.fpCost, si.stamCost);
}

int FindRecording(int weaponId, int arts) {
    for (size_t i = 0; i < g_skillLib.size(); ++i) if (g_skillLib[i].valid && g_skillLib[i].weaponId == weaponId && g_skillLib[i].arts == arts) return (int)i;
    return -1;
}
void FinishRecording() {
    SkillRec& r = g_rec; r.active = false;
    SkillRecording rec = r.rec;
    rec.valid = !rec.ev.empty(); rec.fpObserved = r.fpStart - r.fpMin;
    // Times relative to the FIRST event, not the press: the chord latency and the animation's
    // startup are not part of the effect - a tap replays it at once.
    float t0 = 1e9f; for (const RecEvent& e : rec.ev) if (e.firstT < t0) t0 = e.firstT;
    for (RecEvent& e : rec.ev) { e.firstT -= t0; e.lastT -= t0; if (e.lastT > rec.length) rec.length = e.lastT; }
    if (!rec.valid) { LOGW("[combat] skill: nothing recorded for weapon %d (arts %d) - did the game play the skill? (%s must be the skill button)", rec.weaponId, rec.arts, Cfg().skillButton.c_str()); return; }
    char sum[400]; size_t n = 0; sum[0] = 0;
    for (size_t i = 0; i < rec.ev.size() && n < sizeof(sum) - 48; ++i) {
        const RecEvent& e = rec.ev[i]; int32_t a0 = 0, a1 = 0; memcpy(&a0, e.args, 4); memcpy(&a1, e.args + 4, 4);
        n += (size_t)snprintf(sum + n, sizeof(sum) - n, "%s%u@%.2f", i ? ", " : "", e.id, e.firstT);
        if (e.id == EV_AddSpEffectWeaponArts || e.id == 67 || e.id == 66) n += (size_t)snprintf(sum + n, sizeof(sum) - n, "(spEffect %d/%d)", a0, a1);
        else if (e.id == EV_AttackBehavior) { int32_t j = 0; memcpy(&j, e.args + 8, 4); n += (size_t)snprintf(sum + n, sizeof(sum) - n, "(judge %d)", j); }
        else if (e.id == EV_BulletBehavior) { int32_t j = 0; memcpy(&j, e.args + 8, 4); n += (size_t)snprintf(sum + n, sizeof(sum) - n, "(dmy %d judge %d)", a0, j); }
    }
    const int at = FindRecording(rec.weaponId, rec.arts);
    if (at >= 0) g_skillLib[at] = rec; else g_skillLib.push_back(rec);
    LOGI("[combat] skill recorded for weapon %d arts %d: %zu event instances over %.2f s, FP -%d%s: %s", rec.weaponId, rec.arts, rec.ev.size(), rec.length, rec.fpObserved, rec.hasAttack ? ", has an attack" : "", sum);
    int unlisted = 0; for (const RecEvent& e : rec.ev) if (e.id != EV_AttackBehavior && !ReplayAllowed(e.id)) ++unlisted;
    if (unlisted) LOGI("[combat] skill: %d of those event ids are not in SkillReplayEvents and will not be replayed", unlisted);
}
// Replay = the effect without the animation. Each recorded instance runs at its recorded time
// with is_first_execution on its first frame, then every frame until its recorded end, through
// the slot its id belongs to. The skill's AttackBehavior is not replayed: it arms the next swing.
// Returns false when nothing is recorded for this weapon/arts (the caller then plays it the
// game's way and records); true when handled, including a refusal for lack of FP.
bool StartReplay(uint8_t* player, int h, double now) {
    const SkillInfo& si = g_skillInfo[h];
    const int idx = FindRecording(si.weaponId, si.arts);
    if (idx < 0) return false;
    const Config& c = Cfg();
    const SkillRecording& rec = g_skillLib[idx];
    int maxFp = 0; const int fp = ReadFp(player, &maxFp);
    const int cost = si.fpCost > 0 ? si.fpCost : rec.fpObserved;
    if (c.skillChargeFp && cost > 0 && maxFp > 0 && fp >= 0 && fp < cost) { g_haptic.fetch_or(4u << h); LOGI("[combat] skill: not enough FP (%d < %d)", fp, cost); return true; }
    g_play = SkillPlay{}; g_play.active = true; g_play.start = now; g_play.libIndex = idx; g_play.st.resize(rec.ev.size());
    g_play.fpCost = c.skillChargeFp ? cost : 0; g_play.stamCost = si.stamCost;
    // Replayed bullets bypass the TAE hooks (they go straight to the trampoline), so a recording that shoots
    // arms the aim here: its bullets fly where the two-handed weapon's hand points.
    if (c.castAim) for (const RecEvent& e : rec.ev) if (e.id == EV_BulletBehavior) { ArmCastAim(h, now, "skill replay with bullets"); break; }
    if (rec.hasAttack && c.skillSwingWindow > 0.f) { g_skillSwing.armed = true; g_skillSwing.until = now + c.skillSwingWindow; g_skillSwing.a = rec.attack; }
    LOGI("[combat] skill: replaying %zu events for weapon %d arts %d (FP -%d)%s", rec.ev.size(), rec.weaponId, rec.arts, g_play.fpCost, rec.hasAttack ? " - the next swing is the skill's attack" : "");
    return true;
}
void RunReplay(uint8_t* player, CSChrTaeAnimEvent* ev, double now) {
    SkillPlay& p = g_play; if (!p.active) return;
    if (p.libIndex < 0 || p.libIndex >= (int)g_skillLib.size() || p.st.size() != g_skillLib[p.libIndex].ev.size()) { p.active = false; return; }
    const SkillRecording& rec = g_skillLib[p.libIndex];
    const float t = (float)(now - p.start);
    bool pending = false;
    for (size_t i = 0; i < rec.ev.size(); ++i) {
        const RecEvent& e = rec.ev[i]; SkillPlay::St& s = p.st[i];
        if (s.done) continue;
        if (e.id == EV_AttackBehavior || !ReplayAllowed(e.id) || e.argLen == 0) { s.done = true; continue; }
        const bool behavior = e.id == EV_BulletBehavior || e.id == EV_CommonBehavior;
        if (!behavior && !g_origGeneric) { s.done = true; continue; }
        if (!s.started) {
            if (t < e.firstT) { pending = true; continue; }
            s.started = true;
            memcpy(s.argBuf, e.args, sizeof(s.argBuf));
            memset(&s.args, 0, sizeof(s.args));
            s.args.eventId = e.id; s.args.args = s.argBuf; s.args.from = e.from; s.args.to = e.to; s.args.taeVersion = 0x1a; s.args.animTime = &s.time;
            s.time = e.animTime0; s.args.isFirstExecution = 1;
        } else {
            if (t > e.lastT + 0.001f) { s.done = true; continue; }
            s.time = e.animTime0 + (t - e.firstT); s.args.isFirstExecution = 0;
        }
        pending = true;
        if (behavior) g_origExec(ev, &s.args); else g_origGeneric(ev, &s.args);
        InterlockedIncrement(&g_injected);
    }
    // Costs go on after the first frame's events ran (331 decides "enough FP" on its own).
    if (!p.costCharged) {
        p.costCharged = true;
        if (p.fpCost > 0) { const int fp = ReadFp(player); if (fp >= 0) WriteFp(player, fp - p.fpCost); }
        if (p.stamCost > 0 && Cfg().kineticStaminaMode != "off") { const int s = ReadStamina(player); if (s >= 0) WriteStamina(player, s - p.stamCost); }
    }
    if (!pending) { p.active = false; if (Cfg().combatDebug) LOGD("[combat] skill: replay done after %.2f s", t); }
}
// One frame of the skill system: recording window, trigger requests, replay.
void SkillFrame(uint8_t* player, CSChrTaeAnimEvent* ev, double now) {
    const Config& c = Cfg();
    const bool two = TwoHandedStyle(g_armStyle);
    const int wh = TwoHandedWeaponHand(g_armStyle);
    ResolveSkill(player, wh);
    if (g_rec.active) {
        const int fp = ReadFp(player); if (fp >= 0 && fp < g_rec.fpMin) g_rec.fpMin = fp;
        if (now - g_rec.start >= (c.skillRecordSeconds > 0.5f ? c.skillRecordSeconds : 0.5f)) FinishRecording();
    }
    const int req = g_skillReq.exchange(0);
    if (req && two && c.twoHandSkill != "off") {
        const SkillInfo& si = g_skillInfo[wh];
        if (g_rec.active || g_play.active) { if (c.combatDebug) LOGD("[combat] skill: busy (%s)", g_rec.active ? "recording" : "replaying"); }
        else if (!si.logged) LOGW("[combat] skill: no weapon resolved in the %s hand", HandName(wh));
        else {
            bool gameWay = req == 2 || c.twoHandSkill == "game";
            if (!gameWay && !StartReplay(player, wh, now)) { gameWay = true; LOGI("[combat] skill: nothing recorded yet for weapon %d arts %d: the game plays it this once and ERVR records it", si.weaponId, si.arts); }
            if (gameWay) {
                if (g_chordReq.load() != 0) LOGW("[combat] skill: chord busy, press dropped");
                else {
                    g_chordReq.store(3);
                    const bool record = c.twoHandSkill != "game";
                    if (record) { g_rec = SkillRec{}; g_rec.active = true; g_rec.start = now; g_rec.rec.weaponId = si.weaponId; g_rec.rec.arts = si.arts; const int fp = ReadFp(player); g_rec.fpStart = g_rec.fpMin = fp < 0 ? 0 : fp; }
                    LOGI("[combat] skill: pressing %s (the game plays it)%s", c.skillButton.c_str(), record ? ", recording its events" : "");
                }
            }
        }
    }
    RunReplay(player, ev, now);
    if (g_skillSwing.armed && now > g_skillSwing.until) { g_skillSwing.armed = false; if (c.combatDebug) LOGD("[combat] skill: swing window expired unused"); }
    g_status.skill = g_rec.active ? "recording" : g_play.active ? "replaying" : g_skillSwing.armed ? "window" : "idle";
    g_status.skillsRecorded = (int)g_skillLib.size();
}

// While a menu is open (combatDebug, 1 Hz): snapshot the modules that could make the equipment
// menu refuse "change weapons" - the action-request module's ChrActions qwords (change_weapon_r/l
// are bits 9/10; layout fromsoftware-rs action_request.rs, module container +0x80) and the damage
// module's attack-entry table (+0x98, 16-byte entries {record id, active} - the registrar our
// injected AttackBehavior lands in, confirmed by the event-1 handler disassembly). Comparing a
// refusing menu against a fresh-spawn menu shows which value sticks after injected strikes.
void MenuDiag(uint8_t* player) {
    if (!Cfg().combatDebug) return;
    static double s_last = 0; const double now = NowSec();
    if (now - s_last < 1.0) return; s_last = now;
    uint8_t* mods = Deref(player + Chr_Modules);
    if (!mods) return;
    uint8_t* areq = Deref(mods + 0x80);
    uint8_t* dmg  = Deref(mods + 0x98);
    char line[420]; size_t n = 0; line[0] = 0;
    if (areq) {
        uint64_t q[8] = {}; for (int i = 0; i < 8; ++i) SafeRead(areq + 0x10 + 8 * i, &q[i], 8);
        uint64_t p[4] = {}; for (int i = 0; i < 4; ++i) SafeRead(areq + 0x70 + 8 * i, &p[i], 8);
        n += (size_t)snprintf(line + n, sizeof(line) - n, "areq+10 %llx %llx %llx %llx %llx %llx %llx %llx +70 %llx %llx %llx %llx",
                              (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3],
                              (unsigned long long)q[4], (unsigned long long)q[5], (unsigned long long)q[6], (unsigned long long)q[7],
                              (unsigned long long)p[0], (unsigned long long)p[1], (unsigned long long)p[2], (unsigned long long)p[3]);
    } else n += (size_t)snprintf(line + n, sizeof(line) - n, "areq null");
    if (dmg) {
        uint8_t* b = nullptr; uint8_t* e = nullptr;
        SafeRead(dmg + 0x18, &b, 8); SafeRead(dmg + 0x20, &e, 8);
        long long cnt = b && e && e >= b ? (long long)((e - b) / 16) : -1;
        n += (size_t)snprintf(line + n, sizeof(line) - n, " | dmg entries %lld:", cnt);
        for (long long i = 0; i < cnt && i < 8 && n < sizeof(line) - 24; ++i) {
            int32_t id = 0; uint8_t act = 0;
            SafeRead(b + i * 16, &id, 4); SafeRead(b + i * 16 + 4, &act, 1);
            n += (size_t)snprintf(line + n, sizeof(line) - n, " %d/%u", id, act);
        }
    } else n += (size_t)snprintf(line + n, sizeof(line) - n, " | dmg null");
    LOGD("[combat] menudiag %s", line);
}

// ---------------------------------------------------------------- enemy watcher (game thread)
// Ground truth for "did that swing actually hit": the game's own by-distance ChrIns list gives
// every character near the player; a frame-over-frame HP drop on any of them IS a landed hit
// (and its size IS the damage dealt). The stamina-drop proxy this replaces misread shield
// blocks as strike hits (13:51:38 log: the only "HIT" of the session was a 47-stamina guard
// impact) and went blind when the behavior stamina charge was 1 (masked by regen).
// With other players in the world (co-op, summons) that is only true for the drops the player
// caused: each character records who hit it last, so a drop credited to anyone else - a partner's
// strike on the same enemy, a partner taking a hit - is counted apart and never feeds the swing.
struct EnemyFrame {
    bool  available = false;   // the list was readable this frame
    int   dmgEvents = 0;       // enemies whose HP dropped this frame
    int   dmgTotal = 0;        // sum of those drops
    int   unattributed = 0;    // ...of which no attacker was recorded (still counted as ours)
    int   ownEvents = 0;       // ...of which the attacker recorded is the player
    int   foreignEvents = 0;   // HP drops someone else caused (not counted above)
    int   foreignDmg = 0;
    int   riseEvents = 0;      // HP went UP: regen, or the owner's game overriding a drop we applied locally
    int   riseTotal = 0;
    float nearest = 1e9f;      // distance of the nearest live non-player character
    int   nearestNpc = 0;
    float blade = 1e9f;        // closest a striking blade came to a live character's body line (measured on horseback)
};
struct EnemyPrev { int hp; uint32_t seenFrame; };
std::unordered_map<uint8_t*, EnemyPrev> g_enemyPrev;
uint32_t g_enemyFrame = 0;

// 5 s combat summary at INFO, printed only when something happened: strikes, how they ended, landed
// hit-frames and damage, and whose HP drops the watcher saw. Game thread only.
struct CombatTally {
    int strikes[2] = {}, landed[2] = {}, spent[2] = {};
    int hitFrames = 0, dmg = 0, sliceN = 0; float sliceSum = 0.f;
    int unattributed = 0, foreign = 0, foreignDmg = 0, outside = 0;
    int stamMarks = 0, stamOnly = 0, rises = 0, riseTotal = 0;
    int rideStrikes = 0; float rideBlade = 1e9f, rideNearest = 1e9f;   // on Torrent: reach of the strikes
} g_tally;
double g_tallyAt = 0;

// In a co-op session, the first few HP drops of each kind with the raw handles: whether the game
// records the attacker on characters another player's game owns is what decides the attribution.
void SampleAttribution(int kind, int npc, int delta, uint64_t by, uint64_t self, bool striking) {
    static int s_n[3] = {0, 0, 0};
    static bool s_wasSession = false;
    const bool session = GameERCoopInSession();
    if (session && !s_wasSession) s_n[0] = s_n[1] = s_n[2] = 0;
    s_wasSession = session;
    if (!session || s_n[kind]++ >= 3) return;
    static const char* const kKind[3] = {"ours", "no attacker recorded (counted as ours)", "someone else's"};
    LOGI("[combat] co-op HP drop: npc %d -%d, last_hit_by %08x:%08x, self %08x:%08x - %s%s", npc, delta,
         (uint32_t)by, (uint32_t)(by >> 32), (uint32_t)self, (uint32_t)(self >> 32), kKind[kind], striking ? "" : " (no strike active)");
}
void FlushTally(double now) {
    if (now - g_tallyAt < 5.0) return;
    g_tallyAt = now;
    CombatTally& t = g_tally;
    const int spentEnds = g_spentValleyEnds.exchange(0), spentTimeouts = g_spentTimeoutEnds.exchange(0);
    // On Torrent: did the strikes reach anyone? "never within a metre" (out of reach from the saddle) and "touched
    // an enemy, no HP moved" (the game ignored the strike) are different problems.
    char ride[160] = "";
    if (t.rideStrikes > 0) {
        if (t.rideNearest > 1e8f) snprintf(ride, sizeof(ride), " | on Torrent: %d strikes, no enemy within the watch radius", t.rideStrikes);
        else if (t.rideBlade > 1e8f) snprintf(ride, sizeof(ride), " | on Torrent: %d strikes, nearest enemy %.1f m, blade not measured", t.rideStrikes, t.rideNearest);
        else snprintf(ride, sizeof(ride), " | on Torrent: %d strikes, the blade came within %.2f m of an enemy's body (nearest enemy %.1f m)", t.rideStrikes, t.rideBlade, t.rideNearest);
    }
    if (t.strikes[0] + t.strikes[1] + t.hitFrames + t.foreign + t.outside + t.rises + spentEnds + spentTimeouts > 0)
        LOGI("[combat] 5 s: strikes right %d (%d landed, %d budget-spent) left %d (%d landed, %d budget-spent); spent strikes re-armed at a speed valley %d, by timeout %d | "
             "own hit-frames %d, %d damage, mean hit %.2f of a vanilla hit, %d on-hit stamina marks (%d with no HP drop seen) | HP drops: %d without a recorded attacker (counted as ours), "
             "%d not ours (%d damage), %d of ours outside a strike | HP rose %d times (+%d)%s%s",
             t.strikes[HandRight], t.landed[HandRight], t.spent[HandRight], t.strikes[HandLeft], t.landed[HandLeft], t.spent[HandLeft], spentEnds, spentTimeouts,
             t.hitFrames, t.dmg, t.sliceN ? t.sliceSum / (float)t.sliceN : 0.f, t.stamMarks, t.stamOnly, t.unattributed, t.foreign, t.foreignDmg, t.outside,
             t.rises, t.riseTotal, ride, GameERCoopInSession() ? " | co-op session" : "");
    t = CombatTally{};
}

int ChrHp(uint8_t* chr, int* maxOut) {
    uint8_t* mods = Deref(chr + Chr_Modules); if (!mods) return -1;
    uint8_t* data = Deref(mods + Mod_Data); if (!data) return -1;
    int hp = -1; SafeRead(data + Data_Hp, &hp, 4);
    if (maxOut) { *maxOut = -1; SafeRead(data + Data_MaxHp, maxOut, 4); }
    return hp;
}

// `bladeHands`: hands with a live strike whose blade is measured against every character in reach (on Torrent).
EnemyFrame WatchEnemies(uint8_t* player, bool anyStrikeActive, const Config& c, int bladeHands = 0) {
    EnemyFrame ef;
    if (!c.combatEnemyWatch) return ef;
    Vec3 grip[2], tip[2]; bool blade[2] = {false, false};
    for (int h = 0; h < 2; ++h) blade[h] = (bladeHands & (1 << h)) && GameERBladeWorld(h, &grip[h], &tip[h]);
    uint8_t* wcm = GameERWorldChrMan();
    if (!wcm) return ef;
    uint8_t *first = nullptr, *last = nullptr;
    if (!SafeRead(wcm + GameWorldChrManOffset(WCM_ChrByDistFirst), &first, 8) ||
        !SafeRead(wcm + GameWorldChrManOffset(WCM_ChrByDistLast), &last, 8)) return ef;
    if (!first || !last || last < first) return ef;
    size_t count = (size_t)(last - first) / ChrDist_Stride;
    if (count > 512) return ef;   // torn read / not a vector
    ef.available = true;
    ++g_enemyFrame;
    const float radius = c.combatEnemyWatchRadius > 0.f ? c.combatEnemyWatchRadius : 8.f;
    uint64_t self = 0;
    const bool attribute = c.combatEnemyWatchOwnHits && SafeRead(player + Chr_Handle, &self, 8) && (uint32_t)self != UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        uint8_t* chr = nullptr; float dist = 0.f;
        if (!SafeRead(first + i * ChrDist_Stride, &chr, 8) || !chr || chr == player) continue;
        if (!SafeRead(first + i * ChrDist_Stride + 8, &dist, 4)) continue;
        if (dist > radius) break;   // the list is sorted by distance
        int maxHp = -1; int hp = ChrHp(chr, &maxHp);
        if (hp < 0 || maxHp <= 0) continue;
        int npc = 0; SafeRead(chr + Chr_NpcId, &npc, 4);
        // Torrent is never the enemy: from the saddle it was the "nearest enemy at 1.6 m" (2026-11-02), and a hit
        // an enemy lands on the horse is not a hit of ours.
        if (IsTorrentNpc(npc)) continue;
        if (hp > 0 && dist < ef.nearest) { ef.nearest = dist; ef.nearestNpc = npc; }
        Vec3 at;
        if (hp > 0 && (blade[0] || blade[1]) && GameERChrPosition(chr, &at)) {
            const Vec3 low = at + Vec3{0.f, 0.2f, 0.f}, high = at + Vec3{0.f, 1.7f, 0.f};   // a body line, feet to head
            for (int h = 0; h < 2; ++h)
                if (blade[h]) ef.blade = std::min(ef.blade, SegmentDistance(grip[h], tip[h], low, high));
        }
        auto it = g_enemyPrev.find(chr);
        if (it != g_enemyPrev.end()) {
            const int delta = it->second.hp - hp;
            if (delta > 0) {
                // An unreadable or empty attacker keeps the old meaning (a drop is a hit).
                uint64_t by = 0;
                const bool none = attribute && (!SafeRead(chr + Chr_LastHitBy, &by, 8) || (uint32_t)by == UINT32_MAX);
                const bool ours = !attribute || none || by == self;
                if (ours) { ++ef.dmgEvents; ef.dmgTotal += delta; if (none) ++ef.unattributed; else ++ef.ownEvents; }
                else { ++ef.foreignEvents; ef.foreignDmg += delta; }
                if (attribute) SampleAttribution(none ? 1 : ours ? 0 : 2, npc, delta, by, self, anyStrikeActive);
                if (c.combatDebug) LOGD("[combat] enemy npc %d (%.1f m): hp %d -> %d/%d (-%d)%s%s%s", npc, dist, it->second.hp, hp, maxHp, delta, anyStrikeActive ? "" : " (no strike active)", hp <= 0 ? " KILLED" : "", ours ? "" : " - hit by someone else, not counted");
            } else if (delta < 0 && it->second.hp > 0) { ++ef.riseEvents; ef.riseTotal -= delta; }
            it->second.hp = hp; it->second.seenFrame = g_enemyFrame;
        } else g_enemyPrev[chr] = {hp, g_enemyFrame};
    }
    // Prune characters not seen for ~4 s of frames (despawn frees the ChrIns; a later spawn could
    // reuse the address, so stale previous-HP entries must not linger).
    if ((g_enemyFrame & 0xFF) == 0)
        for (auto it = g_enemyPrev.begin(); it != g_enemyPrev.end();)
            it = g_enemyFrame - it->second.seenFrame > 256 ? g_enemyPrev.erase(it) : std::next(it);
    return ef;
}

void GameFrameBody(uint8_t* player) {
    g_player = player;
    const Config& c = Cfg();
    SwingCmd cmd[2]; bool physical; BlockState blk;
    { std::lock_guard<std::mutex> g(g_mtx); ApplyConfigTemplates(); physical = PhysicalActiveLocked(); cmd[0] = g_cmd[0]; cmd[1] = g_cmd[1]; blk = g_block; }
    g_status.physical = physical;
    int maxStam = -1;
    int stam = ReadStamina(player, &maxStam); g_status.stamina = stam;
    const int stamAtEntry = stam;
    // Every mode, before the physical gate: cast aim needs the weapon classes (the passive aim hand) in
    // buttons mode too, and its state is reported here.
    ReadEquipment(player);
    {
        bool casting = false; const int ah = CombatCastAimHand(&casting);
        static int s_prevHand = -2; static bool s_prevCasting = false;
        if ((ah != s_prevHand || casting != s_prevCasting) && c.combatDebug)
            LOGD("[combat] cast aim: %s aims%s", ah < 0 ? "the head" : ah == HandRight ? "the right hand" : "the left hand", casting ? " (cast live: the body turns to the ray)" : "");
        s_prevHand = ah; s_prevCasting = casting;
        g_status.castAimHand = ah; g_status.casting = casting;
    }
    if (!physical) { RestoreAll(); g_leftShield.store(0); g_wepWeight[0].store(0.f); g_wepWeight[1].store(0.f); EndFrameStamina(player, maxStam); return; }
    // Any open menu: stand down on the game thread too - restore every live param edit, inject
    // nothing, request no chords. The worker side is already idle (CombatHandlesSwings is false).
    if (c.combatMenuGate && g_menuOpen.load()) {
        RestoreAll(); g_leftShield.store(0);
        MenuDiag(player);
        EndFrameStamina(player, maxStam);
        return;
    }
    g_status.paramsReady = ParamsReady();
    const bool kinetic = c.kineticEnabled && g_status.paramsReady && !g_layoutBad;
    const bool leftShield = c.blockEnabled && g_wep[HandLeft].valid && g_wep[HandLeft].isShield && g_armStyle != 3;
    g_leftShield.store(leftShield ? 1 : 0);
    g_status.shield = leftShield;

    CSChrTaeAnimEvent* ev = PlayerTaeEvent(player);
    if (!ev) { static double lastWarn = 0; double n = NowSec(); if (n - lastWarn > 10) { lastWarn = n; LOGW("[combat] player CSChrTaeAnimEvent not found (vtable mismatch?)"); } RestoreAll(); EndFrameStamina(player, maxStam); return; }

    bool anyStrike = false;
    // Per-strike accounting: frames, landed hit-frames, damage dealt (enemy watcher), and what
    // the GAME took in stamina over the whole strike beyond our own drain.
    static int s_strikeStam[2] = {-1, -1}; static uint32_t s_strikeId[2] = {0, 0}; static int s_strikeFrames[2] = {0, 0};
    static int s_strikeHits[2] = {0, 0}; static double s_hitPauseUntil[2] = {0, 0};
    static int s_strikeDmg[2] = {0, 0};
    // Damage budget (continuous model): mult-slices spent on frames that actually damaged an
    // enemy; the strike retires when the budget is gone, so one swing's total damage is bounded
    // no matter how long the blade rests in the target or how high the frame rate.
    static float s_spent[2] = {0, 0}; static float s_lastMult[2] = {0, 0}; static float s_qMax[2] = {0, 0};
    const double nowS = NowSec();
    UpdateTwoHand(nowS);
    SkillFrame(player, ev, nowS);
    g_status.fp = ReadFp(player);
    // Stamina modes: "off" takes the bar out of the loop entirely (no gate, no drain, no fatigue,
    // the attack's behavior cost forced to 1 and that 1 given back), "game" leaves the game's
    // charges alone, "full" adds ERVR's drain + fatigue on top.
    const bool stamOff = c.kineticStaminaMode == "off", stamFull = c.kineticStaminaMode == "full";
    // Stamina goes NEGATIVE in ER, so validity is "the max is known", never the sign of the value.
    const bool stamKnown = maxStam > 0 && g_prevStam != INT_MIN;
    // HIT SIGNAL. Primary: a watched enemy's HP dropped this frame (ground truth - see
    // WatchEnemies). Fallback when the list is unreadable: the one-frame stamina drop the game
    // charges per hit - but never while the shield is up and the drop is guard-impact sized;
    // 13:51:38 log: the session's only stamina-"HIT" was a 47-point blocked enemy attack.
    const bool riding = GameERRiding();
    const int bladeHands = riding ? (cmd[0].active ? 1 : 0) | (cmd[1].active ? 2 : 0) : 0;
    const EnemyFrame ef = WatchEnemies(player, cmd[0].active || cmd[1].active, c, bladeHands);
    if (bladeHands) {
        g_tally.rideBlade = std::min(g_tally.rideBlade, ef.blade);
        if (ef.available) g_tally.rideNearest = std::min(g_tally.rideNearest, ef.nearest);
    }
    // One hit per swing (continuous contacts, KineticSlices=false): the swing's damage lands as ONE
    // quality-scaled hit and the strike is spent. ER takes flat defense off every hit, so the per-frame
    // slices (6-10% of a hit) dealt 3-5 HP each where a full hit deals hundreds (2026-10-31 co-op log).
    const bool oneHit = c.kineticContinuous && !c.kineticSlices;
    // The game charges the strike's behavior stamina when OUR attack lands - an attacker-side mark no one
    // else's hit can make. Stamina off forces that cost to kHitMarkStamina for the strike (refunded), so
    // the mark stands clear of a frame's regen; at 1 it was lost in it.
    const int stamDrop = stamKnown ? g_prevStam - stamAtEntry : 0;
    bool stamHit = stamKnown && stamDrop >= (stamOff ? (oneHit ? kHitMarkStamina / 2 : 1) : c.kineticHitStamina);
    if (blk.raised && stamKnown && stamDrop >= c.blockImpactStamina) stamHit = false;
    const bool striking = cmd[0].active || cmd[1].active;
    // On an enemy another player's game owns, the game records no attacker at all (2026-10-31: every
    // co-op drop read last_hit_by ffffffff, ours included), so a partner's hit there reads as ours and
    // would end our swing. Once the stamina mark has been seen landing with such a drop three times this
    // session, only the mark (or a drop the game does attribute to us) counts - plus drops on two strike
    // frames in a row, which is our own blade inside an enemy (a partner's hit is one event): a mark that
    // stopped working can cost at most a second hit, never a hit every frame.
    static bool s_markTrusted = false, s_wasSession = false;
    static int s_markEvidence = 0, s_dropRun = 0;
    const bool session = GameERCoopInSession();
    if (session != s_wasSession) { s_markTrusted = false; s_markEvidence = 0; s_wasSession = session; }
    if (oneHit && session && !s_markTrusted && striking && stamHit && ef.dmgEvents > 0 && ++s_markEvidence >= 3) {
        s_markTrusted = true;
        LOGI("[combat] co-op: your hits carry the game's own on-hit stamina mark - HP drops with no attacker recorded (a partner's hits) no longer end your swing");
    }
    s_dropRun = striking && ef.dmgEvents > 0 ? s_dropRun + 1 : 0;
    bool hitNow;
    if (!ef.available) hitNow = stamHit;
    else if (s_markTrusted) hitNow = stamHit || ef.ownEvents > 0 || s_dropRun >= 2;
    else hitNow = ef.dmgEvents > 0 || (oneHit && stamHit);
    if (striking && stamHit) { ++g_tally.stamMarks; if (ef.dmgEvents == 0) ++g_tally.stamOnly; }
    if (ef.available) {
        if (striking) { if (ef.dmgEvents > 0) { ++g_tally.hitFrames; g_tally.dmg += ef.dmgTotal; } }
        else g_tally.outside += ef.dmgEvents;
        g_tally.unattributed += ef.unattributed; g_tally.foreign += ef.foreignEvents; g_tally.foreignDmg += ef.foreignDmg;
        g_tally.rises += ef.riseEvents; g_tally.riseTotal += ef.riseTotal;
    }
    static double s_lastHitHaptic[2] = {0, 0};
    for (int h = HandRight; h >= HandLeft; --h) {   // right first (main hand)
        SwingCmd& k = cmd[h];
        if (!k.active) {
            if (s_strikeStam[h] >= 0 && s_strikeHits[h] > 0) ++g_tally.landed[h];
            if (s_strikeStam[h] >= 0 && c.combatDebug) {
                const int now = ReadStamina(player);
                LOGD("[combat] strike %s #%u ended after %d frames, %d hit-frames, %d damage dealt, budget spent %.2f: game charged %d stamina (stamina %d -> %d)", HandName(h), s_strikeId[h], s_strikeFrames[h], s_strikeHits[h], s_strikeDmg[h], s_spent[h], s_strikeStam[h] - now, s_strikeStam[h], now);
            }
            s_strikeStam[h] = -1;
            RestoreAtk(h); continue;
        }
        if (k.first) { s_strikeId[h] = k.id; s_strikeFrames[h] = 0; s_strikeHits[h] = 0; s_hitPauseUntil[h] = 0; s_strikeDmg[h] = 0; s_spent[h] = 0.f; s_lastMult[h] = 0.f; s_qMax[h] = 0.f; }
        ++s_strikeFrames[h];
        if (!k.first && hitNow && nowS >= s_hitPauseUntil[h]) {
            ++s_strikeHits[h];
            s_strikeDmg[h] += ef.available ? ef.dmgTotal : 0;
            // The damage seen this frame came from the sweep of the LAST frame's exec (and its
            // mult); one budget slice per damaged enemy.
            s_spent[h] += s_lastMult[h] * (float)(ef.dmgEvents > 1 ? ef.dmgEvents : 1);
            g_tally.sliceSum += s_lastMult[h]; ++g_tally.sliceN;
            if (nowS - s_lastHitHaptic[h] > 0.1) {
                s_lastHitHaptic[h] = nowS; g_haptic.fetch_or(64u << h);
                BodyHapticsPost(BodyKind::Strike, TwoHandedStyle(g_armStyle) ? -1 : h, Clamp(k.qualityCommit, 0.f, 1.f));
            }
            // The game charged its behavior cost: give it back (only an actual drop - with the
            // watcher signalling the hit there may be nothing to refund).
            if (stamOff && stamAtEntry < g_prevStam) { WriteStamina(player, g_prevStam); stam = g_prevStam; }
            if (c.combatDebug && (!c.kineticContinuous || s_strikeHits[h] == 1)) LOGD("[combat] %s strike HIT #%d (%d damage, stamina %d -> %d)%s", HandName(h), s_strikeHits[h], ef.dmgTotal, g_prevStam, stamAtEntry, stamOff ? " refunded" : "");
            if (oneHit) {   // the swing landed its one hit: spent (the worker re-arms on the next swing)
                std::lock_guard<std::mutex> g(g_mtx);
                if (g_cmd[h].id == k.id) g_cmd[h].active = false;
                continue;
            }
            if (!c.kineticContinuous) {
                if (c.kineticHitPause <= 0.f) {
                    std::lock_guard<std::mutex> g(g_mtx);
                    if (g_cmd[h].id == k.id) g_cmd[h].active = false;
                    continue;   // instance retired; next frame restores the row and logs the strike
                }
                s_hitPauseUntil[h] = nowS + c.kineticHitPause;
            }
        }
        if (!c.kineticContinuous && !k.first && nowS < s_hitPauseUntil[h]) { anyStrike = true; continue; }   // paused: instance kept, no sweep
        if (h == HandLeft && leftShield) { RestoreAtk(h); continue; }   // the shield blocks, it does not strike
        // Strike gate (Kinetic): the game's collision hits whatever hurtbox the blade sweeps, so a blade laid on an
        // enemy's sword or arm would count as a hit. While the simulated blade is at an enemy without having really
        // struck (cut, stuck, pierced, a hard arm hit), this frame sweeps nothing.
        if (kinetic::SwordRigHoldStrike(h)) { RestoreAtk(h); anyStrike = true; continue; }
        Template t;
        {
            std::lock_guard<std::mutex> g(g_mtx);
            int slot = k.slot;
            if (TwoHandedStyle(g_armStyle) && (slot == SlotLight || slot == SlotHeavy)) { const int s2 = slot == SlotLight ? SlotLight2H : SlotHeavy2H; if (g_tpl[s2].valid) slot = s2; }
            t = g_tpl[slot];
            if (!t.valid && (slot == SlotHeavy || slot == SlotHeavy2H)) t = g_tpl[SlotLight];
            if (!t.valid && slot == SlotLeft) { t = g_tpl[SlotLight]; t.a.source = 2; }
        }
        // Skill window: the two-handed weapon's next swing is the skill's own attack row.
        bool skillStrike = false;
        if (g_skillSwing.armed && TwoHandedStyle(g_armStyle) && h == TwoHandedWeaponHand(g_armStyle)) { t.valid = true; t.a = g_skillSwing.a; t.a.attackIndex = 0; skillStrike = true; }
        if (!t.valid) continue;
        if (k.first) {
            // maxStam > 0 means the read is real: ER lets stamina go NEGATIVE after a hit is charged
            // at low stamina, and the old `stam >= 0` (meant for the -1 "unknown" value) waved every
            // strike through while exhausted - free attacks at -50 stamina.
            if (c.combatRequireStamina && !stamOff && (maxStam > 0 || stam >= 0) && stam < c.combatMinStamina) {
                std::lock_guard<std::mutex> g(g_mtx);
                if (g_cmd[h].id == k.id) { g_cmd[h].active = false; g_cmd[h].staminaFail = true; }
                g_haptic.fetch_or(4u << h);
                BodyHapticsPost(BodyKind::Winded, h);
                if (c.combatDebug) LOGD("[combat] %s swing rejected: stamina %d", HandName(h), stam);
                continue;
            }
            { std::lock_guard<std::mutex> g(g_mtx); if (g_cmd[h].id == k.id) g_cmd[h].first = false; }
            InterlockedIncrement(&g_swings);
            ++g_tally.strikes[h];
            if (riding) ++g_tally.rideStrikes;
            if (skillStrike) { g_skillSwing.armed = false; LOGI("[combat] skill strike: judge %d type %d (the skill's own attack row)", t.a.judgeId, t.a.attackType); }
        }
        anyStrike = true;
        // Stable per-hand storage: the game keys the attack instance on the args pointer across
        // frames - stable WITHIN a strike, but a FRESH address for every new strike (round-robin
        // pool). One address reused forever gave every strike of the session the same identity in
        // the game's attack bookkeeping; suspected in the lingering combat state that made the
        // equipment menu say "cannot change weapons now" minutes after the last swing (2026-08-26).
        static AttackBehaviorArgs s_ab[2][8]; static CSTaeAnimEventArgs s_args[2][8];
        static unsigned s_abSlot[2]; static float s_time[2]; static bool s_logNext[2];
        static int s_stamAfterDrain[2];
        if (k.first) s_abSlot[h] = (s_abSlot[h] + 1) & 7;
        AttackBehaviorArgs& ab = s_ab[h][s_abSlot[h]]; CSTaeAnimEventArgs& args = s_args[h][s_abSlot[h]];
        // Kinetic: scale the strike's attack rows by quality x fatigue, refreshed every frame so
        // the value the sweep reads at contact reflects the tip speed at contact.
        float mult = 1.f; bool scaled = false;
        if (kinetic) {
            const HandWeapon& w = g_wep[h];
            AtkResolve* r = w.valid && w.row ? ResolveAttack(h, w.variation, t.a.judgeId) : nullptr;
            const float fat = stamOff ? 1.f : Fatigue(stam, maxStam, c);
            if (k.quality > s_qMax[h]) s_qMax[h] = k.quality;
            if (k.qualityCommit > s_qMax[h]) s_qMax[h] = k.qualityCommit;
            if (c.kineticContinuous && c.kineticSlices) {
                // Every frame of contact is a hit, so each one is a slice of a vanilla hit sized by the
                // tip's speed right now: nothing at rest, KineticContinuousMult at KineticRefSpeed.
                const float ref = c.kineticRefSpeed > 0.5f ? c.kineticRefSpeed : 8.f;
                float qv = (k.tipLive / ref) * (k.tipLive / ref); if (qv > 1.f) qv = 1.f; if (qv < 0.f) qv = 0.f;
                mult = c.kineticContinuousMult * qv * fat * c.kineticDamageScale;
                // Per-swing budget: the whole strike may deal at most QualityToMult(best quality)
                // worth of vanilla-hit slices (x KineticSwingBudget); once spent, the strike
                // retires - the touch dealt its swing's worth and the blade goes inert.
                if (c.kineticSwingBudget > 0.f) {
                    const float budget = c.kineticSwingBudget * QualityToMult(s_qMax[h], c) * c.kineticDamageScale;
                    const float remain = budget - s_spent[h];
                    if (remain <= 0.f) {
                        if (c.combatDebug) LOGD("[combat] strike %s #%u budget spent (%.2f of %.2f): retiring", HandName(h), k.id, s_spent[h], budget);
                        ++g_tally.spent[h];
                        std::lock_guard<std::mutex> g(g_mtx);
                        if (g_cmd[h].id == k.id) g_cmd[h].active = false;
                        continue;   // next frame restores the rows and logs the strike
                    }
                    if (mult > remain) mult = remain;
                }
            } else mult = QualityToMult(k.quality, c) * fat * c.kineticDamageScale;
            mult *= kinetic::SwordRigStrikeScale(h);   // a glance lands weakly (GlanceDamage)
            if (r) {
                // After the first landed hit-frame the strike keeps its damage but loses its hit
                // reaction (dmg_level/hitstop/poise -> 0): one flinch per swing, no stunlock.
                const bool suppress = c.kineticFlinchFirstHitOnly && s_strikeHits[h] > 0;
                ApplyAtk(h, r, mult, suppress);
                if (stamOff) ApplyBhvStamina(h, r, oneHit ? kHitMarkStamina : 1);
                // A weapon whose own variation lacks this judge: point its behaviorVariationId at
                // the candidate that has it for the strike's duration, so the game's own resolve
                // lands on the rows edited above instead of whatever its fallback finds (or fails
                // to find - the 13:50 session's whiffed strikes).
                if (c.kineticAlignVariation && !skillStrike && r->resolvedVar != w.variation) ApplyWepVariation(h, w.row, r->resolvedVar);
                scaled = true;
            }
            s_lastMult[h] = scaled ? mult : 1.f;
            g_status.lastQuality = k.quality; g_status.lastMult = scaled ? mult : 1.f;
        } else s_lastMult[h] = 1.f;
        if (k.first) {
            ab = t.a;
            ab.source = h == HandLeft ? 2 : 1;                       // always the swinging hand's weapon
            if (c.combatAttackIndexRotate > 0) ab.attackIndex = (int32_t)(k.id % (uint32_t)c.combatAttackIndexRotate);
            memset(&args, 0, sizeof(args));
            args.eventId = EV_AttackBehavior; args.args = &ab; args.from = 0.f; args.to = c.swingMaxSeconds > 0 ? c.swingMaxSeconds : 1.f;
            args.taeVersion = 0x1a; s_time[h] = 0.f; args.animTime = &s_time[h]; s_logNext[h] = c.combatDebug;
            int drain = 0;
            if (c.combatExtraStamina[k.slot] > 0) drain += c.combatExtraStamina[k.slot];
            if (kinetic && stamFull) {
                // The game's own cost for this very attack (BehaviorParam stamina) when the row
                // resolved, else the flat fallback scaled by weight. Quality scales it: a lazy
                // strike costs half, a full one all of it.
                float qc = k.qualityCommit; if (qc > 1.5f) qc = 1.5f;
                const AtkResolve& rr = g_resolve[h];
                float base = rr.valid && rr.bhvStamina > 0 ? (float)rr.bhvStamina * c.kineticStaminaScale
                                                           : c.kineticStaminaPerStrike * (1.f + (g_wep[h].valid ? g_wep[h].weight : 0.f) / 12.f);
                drain += (int)(base * (0.5f + 0.5f * qc) + 0.5f);
            }
            if (drain > 0 && stam > 0) { stam -= drain; if (stam < 0) stam = 0; WriteStamina(player, stam); }
            s_stamAfterDrain[h] = stam; s_strikeStam[h] = stam;
            if (c.combatDebug) {
                char nearBuf[64];
                if (!ef.available) snprintf(nearBuf, sizeof(nearBuf), "watcher off");
                else if (ef.nearest > 1e8f) snprintf(nearBuf, sizeof(nearBuf), "no enemy in %.0f m", c.combatEnemyWatchRadius);
                else snprintf(nearBuf, sizeof(nearBuf), "nearest npc %d at %.1f m", ef.nearestNpc, ef.nearest);
                if (scaled) LOGD("[combat] strike %s %s: quality %.2f (tip %.1f m/s, arc %.2f m) mult %.2f drain %d stamina %d/%d | %s", HandName(h), SlotName(k.slot), k.qualityCommit, k.tipSpeed, k.arc, mult, drain, stam, maxStam, nearBuf);
                else LOGD("[combat] strike %s %s: quality %.2f (tip %.1f m/s, arc %.2f m) VANILLA DAMAGE (no attack row to scale) drain %d stamina %d/%d | %s", HandName(h), SlotName(k.slot), k.qualityCommit, k.tipSpeed, k.arc, drain, stam, maxStam, nearBuf);
            }
        } else s_time[h] = (float)(NowSec() - k.start);
        // Continuous contacts follow motion until it stops (or the hit budget
        // retires them). Keep the native event window ahead of that motion too;
        // the gated model's timeout must not make a long windup go inert mid-cut.
        if (c.kineticEnabled && c.kineticContinuous)
            args.to = s_time[h] + (c.swingMaxSeconds > 0.f ? c.swingMaxSeconds : 0.6f);
        args.isFirstExecution = k.first ? 1 : 0;
        g_origExec(ev, &args);
        {
            std::lock_guard<std::mutex> g(g_mtx);
            if (g_cmd[h].id == k.id) g_cmd[h].FinishFrame();
        }
        InterlockedIncrement(&g_injected);
        if (c.combatDebug && k.first) LOGD("[combat] injected %s attack (judgeId %d index %d source %u) stamina %d", SlotName(k.slot), ab.judgeId, ab.attackIndex, ab.source, stam);
        // Whether the GAME charged the behavior's stamina on top of ours: the delta a frame later
        // beyond our own drain (regen is < 1/frame; a blocked hit would also show here).
        if (!k.first && s_logNext[h]) { s_logNext[h] = false; int ns = ReadStamina(player); LOGD("[combat] ... next frame: stamina %d (game charged %d)", ns, s_stamAfterDrain[h] - ns); }
    }

    // ---- shield: guard quality -> shield params; blocked-hit / deflect detection -------------
    g_status.guarding = false; g_status.blockQuality = 0.f;
    if (leftShield && blk.raised) {
        g_status.guarding = true; g_status.blockQuality = blk.quality;
        if (kinetic) ApplyGuard(g_wep[HandLeft].row, blk.quality);
        // A stamina drop in one frame while guarding and not striking is a blocked hit (sprinting
        // drains far less per frame). If the shield was moving into the attack, it is a deflect.
        if (!anyStrike && maxStam > 0 && g_prevStam != INT_MIN && g_prevStam - stamAtEntry >= c.blockImpactStamina) {
            const int drop = g_prevStam - stamAtEntry;
            const bool deflect = blk.into >= c.deflectSpeed && (NowSec() - blk.intoT) < 0.25;
            if (deflect) { WriteStamina(player, g_prevStam); stam = g_prevStam; g_haptic.fetch_or(32u); }
            else g_haptic.fetch_or(16u);
            BodyHapticsPost(deflect ? BodyKind::Deflect : BodyKind::Block, HandLeft, deflect ? 1.f : Clamp(.4f + (float)drop / 100.f, .4f, 1.f));
            if (c.combatDebug) LOGD("[combat] blocked hit: stamina -%d quality %.2f into %.2f m/s%s", drop, blk.quality, blk.into, deflect ? " -> DEFLECT (refunded)" : "");
        }
    } else RestoreGuard();
    EndFrameStamina(player, maxStam);
    FlushTally(nowS);
}

// ---------------------------------------------------------------- swing detection (worker thread)
Vec3 WeaponAxisVR() {
    const Config& c = Cfg();
    float pitch = c.handRotOffX; DebugKeysHandRotX(&pitch);
    const Quat calibration = Quat::FromEulerYXZ(Deg2Rad(c.handRotOffY), Deg2Rad(pitch), Deg2Rad(c.handRotOffZ));
    const Vec3 gameAxis = WeaponShaftAxis(calibration, c.twoHandAxis != "forward");
    return {gameAxis.x, gameAxis.y, -gameAxis.z};
}
float TipSpeed(const Pose& p, float reach, float* handSpeed) {
    Vec3 tip = WeaponTipVelocity(p.linVel * Cfg().handScale, p.angVel, p.rot, WeaponAxisVR(), reach);
    if (handSpeed) *handSpeed = p.linVel.Length() * Cfg().handScale;
    return tip.Length();
}
// Kinetic-energy shaped quality: (v/ref)^2, capped, times the arc factor (a strike that wound up
// through a real arc counts fully; a flick from rest counts 60%). The arc a weapon needs grows
// with its weight: a dagger is a flick, an ultra greatsword is a whole-body swing.
float ArcRefFor(float weight, const Config& c) {
    const float arcRef = c.kineticArcRef > 0.05f ? c.kineticArcRef : 0.5f;
    return arcRef * (1.f + weight / 10.f);
}
// The reference speed is defined for a KineticRefReach lever: a longer weapon's vanilla swing moves its
// tip proportionally faster, so quality 1 scales with the weapon's own length (a dagger flick and a
// colossal-sword arc both measure against what that weapon's real swing would do).
float StrikeQuality(float tipSpeed, float arc, float weight, float reach, const Config& c) {
    float ref = c.kineticRefSpeed > 0.5f ? c.kineticRefSpeed : 8.f;
    if (c.kineticRefReach > 0.05f && reach > 0.05f) ref *= reach / c.kineticRefReach;
    float q = (tipSpeed / ref) * (tipSpeed / ref); if (q > 1.5f) q = 1.5f;
    float a = arc / ArcRefFor(weight, c); if (a > 1.f) a = 1.f;
    return q * (0.6f + 0.4f * a);
}
// Force-limited tip speed: the virtual weapon accelerates towards the real tip speed at most
// KineticAccel / (1 + weight/KineticAccelWeight) m/s^2 (braking is twice as easy).
float StepVirtualSpeed(float vVirt, float tipSpeed, float weight, float dt, const Config& c) {
    const float aMax = (c.kineticAccel > 1.f ? c.kineticAccel : 60.f) / (1.f + weight / (c.kineticAccelWeight > 0.1f ? c.kineticAccelWeight : 4.f));
    const float dv = tipSpeed - vVirt;
    const float lim = aMax * dt;
    if (dv > lim) return vVirt + lim;
    if (dv < -2.f * lim) return vVirt - 2.f * lim;
    return tipSpeed;
}

void StartSwing(int h, int slot, double now, float quality, float arc, float tip) {
    std::lock_guard<std::mutex> g(g_mtx);
    SwingCmd& k = g_cmd[h];
    k.Begin(); k.slot = slot; k.start = now; k.id = g_sm[h].nextId++; k.staminaFail = false;
    k.quality = k.qualityCommit = quality; k.arc = arc; k.tipSpeed = tip; k.tipLive = tip;
}
void UpdateSwing(int h, float quality, float arc, float tip, float tipLive) {
    std::lock_guard<std::mutex> g(g_mtx);
    SwingCmd& k = g_cmd[h];
    if (!k.active) return;
    k.quality = quality; k.arc = arc; k.tipSpeed = tip; k.tipLive = tipLive;
}
void EndSwing(int h, bool cancel = false) {
    std::lock_guard<std::mutex> g(g_mtx);
    g_cmd[h].End(cancel);
}

// The grab (worker): put the free hand on the hilt - within TwoHandGrabRadius of the weapon axis,
// TwoHandGrabMin..TwoHandGrabMax m toward the pommel - and SQUEEZE its grip. That is the whole
// gesture: the grip alone is the hold - two-handed for exactly as long as it stays closed, wherever
// the hands move - and opening it is the only release. (Position gates on the hold - release radii,
// stillness, dwell debounces, cooldowns - were tried 2026-08-25/26 and only broke it: swings read
// as releases threw weapons between hands, and the gates kept honest grabs from ever taking.)
// The right weapon is tried first; closing the right hand on the left weapon two-hands that one
// instead. Lost tracking changes nothing: the grip stream is the hold.
void PollTwoHand(const VRTracking& tr, const VRInputState& in, double now) {
    const Config& c = Cfg();
    TwoHandState s; { std::lock_guard<std::mutex> g(g_mtx); s = g_two; }
    if (c.twoHandMode != "grab") { if (s.want || s.inZone) { std::lock_guard<std::mutex> g(g_mtx); g_two = TwoHandState{}; } return; }
    // Use the same calibrated shaft as the rendered pin, steering, and tip
    // measurement; the negative direction points toward the pommel.
    const bool handleAxis = c.twoHandAxis != "forward";
    auto geometry = [&](int w, float* along, float* perp) {
        const Pose& wp = tr.hand[w]; const Pose& fp = tr.hand[1 - w];
        if (!wp.valid || !fp.valid || !in.controllerConnected[w] || !in.controllerConnected[1 - w]) return false;
        Vec3 d = fp.pos - wp.pos; Vec3 ax = -wp.rot.Rotate(WeaponAxisVR());
        *along = d.Dot(ax); *perp = (d - ax * *along).Length();
        return true;
    };
    // The grip is analog in VRInputState (the backend never sets VRB_LGrip/RGrip); threshold it as virtual_pad does.
    auto gripOf = [&](int h) { return in.handTrigger[h] >= c.gripThreshold; };
    // A grab is a FRESH squeeze at the hilt: the grip must have closed within the last 0.75 s, so a
    // hand that wanders past with its grip long held (guarding, habit) does nothing until it opens
    // and squeezes again - but squeezing slightly before the hand arrives still grabs.
    static bool s_prevGrip[2] = {false, false};
    static double s_gripCloseT[2] = {-1e9, -1e9};
    for (int h = 0; h < 2; ++h) { if (gripOf(h) && !s_prevGrip[h]) s_gripCloseT[h] = now; s_prevGrip[h] = gripOf(h); }
    static const char* s_geoBlock = nullptr;   // why the take is not happening (previous tick; shown in the geometry line)
    // Calibration line (2 Hz): the geometry whenever a grip is squeezed with the hands near each
    // other, so a grab that does not take can be read off the log and the window adjusted.
    static double s_lastGeoLog = 0;
    if (now - s_lastGeoLog > 0.5) {
        for (int w = 0; w < 2; ++w) {
            float along = 0.f, perp = 0.f;
            if (!gripOf(1 - w) || !geometry(w, &along, &perp) || std::fabs(along) > 0.6f || perp > 0.6f) continue;
            s_lastGeoLog = now;
            LOGD("[combat] two-hand geometry: %s hand (grip) vs %s weapon: %.2f m toward the pommel along the %s axis (window %.2f..%.2f) off-axis %.2f m (radius %.2f) weapon %d%s%s%s", HandName(1 - w), HandName(w), along, handleAxis ? "handle" : "aim", c.twoHandGrabMin, c.twoHandGrabMax, perp, c.twoHandGrabRadius, g_wepPresent[w].load(), s.want ? " [held]" : "", !s.want && s_geoBlock ? " blocked: " : "", !s.want && s_geoBlock ? s_geoBlock : "");
        }
    }
    if (!s.want) {
        bool zoneNow = false;
        for (int w = HandRight; w >= HandLeft && !s.want; --w) {
            float along = 0.f, perp = 0.f;
            // A hand choosing on its weapon wheel is not grabbing: the right hand's inward slot lies toward the left hilt.
            // Nor is one that took an item from the belt (a weapon hanging low puts its hilt by the hip).
            if (WeaponWheelOwnsHand(1 - w) || BeltOwnsHand(1 - w)) continue;
            if (!g_wepPresent[w].load() || !geometry(w, &along, &perp)) continue;
            if (!(along >= c.twoHandGrabMin && along <= c.twoHandGrabMax && perp <= c.twoHandGrabRadius)) continue;
            zoneNow = true; s.pinHand = 1 - w; s.weaponHand = w;
            s_geoBlock = !gripOf(1 - w) ? "grip not squeezed"
                       : now - s_gripCloseT[1 - w] > 0.75 ? "grip was already closed - open it and squeeze again"
                       : nullptr;
            if (!s_geoBlock) {
                s.want = true; s.along = along;
                s.grip.Capture(tr.hand[w].rot, tr.hand[1 - w].rot);
                LOGI("[combat] two-hand: %s hand took the %s weapon %.2f m down the hilt (%.2f m off axis)", HandName(1 - w), HandName(w), along, perp);
            }
        }
        s.inZone = zoneNow;
        if (!zoneNow) s_geoBlock = nullptr;
    } else {
        // The grip is the hold, full stop. Half the close threshold as hysteresis rides out analog dips.
        if (in.handTrigger[s.pinHand] < 0.5f * c.gripThreshold) {
            s.want = false;
            LOGI("[combat] two-hand: %s hand let go (grip opened)", HandName(s.pinHand));
        }
        s.inZone = s.want;   // the grabbing grip's pad mapping stays blocked while held
    }
    std::lock_guard<std::mutex> g(g_mtx); g_two = s;
}
// The skill trigger (worker), only while the game reports a two-handed arm style. "effect": a tap
// replays the recorded effect (or records it the first time), holding past TwoHandSkillRelearnHold
// plays it the game's way again and re-records. "game": the press goes straight through.
void PollSkillTrigger(const VRInputState& in, double now) {
    const Config& c = Cfg();
    static bool s_down = false; static double s_downT = 0; static bool s_long = false;
    const uint32_t mask = ParseVRButton(c.twoHandSkillButton);
    // Triggers and grips are analog in VRInputState (the backend sets no VRB_*Trigger/Grip bits): derive them.
    uint32_t buttons = in.buttons;
    if (in.indexTrigger[HandLeft]  >= c.triggerThreshold) buttons |= VRB_LTrigger;
    if (in.indexTrigger[HandRight] >= c.triggerThreshold) buttons |= VRB_RTrigger;
    if (in.handTrigger[HandLeft]   >= c.gripThreshold)    buttons |= VRB_LGrip;
    if (in.handTrigger[HandRight]  >= c.gripThreshold)    buttons |= VRB_RGrip;
    const bool two = TwoHandedStyle(g_armStyleShared.load());
    const bool dn = mask && (buttons & mask) != 0 && two && c.twoHandSkill != "off";
    if (dn && !s_down) {
        s_down = true; s_downT = now; s_long = false;
        if (c.twoHandSkill == "game") { s_long = true; g_skillReq.store(2); }   // straight through, on the press
    } else if (dn && s_down && !s_long && c.twoHandSkillRelearnHold > 0.f && now - s_downT >= c.twoHandSkillRelearnHold) {
        s_long = true; g_skillReq.store(2);
    } else if (!dn && s_down) {
        s_down = false;
        if (!s_long && two) g_skillReq.store(1);
    }
    // The skill (replayed or the game's) flies where the two-handed weapon's hand points: remember that hand for
    // the skill's own aim events (a replay with bullets arms itself in StartReplay).
    if (dn) { g_castPressHand.store(TwoHandedWeaponHand(g_armStyleShared.load())); g_castPressAt.store(now); }
}

// Shield pose against the head -> guard held + block quality. The threat is assumed to be where
// you look, flattened to the horizon (enemies stand on the same ground).
//
// Two separate questions, because the first test showed quality gating the guard itself makes
// blocking flicker: (1) is the shield UP - anywhere in front of you above the hips - then the
// guard button is held continuously (with a release delay); (2) how GOOD is the block - facing,
// coverage of the head/chest line, steadiness - which only shapes absorption and stability.
void PollBlock(const VRTracking& tr, double now) {
    const Config& c = Cfg();
    BlockState b;
    { std::lock_guard<std::mutex> g(g_mtx); b = g_block; }
    const Pose& sh = tr.hand[HandLeft]; const Pose& hd = tr.head;
    static double s_upSince = 0, s_downSince = 0, s_lastLog = 0;
    if (!g_leftShield.load() || !sh.valid || !hd.valid) { b.raised = false; b.quality = 0.f; b.into = 0.f; s_upSince = s_downSince = 0; }
    else {
        Vec3 fwd = hd.rot.Forward(); fwd.y = 0.f; fwd = fwd.Normalized();
        if (fwd.Length() < 0.5f) fwd = {0.f, 0.f, -1.f};
        Vec3 n = sh.rot.Rotate(Quat::AxisAngle({1.f, 0.f, 0.f}, Deg2Rad(c.shieldNormalPitchDeg)).Rotate({0.f, 0.f, -1.f}));
        const float align = Clamp(n.Dot(fwd), 0.f, 1.f);
        Vec3 d = sh.pos - hd.pos;
        const float fd = d.Dot(fwd);
        Vec3 lat = d - fwd * fd; lat.y = 0.f;
        const float lateral = lat.Length(), height = d.y;
        auto band = [](float v, float lo, float hi, float fade) { if (v < lo) return Clamp(1.f - (lo - v) / fade, 0.f, 1.f); if (v > hi) return Clamp(1.f - (v - hi) / fade, 0.f, 1.f); return 1.f; };
        // Up: in front of the body (not hanging behind or at the far side), above the hips.
        const bool up = fd > -0.05f && lateral < 0.6f && height > -0.8f && height < 0.35f;
        if (up) { s_downSince = 0; if (s_upSince == 0) s_upSince = now; }
        else { s_upSince = 0; if (s_downSince == 0) s_downSince = now; }
        if (!b.raised && up && now - s_upSince >= 0.05) b.raised = true;
        if (b.raised && !up && now - s_downSince >= c.blockReleaseSeconds) b.raised = false;
        // Quality: how squarely the shield sits between the threat and the head/chest.
        const float cov = band(fd, 0.10f, 0.80f, 0.20f) * band(lateral, 0.f, 0.30f, 0.30f) * band(height, -0.70f, 0.20f, 0.20f);
        const float brace = 1.f - Clamp((sh.linVel.Length() - 0.6f) / 1.4f, 0.f, 1.f);
        b.quality = b.raised ? Clamp(align * 0.5f + cov * 0.35f + brace * 0.15f, 0.f, 1.f) : 0.f;
        const float v = sh.linVel.Dot(fwd);
        if (v > b.into || now - b.intoT > 0.12) { b.into = v; b.intoT = now; }
        // Calibration line (1 Hz while the shield is up): which controller axis faces forward
        // tells the right ShieldNormalPitch; the geometry tells why coverage is low.
        if (c.combatDebug && b.raised && now - s_lastLog > 1.0) {
            s_lastLog = now;
            const Vec3 ax = sh.rot.Rotate({1.f, 0.f, 0.f}), ay = sh.rot.Rotate({0.f, 1.f, 0.f}), az = sh.rot.Rotate({0.f, 0.f, -1.f});
            LOGD("[combat] shield: q %.2f align %.2f cov %.2f brace %.2f | fwd %.2f lat %.2f height %.2f | axes.fwd X %.2f Y %.2f -Z %.2f", b.quality, align, cov, brace, fd, lateral, height, ax.Dot(fwd), ay.Dot(fwd), az.Dot(fwd));
        }
    }
    std::lock_guard<std::mutex> g(g_mtx); g_block = b;
}

} // namespace

// ---------------------------------------------------------------- public API
bool CombatInit(uint8_t* base, bool versionOk) {
    ParamsInit();
    void** vft = nullptr;
    if (versionOk) vft = (void**)(base + RVA_ChrTaeAnimEventVft);
    void** rttiVft = FindVTable("CS::CSChrTaeAnimEvent");
    if (vft && rttiVft && vft != rttiVft) { LOGW("[combat] RVA vtable %p != RTTI vtable %p; using RTTI", (void*)vft, (void*)rttiVft); vft = rttiVft; }
    if (!vft) vft = rttiVft;
    if (!vft) { LOGW("[combat] CS::CSChrTaeAnimEvent vtable not found; physical combat unavailable"); return false; }
    g_taeVft = vft;
    void* target = nullptr; SafeRead(&vft[kExecSlot], &target, sizeof(target));
    if (!target) return false;
    if (MH_CreateHook(target, (void*)&Hook_ExecEvent, (void**)&g_origExec) == MH_OK && MH_EnableHook(target) == MH_OK) {
        g_status.taeHooked = true;
        LOGI("[combat] TAE behavior-event hook installed (CSChrTaeAnimEvent vft[%d] @ %p)", kExecSlot, target);
    } else LOGW("[combat] failed to hook TAE event exec @ %p", target);
    // Slot 2 (generic events): watched only while a skill is recorded, called for its replay.
    void* generic = nullptr; SafeRead(&vft[kGenericSlot], &generic, sizeof(generic));
    if (generic && MH_CreateHook(generic, (void*)&Hook_GenericEvent, (void**)&g_origGeneric) == MH_OK && MH_EnableHook(generic) == MH_OK)
        LOGI("[combat] TAE generic-event hook installed (vft[%d] @ %p): skill effects can be recorded and replayed", kGenericSlot, generic);
    else { g_origGeneric = nullptr; LOGW("[combat] failed to hook TAE generic events @ %p: skill effects (SpEffect/FFX) will not replay, only bullets and attacks", generic); }
    { std::lock_guard<std::mutex> g(g_mtx); ApplyConfigTemplates(); }
    return g_status.taeHooked;
}

void CombatShutdown() { RestoreAll(); }

// Physical combat exists only while the player is IN THE WORLD. Without the player check the
// attack-button block stayed armed on the title screen and in menus - RB (right trigger) and
// RT (right grip) silently did nothing there, since no swing can fire to replace them
// (reported 2026-08-21: "right trigger seems disabled").
// AND only while the world is not mid-transition (2026-08-23): the arm motion of pressing A
// on the fast-travel confirm reads as a ~4 m/s swing, so every travel injected an
// attack-behavior event into the player 200-400 ms into world teardown - executing the
// game's behavior dispatch there is a coin-flip crash (CS::ChrIns + garbage index, second
// travel of the 21:59 session). GameERTransitionActive covers request, teardown, load and
// arrival-settle; CombatPollHands resets any in-flight swing the moment this goes false.
bool CombatHandlesSwings() {
    if (!GameERPlayerActive()) return false;
    // Menus stand combat down entirely (2026-08-26 "cannot change weapons now"): the equipment
    // menu refuses changes while the game sees combat state, so nothing of ours may run there.
    if (Cfg().combatMenuGate && g_menuOpen.load()) return false;
    if (GameERTransitionActive()) {
        // One line per transition window, so a future postmortem can see the suppression.
        static ULONGLONG s_lastLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastLog > 10000) { s_lastLog = now; LOGD("[combat] swings suppressed: world transition"); }
        return false;
    }
    std::lock_guard<std::mutex> g(g_mtx);
    return g_status.taeHooked && PhysicalActiveLocked();
}

void CombatNotifyButton(uint16_t mask, double now) {
    const Config& c = Cfg();
    std::lock_guard<std::mutex> g(g_mtx);
    if (mask == ParseXInputButton(c.lightAttackButton)) g_lastBtn[SlotLight] = now;
    if (mask == ParseXInputButton(c.heavyAttackButton)) g_lastBtn[SlotHeavy] = now;
    if (mask == ParseXInputButton(c.leftSwingButton))   g_lastBtn[SlotLeft] = now;
}

// Cast aim: which hand a game-facing press belongs to. Light/heavy = the right weapon (two-handed: the
// two-handed weapon's hand, whichever it is); the left button = the left weapon unless it is a shield
// (guard, not a cast); the skill button = the right weapon's skill, the left's only with an empty right hand.
// ---------------------------------------------------------------- action-request diagnostic + pass-2 keep
// "Most controls only work while walking" (long-standing, 2026-09-04 log): the XInput side is proven -
// the game polls at 60 Hz and the button bits go out with the stick idle - and the same LB press casts
// on one idle frame and does nothing on another. What is left is the game's own action pipeline:
// pad -> CSChrActionRequestModule (new press / input buffer / TAE cancel windows) -> HKS. Under full-frame
// stereo that module updates TWICE per sim frame (pass 2 re-runs ChrIns_PreBehavior at steps 0), so a press
// buffered in pass 1 can be re-evaluated in pass 2 against cancel windows the zero-length TAE step never
// re-opened, and be gone before the next real frame. Two tools, both game thread:
//  * CombatActionDiag: for ~0.35 s after any press handed to the game, log the module + TimeAct state once
//    per pass; a baseline line whenever the animation / allowed-input masks change. (Combat Debug)
//  * CombatPass2Begin/End: preserve buffer fields and bounded queue contents across zero-step pass 2
//    unless it consumed an action or changed the player/modules/storage. [VR] Pass2KeepActionRequest.
// The 2026-09-09 log uses queue mode (qm 1) throughout: its actual per-TAE input/cancel
// vectors must survive too. Restoring only the scalar fields never logged a change.
namespace {
std::atomic<double>   g_adiagUntil{0.0};
std::atomic<uint32_t> g_adiagRise{0};
struct ARSnap {
    uint8_t* player = nullptr; uint8_t* mods = nullptr; uint8_t* areq = nullptr; uint8_t* ta = nullptr;
    uint8_t* ev = nullptr; uint8_t* chrAsm = nullptr;
    uint64_t req = 0, newp = 0, ready = 0, queued = 0, dis = 0, in = 0, can = 0, prevIn = 0;
    uint64_t readback[4]{};
    ActionQueueSnapshot inputQueue, cancelQueue;
    int32_t queueTae = -1; uint32_t equipment[3]{};
    uint32_t mv = 0, tae = 0; uint8_t qmode = 0;
    int32_t anim = -1; uint32_t readIdx = 0; float t = 0.f, len = 0.f;
    int32_t reqAnim = -1, idleAnim = -1; uint32_t stay = 0;
    bool ok = false, animOk = false, eventOk = false, equipmentOk = false;
};
ARSnap ReadAR(uint8_t* player) {
    ARSnap s;
    if (!player) return s;
    uint8_t* mods = Deref(player + Chr_Modules); if (!mods) return s;
    s.player = player; s.mods = mods;
    s.areq = Deref(mods + Mod_ActionRequest); s.ta = Deref(mods + Mod_TimeAct);
    if (!s.areq || Deref(s.areq + 8) != player || Deref(s.areq + 0x48) != s.areq) return s;
    s.ok = SafeRead(s.areq + AR_Requests, &s.req, 8) && SafeRead(s.areq + AR_NewPresses, &s.newp, 8) &&
           SafeRead(s.areq + AR_CancelReady, &s.ready, 8) && SafeRead(s.areq + AR_Queued, &s.queued, 8) &&
           SafeRead(s.areq + AR_Disabled, &s.dis, 8) && SafeRead(s.areq + AR_PossibleInputs, &s.in, 8) &&
           SafeRead(s.areq + AR_PossibleCancels, &s.can, 8) && SafeRead(s.areq + AR_PrevPossibleIn, &s.prevIn, 8) &&
           SafeRead(s.areq + AR_MoveFlags, &s.mv, 4) && SafeRead(s.areq + AR_TaeCancels, &s.tae, 4) &&
           SafeRead(s.areq + AR_QueueMode, &s.qmode, 1) &&
           SafeRead(s.areq + AR_QueueTae, &s.queueTae, 4) &&
           SafeRead(s.areq + AR_Readback, s.readback, sizeof(s.readback));
    if (s.qmode == 1) {
        s.inputQueue = ReadActionQueue(s.areq + AR_InputQueue, SafeRead);
        s.cancelQueue = ReadActionQueue(s.areq + AR_CancelQueue, SafeRead);
    }
    if (s.ta && Deref(s.ta + 8) == player && SafeRead(s.ta + TA_ReadIdx, &s.readIdx, 4)) {
        uint8_t* e = s.ta + TA_AnimQueue + 16 * (s.readIdx % 10);
        s.animOk = SafeRead(e, &s.anim, 4) && SafeRead(e + 4, &s.t, 4) && SafeRead(e + 12, &s.len, 4) &&
                   s.anim >= 0 && std::isfinite(s.t) && std::isfinite(s.len) && s.t >= 0.f && s.len > 0.f;
    }
    s.ev = Deref(mods + Mod_Event);
    if (s.ev) s.eventOk = SafeRead(s.ev + 0x18, &s.reqAnim, 4) && SafeRead(s.ev + 0x1C, &s.idleAnim, 4);
    s.chrAsm = Deref(player + Player_ChrAsm);
    if (s.chrAsm) s.equipmentOk = SafeRead(s.chrAsm + Asm_ArmStyle, s.equipment, sizeof(s.equipment)) &&
                                   s.equipment[0] <= 3 && s.equipment[1] <= 2 && s.equipment[2] <= 2;
    if (uint8_t* af = Deref(mods + Mod_ActionFlag)) { uint32_t f = 0; SafeRead(af + 0x10, &f, 4); s.stay = f & 1u; }
    return s;
}
ARSnap g_p2Snap;
} // namespace

void CombatActionDiag(uint8_t* player, int pass, uint32_t frame) {
    if (!Cfg().combatDebug || !player) return;
    const double now = NowSec();
    const bool armed = g_adiagUntil.load() > now;
    static uint32_t s_lastFrame[3] = {~0u, ~0u, ~0u};
    static int32_t s_anim = -2; static uint64_t s_in = ~0ull, s_can = ~0ull; static uint8_t s_qm = 2;
    static double s_lastBase = 0; static int s_baseBudget = 0;
    if (pass < 0 || pass > 2) pass = 0;
    if (s_lastFrame[pass] == frame) return;   // the Havok hook fires several times per pass
    s_lastFrame[pass] = frame;
    if (!armed) {
        if (pass == 2) return;
        ARSnap s = ReadAR(player); if (!s.ok) return;
        if (s.anim == s_anim && s.in == s_in && s.can == s_can && s.qmode == s_qm) return;
        if (now - s_lastBase > 1.0) { s_lastBase = now; s_baseBudget = 4; }
        if (s_baseBudget-- <= 0) return;
        s_anim = s.anim; s_in = s.in; s_can = s.can; s_qm = s.qmode;
        LOGD("[combat] adiag state f%u: anim %d (%.2f/%.2f) idle %d | in %llx can %llx dis %llx tae %x mv %x qm %d stay %u",
             frame, s.anim, s.t, s.len, s.idleAnim, (unsigned long long)s.in, (unsigned long long)s.can,
             (unsigned long long)s.dis, s.tae, s.mv, s.qmode, s.stay);
        return;
    }
    ARSnap s = ReadAR(player); if (!s.ok) return;
    LOGD("[combat] adiag p%d f%u rise 0x%04X | req %llx new %llx q %llx ready %llx in %llx can %llx dis %llx tae %x mv %x qm %d stay %u | anim %d t %.2f/%.2f req %d idle %d | queues %zu/%zu mask %llx/%llx valid %d/%d qtae %d",
         pass, frame, g_adiagRise.load(), (unsigned long long)s.req, (unsigned long long)s.newp, (unsigned long long)s.queued,
         (unsigned long long)s.ready, (unsigned long long)s.in, (unsigned long long)s.can, (unsigned long long)s.dis,
         s.tae, s.mv, s.qmode, s.stay, s.anim, s.t, s.len, s.reqAnim, s.idleAnim,
         s.inputQueue.count, s.cancelQueue.count, (unsigned long long)ActionQueueMask(s.inputQueue),
         (unsigned long long)ActionQueueMask(s.cancelQueue), s.inputQueue.valid, s.cancelQueue.valid, s.queueTae);
}

void CombatPass2Begin(uint8_t* player) {
    g_p2Snap = ARSnap{};
    if (!Cfg().vrPass2KeepActionRequest || Cfg().vrPass2Steps != 0 || !player ||
        player != GameERMainPlayer() || GameERTransitionActive()) return;
    g_p2Snap = ReadAR(player);
}
void CombatPass2End(uint8_t* player) {
    const ARSnap b = g_p2Snap; g_p2Snap = ARSnap{};
    if (!b.ok || !player || player != b.player || player != GameERMainPlayer() || GameERTransitionActive()) return;
    const ARSnap a = ReadAR(player);
    if (!a.ok || a.mods != b.mods || a.areq != b.areq || a.ta != b.ta || a.ev != b.ev ||
        a.chrAsm != b.chrAsm || a.qmode != b.qmode || a.qmode > 1) return;
    if (!ActionRequestRollbackAllowed(b.dis, a.dis, b.tae, a.tae)) return;
    // The ring index advances for animation UPDATES too. Require the same animation/time
    // instead; a restarted/advanced animation or an equipment change consumed the input.
    if (!a.animOk || !b.animOk || a.anim != b.anim || std::fabs(a.t - b.t) > 0.0005f ||
        !a.eventOk || !b.eventOk || a.reqAnim != b.reqAnim || a.idleAnim != b.idleAnim ||
        !a.equipmentOk || !b.equipmentOk || std::memcmp(a.equipment, b.equipment, sizeof(a.equipment))) return;
    if (b.qmode && (a.queueTae != b.queueTae || !CanRestoreActionQueue(b.inputQueue, a.inputQueue) ||
                   !CanRestoreActionQueue(b.cancelQueue, a.cancelQueue))) return;
    bool changed = false;
    auto put = [&](size_t off, const void* v, size_t n) { changed |= SafeWrite(b.areq + off, v, n); };
    if (b.qmode) {
        changed |= RestoreActionQueue(b.areq + AR_InputQueue, b.inputQueue, a.inputQueue, SafeWrite);
        changed |= RestoreActionQueue(b.areq + AR_CancelQueue, b.cancelQueue, a.cancelQueue, SafeWrite);
    }
    if (a.queued != b.queued) put(AR_Queued, &b.queued, 8);
    if (a.ready  != b.ready)  put(AR_CancelReady, &b.ready, 8);
    if (a.in     != b.in)     put(AR_PossibleInputs, &b.in, 8);
    if (a.can    != b.can)    put(AR_PossibleCancels, &b.can, 8);
    if (a.prevIn != b.prevIn) put(AR_PrevPossibleIn, &b.prevIn, 8);
    const uint32_t restoredTae = (a.tae & ~ActionRequestTransientCancels) | (b.tae & ActionRequestTransientCancels);
    if (a.tae != restoredTae) put(AR_TaeCancels, &restoredTae, 4);
    if (std::memcmp(a.readback, b.readback, sizeof(b.readback))) put(AR_Readback, b.readback, sizeof(b.readback));
    if (changed && Cfg().combatDebug) {
        static double s_last = 0; static int s_budget = 0; const double now = NowSec();
        if (now - s_last > 1.0) { s_last = now; s_budget = 3; }
        if (s_budget-- > 0)
            LOGD("[combat] pass2 action-request restored: q %llx->%llx ready %llx->%llx in %llx->%llx can %llx->%llx tae %x->%x (anim %d) queues %zu/%zu -> %zu/%zu",
                 (unsigned long long)a.queued, (unsigned long long)b.queued, (unsigned long long)a.ready, (unsigned long long)b.ready,
                 (unsigned long long)a.in, (unsigned long long)b.in, (unsigned long long)a.can, (unsigned long long)b.can, a.tae, b.tae, a.anim,
                 a.inputQueue.count, a.cancelQueue.count, b.inputQueue.count, b.cancelQueue.count);
    }
}

namespace {
void NoteDiagPress(uint16_t rise, double now) {
    if (!rise || !Cfg().combatDebug) return;
    g_adiagRise.store(rise); g_adiagUntil.store(now + 0.35);
}
} // namespace

void CombatNotifyPad(uint16_t mask, double now) {
    const Config& c = Cfg();
    const uint16_t rise = (uint16_t)(mask & ~g_padPrev);
    g_padPrev = mask;
    NoteDiagPress(rise, now);
    if (!c.castAim) return;
    const uint16_t light = ParseXInputButton(c.lightAttackButton), heavy = ParseXInputButton(c.heavyAttackButton);
    const uint16_t left = ParseXInputButton(c.leftSwingButton), skill = ParseXInputButton(c.skillButton);
    const int style = g_armStyleShared.load();
    int hand = -1; const char* why = "press";
    if (rise & (light | heavy)) { hand = TwoHandedStyle(style) ? TwoHandedWeaponHand(style) : HandRight; why = (rise & light) ? "light attack" : "heavy attack"; }
    if ((rise & left) && !g_wepShield[HandLeft].load()) { hand = TwoHandedStyle(style) ? TwoHandedWeaponHand(style) : HandLeft; why = "left-hand attack"; }
    if (rise & skill) {
        if (TwoHandedStyle(style)) hand = TwoHandedWeaponHand(style);
        else hand = (g_wepRanged[HandRight].load() || !g_wepRanged[HandLeft].load()) ? HandRight : HandLeft;
        why = "skill button";
    }
    // A press only REMEMBERS the hand; the game's aim events arm the aim (a press by itself must not - guard
    // and grip-mapped buttons are presses too).
    if (hand >= 0) { g_castPressHand.store(hand); g_castPressAt.store(now); if (c.combatDebug) LOGD("[combat] cast aim: %s hand pressed (%s)", HandName(hand), why); }
}

int CombatCastAimHand(bool* casting) {
    const Config& c = Cfg();
    if (casting) *casting = false;
    if (!c.castAim || !GameERPlayerActive()) return -1;
    // Only a LIVE cast aims. A merely held catalyst used to aim passively (matrix + lock target down its ray)
    // and in lock mode the game turned the body toward that target while moving: a seal held at your side
    // spun the character around (2026-09-04). The passive hand (g_castWepHand) now only routes buttons.
    if (g_castUntil.load() <= NowSec()) return -1;
    int hand;
    if (c.castAimHand == "right") hand = HandRight;
    else if (c.castAimHand == "left") hand = HandLeft;
    else hand = g_castPressHand.load();
    if (casting) *casting = hand >= 0;
    return hand;
}

uint32_t CombatTakeHapticRequests() { return g_haptic.exchange(0); }

uint16_t CombatBlockedButtons() {
    const Config& c = Cfg();
    if (!CombatHandlesSwings()) return 0;
    // A catalyst / bow in the attacking hand CASTS with the light/heavy buttons (no animation to keep off
    // the arms): those pass. Two-handed, the attacking hand is the two-handed weapon's.
    const int attackHand = TwoHandedStyle(g_armStyleShared.load()) ? TwoHandedWeaponHand(g_armStyleShared.load()) : HandRight;
    const bool castsWithButtons = c.castAim && g_wepRanged[attackHand].load() != 0;
    uint16_t m = c.combatBlockAttackButtons && !castsWithButtons ? (uint16_t)(ParseXInputButton(c.lightAttackButton) | ParseXInputButton(c.heavyAttackButton)) : 0;
    if (c.twoHandMode == "grab") {
        // The grip that holds the weapon is a grab, not the button it is mapped to (LT = the skill).
        std::lock_guard<std::mutex> g(g_mtx);
        if (g_two.want || g_two.inZone) m |= ParseXInputButton(g_two.pinHand == HandLeft ? c.btnLGrip : c.btnRGrip);
    }
    // The two-handed skill trigger must not also send what it is mapped to.
    if (c.twoHandSkill != "off" && TwoHandedStyle(g_armStyleShared.load())) m |= MappedButtonOf(c.twoHandSkillButton, c);
    return m;
}

// The game's own chords, played on the pad after the block: modifier down, the key a little
// later (the game wants the modifier seen first), both up. A lone key (skill / learn press) is
// just the key. One chord at a time; a new request waits for the running one to finish.
uint16_t CombatForcedButtons(double now) {
    const Config& c = Cfg();
    ChordRun& r = g_chord;
    if (!CombatHandlesSwings()) { r.phase = 0; g_chordReq.store(0); return 0; }
    if (r.phase == 0) {
        const int req = g_chordReq.exchange(0);
        if (!req) return 0;
        r.kind = req; r.t = now; r.phase = 1; r.lead = 0; r.key = 0;
        switch (req) {
        case 1: r.lead = ParseXInputButton(c.twoHandModifierButton); r.key = ParseXInputButton(c.lightAttackButton); break;
        case 2: r.lead = ParseXInputButton(c.twoHandModifierButton); r.key = ParseXInputButton(c.guardButton); break;
        case 3: r.key = ParseXInputButton(c.skillButton); break;
        case 4: r.key = ParseXInputButton(c.lightAttackButton); break;
        case 5: r.key = ParseXInputButton(c.heavyAttackButton); break;
        // Mounted attacks (RideAttacks=buttons): pressed for the game, not a learn press.
        case 6: r.key = ParseXInputButton(c.lightAttackButton); break;
        case 7: r.key = ParseXInputButton(c.heavyAttackButton); break;
        case 8: r.key = ParseXInputButton(c.guardButton); break;
        default: r.phase = 0; return 0;
        }
        if (!r.key) { r.phase = 0; LOGW("[combat] chord %d: no button configured", req); return 0; }
        if (req == 4 || req == 5) { std::lock_guard<std::mutex> g(g_mtx); g_lastBtn[req == 4 ? SlotLight : SlotHeavy] = now; }   // the hook learns from this press
        if (!r.lead) r.phase = 2;
        if (c.combatDebug) LOGD("[combat] chord %d: %s%s%s", req, r.lead ? XInputButtonName(r.lead) : "", r.lead ? " + " : "", XInputButtonName(r.key));
    }
    // A lone key (skill / learn press) uses the gesture press duration; a chord uses the slower,
    // human-like two-hand timing (the game watches for RB/LB while Y is held).
    const double press = r.lead ? (c.twoHandChordPress > 0.05f ? c.twoHandChordPress : 0.05) : (c.gesturePressDuration > 0.08 ? c.gesturePressDuration : 0.08);
    const double lead = c.twoHandChordLead > 0.05f ? c.twoHandChordLead : 0.05, tail = c.twoHandChordTail > 0.02f ? c.twoHandChordTail : 0.02;
    switch (r.phase) {
    case 1: if (now - r.t >= lead) { r.phase = 2; r.t = now; } return r.lead;                       // modifier down first
    case 2: if (now - r.t >= press) { r.phase = 3; r.t = now; } return (uint16_t)(r.lead | r.key);   // + the key
    case 3: if (now - r.t >= tail) r.phase = 0; return r.lead;                                      // key up, modifier lingers
    }
    r.phase = 0; return 0;
}

bool CombatTwoHandPin(int* pinHand, int* weaponHand, float* alongToPommel, bool* handleAxis,
                      TwoHandGripOrientation* grip) {
    const Config& c = Cfg();
    if (!c.twoHandPinHand) return false;
    const int style = g_armStyleShared.load();
    std::lock_guard<std::mutex> g(g_mtx);
    if (!g_two.want || !TwoHandedStyle(style) || TwoHandedWeaponHand(style) != g_two.weaponHand) return false;
    *pinHand = g_two.pinHand; *weaponHand = g_two.weaponHand; *alongToPommel = g_two.along; *handleAxis = c.twoHandAxis != "forward";
    if (grip) *grip = g_two.grip;
    return true;
}

bool CombatTwoHandGrip(int hand, bool* holding) {
    std::lock_guard<std::mutex> g(g_mtx);
    const bool mine = g_two.pinHand == hand;
    if (holding) *holding = mine && g_two.want;
    return mine && (g_two.want || g_two.inZone);
}

float CombatHandWeight(int hand) { return hand == HandLeft || hand == HandRight ? g_wepWeight[hand].load() : 0.f; }
int CombatTwoHandWeaponHand() { const int style = g_armStyleShared.load(); return TwoHandedStyle(style) ? TwoHandedWeaponHand(style) : -1; }
int CombatPlayerStamina(int* maxOut) {
    uint8_t* player = GameERMainPlayer();
    if (!player) return -1;
    return ReadStamina(player, maxOut);
}
bool CombatSetPlayerStamina(int value) {
    uint8_t* player = GameERMainPlayer();
    return player && WriteStamina(player, value);
}
int CombatPlayerHp(int* maxOut) {
    uint8_t* player = GameERMainPlayer();
    return player ? ChrHp(player, maxOut) : -1;
}
bool CombatSetPlayerHp(int value) {
    uint8_t* player = GameERMainPlayer(); if (!player) return false;
    uint8_t* mods = Deref(player + Chr_Modules); uint8_t* data = mods ? Deref(mods + Mod_Data) : nullptr;
    if (!data) return false;
    int maxHp = -1; SafeRead(data + Data_MaxHp, &maxHp, 4);
    if (maxHp > 0 && value > maxHp) value = maxHp;
    if (value < 1) value = 1;
    return SafeWrite(data + Data_Hp, &value, 4);
}
bool CombatLeftIsShield() { return g_wep[HandLeft].valid && g_wep[HandLeft].isShield && g_armStyle != 3; }
float CombatHandReach(int hand) {
    if (hand != HandLeft && hand != HandRight) return 0.f;
    const float r = g_wepReach[hand].load();
    return r > 0.05f ? r : (hand == HandRight ? Cfg().weaponReachRight : Cfg().weaponReachLeft);
}
float CombatHandCom(int hand) {
    if (hand != HandLeft && hand != HandRight) return 0.f;
    const float m = g_wepCom[hand].load();
    return m > 0.01f ? m : 0.45f * CombatHandReach(hand);
}

void CombatSetMenuOpen(bool open) {
    const int v = open ? 1 : 0;
    if (g_menuOpen.exchange(v) != v && g_status.taeHooked)
        LOGD("[combat] menu %s: physical combat %s", open ? "open" : "closed", open ? "standing down" : "re-armed");
}
bool CombatMenuActive() { return g_menuOpen.load() != 0; }
bool CombatMenuOpen() { return Cfg().combatMenuGate && CombatMenuActive(); }

uint16_t CombatHeldButtons() {
    const Config& c = Cfg();
    if (!c.blockEnabled || !g_leftShield.load() || !CombatHandlesSwings()) return 0;
    bool raised; { std::lock_guard<std::mutex> g(g_mtx); raised = g_block.raised; }
    return raised ? ParseXInputButton(c.guardButton) : 0;
}

void CombatPollHands(const VRTracking& tr, const VRInputState& in, double now) {
    const Config& c = Cfg();
    const bool continuous = c.kineticEnabled && c.kineticContinuous;
    if (!CombatHandlesSwings()) {
        for (int h = 0; h < 2; ++h) {
            EndSwing(h, true);
            const uint32_t next = g_sm[h].nextId;
            g_sm[h] = HandSM{}; g_sm[h].nextId = next;
        }
        std::lock_guard<std::mutex> g(g_mtx); g_block = BlockState{}; g_two = TwoHandState{};
        return;
    }
    PollBlock(tr, now);
    PollTwoHand(tr, in, now);
    PollSkillTrigger(in, now);
    const bool leftShield = g_leftShield.load() != 0;
    const bool riding = GameERRiding();
    // Two-handed: the free hand is on the weapon, it never swings on its own (the game's arm style
    // is the authority; a grab in progress already holds that hand still).
    const int style = g_armStyleShared.load();
    int freeHand = TwoHandedStyle(style) ? 1 - TwoHandedWeaponHand(style) : -1;
    { std::lock_guard<std::mutex> g(g_mtx); if (g_two.want) freeHand = g_two.pinHand; }
    for (int h = 0; h < 2; ++h) {
        HandSM& s = g_sm[h];
        const Pose& p = tr.hand[h];
        bool grip = in.handTrigger[h] >= c.gripThreshold;   // analog in VRInputState; VRB_*Grip is never set by the backend
        // A hand choosing on the weapon wheel is not swinging, however fast it moves onto a weapon, and neither is
        // one carrying an item from the belt.
        bool allowed = p.valid && in.controllerConnected[h] && h != freeHand && (h == HandRight || (c.combatLeftHand && !leftShield)) &&
                       (!c.gestureRequiresGrip || grip) && !WeaponWheelOwnsHand(h) && !BeltOwnsHand(h);
        if (!allowed) {
            // Also cancel an undelivered short contact while already cooling
            // down; tracking loss or taking the support grip must never replay it.
            EndSwing(h, true);
            const uint32_t next = s.nextId;
            s = HandSM{}; s.nextId = next;
            continue;
        }
        const float reach = CombatHandReach(h);
        const float onSpeed = continuous ? c.contactOnSpeed : c.swingOnSpeed;
        const float offSpeed = continuous ? c.contactOffSpeed : c.swingOffSpeed;
        float handSpd = 0.f;
        float tip = allowed ? TipSpeed(p, reach, &handSpd) : 0.f;
        const float weight = g_wepWeight[h].load();
        const float rawTip = tip;   // the controller's own tip speed (the inertial weapon's replaces it below)
        // With the inertial arm (game_er) the WEAPON the game renders and sweeps lags the
        // controller by its weight; its motion is the truth for speed and arc, so damage follows
        // what you see hit. The worker-side virtual-speed model is the fallback without it.
        float wmTip = 0.f, wmHand = 0.f, wmArc = 0.f;
        const bool inertial = allowed && GameERWeaponMotion(h, &wmTip, &wmHand, &wmArc);
        if (inertial) { tip = wmTip; handSpd = wmHand; s.arc = wmArc; s.tipValid = false; }
        else if (p.valid) {
            // Arc: tip path length since the tip last came to rest (captures the windup before the
            // on-threshold).
            Vec3 tipPos = p.pos * c.handScale + p.rot.Rotate(WeaponAxisVR()) * reach;
            if (s.tipValid && tip >= offSpeed) s.arc += (tipPos - s.lastTip).Length();
            else if (tip < offSpeed) s.arc = 0.f;
            s.lastTip = tipPos; s.tipValid = true;
        } else { s.tipValid = false; s.arc = 0.f; }
        s.vSmooth = s.vSmooth * 0.6f + tip * 0.4f;
        {
            float dt = s.lastPoll > 0 ? (float)(now - s.lastPoll) : 0.f; s.lastPoll = now;
            if (dt < 0.f) dt = 0.f; if (dt > 0.05f) dt = 0.05f;
            s.vVirt = inertial ? s.vSmooth : StepVirtualSpeed(s.vVirt, tip, weight, dt, c);
            if (s.st == HandSM::Idle) s.vVirtPeak = s.vVirt; else if (s.vVirt > s.vVirtPeak) s.vVirtPeak = s.vVirt;
        }
        bool fail = false, pending = false;
        {
            std::lock_guard<std::mutex> g(g_mtx);
            if (g_cmd[h].staminaFail) { g_cmd[h].staminaFail = false; fail = true; }
            pending = g_cmd[h].active && g_cmd[h].first;
        }
        if (fail && s.st == HandSM::Swinging) { s.st = HandSM::Cooldown; s.t = now; }
        if (s.rearmMin > 0.f && (s.st == HandSM::Cooldown || s.st == HandSM::Idle)) s.rearmMin = std::min(s.rearmMin, std::max(tip, 0.01f));
        switch (s.st) {
        case HandSM::Idle:
            if (pending || !WeaponContactStarts(allowed, continuous, tip, handSpd, onSpeed, c.minHandSpeed) || !RearmReady(tip, s.rearmMin)) break;
            s.st = HandSM::Armed; s.t = now; s.peak = tip; s.rearmMin = 0.f;
            [[fallthrough]];
        case HandSM::Armed: {
            if (tip > s.peak) s.peak = tip;
            bool decel = tip < s.prevTip && tip < s.peak * 0.85f;
            if (!allowed || tip < offSpeed) { s.st = HandSM::Idle; break; }
            if (WeaponContactReady(continuous, inertial, c.kineticEnabled, now - s.t,
                                   c.swingArmWindow, weight, c.kineticArmPerWeight, decel)) {
                const float vq = c.kineticEnabled ? s.vVirtPeak : s.peak;
                const float q = StrikeQuality(vq, s.arc, weight, reach, c);
                int slot;
                if (h == HandLeft && !TwoHandedStyle(style)) slot = SlotLeft;
                else if (c.kineticEnabled) slot = (q >= c.kineticHeavyQuality && s.arc >= 0.8f * ArcRefFor(weight, c)) ? SlotHeavy : SlotLight;
                else slot = s.peak >= c.heavySwingSpeed ? SlotHeavy : SlotLight;
                // On Torrent with RideAttacks=buttons the swing is the game's own mounted attack: the matching
                // button is pressed (never learned from - mounted rows are not the on-foot ones) and nothing is
                // injected. The arms stay on the controllers either way.
                if (riding && c.rideAttacks == "buttons") {
                    if (g_chordReq.load() == 0) g_chordReq.store(slot == SlotLeft ? 8 : slot == SlotHeavy ? 7 : 6);
                    g_haptic.fetch_or(1u << h);
                    s.st = HandSM::Cooldown; s.t = now; s.restSpeed = 0.f;
                    if (c.combatDebug) LOGD("[combat] %s swing on Torrent: pressing the mounted %s attack (quality %.2f)", HandName(h), SlotName(slot), q);
                    break;
                }
                // Two-handed rows not known yet (auto mode): this swing presses the button instead, so the
                // game plays the two-handed attack once and the hook learns its row (as 1H learning does).
                // Never from the saddle: that press is a mounted attack, and its row is not the 2H one.
                if (TwoHandedStyle(style) && c.combatLearn && c.combatMode == "auto" && slot != SlotLeft && !riding) {
                    bool known; { std::lock_guard<std::mutex> g(g_mtx); known = g_tpl[slot == SlotLight ? SlotLight2H : SlotHeavy2H].valid; }
                    if (!known) {
                        if (g_chordReq.load() == 0) { g_chordReq.store(slot == SlotLight ? 4 : 5); LOGI("[combat] two-handed %s row unknown: pressing %s so the game teaches it", SlotName(slot), slot == SlotLight ? c.lightAttackButton.c_str() : c.heavyAttackButton.c_str()); }
                        s.st = HandSM::Cooldown; s.t = now;
                        break;
                    }
                }
                StartSwing(h, slot, now, q, s.arc, vq); s.st = HandSM::Swinging; s.t = now; s.spentAt = 0;
                UpdateSwing(h, q, s.arc, vq, tip);
                g_haptic.fetch_or(1u << h);
                if (c.combatDebug) LOGD("[combat] %s swing start: %s (tip %.2f m/s, controller tip %.2f m/s, virtual %.2f m/s, hand %.2f m/s, arc %.2f/%.2f m, weight %.1f, reach %.2f m, quality %.2f)", HandName(h), SlotName(slot), s.peak, rawTip, s.vVirtPeak, handSpd, s.arc, ArcRefFor(weight, c), weight, reach, q);
            }
            break; }
        case HandSM::Swinging: {
            if (tip > s.peak) s.peak = tip;
            // Live quality: the virtual weapon's speed now, floored at 70% of its peak so a hit
            // landing late in the arc is not punished for the natural deceleration.
            float vEff = c.kineticEnabled ? (s.vVirt > 0.7f * s.vVirtPeak ? s.vVirt : 0.7f * s.vVirtPeak)
                                          : (s.vSmooth > 0.7f * s.peak ? s.vSmooth : 0.7f * s.peak);
            // A strike the game thread retired (swing budget spent) is an inert blade, and a continuous
            // one only ended once the tip came to rest. A long weapon's tip seldom drops to ContactOffSpeed
            // mid-fight, so every following swing was swallowed by the spent one (2026-10-31, 1.6 m
            // colossal weapon: "attacks stopped registering after some point"). End it at the tip's next
            // speed valley instead, or ContactSpentSeconds after it was spent; the next acceleration then
            // opens a new strike with its own budget.
            bool spent = false;
            if (continuous) { std::lock_guard<std::mutex> g(g_mtx); spent = !g_cmd[h].active; }
            if (spent && s.spentAt == 0) s.spentAt = now;
            const int spentEnd = SpentContactEnd(spent, tip, s.peak, offSpeed, c.contactValleyFrac, now - s.spentAt, c.contactSpentSeconds);
            const bool valleyEnd = spentEnd == 1, timeoutEnd = spentEnd == 2;
            const float valley = std::max(offSpeed, c.contactValleyFrac * s.peak);
            if (valleyEnd || timeoutEnd || !WeaponContactContinues(continuous, tip, offSpeed, now - s.t, c.swingMaxSeconds)) {
                if (c.combatDebug) LOGD("[combat] %s swing end after %.0f ms (peak %.2f m/s, virtual peak %.2f m/s, arc %.2f m)%s", HandName(h), (now - s.t) * 1000.0, s.peak, s.vVirtPeak, s.arc,
                                        valleyEnd ? " - spent strike, speed valley" : timeoutEnd ? " - spent strike, timeout" : "");
                EndSwing(h); s.st = HandSM::Cooldown; s.t = now;
                // Re-arm from the valley (or at once after a timeout) rather than from rest; after a
                // valley only once the tip speeds up again.
                s.restSpeed = valleyEnd ? valley : timeoutEnd ? 1e9f : 0.f;
                s.rearmMin = valleyEnd ? std::max(tip, 0.01f) : 0.f;
                if (valleyEnd) g_spentValleyEnds.fetch_add(1, std::memory_order_relaxed);
                if (timeoutEnd) g_spentTimeoutEnds.fetch_add(1, std::memory_order_relaxed);
            } else UpdateSwing(h, StrikeQuality(vEff, s.arc, weight, reach, c), s.arc, vEff, tip);
            break; }
        case HandSM::Cooldown: {
            const float rest = !continuous ? onSpeed : s.restSpeed > offSpeed ? s.restSpeed : offSpeed;
            if (tip < rest && (continuous || now - s.t >= c.swingCooldown)) { s.st = HandSM::Idle; s.restSpeed = 0.f; }
            break; }
        }
        s.prevTip = tip;
    }
}

void CombatGameFrame(uint8_t* player, uint32_t frame) {
    if (g_tripped || !g_status.taeHooked || !player) return;
    if (frame == g_lastFrame) return;
    g_lastFrame = frame;
    __try { GameFrameBody(player); } __except (SehFilter(GetExceptionCode())) { g_tripped = true; }
}

CombatStatus CombatGetStatus() {
    // Live, not the value last written by the in-world frame body: that one reads 0 whenever
    // the player is not spawned, which hid an armed RB/RT block behind "physical 0".
    const bool phys = CombatHandlesSwings();
    CombatStatus s = g_status;
    s.physical = phys;
    { std::lock_guard<std::mutex> g(g_mtx); s.learnedLight = g_tpl[SlotLight].valid; s.learnedHeavy = g_tpl[SlotHeavy].valid; s.learnedLeft = g_tpl[SlotLeft].valid; s.twoHandWanted = g_two.want; }
    s.armStyle = g_armStyleShared.load();
    s.injected = InterlockedExchange(&g_injected, 0); s.swings = InterlockedExchange(&g_swings, 0);
    return s;
}
}
