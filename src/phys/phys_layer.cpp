#include "phys_layer.h"

#include "chr_tracker.h"
#include "config.h"
#include "enemy_contact.h"
#include "game_er.h"
#include "hit_judge.h"
#include "game_memory.h"
#include "log.h"
#include "pose_override.h"
#include "sword_rig.h"
#include "vrmath.h"

#include <windows.h>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>

namespace kinetic {

namespace {
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_tripped{false};
bool g_initialized = false;
double g_lastTrack = 0;

// S1 test target (PoseOverride does the writing).
uint8_t* g_testChr = nullptr;
uint8_t* g_testHkb = nullptr;
int g_testBone = -1;
double g_lastTestLog = 0;

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void Apply(const char* why) {
    const bool want = ervr::Cfg().physicsEnabled && !g_tripped.load();
    if (g_initialized && want == g_enabled.load()) return;
    g_enabled = want;
    LOGI("[phys] Kinetic physics: %s (%s)", want ? "on" : "off", why);
    if (want) {
        const auto& c = ervr::Cfg();
        LOGI("[phys] M1 spike: tracking within %.1f m, bone dump %s, test bone '%s' axis %s %.0f deg within %.1f m",
             c.physicsTrackRadius, c.physicsDumpBones ? "on" : "off", c.physicsTestBone.c_str(),
             c.physicsTestAxis.c_str(), c.physicsTestDeg, c.physicsTestRadius);
        LOGI("[phys] Kinetic build %s: B&S-style sword %s, blade contact with enemies %s", ERVR_VERSION,
             !KINETIC_HAS_PHYSX ? "NOT built in (no PhysX in this build)" : c.physicsSword ? "on" : "off (Sword=false)",
             c.physicsContact ? "on" : "off");
        if (c.physicsContact)
            LOGI("[phys] contact: enemies within %.1f m; enemy arm inertia %.2f kg*m^2, stiffness %.0f N*m/rad, damping %.2f, swing max %.0f deg; weapon bone '%s' axis %s length %.2f m",
                 c.physicsContactRadius, c.physicsEnemyArmInertia, c.physicsEnemyArmStiffness, c.physicsEnemyArmDamping,
                 c.physicsEnemyArmMaxDeg, c.physicsEnemyWeaponBone.c_str(), c.physicsEnemyWeaponAxis.c_str(), c.physicsEnemyWeaponLength);
    }
    TrackerReset();
    ContactReset();
    HitJudgeReset();
    PoseOverrideClear();
    g_testChr = nullptr; g_testHkb = nullptr; g_testBone = -1;
}

ervr::Vec3 TestAxis() {
    const std::string& a = ervr::Cfg().physicsTestAxis;
    const char c = a.empty() ? 'z' : (char)std::tolower((unsigned char)a[0]);
    return c == 'y' ? ervr::Vec3{0, 1, 0} : c == 'x' ? ervr::Vec3{1, 0, 0} : ervr::Vec3{0, 0, 1};
}

// S1: rotate one bone of the nearest tracked character by a fixed angle about its local axis, on top of its
// animation. Play test 4 (z axis) showed it: the soldier's arm swung back while idle.
void DeclareTestBone() {
    const auto& c = ervr::Cfg();
    if (c.physicsTestBone.empty()) { g_testChr = nullptr; return; }
    const auto& list = TrackerList();
    const TrackedChr* target = nullptr;
    for (const auto& t : list) if (t.hkb && t.distance <= c.physicsTestRadius) { target = &t; break; }
    // A little hysteresis so a target walking along the radius does not flicker in and out.
    if (!target && g_testChr)
        for (const auto& t : list) if (t.chr == g_testChr && t.hkb && t.distance <= c.physicsTestRadius + 1.f) { target = &t; break; }
    if (!target) { g_testChr = nullptr; return; }
    const int bone = TrackerBoneIndex(target->hkb, c.physicsTestBone);
    const double now = NowSec();
    if (bone < 0 || bone >= target->boneCount) {
        g_testChr = nullptr;
        if (now - g_lastTestLog > 5.0) { g_lastTestLog = now; LOGW("[phys] test: npc %d has no bone '%s'", target->npcId, c.physicsTestBone.c_str()); }
        return;
    }
    if (target->hkb != g_testHkb || bone != g_testBone || target->chr != g_testChr) {
        g_testChr = target->chr; g_testHkb = target->hkb; g_testBone = bone;
        LOGI("[phys] test: overriding '%s' (#%d) of npc %d at %.1f m", c.physicsTestBone.c_str(), bone, target->npcId, target->distance);
    }
    PoseOverrideAdd(target->chr, target->hkb, bone, ervr::Quat{},
                    ervr::Quat::AxisAngle(TestAxis(), c.physicsTestDeg * 3.14159265f / 180.f));
}
}  // namespace

void PhysInit() {
    Apply("startup");
    g_initialized = true;
}

void PhysConfigChanged() {
    if (!g_initialized) return;
    Apply("ini reload");
}

bool PhysEnabled() { return g_enabled.load(); }

void PhysTrip() {
    g_tripped = true;
    g_enabled = false;
}

void PhysGameFrame(uint8_t* player, int pass) {
    if (!g_enabled.load() || !player) return;
    // The pose hook runs several times per rendered frame; the character list and the overrides only need one
    // rebuild per simulation frame, and never from the second eye's re-run. Writing happens on every call.
    const double now = NowSec();
    if (pass != 2 && now - g_lastTrack > 1.0 / 90.0) {
        g_lastTrack = now;
        TrackerUpdate(player, ervr::Cfg().physicsTrackRadius);
        PoseOverrideBegin();
        DeclareTestBone();
        ContactFrame(player, now);
        PoseOverrideEnd();
        HitJudgeFrame(player, now);
    }
    PoseOverrideWrite(0, nullptr);
    PoseOverrideLog();
    SwordRigPinStamina();
}

void PhysBeforeModelPos(void* ctrl) {
    if (!g_enabled.load() || !ctrl) return;
    PoseOverrideWrite(1, ctrl);
    SwordRigPinStamina();
}

void PhysAfterModelPos(void* ctrl) {
    if (!g_enabled.load() || !ctrl) return;
    PoseOverrideWrite(2, ctrl);
    SwordRigPinStamina();
}

}  // namespace kinetic
