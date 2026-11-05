#include "hit_judge.h"

#include "combat.h"
#include "config.h"
#include "enemy_contact.h"
#include "game_er.h"
#include "log.h"
#include "vrmath.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace kinetic {

namespace {

using ervr::Vec3;

// Candidate blade directions in the weapon bone's frame, spread evenly over the sphere.
constexpr int kDirs = 128;
Vec3 g_dirs[kDirs];
struct Sample { double t; float dBody; bool shield; float speed; float dRoot; float dCand[kDirs]; };
struct WeaponTrack { Vec3 lastTip{}; double lastT = 0; bool have = false; int npcId = 0; std::deque<Sample> hist; };
// Per enemy kind: how many blows each candidate direction would have reached you on.
struct Learn { int reach[kDirs] = {}; int trials = 0; bool known = false; Vec3 dir{1, 0, 0}; float share = 0.f; };
std::mutex g_learnMtx;
std::unordered_map<int, Learn> g_learn;
bool g_learnLoaded = false;

std::unordered_map<uint32_t, WeaponTrack> g_tracks;
int g_prevHp = -1;
double g_logAt = 0;
int g_kept = 0, g_refunded = 0, g_grazed = 0, g_shielded = 0, g_untouched = 0;   // statistics for the log
long g_hpBack = 0, g_hpKept = 0, g_shieldStam = 0;

float Clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

// Closest distance between segments p1-q1 and p2-q2.
float SegSegDistance(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = d1.Dot(d1), e = d2.Dot(d2), f = d2.Dot(r);
    float s = 0.f, t = 0.f;
    if (a <= 1e-8f && e <= 1e-8f) return r.Length();
    if (a <= 1e-8f) { t = Clamp01(f / e); }
    else {
        const float c = d1.Dot(r);
        if (e <= 1e-8f) { s = Clamp01(-c / a); }
        else {
            const float b = d1.Dot(d2), den = a * e - b * b;
            s = den > 1e-8f ? Clamp01((b * f - c * e) / den) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f) { t = 0.f; s = Clamp01(-c / a); }
            else if (t > 1.f) { t = 1.f; s = Clamp01((b - c) / a); }
        }
    }
    return ((p1 + d1 * s) - (p2 + d2 * t)).Length();
}

float SegPointDistance(const Vec3& p, const Vec3& q, const Vec3& x) { return SegSegDistance(p, q, x, x); }

}  // namespace

void HitJudgeReset() { g_tracks.clear(); g_prevHp = -1; }

namespace {

std::wstring LearnPath() {
    std::wstring p = ervr::Cfg().iniPath;
    const size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1)) + L"KineticWeapons.txt";
}

void InitDirs() {
    static bool done = false;
    if (done) return;
    done = true;
    const float ga = 2.39996323f;   // golden angle
    for (int i = 0; i < kDirs; ++i) {
        const float y = 1.f - 2.f * (i + 0.5f) / kDirs, r = std::sqrt(std::max(0.f, 1.f - y * y));
        g_dirs[i] = Vec3{std::cos(ga * i) * r, y, std::sin(ga * i) * r};
    }
}

// Lines "npcId dx dy dz reach trials": what earlier sessions learned (game thread, once).
void LoadLearned() {
    if (g_learnLoaded) return;
    g_learnLoaded = true;
    FILE* f = _wfopen(LearnPath().c_str(), L"r");
    if (!f) return;
    int npc, reach, trials; float x, y, z; int n = 0;
    std::lock_guard<std::mutex> lock(g_learnMtx);
    while (fscanf(f, "%d %f %f %f %d %d", &npc, &x, &y, &z, &reach, &trials) == 6) {
        Learn& L = g_learn[npc];
        L.known = true; L.dir = Vec3{x, y, z}.Normalized(); L.trials = trials; L.share = trials > 0 ? (float)reach / trials : 0.f;
        // seed the counts so new blows refine it rather than start over
        for (int k = 0; k < kDirs; ++k) if (g_dirs[k].Dot(L.dir) > 0.94f) L.reach[k] = reach;
        ++n;
    }
    fclose(f);
    LOGI("[phys] weapon directions: %d enemy kind(s) loaded from KineticWeapons.txt", n);
}

void SaveLearned() {
    FILE* f = _wfopen(LearnPath().c_str(), L"w");
    if (!f) return;
    for (const auto& kv : g_learn)
        if (kv.second.known)
            fprintf(f, "%d %.4f %.4f %.4f %d %d\n", kv.first, kv.second.dir.x, kv.second.dir.y, kv.second.dir.z,
                    (int)std::lround(kv.second.share * kv.second.trials), kv.second.trials);
    fclose(f);
}

}  // namespace

bool HitJudgeWeaponDir(int npcId, float dir[3]) {
    std::lock_guard<std::mutex> lock(g_learnMtx);
    auto it = g_learn.find(npcId);
    if (it == g_learn.end() || !it->second.known) return false;
    dir[0] = it->second.dir.x; dir[1] = it->second.dir.y; dir[2] = it->second.dir.z;
    return true;
}

namespace {
bool AutoAxis() {
    const std::string& a = ervr::Cfg().physicsEnemyWeaponAxis;
    return a.empty() || a[0] == 'a' || a[0] == 'A';
}
}  // namespace

void HitJudgeFrame(uint8_t* player, double now) {
    const auto& c = ervr::Cfg();
    if (!c.physicsHitJudge || !player) { g_prevHp = -1; return; }
    const double window = std::max(c.physicsHitWindow, 0.05f);
    InitDirs();
    LoadLearned();

    // Your body: feet to eyes.
    Vec3 feet; ervr::Quat rootQ;
    if (!ervr::GameERChrModelRoot(player, &feet, &rootQ)) return;
    Vec3 eye;
    if (!ervr::GameEREyeWorld(&eye) || (eye - feet).Length() > 3.f) eye = feet + Vec3{0, 1.6f, 0};
    const Vec3 b0 = feet + Vec3{0, 0.15f, 0}, b1 = eye - Vec3{0, 0.05f, 0};
    const float bodyR = std::max(c.physicsBodyRadius, 0.05f);
    // The shield: a disc at the left grip, as a sphere.
    bool shield = ervr::CombatLeftIsShield();
    Vec3 grip, tip, shieldC;
    if (shield && ervr::GameERBladeWorld(0, &grip, &tip)) {
        const Vec3 d = tip - grip;
        shieldC = d.Length() > 1e-3f ? grip + d.Normalized() * 0.08f : grip;
    } else shield = false;
    const float shieldR = std::max(c.physicsShieldRadius, 0.05f);

    // Measure every enemy weapon this frame.
    ContactPart parts[128];
    const int n = ContactParts(parts, 128);
    for (int i = 0; i < n; ++i) {
        if (parts[i].kind != 2) continue;
        WeaponTrack& w = g_tracks[parts[i].key];
        w.npcId = parts[i].npcId;
        // Where the weapon really is (held off by your blade, its arm turned), not where the animation wants it.
        const Vec3 a = parts[i].da, b = parts[i].db;
        float speed = 0.f;
        if (w.have && now > w.lastT) speed = (b - w.lastTip).Length() / (float)(now - w.lastT);
        w.lastTip = b; w.lastT = now; w.have = true;
        const float dBody = SegSegDistance(a, b, b0, b1) - bodyR - parts[i].radius;
        const bool onShield = shield && SegPointDistance(a, b, shieldC) < shieldR + parts[i].radius;
        Sample smp{now, dBody, onShield, speed, SegPointDistance(b0, b1, a) - bodyR, {}};
        // Every candidate blade direction (in the weapon bone's frame) from the grip: how close it came to you.
        const float rootD = smp.dRoot;
        for (int k = 0; k < kDirs; ++k) {
            if (rootD > parts[i].length + 0.3f) { smp.dCand[k] = rootD; continue; }   // out of reach whichever way
            const Vec3& d = g_dirs[k];
            const Vec3 ax = parts[i].axis[0] * d.x + parts[i].axis[1] * d.y + parts[i].axis[2] * d.z;
            smp.dCand[k] = SegSegDistance(a, a + ax * parts[i].length, b0, b1) - bodyR - parts[i].radius;
        }
        w.hist.push_back(smp);
    }
    for (auto it = g_tracks.begin(); it != g_tracks.end();) {
        auto& h = it->second.hist;
        while (!h.empty() && now - h.front().t > window) h.pop_front();
        if (h.empty() && now - it->second.lastT > window) it = g_tracks.erase(it); else ++it;
    }

    int maxHp = -1;
    int hp = ervr::CombatPlayerHp(&maxHp);
    if (hp < 0) { g_prevHp = -1; return; }
    if (g_prevHp > 0 && hp > 0 && hp < g_prevHp) {
        const int drop = g_prevHp - hp;
        bool near = false, touched = false, shielded = false;
        float touchSpeed = 0.f;
        double firstShield = 1e18, firstBody = 1e18;
        // Per weapon (enemy kind) near you in the window: the closest each candidate direction came.
        struct Near { int npc; float candMin[kDirs]; };
        std::vector<Near> nears;
        for (const auto& kv : g_tracks) {
            bool trackNear = false;
            Near nr{kv.second.npcId, {}};
            for (int k = 0; k < kDirs; ++k) nr.candMin[k] = 1e9f;
            bool learned = !AutoAxis();
            { float dd[3]; learned = learned || HitJudgeWeaponDir(kv.second.npcId, dd); }
            for (const Sample& s : kv.second.hist) {
                if (s.dRoot < 1.2f) { near = true; trackNear = true; }   // its grip came within reach, whatever its blade direction
                if (s.shield) { shielded = true; firstShield = std::min(firstShield, s.t); }
                float any = 1e9f;
                for (int k = 0; k < kDirs; ++k) { nr.candMin[k] = std::min(nr.candMin[k], s.dCand[k]); any = std::min(any, s.dCand[k]); }
                // Until the blade direction is known, any direction that would have reached you counts as a touch.
                const float d = learned ? s.dBody : std::min(s.dBody, any);
                if (d < 0.f) { touched = true; touchSpeed = std::max(touchSpeed, s.speed); firstBody = std::min(firstBody, s.t); }
            }
            if (trackNear) nears.push_back(nr);
        }
        // Learn the direction: on a blow that came with one enemy weapon near, its true blade must have reached you.
        if (nears.size() == 1 && !shielded) {
            const Near& nr = nears[0];
            std::lock_guard<std::mutex> lock(g_learnMtx);
            Learn& L = g_learn[nr.npc];
            ++L.trials;
            for (int k = 0; k < kDirs; ++k) if (nr.candMin[k] < 0.05f) ++L.reach[k];
            // Smoothed over neighbouring directions (within ~25 deg): the best cone, and its weighted centre.
            int best = -1; float bestScore = 0.f;
            for (int k = 0; k < kDirs; ++k) {
                float sc = 0.f;
                for (int j = 0; j < kDirs; ++j) { const float w = g_dirs[k].Dot(g_dirs[j]) - 0.906f; if (w > 0.f) sc += w * L.reach[j]; }
                if (sc > bestScore) { bestScore = sc; best = k; }
            }
            if (best >= 0) {
                Vec3 m{};
                for (int j = 0; j < kDirs; ++j) { const float w = g_dirs[best].Dot(g_dirs[j]) - 0.906f; if (w > 0.f) m = m + g_dirs[j] * (w * L.reach[j]); }
                const Vec3 dir = m.Length() > 1e-4f ? m.Normalized() : g_dirs[best];
                const float share = (float)L.reach[best] / L.trials;
                const bool was = L.known;
                const float moved = was ? std::acos(std::min(1.f, std::max(-1.f, dir.Dot(L.dir)))) * 57.3f : 180.f;
                if (L.trials >= 3 && L.reach[best] >= 3 && share >= 0.5f && moved > 5.f) {
                    L.known = true; L.dir = dir; L.share = share;
                    SaveLearned();
                    LOGI("[phys] weapon direction npc %d: (%.2f %.2f %.2f) in its weapon bone's frame, reached you on %d of %d blows%s", nr.npc,
                         dir.x, dir.y, dir.z, L.reach[best], L.trials, was ? " (refined)" : " - its capsule now uses it");
                } else if (L.known) L.share = share;
                if (c.physicsContactLog)
                    LOGI("[phys] weapon direction check npc %d: best (%.2f %.2f %.2f) reached %d of %d blows%s", nr.npc, g_dirs[best].x, g_dirs[best].y,
                         g_dirs[best].z, L.reach[best], L.trials, L.known ? "" : " (not yet sure)");
            }
        }
        // With [Combat] Block on the shield is the game's own guard: it already blocked (and charged stamina).
        if (c.blockEnabled) shielded = false;
        if (near || shielded) {
            int refund = 0;
            const char* why;
            if (shielded && (!touched || firstShield <= firstBody)) {
                // The shield always holds; it costs stamina by how hard the blow was.
                refund = drop;
                const int cost = (int)std::lround(drop * std::max(c.physicsShieldStaminaPerDamage, 0.f));
                int sMax = -1; const int st = ervr::CombatPlayerStamina(&sMax);
                if (st > 0 && cost > 0) ervr::CombatSetPlayerStamina(st - cost);
                g_shieldStam += cost; ++g_shielded; why = "on the shield";
            } else if (touched) {
                const float ref = std::max(c.physicsEnemyWeaponRefSpeed, 0.5f);
                const float f = std::min(std::max(touchSpeed / ref, std::min(std::max(c.physicsGrazeMin, 0.f), 1.f)), 1.f);
                refund = (int)std::lround(drop * (1.f - f));
                if (refund > 0) ++g_grazed; else ++g_kept;
                why = refund > 0 ? "grazed" : "full hit";
            } else {
                refund = drop; ++g_untouched; why = "never reached you";
            }
            if (refund > 0) { ervr::CombatSetPlayerHp(hp + refund); hp += refund; ++g_refunded; }
            g_hpBack += refund; g_hpKept += drop - refund;
            if (c.physicsContactLog)
                LOGI("[phys] hit judge: the game took %d HP, enemy weapon %s (touch speed %.1f m/s) -> %d given back", drop, why, touchSpeed, refund);
        }
    }
    g_prevHp = hp;

    if (now - g_logAt >= 5.0) {
        if (c.physicsContactLog && (g_refunded || g_kept || g_grazed || g_shielded || g_untouched))
            LOGI("[phys] hit judge (5 s): blows %d never reached you, %d on the shield (%ld stamina), %d grazed, %d full; HP given back %ld, kept %ld",
                 g_untouched, g_shielded, g_shieldStam, g_grazed, g_kept, g_hpBack, g_hpKept);
        g_logAt = now; g_kept = g_refunded = g_grazed = g_shielded = g_untouched = 0; g_hpBack = g_hpKept = g_shieldStam = 0;
    }
}

}  // namespace kinetic
