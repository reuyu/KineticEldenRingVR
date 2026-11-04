#include "phys_layer.h"

#include "chr_tracker.h"
#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "log.h"
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

// S1 bone-override test state: what we wrote last, so repeated hook calls in one frame
// rotate the animation's pose once instead of piling rotations on top of our own write.
uint8_t* g_testHkb = nullptr;
int g_testBone = -1;
float g_lastWritten[4] = {};
float g_base[4] = {};
bool g_haveWritten = false;
double g_lastTestLog = 0;
std::mutex g_testMtx;
uint8_t* g_testChr = nullptr;
long g_modelPosWrites = 0, g_poseHookWrites = 0;

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
        LOGI("[phys] Kinetic build %s: B&S-style sword %s", ERVR_VERSION,
             !KINETIC_HAS_PHYSX ? "NOT built in (no PhysX in this build)" : c.physicsSword ? "on" : "off (Sword=false)");
    }
    TrackerReset();
    { std::lock_guard<std::mutex> lock(g_testMtx); g_testChr = nullptr; g_testHkb = nullptr; g_haveWritten = false; }
}

ervr::Vec3 TestAxis() {
    const std::string& a = ervr::Cfg().physicsTestAxis;
    const char c = a.empty() ? 'x' : (char)std::tolower((unsigned char)a[0]);
    return c == 'y' ? ervr::Vec3{0, 1, 0} : c == 'z' ? ervr::Vec3{0, 0, 1} : ervr::Vec3{1, 0, 0};
}

// M1.3: rotate one bone of the nearest tracked character by a fixed angle about a local axis, on top of
// whatever the animation put there this frame. Both stereo passes, so both eyes show it.
//
// First test (0.7.2): written from the player's pose hook only, the override was logged but never seen - an
// enemy's animation runs after that point. The write now also happens at the character's own model-pos update
// (Hook_UpdateChrModelPos), where ERVR's player arms are consumed by the renderer.

// Picks the target for this frame. Game thread, from the pose hook.
void SelectTestTarget() {
    const auto& c = ervr::Cfg();
    std::lock_guard<std::mutex> lock(g_testMtx);
    if (c.physicsTestBone.empty()) { g_testChr = nullptr; g_testHkb = nullptr; g_haveWritten = false; return; }
    const auto& list = TrackerList();
    const TrackedChr* target = nullptr;
    for (const auto& t : list) if (t.hkb && t.distance <= c.physicsTestRadius) { target = &t; break; }
    // A little hysteresis so a target walking along the radius does not flicker in and out.
    if (!target && g_testChr)
        for (const auto& t : list) if (t.chr == g_testChr && t.hkb && t.distance <= c.physicsTestRadius + 1.f) { target = &t; break; }
    if (!target) { g_testChr = nullptr; g_testHkb = nullptr; g_haveWritten = false; return; }

    const int bone = TrackerBoneIndex(target->hkb, c.physicsTestBone);
    if (bone < 0 || bone >= target->boneCount) {
        g_testChr = nullptr; g_testHkb = nullptr;
        const double now = NowSec();
        if (now - g_lastTestLog > 5.0) { g_lastTestLog = now; LOGW("[phys] test: npc %d has no bone '%s'", target->npcId, c.physicsTestBone.c_str()); }
        return;
    }
    if (target->hkb != g_testHkb || bone != g_testBone) {
        g_testChr = target->chr; g_testHkb = target->hkb; g_testBone = bone; g_haveWritten = false;
        LOGI("[phys] test: overriding '%s' (#%d) of npc %d at %.1f m", c.physicsTestBone.c_str(), bone, target->npcId, target->distance);
    }
    const double now = NowSec();
    if (now - g_lastTestLog > 5.0) {
        g_lastTestLog = now;
        LOGI("[phys] test: npc %d at %.1f m, writes in 5 s: %ld at its model-pos update, %ld from the pose hook",
             target->npcId, target->distance, g_modelPosWrites, g_poseHookWrites);
        g_modelPosWrites = g_poseHookWrites = 0;
    }
}

// Writes the rotated bone into the target's live pose. Caller holds g_testMtx.
bool WriteTestBone() {
    if (!g_testHkb || g_testBone < 0) return false;
    float* pose = ervr::GameERHkbLivePose(g_testHkb);
    if (!pose) return false;
    float* q = pose + g_testBone * 12 + 4;   // hkQsTransform {t[4], q[4], s[4]}
    float cur[4];
    if (!ervr::SafeRead(q, cur, sizeof(cur))) return false;
    // Our own write still in the buffer = the animation has not run since: rotate its pose, not ours.
    const bool ours = g_haveWritten && cur[0] == g_lastWritten[0] && cur[1] == g_lastWritten[1] &&
                      cur[2] == g_lastWritten[2] && cur[3] == g_lastWritten[3];
    if (!ours) for (int i = 0; i < 4; ++i) g_base[i] = cur[i];
    const ervr::Quat base{g_base[0], g_base[1], g_base[2], g_base[3]};
    const ervr::Quat out = (base * ervr::Quat::AxisAngle(TestAxis(), ervr::Cfg().physicsTestDeg * 3.14159265f / 180.f)).Normalized();
    const float w[4] = {out.x, out.y, out.z, out.w};
    if (!ervr::SafeWrite(q, w, sizeof(w))) return false;
    for (int i = 0; i < 4; ++i) g_lastWritten[i] = w[i];
    g_haveWritten = true;
    return true;
}

void TestBoneOverride() {
    SelectTestTarget();
    std::lock_guard<std::mutex> lock(g_testMtx);
    if (WriteTestBone()) ++g_poseHookWrites;
}

void ModelPosOverride(void* ctrl) {
    std::lock_guard<std::mutex> lock(g_testMtx);
    if (!g_testChr || !ctrl || ervr::GameERChrCtrl(g_testChr) != (uint8_t*)ctrl) return;
    if (WriteTestBone()) ++g_modelPosWrites;
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
    // The pose hook runs several times per rendered frame; the character list only needs one refresh per
    // simulation frame, and never from the second eye's re-run.
    const double now = NowSec();
    if (pass != 2 && now - g_lastTrack > 1.0 / 90.0) {
        g_lastTrack = now;
        TrackerUpdate(player, ervr::Cfg().physicsTrackRadius);
    }
    TestBoneOverride();
}

void PhysBeforeModelPos(void* ctrl) {
    if (!g_enabled.load() || !ctrl) return;
    ModelPosOverride(ctrl);
}

}  // namespace kinetic
