#include "pose_override.h"

#include "game_er.h"
#include "game_memory.h"
#include "log.h"

#include <windows.h>
#include <cmath>
#include <mutex>
#include <vector>

namespace kinetic {

namespace {

struct BoneState {
    int bone = -1;
    ervr::Quat pre{}, post{};
    // per pose buffer: the animation's value we built on and the value we wrote
    float base[2][4]{};
    float last[2][4]{};
    bool have[2]{};
};

struct ChrState {
    uint8_t* chr = nullptr;
    uint8_t* hkb = nullptr;
    std::vector<BoneState> bones;
    float* pose[2]{};
    int nPose = 0;
    bool live = false;                 // declared this frame
    // statistics per write point: the animation had run since our last write (fresh) or not (ours still there)
    long fresh[3]{}, kept[3]{}, writes[3]{};
};

std::mutex g_mtx;
std::vector<ChrState> g_chr;
double g_lastLog = 0;

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

bool Same4(const float* a, const float* b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

bool IsIdentity(const ervr::Quat& q) { return std::fabs(q.w) > 0.999999f; }

void WriteChr(ChrState& c, int point) {
    float* found[2] = {};
    const int n = ervr::GameERChrPoseBuffers(c.chr, found);
    for (int i = 0; i < 2; ++i) {
        float* want = i < n ? found[i] : nullptr;
        if (c.pose[i] != want) { c.pose[i] = want; for (auto& b : c.bones) b.have[i] = false; }
    }
    c.nPose = n;
    bool wrote = false;
    for (int i = 0; i < n; ++i) {
        if (!c.pose[i]) continue;
        for (auto& b : c.bones) {
            float* q = c.pose[i] + b.bone * 12 + 4;   // hkQsTransform {t[4], q[4], s[4]}
            float cur[4];
            if (!ervr::SafeRead(q, cur, sizeof(cur))) continue;
            const bool ours = b.have[i] && Same4(cur, b.last[i]);
            if (ours) ++c.kept[point]; else { ++c.fresh[point]; for (int k = 0; k < 4; ++k) b.base[i][k] = cur[k]; }
            if (IsIdentity(b.pre) && IsIdentity(b.post)) {   // nothing to add: leave the animation alone
                if (ours) ervr::SafeWrite(q, b.base[i], sizeof(cur));
                b.have[i] = false;
                continue;
            }
            const ervr::Quat base{b.base[i][0], b.base[i][1], b.base[i][2], b.base[i][3]};
            const ervr::Quat out = (b.pre * base * b.post).Normalized();
            const float w[4] = {out.x, out.y, out.z, out.w};
            if (!ervr::SafeWrite(q, w, sizeof(w))) continue;
            for (int k = 0; k < 4; ++k) b.last[i][k] = w[k];
            b.have[i] = true;
            wrote = true;
        }
    }
    if (wrote) ++c.writes[point];
}

}  // namespace

// Declarations are collected first and applied in End, so a model-pos write on another thread never sees a
// half-built frame.
struct Pending { uint8_t* chr; uint8_t* hkb; int bone; ervr::Quat pre, post; };
std::vector<Pending> g_pending;
std::mutex g_pendMtx;

void PoseOverrideBegin() {
    std::lock_guard<std::mutex> lock(g_pendMtx);
    g_pending.clear();
}

void PoseOverrideAdd(uint8_t* chr, uint8_t* hkb, int bone, const ervr::Quat& pre, const ervr::Quat& post) {
    if (!chr || !hkb || bone < 0 || bone > 1024) return;
    std::lock_guard<std::mutex> lock(g_pendMtx);
    for (auto& p : g_pending)
        if (p.chr == chr && p.bone == bone) { p.pre = (pre * p.pre).Normalized(); p.post = (p.post * post).Normalized(); return; }
    if (g_pending.size() < 256) g_pending.push_back({chr, hkb, bone, pre, post});
}

void PoseOverrideEnd() {
    std::vector<Pending> pend;
    { std::lock_guard<std::mutex> lock(g_pendMtx); pend.swap(g_pending); }
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& c : g_chr) { c.live = false; for (auto& b : c.bones) { b.pre = ervr::Quat{}; b.post = ervr::Quat{}; } }
    for (const auto& p : pend) {
        ChrState* c = nullptr;
        for (auto& s : g_chr) if (s.chr == p.chr) { c = &s; break; }
        if (c && c->hkb != p.hkb) { *c = ChrState{}; c->chr = p.chr; }   // same address, another character
        if (!c) { if (g_chr.size() >= 16) continue; g_chr.push_back(ChrState{}); c = &g_chr.back(); c->chr = p.chr; }
        c->hkb = p.hkb;
        c->live = true;
        BoneState* b = nullptr;
        for (auto& s : c->bones) if (s.bone == p.bone) { b = &s; break; }
        if (!b) { if (c->bones.size() >= 32) continue; c->bones.push_back(BoneState{}); b = &c->bones.back(); b->bone = p.bone; }
        b->pre = p.pre; b->post = p.post;
    }
    // A character nobody overrides any more gets its animation back once, then is forgotten.
    for (size_t i = 0; i < g_chr.size();) {
        ChrState& c = g_chr[i];
        if (c.live) { ++i; continue; }
        for (int p = 0; p < 2; ++p) {
            if (!c.pose[p]) continue;
            for (auto& b : c.bones) {
                if (!b.have[p]) continue;
                float* q = c.pose[p] + b.bone * 12 + 4;
                float cur[4];
                if (ervr::SafeRead(q, cur, sizeof(cur)) && Same4(cur, b.last[p])) ervr::SafeWrite(q, b.base[p], sizeof(cur));
            }
        }
        g_chr.erase(g_chr.begin() + i);
    }
}

void PoseOverrideClear() {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_chr.clear();
}

void PoseOverrideWrite(int point, void* ctrl) {
    if (point < 0 || point > 2) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& c : g_chr) {
        if (c.bones.empty()) continue;
        if (point > 0 && (!ctrl || ervr::GameERChrCtrl(c.chr) != (uint8_t*)ctrl)) continue;
        WriteChr(c, point);
    }
}

bool PoseOverrideAnimQ(uint8_t* chr, const float* pose, int bone, float q[4]) {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& c : g_chr) {
        if (c.chr != chr) continue;
        for (int i = 0; i < 2; ++i) {
            if (c.pose[i] != pose) continue;
            for (auto& b : c.bones) {
                if (b.bone != bone || !b.have[i]) continue;
                float cur[4];
                if (!ervr::SafeRead(pose + bone * 12 + 4, cur, sizeof(cur)) || !Same4(cur, b.last[i])) return false;
                for (int k = 0; k < 4; ++k) q[k] = b.base[i][k];
                return true;
            }
        }
    }
    return false;
}

void PoseOverrideLog() {
    const double now = NowSec();
    if (now - g_lastLog < 5.0) return;
    g_lastLog = now;
    std::lock_guard<std::mutex> lock(g_mtx);
    for (auto& c : g_chr) {
        // fresh at a point = the animation had rewritten the bone since our previous write. fresh at "after" = it
        // was rewritten INSIDE the game's model-pos update, after our "before" write (what attacks were suspected of).
        LOGI("[phys] override %p: %d bone(s), %d buffer(s); writes/fresh/kept: pose hook %ld/%ld/%ld, before model-pos %ld/%ld/%ld, after model-pos %ld/%ld/%ld",
             (void*)c.chr, (int)c.bones.size(), c.nPose, c.writes[0], c.fresh[0], c.kept[0], c.writes[1], c.fresh[1], c.kept[1],
             c.writes[2], c.fresh[2], c.kept[2]);
        for (int p = 0; p < 3; ++p) c.writes[p] = c.fresh[p] = c.kept[p] = 0;
    }
}

}  // namespace kinetic
