#include "hurtbox_probe.h"

#include "chr_tracker.h"
#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "log.h"
#include "rtti.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <vector>

// Kinetic: find where ELDEN RING keeps a character's own body shapes (the Havok ragdoll the game hits and is hit
// with) by walking the objects reachable from its ChrCtrl and naming each by its RTTI. Read-only, once per
// enemy kind, logged under "[hurt]". What it finds tells the next build how to read the shapes directly.

namespace kinetic {

namespace {

std::set<int> g_done;

uint8_t* ModuleBase() { static uint8_t* b = (uint8_t*)GetModuleHandleW(nullptr); return b; }
bool InModule(uintptr_t p) { return p >= (uintptr_t)ModuleBase() && p < (uintptr_t)ModuleBase() + 0x10000000ull; }
bool PtrLike(uintptr_t v) { return v >= 0x10000 && v < 0x7FFFFFFFFFFFull && !(v & 7); }

std::string NameAt(uintptr_t obj) {
    uintptr_t vt = 0;
    if (!PtrLike(obj) || !ervr::ProbeRead((void*)obj, &vt, 8) || !InModule(vt)) return {};
    uintptr_t col = 0;
    if (!ervr::ProbeRead((void*)(vt - 8), &col, 8) || !InModule(col)) return {};
    return ervr::RttiNameOfVTable((void**)vt);
}

bool Interesting(const std::string& n) {
    std::string l = n;
    for (auto& ch : l) ch = (char)std::tolower((unsigned char)ch);
    for (const char* k : {"ragdoll", "hkn", "hka", "hkp", "collision", "shape", "body", "phys", "hit", "capsule", "rigid", "world", "skeleton"})
        if (l.find(k) != std::string::npos) return true;
    return false;
}

void DumpFloats(const char* tag, uintptr_t at, int bytes) {
    float f[32] = {};
    bytes = std::min(bytes, (int)sizeof(f));
    if (!ervr::ProbeRead((void*)at, f, bytes)) return;
    char buf[1024]; int o = 0;
    for (int i = 0; i < bytes / 4 && o < (int)sizeof(buf) - 16; ++i) o += snprintf(buf + o, sizeof(buf) - o, "%s%.3g", i ? " " : "", f[i]);
    LOGI("[hurt]     %s floats @%p: %s", tag, (void*)at, buf);
}

struct Node { uintptr_t addr; int depth; std::string path; };

void Walk(uintptr_t root, const char* rootName, int& lines) {
    std::deque<Node> q;
    std::set<uintptr_t> seen;
    q.push_back({root, 0, rootName});
    seen.insert(root);
    while (!q.empty() && lines < 700) {
        Node n = q.front(); q.pop_front();
        const std::string nm = NameAt(n.addr);
        LOGI("[hurt] %s @%p = %s", n.path.c_str(), (void*)n.addr, nm.empty() ? "?" : nm.c_str()); ++lines;
        std::string l = nm; for (auto& ch : l) ch = (char)std::tolower((unsigned char)ch);
        if (l.find("shape") != std::string::npos || l.find("capsule") != std::string::npos) { DumpFloats("shape", n.addr, 0x80); ++lines; }
        if (n.depth >= 4) continue;
        uint8_t blob[0x300];
        int size = (int)sizeof(blob);
        if (!ervr::ProbeRead((void*)n.addr, blob, size)) { size = 0x100; if (!ervr::ProbeRead((void*)n.addr, blob, size)) continue; }
        for (int off = 8; off + 8 <= size && lines < 700; off += 8) {
            uintptr_t v; std::memcpy(&v, blob + off, 8);
            if (!PtrLike(v) || InModule(v)) continue;
            const std::string cn = NameAt(v);
            char path[256];
            if (!cn.empty()) {
                if (!Interesting(cn)) continue;
                LOGI("[hurt]   %s+0x%x -> %s", n.path.c_str(), off, cn.c_str()); ++lines;
                if (!seen.count(v)) { seen.insert(v); snprintf(path, sizeof(path), "%s+0x%x", n.path.c_str(), off); q.push_back({v, n.depth + 1, path}); }
                continue;
            }
            // hkArray<T> {T* data; int size; int capacityAndFlags}: a data pointer followed by a small count.
            if (off + 16 > size) continue;
            int cnt; std::memcpy(&cnt, blob + off + 8, 4);
            int cap; std::memcpy(&cap, blob + off + 12, 4);
            if (cnt < 1 || cnt > 512 || (cap & 0x3FFFFFFF) < cnt) continue;
            uintptr_t e[4] = {};
            if (!ervr::ProbeRead((void*)v, e, sizeof(e))) continue;
            std::string en = NameAt(e[0]);
            if (!en.empty()) {
                LOGI("[hurt]   %s+0x%x -> array[%d] of %s* (%s, %s)", n.path.c_str(), off, cnt, en.c_str(), NameAt(e[1]).c_str(), NameAt(e[2]).c_str()); ++lines;
                if (Interesting(en))
                    for (int i = 0; i < 2 && i < cnt; ++i)
                        if (!seen.count(e[i])) { seen.insert(e[i]); snprintf(path, sizeof(path), "%s+0x%x[%d]", n.path.c_str(), off, i); q.push_back({e[i], n.depth + 1, path}); }
            } else if (Interesting(nm)) {
                LOGI("[hurt]   %s+0x%x -> array[%d] (cap %d) of plain data", n.path.c_str(), off, cnt, cap & 0x3FFFFFFF); ++lines;
                DumpFloats("data", v, 0x60); ++lines;
            }
        }
    }
}

}  // namespace

void HurtboxProbeFrame(uint8_t* player) {
    const auto& c = ervr::Cfg();
    if (!c.physicsHurtboxProbe || !player || g_done.size() >= 4) return;
    if (!g_done.count(-1)) {
        // Your own character first: the same shapes decide where enemies hit you.
        g_done.insert(-1);
        uint8_t* ctrl = ervr::GameERChrCtrl(player);
        LOGI("[hurt] probe player: chr %p ctrl %p", (void*)player, (void*)ctrl);
        int lines = 0;
        if (ctrl) {
            uintptr_t rag = 0;
            ervr::ProbeRead(ctrl + 0x28, &rag, 8);
            if (PtrLike(rag)) Walk(rag, "player.ctrl.ragdoll", lines);
        }
        LOGI("[hurt] probe player done (%d lines)", lines);
        return;
    }
    for (const TrackedChr& t : TrackerList()) {
        if (!t.chr || !t.hkb || t.distance > 4.f || g_done.count(t.npcId)) continue;
        g_done.insert(t.npcId);
        ervr::Vec3 rp; ervr::Quat rq;
        const bool root = ervr::GameERChrModelRoot(t.chr, &rp, &rq);
        uint8_t* ctrl = ervr::GameERChrCtrl(t.chr);
        LOGI("[hurt] probe npc %d at %.1f m: chr %p ctrl %p, model root (%.2f %.2f %.2f)%s", t.npcId, t.distance, (void*)t.chr, (void*)ctrl,
             rp.x, rp.y, rp.z, root ? "" : " (none)");
        const int pel = TrackerBoneIndex(t.hkb, "Pelvis"), head = TrackerBoneIndex(t.hkb, "Head");
        LOGI("[hurt] bones: Pelvis #%d Head #%d of %d", pel, head, t.boneCount);
        int lines = 0;
        if (ctrl) {
            uintptr_t rag = 0, col = 0;
            ervr::ProbeRead(ctrl + 0x28, &rag, 8);
            ervr::ProbeRead(ctrl + 0x30, &col, 8);
            if (PtrLike(rag)) Walk(rag, "ctrl.ragdoll", lines);
            if (PtrLike(col)) Walk(col, "ctrl.collision", lines);
            Walk((uintptr_t)ctrl, "ctrl", lines);
        }
        Walk((uintptr_t)t.chr, "chr", lines);
        LOGI("[hurt] probe npc %d done (%d lines)", t.npcId, lines);
        return;   // one per frame
    }
}

void HurtboxProbeReset() {}

}  // namespace kinetic
