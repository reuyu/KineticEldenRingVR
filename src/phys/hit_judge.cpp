#include "hit_judge.h"

#include "combat.h"
#include "config.h"
#include "enemy_contact.h"
#include "game_er.h"
#include "log.h"
#include "vrmath.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <deque>
#include <unordered_map>

namespace kinetic {

namespace {

using ervr::Vec3;

struct Sample { double t; float dBody; bool shield; float speed; float dRoot; float dCand[6]; };
struct WeaponTrack { Vec3 lastTip{}; double lastT = 0; bool have = false; std::deque<Sample> hist; };

std::unordered_map<uint32_t, WeaponTrack> g_tracks;
int g_prevHp = -1;
double g_logAt = 0;
int g_kept = 0, g_refunded = 0, g_grazed = 0, g_shielded = 0, g_untouched = 0;   // statistics for the log
long g_hpBack = 0, g_hpKept = 0, g_shieldStam = 0;
// Weapon axis learning: per candidate axis, how many blows it would have reached you on.
int g_axisReach[6] = {}, g_axisTrials = 0;
std::atomic<int> g_axisBest{-1};
const char* kAxisName[6] = {"+x", "-x", "+y", "-y", "+z", "-z"};

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

int HitJudgeWeaponAxis() { return g_axisBest.load(); }

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
        // Where the weapon really is (held off by your blade, its arm turned), not where the animation wants it.
        const Vec3 a = parts[i].da, b = parts[i].db;
        float speed = 0.f;
        if (w.have && now > w.lastT) speed = (b - w.lastTip).Length() / (float)(now - w.lastT);
        w.lastTip = b; w.lastT = now; w.have = true;
        const float dBody = SegSegDistance(a, b, b0, b1) - bodyR - parts[i].radius;
        const bool onShield = shield && SegPointDistance(a, b, shieldC) < shieldR + parts[i].radius;
        Sample smp{now, dBody, onShield, speed, SegPointDistance(b0, b1, a) - bodyR, {}};
        for (int k = 0; k < 6; ++k) {
            const Vec3 ax = parts[i].axis[k / 2] * (k % 2 ? -1.f : 1.f);
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
        float candMin[6] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
        const bool learned = !AutoAxis() || g_axisBest.load() >= 0;
        for (const auto& kv : g_tracks)
            for (const Sample& s : kv.second.hist) {
                if (s.dRoot < 1.2f) near = true;   // the weapon's grip came within reach, whatever its blade axis
                if (s.shield) { shielded = true; firstShield = std::min(firstShield, s.t); }
                for (int k = 0; k < 6; ++k) candMin[k] = std::min(candMin[k], s.dCand[k]);
                // Until the blade axis is known, any axis that would have reached you counts as a touch.
                float d = s.dBody;
                if (!learned) for (int k = 0; k < 6; ++k) d = std::min(d, s.dCand[k]);
                if (d < 0.f) { touched = true; touchSpeed = std::max(touchSpeed, s.speed); firstBody = std::min(firstBody, s.t); }
            }
        // Learn the axis: on a blow that came with an enemy weapon near, the true blade axis must have reached you.
        if (near && !shielded) {
            ++g_axisTrials;
            for (int k = 0; k < 6; ++k) if (candMin[k] < 0.05f) ++g_axisReach[k];
            int best = -1, second = 0;
            for (int k = 0; k < 6; ++k) {
                if (best < 0 || g_axisReach[k] > g_axisReach[best]) { if (best >= 0) second = std::max(second, g_axisReach[best]); best = k; }
                else second = std::max(second, g_axisReach[k]);
            }
            const int prev = g_axisBest.load();
            if (g_axisTrials >= 3 && best >= 0 && g_axisReach[best] >= 2 && g_axisReach[best] > second && best != prev) {
                g_axisBest.store(best);
                LOGI("[phys] weapon axis: %s reached you on %d of %d blows (next best %d)%s", kAxisName[best], g_axisReach[best], g_axisTrials, second,
                     AutoAxis() ? " - the enemy weapon capsule now uses it" : " - set [Physics] EnemyWeaponAxis to it (or auto)");
            }
            if (c.physicsContactLog)
                LOGI("[phys] weapon axis check: closest to your body +x %.2f -x %.2f +y %.2f -y %.2f +z %.2f -z %.2f m (reached so far %d/%d/%d/%d/%d/%d of %d)",
                     candMin[0], candMin[1], candMin[2], candMin[3], candMin[4], candMin[5],
                     g_axisReach[0], g_axisReach[1], g_axisReach[2], g_axisReach[3], g_axisReach[4], g_axisReach[5], g_axisTrials);
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
