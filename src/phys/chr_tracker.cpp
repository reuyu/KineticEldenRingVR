#include "chr_tracker.h"

#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "game_version.h"
#include "log.h"
#include "vrmath.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace kinetic {

namespace {
using ervr::SafeRead;

// WorldChrMan's by-distance list (same layout ERVR's body haptics and combat watcher read).
constexpr size_t WCM_ChrByDistFirst = 0x1F1D8;
constexpr size_t WCM_ChrByDistLast = 0x1F1E0;
constexpr size_t ChrDist_Stride = 0x10;   // {ChrIns* @0, f32 distance @8, pad}
constexpr size_t Chr_NpcId = 0x64;
constexpr size_t Chr_Modules = 0x190;
constexpr size_t Mod_Data = 0x00;
constexpr size_t Data_Hp = 0x138;
constexpr size_t Data_MaxHp = 0x13C;

struct RigInfo {
    std::vector<std::string> names;
    std::vector<int16_t> parents;
    std::unordered_map<std::string, int> index;
};

std::vector<TrackedChr> g_list;
std::unordered_map<uint8_t*, RigInfo> g_rigs;          // per hkbCharacter
std::unordered_set<size_t> g_dumpedRigs;               // rig signatures already dumped to the log
std::unordered_set<uint8_t*> g_seen;                   // ChrIns logged as entering the radius
std::vector<std::string> g_playerRig;
double g_lastSummary = 0;

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

uint8_t* Deref(const void* addr) { uint8_t* p = nullptr; return SafeRead(addr, &p, sizeof(p)) ? p : nullptr; }

size_t RigSignature(const std::vector<std::string>& names) {
    size_t h = names.size();
    for (const auto& n : names) h = h * 1000003u ^ std::hash<std::string>{}(n);
    return h;
}

const RigInfo* Rig(uint8_t* hkb) {
    if (!hkb) return nullptr;
    auto it = g_rigs.find(hkb);
    if (it != g_rigs.end()) return &it->second;
    RigInfo r;
    if (!ervr::GameERHkbSkeleton(hkb, r.names, r.parents)) return nullptr;
    for (int i = 0; i < (int)r.names.size(); ++i) r.index.emplace(r.names[i], i);
    if (g_rigs.size() > 256) g_rigs.clear();   // characters come and go; a stale entry is only a cache miss
    return &g_rigs.emplace(hkb, std::move(r)).first->second;
}

void DumpRig(int npcId, const RigInfo& r, bool same) {
    const size_t sig = RigSignature(r.names);
    if (!ervr::Cfg().physicsDumpBones || !g_dumpedRigs.insert(sig).second) return;
    LOGI("[phys] rig of npc %d: %d bones%s", npcId, (int)r.names.size(), same ? " (same as the player's)" : "");
    if (same) return;   // the player's rig is in the log already when [FirstPerson] DumpSkeleton is on
    std::string line;
    for (int i = 0; i < (int)r.names.size(); ++i) {
        line += std::to_string(i) + ":" + r.names[i] + "(p" + std::to_string(r.parents[i]) + ") ";
        if (line.size() > 1500) { LOGI("[phys]   %s", line.c_str()); line.clear(); }
    }
    if (!line.empty()) LOGI("[phys]   %s", line.c_str());
}
}  // namespace

void TrackerReset() {
    g_list.clear(); g_rigs.clear(); g_seen.clear(); g_playerRig.clear();
}

const std::vector<TrackedChr>& TrackerList() { return g_list; }

int TrackerBoneIndex(uint8_t* hkb, const std::string& name) {
    const RigInfo* r = Rig(hkb);
    if (!r) return -1;
    auto it = r->index.find(name);
    return it == r->index.end() ? -1 : it->second;
}

void TrackerUpdate(uint8_t* player, float radius) {
    g_list.clear();
    if (!player) return;
    if (g_playerRig.empty()) {
        std::vector<int16_t> parents;
        ervr::GameERHkbSkeleton(ervr::GameERChrHkbCharacter(player), g_playerRig, parents);
    }
    ervr::Vec3 me;
    if (!ervr::GameERChrPosition(player, &me)) return;
    uint8_t* wcm = ervr::GameERWorldChrMan();
    uint8_t *first = nullptr, *last = nullptr;
    if (!wcm || !SafeRead(wcm + ervr::GameWorldChrManOffset(WCM_ChrByDistFirst), &first, 8) ||
        !SafeRead(wcm + ervr::GameWorldChrManOffset(WCM_ChrByDistLast), &last, 8) || !first || last < first) return;
    const size_t count = (size_t)(last - first) / ChrDist_Stride;
    if (count > 512) return;   // torn read / not a vector

    for (size_t i = 0; i < count; ++i) {
        uint8_t* chr = nullptr;
        if (!SafeRead(first + i * ChrDist_Stride, &chr, 8) || !chr || chr == player) continue;
        ervr::Vec3 p;
        if (!ervr::GameERChrPosition(chr, &p)) continue;
        const float d = (p - me).Length();
        if (!std::isfinite(d) || d > radius) continue;
        TrackedChr t;
        t.chr = chr; t.distance = d;
        SafeRead(chr + Chr_NpcId, &t.npcId, 4);
        if (uint8_t* mods = Deref(chr + Chr_Modules)) {
            if (uint8_t* data = Deref(mods + Mod_Data)) {
                SafeRead(data + Data_Hp, &t.hp, 4);
                SafeRead(data + Data_MaxHp, &t.maxHp, 4);
            }
        }
        t.hkb = ervr::GameERChrHkbCharacter(chr);
        if (const RigInfo* r = Rig(t.hkb)) {
            t.boneCount = (int)r->names.size();
            t.sameRigAsPlayer = !g_playerRig.empty() && r->names == g_playerRig;
            if (g_seen.insert(chr).second) {
                LOGI("[phys] near: npc %d at %.1f m, hp %d/%d, %d bones%s, chr %p hkb %p", t.npcId, d, t.hp, t.maxHp,
                     t.boneCount, t.sameRigAsPlayer ? " (player rig)" : "", (void*)chr, (void*)t.hkb);
                DumpRig(t.npcId, *r, t.sameRigAsPlayer);
            }
        }
        g_list.push_back(t);
    }
    std::sort(g_list.begin(), g_list.end(), [](const TrackedChr& a, const TrackedChr& b) { return a.distance < b.distance; });
    if (g_seen.size() > 1024) g_seen.clear();

    const double now = NowSec();
    if (now - g_lastSummary > 10.0) {
        g_lastSummary = now;
        std::string s;
        for (size_t i = 0; i < g_list.size() && i < 6; ++i) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "npc %d %.1fm hp %d %s; ", g_list[i].npcId, g_list[i].distance, g_list[i].hp,
                          g_list[i].hkb ? (g_list[i].sameRigAsPlayer ? "player-rig" : "own-rig") : "no-skeleton");
            s += buf;
        }
        LOGI("[phys] %d within %.0f m: %s", (int)g_list.size(), radius, s.empty() ? "(none)" : s.c_str());
    }
}

}  // namespace kinetic
