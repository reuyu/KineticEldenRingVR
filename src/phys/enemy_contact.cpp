#include "enemy_contact.h"

#include "chr_tracker.h"
#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "game_shapes.h"
#include "hit_judge.h"
#include "log.h"
#include "pose_override.h"

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <set>
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

// The upper body's sway away from its animation: a rotation vector about the pelvis, driven by blows on the
// torso and head and springing back. Drawn by turning the Spine bone; the arms, weapon and head go with it.
struct Torso {
    uint8_t* chr = nullptr;
    Vec3 theta{}, omega{}, pivot{}, angImpulse{};
    bool seen = false;
};

// The game's own body capsules of an enemy (game_shapes), read once and kept while it stays near.
struct Shapes {
    uint8_t* chr = nullptr;
    double srcAt = -10.0;
    uintptr_t src = 0;            // its ragdoll data: a change means the character was reloaded
    bool ok = false, checked = false, seen = false;
    std::string why;
    std::vector<GameCapsule> caps;
    std::vector<int> kind, side;  // ContactPart kind/side per capsule, from the bone it follows
};

// The weapon nodes of an enemy's model (game_shapes GameWeaponNodesRead), re-read every second.
struct Weapons {
    uint8_t* chr = nullptr;
    double at = -10.0;
    bool ok = false, seen = false;
    std::string why;
    std::vector<WeaponNode> nodes;
};

std::mutex g_mtx;
std::vector<Weapons> g_weapons;
std::set<int> g_weaponsLogged;
std::vector<Shapes> g_shapes;
std::set<int> g_shapesLogged;
std::vector<Arm> g_arms;
std::vector<Torso> g_torsos;
double g_bodyLogAt = 0;
std::vector<ContactPart> g_parts;
double g_last = 0;
// statistics for the 5 s log
double g_logAt = 0;
int g_hits = 0;
float g_maxImpulse = 0, g_maxSwingDeg = 0;
int g_maxSwingNpc = 0, g_maxSwingSide = 0;
int g_held = 0;   // frames an enemy weapon was held off by your blade
float g_maxSwayDeg = 0;

uint32_t PartKey(uint8_t* chr, int side, int kind) {
    const uint64_t a = (uint64_t)(uintptr_t)chr;
    return (uint32_t)(((a >> 4) ^ (a >> 32)) * 16u) + (uint32_t)(side * 8 + kind);
}

// A game body capsule's key: clear of PartKey's (side * 8 + kind < 16) for the same enemy.
uint32_t ShapeKey(uint8_t* chr, int i) {
    const uint64_t a = (uint64_t)(uintptr_t)chr;
    return (uint32_t)(((a >> 4) ^ (a >> 32)) * 16u) + 0x40000000u + (uint32_t)i * 0x10000u;
}

bool Under(const std::vector<int16_t>& par, int b, int anc) {
    for (int i = 0; b >= 0 && b < (int)par.size() && i < 256; ++i) {
        if (b == anc) return true;
        b = par[b];
    }
    return false;
}

Vec3 AxisFrom(const std::string& s) {
    const bool neg = !s.empty() && s[0] == '-';
    const char c = s.empty() ? 'x' : (char)std::tolower((unsigned char)s[neg ? 1 : 0]);
    Vec3 v = c == 'y' ? Vec3{0, 1, 0} : c == 'z' ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    return neg ? -v : v;
}

// The weapon nodes EnemyWeapons names for this model ("4311:R_Weapon_Spear,L_Weapon_Shield;..."); listed = it is there.
std::vector<std::string> WantedWeapons(const std::string& cfg, int model, bool& listed) {
    std::vector<std::string> out;
    listed = false;
    size_t i = 0;
    while (i <= cfg.size()) {
        size_t e = cfg.find(';', i);
        if (e == std::string::npos) e = cfg.size();
        const std::string ent = cfg.substr(i, e - i);
        const size_t colon = ent.find(':');
        if (colon != std::string::npos && std::atoi(ent.substr(0, colon).c_str()) == model) {
            listed = true;
            size_t j = colon + 1;
            while (j <= ent.size()) {
                size_t k = ent.find(',', j);
                if (k == std::string::npos) k = ent.size();
                std::string nm = ent.substr(j, k - j);
                while (!nm.empty() && (nm.front() == ' ' || nm.front() == '+')) nm.erase(nm.begin());
                while (!nm.empty() && nm.back() == ' ') nm.pop_back();
                if (!nm.empty()) out.push_back(nm);
                j = k + 1;
            }
        }
        i = e + 1;
    }
    return out;
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

Torso* FindTorso(uint8_t* chr) {
    for (auto& t : g_torsos) if (t.chr == chr) return &t;
    return nullptr;
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
    g_arms.clear(); g_torsos.clear(); g_parts.clear(); g_shapes.clear(); g_weapons.clear(); g_last = 0;
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
        if (p.kind == 3 || p.kind == 4 || (p.kind == 7 && p.side == 0)) {
            Torso* t = FindTorso(p.chr);
            if (t) t->angImpulse = t->angImpulse + (point - t->pivot).Cross(impulse);
            ++g_hits; g_maxImpulse = std::max(g_maxImpulse, impulse.Length());
            return;
        }
        if (p.kind > 4) return;   // legs and hips: solid, they do not sway
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

void ContactWeaponSim(uint32_t key, const Vec3& grip, const Vec3& centre, float deviation) {
    (void)grip;
    std::lock_guard<std::mutex> lock(g_mtx);
    for (const auto& p : g_parts) {
        if (p.key != key || p.kind != 2) continue;
        Arm* a = FindArm(p.chr, p.side);
        if (!a) return;
        const double now = NowSecLocal();
        // Both hands' scenes may hold the same weapon: within one frame the larger deviation wins.
        if (now - a->simAt < 0.004 && deviation < a->simDev) return;
        a->simTip = centre; a->simDev = deviation; a->simAt = now;
        return;
    }
}

void ContactFrame(uint8_t* player, double now) {
    const auto& c = ervr::Cfg();
    std::lock_guard<std::mutex> lock(g_mtx);
    g_parts.clear();
    if (!c.physicsContact || !player) { g_arms.clear(); g_torsos.clear(); g_shapes.clear(); g_weapons.clear(); return; }
    const float dt = g_last > 0 ? (float)std::min(std::max(now - g_last, 0.0), 0.05) : 0.f;
    g_last = now;

    const float inertia = std::max(c.physicsEnemyArmInertia, 0.02f);
    const float k = std::max(c.physicsEnemyArmStiffness, 0.f);
    const float damp = 2.f * std::max(c.physicsEnemyArmDamping, 0.f) * std::sqrt(k * inertia);
    const float maxRad = std::min(std::max(c.physicsEnemyArmMaxDeg, 0.f), 150.f) * 0.0174533f;
    const bool autoAxis = c.physicsEnemyWeaponAxis.empty() || std::tolower((unsigned char)c.physicsEnemyWeaponAxis[0]) == 'a';
    const Vec3 fixedAxis = AxisFrom(c.physicsEnemyWeaponAxis);

    for (auto& w : g_weapons) w.seen = false;
    for (auto& a : g_arms) a.seen = false;
    for (auto& t : g_torsos) t.seen = false;
    for (auto& sh : g_shapes) sh.seen = false;
    const float tI = std::max(c.physicsTorsoInertia, 0.1f), tK = std::max(c.physicsTorsoStiffness, 0.f);
    const float tD = 2.f * std::max(c.physicsTorsoDamping, 0.f) * std::sqrt(tK * tI);
    const float tMax = std::min(std::max(c.physicsTorsoMaxDeg, 0.f), 60.f) * 0.0174533f;
    bool bodyLogged = false;
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
        if (ua[0] < 0 && ua[1] < 0) continue;   // no humanoid arms
        Vec3 rootP; Quat rootQ;
        if (!ervr::GameERChrModelRoot(t.chr, &rootP, &rootQ)) continue;
        if (!ModelPose(t.chr, t.hkb, (int)par->size(), *par, mq, mp)) continue;
        ++enemies;
        auto world = [&](int b) { return rootP + rootQ.Rotate(mp[b]); };
        // Upper-body sway (torso and head blows): about the pelvis, drawn on the Spine bone.
        const int pel = TrackerBoneIndex(t.hkb, "Pelvis"), spine = TrackerBoneIndex(t.hkb, "Spine");
        Quat TR{};
        Vec3 P{};
        if (pel >= 0 && spine >= 0 && c.physicsTorsoSway) {
            Torso* tr = FindTorso(t.chr);
            if (!tr) { g_torsos.push_back(Torso{}); tr = &g_torsos.back(); tr->chr = t.chr; }
            tr->seen = true;
            P = world(pel); tr->pivot = P;
            if (dt > 0.f) {
                tr->omega = tr->omega + tr->angImpulse * (1.f / tI);
                tr->omega = tr->omega + (tr->theta * -tK - tr->omega * tD) * (dt / tI);
                tr->theta = tr->theta + tr->omega * dt;
                const float ang = tr->theta.Length();
                if (ang > tMax && ang > 1e-6f) {
                    const Vec3 dir = tr->theta * (1.f / ang);
                    tr->theta = dir * tMax;
                    const float out = tr->omega.Dot(dir);
                    if (out > 0.f) tr->omega = tr->omega - dir * out;
                }
            }
            tr->angImpulse = Vec3{};
            TR = FromRotVec(tr->theta);
            g_maxSwayDeg = std::max(g_maxSwayDeg, tr->theta.Length() * 57.2958f);
            if (tr->theta.Length() > 0.0035f) {
                const int pp = (*par)[spine];
                const Quat Mp = pp >= 0 ? mq[pp] : Quat{};
                const Quat Rm = (rootQ.Conj() * TR * rootQ).Normalized();
                PoseOverrideAdd(t.chr, t.hkb, spine, (Mp.Conj() * Rm * Mp).Normalized(), Quat{});
            }
        }
        // Upper-body bones in the world with the sway applied.
        auto worldU = [&](int b) { return P + TR.Rotate(world(b) - P); };
        // The game's own body capsules for this enemy (read once; re-read if the character was reloaded).
        Shapes* shp = nullptr;
        if (c.physicsBodyContact && c.physicsGameShapes) {
            for (auto& sh : g_shapes) if (sh.chr == t.chr) { shp = &sh; break; }
            if (!shp) { g_shapes.push_back(Shapes{}); shp = &g_shapes.back(); shp->chr = t.chr; shp->src = ~(uintptr_t)0; }
            shp->seen = true;
            // Twice a second is enough to notice a reloaded character.
            const uintptr_t src = now - shp->srcAt >= 0.5 ? GameShapesSource(t.chr) : shp->src;
            if (now - shp->srcAt >= 0.5) shp->srcAt = now;
            if (src != shp->src) {
                shp->src = src; shp->checked = false;
                shp->ok = src && GameShapesRead(t.chr, t.hkb, shp->caps, shp->why);
                shp->kind.assign(shp->caps.size(), 3); shp->side.assign(shp->caps.size(), 0);
                if (shp->ok) {
                    const int neckB = TrackerBoneIndex(t.hkb, "Neck"), headB = TrackerBoneIndex(t.hkb, "Head");
                    static const char* thighN[2] = {"L_Thigh", "R_Thigh"}, *calfN[2] = {"L_Calf", "R_Calf"};
                    for (size_t i = 0; i < shp->caps.size(); ++i) {
                        const int b = shp->caps[i].animBone;
                        int kd = 3, sd = 0;
                        for (int s = 0; s < 2; ++s) {
                            if (ua[s] >= 0 && Under(*par, b, ua[s])) { kd = fa[s] >= 0 && Under(*par, b, fa[s]) ? 1 : 0; sd = s; }
                            const int th = TrackerBoneIndex(t.hkb, thighN[s]), ca = TrackerBoneIndex(t.hkb, calfN[s]);
                            if (th >= 0 && Under(*par, b, th)) { kd = ca >= 0 && Under(*par, b, ca) ? 6 : 5; sd = s; }
                        }
                        if (kd == 3 && ((headB >= 0 && Under(*par, b, headB)) || (neckB >= 0 && b == neckB && headB < 0))) kd = 4;
                        shp->kind[i] = kd; shp->side[i] = sd;
                    }
                }
            }
        }
        // The weapons it holds: the nodes EnemyWeapons names for its model (else EnemyWeaponBone), each with the box
        // its mesh fills in the model data. One per hand; an L_ node is the left hand's.
        Weapons* wps = nullptr;
        for (auto& w : g_weapons) if (w.chr == t.chr) { wps = &w; break; }
        if (!wps) { g_weapons.push_back(Weapons{}); wps = &g_weapons.back(); wps->chr = t.chr; }
        wps->seen = true;
        if (now - wps->at >= 1.0) { wps->at = now; wps->ok = GameWeaponNodesRead(t.chr, wps->nodes, wps->why); }
        struct Held { int bone = -1; Vec3 lo{}, hi{}; bool shield = false, fromModel = false; std::string name; };
        Held hands[2];
        {
            bool listed = false;
            const std::vector<std::string> want = WantedWeapons(c.physicsEnemyWeapons, t.npcId, listed);
            std::vector<std::string> names = want;
            if (!listed && !c.physicsEnemyWeaponBone.empty()) names.push_back(c.physicsEnemyWeaponBone);
            std::string missing;
            for (const std::string& nm : names) {
                const int s = nm.rfind("L_", 0) == 0 ? 0 : 1;
                if (hands[s].bone >= 0) continue;
                const int b = TrackerBoneIndex(t.hkb, nm);
                const WeaponNode* node = nullptr;
                if (wps->ok) for (const auto& wn : wps->nodes) if (wn.name == nm) { node = &wn; break; }
                if (b < 0 || (!node && listed)) { missing += " " + nm + (b < 0 ? " (no such bone)" : " (no mesh in the model data)"); continue; }
                Held& h = hands[s];
                h.bone = b; h.name = nm; h.shield = nm.find("Shield") != std::string::npos;
                if (node) { h.lo = node->min; h.hi = node->max; h.fromModel = true; }
                else if (c.physicsEnemyWeaponLength > 0.05f) { h.lo = Vec3{0, -0.05f, -0.05f}; h.hi = Vec3{c.physicsEnemyWeaponLength, 0.05f, 0.05f}; }
                else h.bone = -1;
            }
            if (c.physicsContactLog && g_weaponsLogged.insert(t.npcId).second) {
                std::string all;
                for (const auto& wn : wps->nodes) {
                    char e[160];
                    snprintf(e, sizeof(e), " %s x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f;", wn.name.c_str(), wn.min.x, wn.max.x, wn.min.y, wn.max.y, wn.min.z, wn.max.z);
                    all += e;
                }
                std::string use;
                for (int s = 0; s < 2; ++s)
                    if (hands[s].bone >= 0) use += std::string(" ") + (s ? "right " : "left ") + hands[s].name + (hands[s].shield ? " (shield)" : "") +
                                                  (hands[s].fromModel ? " from its mesh" : " EnemyWeaponLength along EnemyWeaponAxis");
                LOGI("[phys] weapon nodes npc %d: %s:%s | holds (%s):%s%s%s", t.npcId, wps->why.c_str(), all.c_str(),
                     listed ? "EnemyWeapons" : "not in EnemyWeapons, EnemyWeaponBone", use.empty() ? " nothing" : use.c_str(),
                     missing.empty() ? "" : " | left out:", missing.c_str());
            }
        }
        Vec3 armS[2]{}; Quat armR[2]{}; bool armOn[2]{};
        for (int s = 0; s < 2; ++s) {
            if (ua[s] < 0 || fa[s] < 0 || hd[s] < 0) continue;
            Arm* a = FindArm(t.chr, s);
            if (!a) { g_arms.push_back(Arm{}); a = &g_arms.back(); a->chr = t.chr; a->side = s; }
            a->seen = true; a->npcId = t.npcId;
            const Vec3 S = worldU(ua[s]);
            a->shoulder = S;
            a->axisDir = (worldU(fa[s]) - S).Normalized();
            // The weapon (or shield) in this hand as the animation wants it: its node's frame, its box's centre.
            const Held& hw = hands[s];
            const bool hasWeapon = hw.bone >= 0;
            Vec3 W0{}, C0{}; Quat wq0{};
            if (hasWeapon) {
                W0 = worldU(hw.bone);
                wq0 = (TR * rootQ * mq[hw.bone]).Normalized();
                if (!hw.fromModel) {
                    // no mesh box: EnemyWeaponLength along EnemyWeaponAxis (auto: learned from the blows you took, +x until then)
                    Vec3 ax = autoAxis ? Vec3{1, 0, 0} : fixedAxis;
                    float ld[3];
                    if (autoAxis && HitJudgeWeaponDir(t.npcId, ld)) ax = Vec3{ld[0], ld[1], ld[2]};
                    wq0 = (wq0 * Quat::FromTo(Vec3{1, 0, 0}, ax)).Normalized();
                }
                C0 = W0 + wq0.Rotate((hw.lo + hw.hi) * 0.5f);
            }
            const bool held = hasWeapon && now - a->simAt < 0.1;
            if (held) {
                // Your blade holds the weapon off its path: the arm turns about the shoulder to carry it there.
                const Vec3 u = (C0 - S).Normalized(), v = (a->simTip - S).Normalized();
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
            const Vec3 E = swung(worldU(fa[s])), H = swung(worldU(hd[s]));
            armS[s] = S; armR[s] = R; armOn[s] = true;
            if (!(shp && shp->ok)) {
                g_parts.push_back(MakePart(PartKey(t.chr, s, 0), t.chr, s, 0, S, E, 0.07f));
                g_parts.push_back(MakePart(PartKey(t.chr, s, 1), t.chr, s, 1, E, H, 0.055f));
            }
            if (hasWeapon) {
                // Its line along the node's x through the box (for the hit judge) and the box itself (for the rig).
                const float cy = (hw.lo.y + hw.hi.y) * 0.5f, cz = (hw.lo.z + hw.hi.z) * 0.5f;
                const Vec3 A0 = W0 + wq0.Rotate(Vec3{hw.lo.x, cy, cz}), B0 = W0 + wq0.Rotate(Vec3{hw.hi.x, cy, cz});
                ContactPart wp = MakePart(PartKey(t.chr, s, 2), t.chr, s, 2, A0, B0,
                                          std::max(hw.hi.y - hw.lo.y, hw.hi.z - hw.lo.z) * 0.5f);
                wp.box = true; wp.shield = hw.shield;
                wp.boneP = W0; wp.boneQ = wq0; wp.lo = hw.lo; wp.hi = hw.hi;
                wp.length = hw.hi.x - hw.lo.x;
                wp.npcId = t.npcId;
                wp.da = swung(A0); wp.db = swung(B0);
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
        // The body: torso and head (they sway the upper body), thighs and calves (solid; the blade cuts, sticks or
        // glances on all of them).
        if (c.physicsBodyContact) {
            const int neck = TrackerBoneIndex(t.hkb, "Neck"), head = TrackerBoneIndex(t.hkb, "Head");
            const float bs = std::min(std::max(c.physicsBodyScale, 0.3f), 3.f);
            if (shp && shp->ok) {
                // The game's own capsules, each on the bone it follows, with the upper-body sway and the arms'
                // swing applied like the animation bones they ride on.
                const Quat bodyQ = (TR * rootQ).Normalized();
                std::vector<ContactPart> gp;
                float worst = 0.f; int worstI = -1;
                for (size_t i = 0; i < shp->caps.size(); ++i) {
                    const GameCapsule& gc = shp->caps[i];
                    const int b = gc.animBone;
                    if (b < 0 || b >= (int)mq.size()) continue;
                    const Vec3 bp = world(b);
                    const Quat bq = (rootQ * mq[b]).Normalized();
                    const Vec3 cp = bp + bq.Rotate(gc.t);
                    const Quat cq = (bq * gc.q).Normalized();
                    Vec3 A = cp + cq.Rotate(gc.a), B = cp + cq.Rotate(gc.b);
                    const float off = ((A + B) * 0.5f - bp).Length();
                    if (off > worst) { worst = off; worstI = (int)i; }
                    if (spine >= 0 && Under(*par, b, spine)) { A = P + TR.Rotate(A - P); B = P + TR.Rotate(B - P); }
                    const int kd = shp->kind[i], sd = shp->side[i];
                    if ((kd == 0 || kd == 1) && armOn[sd]) {
                        A = armS[sd] + armR[sd].Rotate(A - armS[sd]); B = armS[sd] + armR[sd].Rotate(B - armS[sd]);
                    }
                    if ((B - A).Length() < 0.002f) B = A + bodyQ.Rotate(Vec3{0, 0.002f, 0});   // a sphere: keep an axis
                    gp.push_back(MakePart(ShapeKey(t.chr, (int)i), t.chr, sd, kd, A, B, gc.radius * bs));
                }
                if (!shp->checked) {
                    // Once: every capsule must sit on (or near) its bone, or the layout was misread - fall back.
                    shp->checked = true;
                    if (worst > 0.8f) {
                        shp->ok = false;
                        shp->why = "capsule " + (worstI >= 0 ? shp->caps[worstI].ragName : std::string("?")) + " is " + std::to_string(worst) + " m off its bone";
                        gp.clear();
                    }
                    if (c.physicsContactLog && !g_shapesLogged.count(t.npcId)) {
                        g_shapesLogged.insert(t.npcId);
                        if (!shp->ok) LOGI("[phys] game body npc %d: not used (%s), hand-made capsules instead", t.npcId, shp->why.c_str());
                        else {
                            std::string list;
                            static const char* kn[8] = {"arm", "forearm", "weapon", "trunk", "head", "thigh", "calf", "bar"};
                            for (size_t i = 0; i < shp->caps.size(); ++i) {
                                const GameCapsule& gc = shp->caps[i];
                                char b[160];
                                snprintf(b, sizeof(b), "%s%s>%s %s r%.2f L%.2f off%.2f", i ? ", " : "", gc.ragName.c_str(), gc.animName.c_str(),
                                         kn[shp->kind[i] & 7], gc.radius, (gc.b - gc.a).Length(), gc.t.Length());
                                list += b;
                            }
                            LOGI("[phys] game body npc %d: %s (largest offset from its bone %.2f m) | %s", t.npcId, shp->why.c_str(), worst, list.c_str());
                        }
                    }
                }
                for (auto& q : gp) g_parts.push_back(q);
            }
            if (!(shp && shp->ok) && pel >= 0 && neck >= 0) {
                // The trunk as it is drawn (armour included), not a stick down the spine: two upright capsules side
                // by side from the hips to the shoulders (wide and not deep, like a chest), a bar across the
                // shoulders and one across the hips.
                const Vec3 P0 = world(pel), N0 = worldU(neck);
                Vec3 axis = (N0 - P0).Normalized();
                Vec3 lat = (ua[0] >= 0 && ua[1] >= 0) ? worldU(ua[0]) - worldU(ua[1]) : (TR * rootQ).Rotate(Vec3{1, 0, 0});
                lat = (lat - axis * lat.Dot(axis)).Normalized();
                const float off = 0.10f * bs, rr = 0.15f * bs;
                const Vec3 lo = P0 + axis * 0.02f, hi = N0 - axis * 0.10f;
                g_parts.push_back(MakePart(PartKey(t.chr, 0, 3), t.chr, 0, 3, lo + lat * off, hi + lat * off, rr));
                g_parts.push_back(MakePart(PartKey(t.chr, 1, 3), t.chr, 1, 3, lo - lat * off, hi - lat * off, rr));
                if (ua[0] >= 0 && ua[1] >= 0)
                    g_parts.push_back(MakePart(PartKey(t.chr, 0, 7), t.chr, 0, 7, worldU(ua[0]), worldU(ua[1]), 0.11f * bs));
                const int lt = TrackerBoneIndex(t.hkb, "L_Thigh"), rt = TrackerBoneIndex(t.hkb, "R_Thigh");
                if (lt >= 0 && rt >= 0) g_parts.push_back(MakePart(PartKey(t.chr, 1, 7), t.chr, 1, 7, world(lt), world(rt), 0.14f * bs));
            }
            if (!(shp && shp->ok) && head >= 0) {
                const Vec3 hp = worldU(head), up = (TR * rootQ).Rotate(Vec3{0, 1, 0});
                g_parts.push_back(MakePart(PartKey(t.chr, 0, 4), t.chr, 0, 4, hp, hp + up * 0.10f, 0.13f * bs));
            }
            static const char* thighN[2] = {"L_Thigh", "R_Thigh"}, *calfN[2] = {"L_Calf", "R_Calf"}, *footN[2] = {"L_Foot", "R_Foot"};
            for (int s = 0; s < 2 && !(shp && shp->ok); ++s) {
                const int th = TrackerBoneIndex(t.hkb, thighN[s]), ca = TrackerBoneIndex(t.hkb, calfN[s]), fo = TrackerBoneIndex(t.hkb, footN[s]);
                if (th >= 0 && ca >= 0) g_parts.push_back(MakePart(PartKey(t.chr, s, 5), t.chr, s, 5, world(th), world(ca), 0.10f * bs));
                if (ca >= 0 && fo >= 0) g_parts.push_back(MakePart(PartKey(t.chr, s, 6), t.chr, s, 6, world(ca), world(fo), 0.08f * bs));
            }
            // Where the capsules are, in this enemy's own frame (x right, y up, z its forward as the model root
            // has it), next to your blade: if the blade passes through a body as if through air, this shows
            // whether the capsule is somewhere else.
            if (c.physicsContactLog && !bodyLogged && now - g_bodyLogAt >= 1.0 && t.distance < 2.5f && neck >= 0 && pel >= 0) {
                bodyLogged = true; g_bodyLogAt = now;
                const Quat inv = rootQ.Conj();
                auto L = [&](const Vec3& w) { return inv.Rotate(w - rootP); };
                const Vec3 pv = L(world(pel)), nk = L(worldU(neck)), hdp = head >= 0 ? L(worldU(head)) : Vec3{};
                const Vec3 rs = ua[1] >= 0 ? L(worldU(ua[1])) : Vec3{}, rh = hd[1] >= 0 ? L(worldU(hd[1])) : Vec3{};
                Vec3 g{}, tp{}; const bool blade = ervr::GameERBladeWorld(1, &g, &tp);
                const Vec3 bg = blade ? L(g) : Vec3{}, bt = blade ? L(tp) : Vec3{};
                Vec3 feet; Quat pq; const bool pl = ervr::GameERChrModelRoot(player, &feet, &pq);
                const Vec3 me = pl ? L(feet) : Vec3{};
                LOGI("[phys] body check npc %d at %.1f m (its frame, m): pelvis (%.2f %.2f %.2f) neck (%.2f %.2f %.2f) head (%.2f %.2f %.2f) R shoulder (%.2f %.2f %.2f) R hand (%.2f %.2f %.2f) | your feet (%.2f %.2f %.2f) blade grip (%.2f %.2f %.2f) tip (%.2f %.2f %.2f)%s | root pose: model bone 0 at (%.2f %.2f %.2f)",
                     t.npcId, t.distance, pv.x, pv.y, pv.z, nk.x, nk.y, nk.z, hdp.x, hdp.y, hdp.z, rs.x, rs.y, rs.z, rh.x, rh.y, rh.z,
                     me.x, me.y, me.z, bg.x, bg.y, bg.z, bt.x, bt.y, bt.z, blade ? "" : " (no blade)", mp[0].x, mp[0].y, mp[0].z);
                // Where the enemy's weapon really is: your blade (both hands) in its right hand's frame. Lay your
                // blade along its blade and the weapon's line shows here; the attach bones' offsets from the hand
                // (logged once per enemy kind) say which one it hangs on and along which of its axes.
                if (hd[1] >= 0) {
                    const Vec3 Hp = world(hd[1]);
                    const Quat Hi = (rootQ * mq[hd[1]]).Normalized().Conj();
                    char bl[2][96] = {};
                    for (int hh = 0; hh < 2; ++hh) {
                        Vec3 g2{}, t2{};
                        if (ervr::GameERBladeWorld(hh, &g2, &t2)) {
                            const Vec3 a = Hi.Rotate(g2 - Hp), b = Hi.Rotate(t2 - Hp);
                            snprintf(bl[hh], sizeof(bl[hh]), "grip (%.2f %.2f %.2f) tip (%.2f %.2f %.2f)", a.x, a.y, a.z, b.x, b.y, b.z);
                        } else snprintf(bl[hh], sizeof(bl[hh]), "none");
                    }
                    LOGI("[phys] weapon calib npc %d: your blade in its R_Hand frame: L %s | R %s", t.npcId, bl[0], bl[1]);
                    static std::set<int> s_wlogged;
                    if (s_wlogged.insert(t.npcId).second) {
                        std::string list;
                        for (const char* nm : {"R_Weapon_Sword", "R_Weapon_Sword_large", "R_Weapon_Spear", "R_Weapon_Lucerne", "R_Weapon_Warpick", "R_Weapon_Crossbow", "R_Weapon_Whistle"}) {
                            const int b = TrackerBoneIndex(t.hkb, nm);
                            if (b < 0) continue;
                            const Vec3 o = Hi.Rotate(world(b) - Hp);
                            const Quat q = (Hi * rootQ * mq[b]).Normalized();
                            const Vec3 x = q.Rotate(Vec3{1, 0, 0}), y = q.Rotate(Vec3{0, 1, 0}), z = q.Rotate(Vec3{0, 0, 1});
                            char e[200];
                            snprintf(e, sizeof(e), " %s at (%.2f %.2f %.2f) x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f);", nm, o.x, o.y, o.z,
                                     x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z);
                            list += e;
                        }
                        LOGI("[phys] weapon bones npc %d (in its R_Hand frame):%s", t.npcId, list.c_str());
                    }
                }
            }
        }
    }
    g_torsos.erase(std::remove_if(g_torsos.begin(), g_torsos.end(), [](const Torso& t) { return !t.seen; }), g_torsos.end());
    g_arms.erase(std::remove_if(g_arms.begin(), g_arms.end(), [](const Arm& a) { return !a.seen; }), g_arms.end());
    g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(), [](const Shapes& s) { return !s.seen; }), g_shapes.end());
    g_weapons.erase(std::remove_if(g_weapons.begin(), g_weapons.end(), [](const Weapons& w) { return !w.seen; }), g_weapons.end());

    if (now - g_logAt >= 5.0) {
        if (c.physicsContactLog && (enemies > 0 || g_hits > 0))
            LOGI("[phys] contact: %d enemy(ies), %d capsule(s); blade contacts %d (largest impulse %.1f N*s); largest arm swing %.0f deg (npc %d %s arm); weapon held off by your blade %d frame(s); largest upper-body sway %.0f deg",
                 enemies, (int)g_parts.size(), g_hits, g_maxImpulse, g_maxSwingDeg, g_maxSwingNpc, g_maxSwingSide ? "right" : "left", g_held, g_maxSwayDeg);
        g_logAt = now; g_hits = 0; g_held = 0; g_maxSwayDeg = 0; g_maxImpulse = 0; g_maxSwingDeg = 0;
    }
}

}  // namespace kinetic
