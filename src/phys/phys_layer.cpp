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
    }
    TrackerReset();
    g_testHkb = nullptr; g_haveWritten = false;
}

ervr::Vec3 TestAxis() {
    const std::string& a = ervr::Cfg().physicsTestAxis;
    const char c = a.empty() ? 'x' : (char)std::tolower((unsigned char)a[0]);
    return c == 'y' ? ervr::Vec3{0, 1, 0} : c == 'z' ? ervr::Vec3{0, 0, 1} : ervr::Vec3{1, 0, 0};
}

// M1.3: rotate one bone of the nearest tracked character by a fixed angle about a local axis, on top of
// whatever the animation put there this frame. Both stereo passes, so both eyes show it.
void TestBoneOverride() {
    const auto& c = ervr::Cfg();
    if (c.physicsTestBone.empty()) { g_testHkb = nullptr; g_haveWritten = false; return; }
    const auto& list = TrackerList();
    const TrackedChr* target = nullptr;
    for (const auto& t : list) if (t.hkb && t.distance <= c.physicsTestRadius) { target = &t; break; }
    if (!target) { g_testHkb = nullptr; g_haveWritten = false; return; }

    const int bone = TrackerBoneIndex(target->hkb, c.physicsTestBone);
    if (bone < 0 || bone >= target->boneCount) {
        const double now = NowSec();
        if (now - g_lastTestLog > 5.0) { g_lastTestLog = now; LOGW("[phys] test: npc %d has no bone '%s'", target->npcId, c.physicsTestBone.c_str()); }
        return;
    }
    if (target->hkb != g_testHkb || bone != g_testBone) {
        g_testHkb = target->hkb; g_testBone = bone; g_haveWritten = false;
        LOGI("[phys] test: overriding '%s' (#%d) of npc %d at %.1f m", c.physicsTestBone.c_str(), bone, target->npcId, target->distance);
    }
    float* pose = ervr::GameERHkbLivePose(target->hkb);
    if (!pose) return;
    float* q = pose + bone * 12 + 4;   // hkQsTransform {t[4], q[4], s[4]}
    float cur[4];
    if (!ervr::SafeRead(q, cur, sizeof(cur))) return;
    // Our own write still in the buffer = the animation has not run since: rotate its pose, not ours.
    const bool ours = g_haveWritten && cur[0] == g_lastWritten[0] && cur[1] == g_lastWritten[1] &&
                      cur[2] == g_lastWritten[2] && cur[3] == g_lastWritten[3];
    if (!ours) for (int i = 0; i < 4; ++i) g_base[i] = cur[i];
    const ervr::Quat base{g_base[0], g_base[1], g_base[2], g_base[3]};
    const ervr::Quat out = (base * ervr::Quat::AxisAngle(TestAxis(), c.physicsTestDeg * 3.14159265f / 180.f)).Normalized();
    const float w[4] = {out.x, out.y, out.z, out.w};
    if (ervr::SafeWrite(q, w, sizeof(w))) {
        for (int i = 0; i < 4; ++i) g_lastWritten[i] = w[i];
        g_haveWritten = true;
    }
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

}  // namespace kinetic
