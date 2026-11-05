#include "enemy_contact.h"

#include "chr_tracker.h"
#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "hit_judge.h"
#include "log.h"
#include "pose_override.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace kinetic {

namespace {

using ervr::Quat;
using ervr::Vec3;

// One enemy arm's swing away from its animation: a rotation vector about the shoulder (world) and its rate.
struct Arm {
    uint8_t* chr = nullptr;
    int side = 0;
    int npcId = 0;
    Vec3 theta{}, omega{};
    Vec3 shoulder{};          // last frame's shoulder, for impulses that arrive before the next one
    Vec3 angImpulse{};        // accumulated since the last frame (N*m*s)
    Vec3 axisDir{};           // shoulder to elbow (no twist about it)
    // the physics weapon (right arm): where your blade holds it, and when that was last reported
    Vec3 simTip{}; float simDev = 0.f; double simAt = -10.0;
    bool seen = false;
};

std::mutex g_mtx;
std::vector<Arm> g_arms;
std::vector<ContactPart> g_parts;
double g_last = 0;
// statistics for the 5 s log
double g_logAt = 0;
int g_hits = 0;
float g_maxImpulse = 0, g_maxSwingDeg = 0;
int g_maxSwingNpc = 0, g_maxSwingSide = 0;
int g_held = 0;   // frames an enemy weapon was held off by your blade

uint32_t PartKey(uint8_t* chr, int side, int kind) {
    const uint64_t a = (uint64_t)(uintptr_t)chr;
    return (uint32_t)(((a >> 4) ^ (a >> 32)) * 16u) + (uint32_t)(side * 8 + kind);
}

Vec3 AxisFrom(const std::string& s) {
    const bool neg = !s.empty() && s[0] == '-';
    const char c = s.empty() ? 'x' : (char)std::tolower((unsigned char)s[neg ? 1 : 0]);
    Vec3 v = c == 'y' ? Vec3{0, 1, 0} : c == 'z' ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    return neg ? -v : v;
}

Quat FromRotVec(const Vec3& r) {
    const float a = r.Length();
    if (a < 1e-6f) return Quat{};
    return Quat::AxisAngle(r * (1.f / a), a);
}

double NowSecLocal() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

ContactPart MakePart(uint32_t key, uint8_t* chr, int side, int kind, const Vec3& a, const Vec3& b, float r) {
    ContactPart p{};
    p.key = key; p.chr = chr; p.side = side; p.kind = kind; p.a = a; p.b = b; p.radius = r; p.da = a; p.db = b;
    return p;
}

Arm* FindArm(uint8_t* chr, int side) {
    for (auto& a : g_arms) if (a.chr == chr && a.side == side) return &a;
    return nullptr;
}

// Model-space rotation and position of every bone, from the animation's local pose (our own overrides taken out).
bool ModelPose(uint8_t* chr, uint8_t* hkb, int n, const std::vector<int16_t>& par, std::vector<Quat>& mq, std::vector<Vec3>& mp) {
    float* pose = ervr::GameERHkbLivePose(hkb);
    if (!pose || n <= 0 || n > 1024 || (int)par.size() != n) return false;
    std::vector<float> loc((size_t)n * 12);
    if (!ervr::SafeRead(pose, loc.data(), loc.size() * sizeof(float))) return false;
    mq.resize(n); mp.resize(n);
    for (int i = 0; i < n; ++i) {
        float* l = &loc[(size_t)i * 12];
        float anim[4];
        if (PoseOverrideAnimQ(chr, pose, i, anim)) for (int k = 0; k < 4; ++k) l[4 + k] = anim[k];
        const Quat q = Quat{l[4], l[5], l[6], l[7]}.Normalized();
        const Vec3 t{l[0], l[1], l[2]};
        const int p = par[i];
        if (p < 0 || p >= i) { mq[i] = q; mp[i] = t; }
        else { mq[i] = (mq[p] * q).Normalized(); mp[i] = mp[p] + mq[p].Rotate(t); }
    }
    return true;
}

}  // namespace

void ContactReset() {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_arms.clear(); g_parts.clear(); g_last = 0;
}

int ContactParts(ContactPart* out, int max) {
    std::lock_guard<std::mutex> lock(g_mtx);
    const int n = std::min((int)g_parts.size(), max);
    for (int i = 0; i < n; ++i) out[i] = g_parts[i];
    return n;
}

void ContactImpulse(uint32_t key, const Vec3& point, const Vec3& impulse) {
    std::lock_guard<std::mutex> lock(g_mtx);
    for (const auto& p : g_parts) {
        if (p.key != key) continue;
        if (p.kind == 2) return;   // the weapon is a physics body of its own (ContactWeaponSim)
        Arm* a = FindArm(p.chr, p.side);
        if (!a) return;
        Vec3 L = (point - a->shoulder).Cross(impulse);
        // An arm does not spin about its own length when pushed: keep only the swing.
        L = L - a->axisDir * L.Dot(a->axisDir);
        a->angImpulse = a->angImpulse + L;
        ++g_hits;
        g_maxImpulse = std::max(g_maxImpulse, impulse.Length());
        return;
    }
}

void ContactWeaponSim(uint32_t key, const Vec3& grip, const Vec3& tip, float deviation) {
    (void)grip;
    std::lock_guard<std::mutex> lock(g_mtx);
    for (const auto& p : g_parts) {
        if (p.key != key || p.kind != 2) continue;
        Arm* a = FindArm(p.chr, p.side);
        if (!a) return;
        const double now = NowSecLocal();
        // Both hands' scenes may hold the same weapon: within one frame the larger deviation wins.
        if (now - a->simAt < 0.004 && deviation < a->simDev) return;
        a->simTip = tip; a->simDev = deviation; a->simAt = now;
        return;
    }
}

void ContactFrame(uint8_t* player, double now) {
    const auto& c = ervr::Cfg();
    std::lock_guard<std::mutex> lock(g_mtx);
    g_parts.clear();
    if (!c.physicsContact || !player) { g_arms.clear(); return; }
    const float dt = g_last > 0 ? (float)std::min(std::max(now - g_last, 0.0), 0.05) : 0.f;
    g_last = now;

    const float inertia = std::max(c.physicsEnemyArmInertia, 0.02f);
    const float k = std::max(c.physicsEnemyArmStiffness, 0.f);
    const float damp = 2.f * std::max(c.physicsEnemyArmDamping, 0.f) * std::sqrt(k * inertia);
    const float maxRad = std::min(std::max(c.physicsEnemyArmMaxDeg, 0.f), 150.f) * 0.0174533f;
    Vec3 weaponAxis = AxisFrom(c.physicsEnemyWeaponAxis);
    if (c.physicsEnemyWeaponAxis.empty() || std::tolower((unsigned char)c.physicsEnemyWeaponAxis[0]) == 'a') {
        // auto: the axis the hit judge has learned from the blows you took (+x until it knows).
        static const Vec3 kAxes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const int learned = HitJudgeWeaponAxis();
        weaponAxis = learned >= 0 && learned < 6 ? kAxes[learned] : kAxes[0];
    }

    for (auto& a : g_arms) a.seen = false;
    std::vector<Quat> mq; std::vector<Vec3> mp;
    int enemies = 0;
    for (const TrackedChr& t : TrackerList()) {
        if (!t.hkb || t.distance > c.physicsContactRadius || t.hp == 0) continue;
        if (enemies >= 4) break;
        const auto* par = TrackerBoneParents(t.hkb);
        if (!par) continue;
        const int ua[2] = {TrackerBoneIndex(t.hkb, "L_UpperArm"), TrackerBoneIndex(t.hkb, "R_UpperArm")};
        const int fa[2] = {TrackerBoneIndex(t.hkb, "L_Forearm"), TrackerBoneIndex(t.hkb, "R_Forearm")};
        const int hd[2] = {TrackerBoneIndex(t.hkb, "L_Hand"), TrackerBoneIndex(t.hkb, "R_Hand")};
        const int wb = c.physicsEnemyWeaponBone.empty() ? -1 : TrackerBoneIndex(t.hkb, c.physicsEnemyWeaponBone);
        if (ua[0] < 0 && ua[1] < 0) continue;   // no humanoid arms
        Vec3 rootP; Quat rootQ;
        if (!ervr::GameERChrModelRoot(t.chr, &rootP, &rootQ)) continue;
        if (!ModelPose(t.chr, t.hkb, (int)par->size(), *par, mq, mp)) continue;
        ++enemies;
        auto world = [&](int b) { return rootP + rootQ.Rotate(mp[b]); };
        for (int s = 0; s < 2; ++s) {
            if (ua[s] < 0 || fa[s] < 0 || hd[s] < 0) continue;
            Arm* a = FindArm(t.chr, s);
            if (!a) { g_arms.push_back(Arm{}); a = &g_arms.back(); a->chr = t.chr; a->side = s; }
            a->seen = true; a->npcId = t.npcId;
            const Vec3 S = world(ua[s]);
            a->shoulder = S;
            a->axisDir = (world(fa[s]) - S).Normalized();
            // The weapon as the animation wants it.
            const bool hasWeapon = s == 1 && wb >= 0 && c.physicsEnemyWeaponLength > 0.05f;
            Vec3 W0{}, T0{}; Quat wq0{};
            if (hasWeapon) {
                W0 = world(wb);
                wq0 = (rootQ * mq[wb]).Normalized();
                T0 = W0 + wq0.Rotate(weaponAxis) * c.physicsEnemyWeaponLength;
            }
            const bool held = hasWeapon && now - a->simAt < 0.1;
            if (held) {
                // Your blade holds the weapon off its path: the arm turns about the shoulder to carry it there.
                const Vec3 u = (T0 - S).Normalized(), v = (a->simTip - S).Normalized();
                Quat q = Quat::FromTo(u, v).Normalized();
                if (q.w < 0.f) q = Quat{-q.x, -q.y, -q.z, -q.w};
                const float ang = 2.f * std::acos(std::min(q.w, 1.f));
                Vec3 rv{};
                if (ang > 1e-4f) { const Vec3 ax = Vec3{q.x, q.y, q.z}.Normalized(); rv = ax * std::min(ang, maxRad); }
                a->omega = dt > 0.f ? (rv - a->theta) * (1.f / dt) : Vec3{};
                a->omega = a->omega * 0.5f;   // released, it comes back with some of that speed
                a->theta = rv;
                a->angImpulse = Vec3{};
            } else if (dt > 0.f) {
                // Swing: the impulses of the last frame, then a damped spring back into the animation.
                a->omega = a->omega + a->angImpulse * (1.f / inertia);
                a->omega = a->omega + (a->theta * -k - a->omega * damp) * (dt / inertia);
                a->theta = a->theta + a->omega * dt;
                const float ang = a->theta.Length();
                if (ang > maxRad && ang > 1e-6f) {
                    const Vec3 dir = a->theta * (1.f / ang);
                    a->theta = dir * maxRad;
                    const float out = a->omega.Dot(dir);
                    if (out > 0.f) a->omega = a->omega - dir * out;
                }
            }
            a->angImpulse = Vec3{};
            const float swingDeg = a->theta.Length() * 57.2958f;
            if (swingDeg > g_maxSwingDeg) { g_maxSwingDeg = swingDeg; g_maxSwingNpc = t.npcId; g_maxSwingSide = s; }
            const Quat R = FromRotVec(a->theta);
            auto swung = [&](const Vec3& p) { return S + R.Rotate(p - S); };
            const Vec3 E = swung(world(fa[s])), H = swung(world(hd[s]));
            g_parts.push_back(MakePart(PartKey(t.chr, s, 0), t.chr, s, 0, S, E, 0.08f));
            g_parts.push_back(MakePart(PartKey(t.chr, s, 1), t.chr, s, 1, E, H, 0.065f));
            if (hasWeapon) {
                ContactPart wp = MakePart(PartKey(t.chr, s, 2), t.chr, s, 2, W0, T0, 0.035f);
                wp.length = c.physicsEnemyWeaponLength;
                wp.da = swung(W0); wp.db = held ? a->simTip : swung(T0);
                const Quat wq = (R * wq0).Normalized();
                wp.axis[0] = wq.Rotate(Vec3{1, 0, 0}); wp.axis[1] = wq.Rotate(Vec3{0, 1, 0}); wp.axis[2] = wq.Rotate(Vec3{0, 0, 1});
                g_parts.push_back(wp);
                if (held) ++g_held;
            }
            // Draw the swing: rotate the upper arm in model space about its own origin (the shoulder).
            if (a->theta.Length() > 0.0035f) {
                const int p = (*par)[ua[s]];
                const Quat Mp = p >= 0 ? mq[p] : Quat{};
                const Quat Rm = (rootQ.Conj() * R * rootQ).Normalized();
                PoseOverrideAdd(t.chr, t.hkb, ua[s], (Mp.Conj() * Rm * Mp).Normalized(), Quat{});
            }
        }
        // The body: torso, head, thighs and calves. These do not swing; the blade cuts, sticks or glances on them.
        if (c.physicsBodyContact) {
            const int pel = TrackerBoneIndex(t.hkb, "Pelvis"), neck = TrackerBoneIndex(t.hkb, "Neck"), head = TrackerBoneIndex(t.hkb, "Head");
            if (pel >= 0 && neck >= 0) g_parts.push_back(MakePart(PartKey(t.chr, 0, 3), t.chr, 0, 3, world(pel), world(neck), 0.21f));
            if (head >= 0) {
                const Vec3 hp = world(head), up = rootQ.Rotate(Vec3{0, 1, 0});
                g_parts.push_back(MakePart(PartKey(t.chr, 0, 4), t.chr, 0, 4, hp, hp + up * 0.12f, 0.13f));
            }
            static const char* thighN[2] = {"L_Thigh", "R_Thigh"}, *calfN[2] = {"L_Calf", "R_Calf"}, *footN[2] = {"L_Foot", "R_Foot"};
            for (int s = 0; s < 2; ++s) {
                const int th = TrackerBoneIndex(t.hkb, thighN[s]), ca = TrackerBoneIndex(t.hkb, calfN[s]), fo = TrackerBoneIndex(t.hkb, footN[s]);
                if (th >= 0 && ca >= 0) g_parts.push_back(MakePart(PartKey(t.chr, s, 5), t.chr, s, 5, world(th), world(ca), 0.10f));
                if (ca >= 0 && fo >= 0) g_parts.push_back(MakePart(PartKey(t.chr, s, 6), t.chr, s, 6, world(ca), world(fo), 0.075f));
            }
        }
    }
    g_arms.erase(std::remove_if(g_arms.begin(), g_arms.end(), [](const Arm& a) { return !a.seen; }), g_arms.end());

    if (now - g_logAt >= 5.0) {
        if (c.physicsContactLog && (enemies > 0 || g_hits > 0))
            LOGI("[phys] contact: %d enemy(ies), %d capsule(s); blade contacts %d (largest impulse %.1f N*s); largest arm swing %.0f deg (npc %d %s arm); weapon held off by your blade %d frame(s)",
                 enemies, (int)g_parts.size(), g_hits, g_maxImpulse, g_maxSwingDeg, g_maxSwingNpc, g_maxSwingSide ? "right" : "left", g_held);
        g_logAt = now; g_hits = 0; g_held = 0; g_maxImpulse = 0; g_maxSwingDeg = 0;
    }
}

}  // namespace kinetic
