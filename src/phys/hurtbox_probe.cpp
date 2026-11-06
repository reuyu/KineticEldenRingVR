#include "hurtbox_probe.h"

#include "chr_tracker.h"
#include "config.h"
#include "game_er.h"
#include "game_memory.h"
#include "log.h"
#include "params_er.h"
#include "rtti.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
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
    if (!PtrLike(obj) || !ervr::SafeRead((void*)obj, &vt, 8) || !InModule(vt)) return {};
    uintptr_t col = 0;
    if (!ervr::SafeRead((void*)(vt - 8), &col, 8) || !InModule(col)) return {};
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

// ---------------------------------------------------------------------------------------------------------------
// Weapon dummy points. FLVER2 layout (SoulsFormats): header 0x80 (0x14 dummy count, 0x18 material count,
// 0x1C bone count); dummies 0x40 each from 0x80 (pos 3f, colour, forward 3f, int16 refId, int16 parentBone,
// upward 3f, int16 attachBone, ...); materials 0x20 each; bones 0x80 each (translation 3f, int nameOffset,
// rotation 3f (euler), int16 parent, int16 child, scale 3f, ...); names UTF-16 at file offsets.

namespace kinetic {

namespace {

std::set<int> g_dmyDone;

std::string WName(uintptr_t at) {
    wchar_t w[48] = {};
    const size_t room = 0x1000 - (at & 0xFFF);
    if (!ervr::ProbeRead((void*)at, w, std::min(room, sizeof(w) - 2))) return {};
    std::string s;
    for (int i = 0; i < 47 && w[i]; ++i) s += (char)(w[i] < 128 ? w[i] : '?');
    return s;
}

bool IsFlver(uintptr_t p) {
    char m[8] = {};
    return PtrLike(p) && ervr::ProbeRead((void*)p, m, 8) && std::memcmp(m, "FLVER\0L\0", 8) == 0;
}

void DumpFlver(uintptr_t f, const std::string& path, int npc) {
    int32_t h[8] = {};
    ervr::ProbeRead((void*)(f + 0x08), h, sizeof(h));
    const int ver = h[0], nd = h[3], nm = h[4], nb = h[5];
    LOGI("[dmy] npc %d: FLVER at %s (%p) version 0x%x, %d dummies, %d materials, %d bones", npc, path.c_str(), (void*)f, ver, nd, nm, nb);
    if (nd < 0 || nd > 2000 || nb < 0 || nb > 2000 || nm < 0 || nm > 2000) return;
    const uintptr_t bones = f + 0x80 + (uintptr_t)nd * 0x40 + (uintptr_t)nm * 0x20;
    std::vector<std::string> bname(nb);
    struct Bone { float t[3]; int name; float r[3]; int16_t parent, child; float s[3]; };
    std::vector<Bone> bs(nb);
    for (int i = 0; i < nb; ++i) {
        uint8_t raw[0x80];
        if (!ervr::ProbeRead((void*)(bones + (uintptr_t)i * 0x80), raw, sizeof(raw))) return;
        std::memcpy(&bs[i], raw, sizeof(Bone));
        bname[i] = WName(f + (uint32_t)bs[i].name);
    }
    auto isWeapon = [&](int b) { return b >= 0 && b < nb && bname[b].find("Weapon") != std::string::npos; };
    int shown = 0;
    for (int i = 0; i < nd && shown < 60; ++i) {
        float d[16];
        if (!ervr::ProbeRead((void*)(f + 0x80 + (uintptr_t)i * 0x40), d, sizeof(d))) return;
        int16_t ref, parent, attach;
        std::memcpy(&ref, (char*)d + 0x1C, 2); std::memcpy(&parent, (char*)d + 0x1E, 2); std::memcpy(&attach, (char*)d + 0x2C, 2);
        if (!isWeapon(attach) && !isWeapon(parent)) continue;
        ++shown;
        LOGI("[dmy]   dummy %d ref %d parent %d:%s attach %d:%s pos (%.3f %.3f %.3f) fwd (%.2f %.2f %.2f) up (%.2f %.2f %.2f)", i, ref, parent,
             parent >= 0 && parent < nb ? bname[parent].c_str() : "-", attach, attach >= 0 && attach < nb ? bname[attach].c_str() : "-",
             d[0], d[1], d[2], d[4], d[5], d[6], d[8], d[9], d[10]);
    }
    LOGI("[dmy]   %d weapon dummies", shown);
    // the weapon bones and their chain to the root (bind pose, local)
    std::set<int> chain;
    for (int i = 0; i < nb; ++i)
        if (isWeapon(i)) for (int b = i, k = 0; b >= 0 && b < nb && k < 64; b = bs[b].parent, ++k) chain.insert(b);
    for (int b : chain)
        LOGI("[dmy]   bone %d %s parent %d t (%.3f %.3f %.3f) r (%.3f %.3f %.3f) s (%.2f %.2f %.2f)", b, bname[b].c_str(), bs[b].parent,
             bs[b].t[0], bs[b].t[1], bs[b].t[2], bs[b].r[0], bs[b].r[1], bs[b].r[2], bs[b].s[0], bs[b].s[1], bs[b].s[2]);
}

// Breadth-first over the pointers in each object's first 0x400 bytes, depth <= 4, for a FLVER file in memory.
[[maybe_unused]] bool FindFlver(const std::vector<std::pair<uintptr_t, std::string>>& roots, int npc) {
    std::deque<Node> q;
    std::set<uintptr_t> seen;
    for (auto& r : roots) if (PtrLike(r.first)) { q.push_back({r.first, 0, r.second}); seen.insert(r.first); }
    int visited = 0;
    while (!q.empty() && visited < 600) {
        Node n = q.front(); q.pop_front(); ++visited;
        if (IsFlver(n.addr)) { DumpFlver(n.addr, n.path, npc); return true; }
        if (n.depth >= 4) continue;
        uintptr_t blob[0x80];
        if (!ervr::ProbeRead((void*)n.addr, blob, sizeof(blob))) { if (!ervr::ProbeRead((void*)n.addr, blob, 0x100)) continue; std::memset(blob + 0x20, 0, 0x300); }
        for (int i = 0; i < 0x80; ++i) {
            const uintptr_t v = blob[i];
            if (!PtrLike(v) || InModule(v) || seen.count(v)) continue;
            seen.insert(v);
            char p[256]; snprintf(p, sizeof(p), "%s+0x%x", n.path.c_str(), i * 8);
            if (IsFlver(v)) { DumpFlver(v, p, npc); return true; }
            const std::string nm = NameAt(v);
            if (!nm.empty() || n.depth < 2) q.push_back({v, n.depth + 1, nm.empty() ? std::string(p) : std::string(p) + "(" + nm + ")"});
        }
    }
    LOGI("[dmy] npc %d: no FLVER within %d objects", npc, visited);
    return false;
}

// One object: every 8-byte slot of its first 0x200 bytes. Pointers to named objects by name (and walked one level
// further), (begin, end) pairs as vectors with their first elements both as floats and as int16s, and any vector
// whose size is a multiple of 0x40 also read as FLVER dummies (pos, refId at 0x1C, parent 0x1E, attach 0x2C).
// SafeRead only: fast (ProbeRead's VirtualQuery made the last probe stall for seconds).
void DumpObj(uintptr_t obj, const std::string& path, int npc, int depth) {
    if (!PtrLike(obj)) return;
    uintptr_t q[0x40] = {};
    if (!ervr::SafeRead((void*)obj, q, sizeof(q))) { LOGI("[dmy] %s @%p unreadable", path.c_str(), (void*)obj); return; }
    LOGI("[dmy] npc %d %s @%p = %s", npc, path.c_str(), (void*)obj, NameAt(obj).c_str());
    for (int i = 1; i < 0x40; ++i) {
        const uintptr_t v = q[i];
        if (!PtrLike(v) || InModule(v)) {
            // small scalars: show as two ints / two floats
            if (v && v < 0x10000000000ull) {
                int32_t a = (int32_t)(v & 0xFFFFFFFF), b = (int32_t)(v >> 32); float fa, fb; std::memcpy(&fa, &a, 4); std::memcpy(&fb, &b, 4);
                LOGI("[dmy]   %s+0x%x: int %d %d / float %.4g %.4g", path.c_str(), i * 8, a, b, fa, fb);
            }
            continue;
        }
        const std::string nm = NameAt(v);
        if (!nm.empty()) {
            LOGI("[dmy]   %s+0x%x -> %s @%p", path.c_str(), i * 8, nm.c_str(), (void*)v);
            if (depth < 1) { char p[256]; snprintf(p, sizeof(p), "%s+0x%x", path.c_str(), i * 8); DumpObj(v, p, npc, depth + 1); }
            continue;
        }
        const uintptr_t e = i + 1 < 0x40 ? q[i + 1] : 0;
        const bool vec = PtrLike(e) && e > v && e - v <= 0x40000;
        float f[24] = {}; int16_t h[24] = {};
        ervr::SafeRead((void*)v, f, sizeof(f)); std::memcpy(h, f, sizeof(h));
        char fb[512]; int o = 0;
        for (int k = 0; k < 24; ++k) o += snprintf(fb + o, sizeof(fb) - o, "%s%.4g", k ? " " : "", f[k]);
        char hb[256]; int o2 = 0;
        for (int k = 0; k < 24; ++k) o2 += snprintf(hb + o2, sizeof(hb) - o2, "%s%d", k ? " " : "", h[k]);
        LOGI("[dmy]   %s+0x%x -> %s%p (%d bytes): f[%s] h[%s]", path.c_str(), i * 8, vec ? "vector " : "", (void*)v, vec ? (int)(e - v) : -1, fb, hb);
        if (vec && (e - v) % 0x40 == 0 && (e - v) / 0x40 <= 400) {
            const int n = (int)((e - v) / 0x40);
            int shown = 0;
            for (int k = 0; k < n && shown < 40; ++k) {
                float d[16];
                if (!ervr::SafeRead((void*)(v + (uintptr_t)k * 0x40), d, sizeof(d))) break;
                int16_t ref, par, att;
                std::memcpy(&ref, (char*)d + 0x1C, 2); std::memcpy(&par, (char*)d + 0x1E, 2); std::memcpy(&att, (char*)d + 0x2C, 2);
                LOGI("[dmy]     as dummy %d: pos (%.3f %.3f %.3f) fwd (%.2f %.2f %.2f) ref %d parent %d attach %d up (%.2f %.2f %.2f)", k,
                     d[0], d[1], d[2], d[4], d[5], d[6], ref, par, att, d[8], d[9], d[10]);
                ++shown;
            }
        }
        if (vec) ++i;   // the end pointer is not an object of its own
    }
}

// CSFD4FlverModelData, from probe v2: +0x18 int32 count (409 for npc 4311) and int32 (178), +0x20 a table of
// 0x30-byte entries (two float4 with w = 1, a pointer, four int16), +0x30 the bone names as UTF-16 strings one
// after another. Dump all of it: every entry (both float4, the pointer, the int16s) and every name with its index.
[[maybe_unused]] void DumpDummyTable(uintptr_t fl, int npc) {
    if (!PtrLike(fl)) return;
    int32_t cnt[2] = {};
    uintptr_t tab = 0, names = 0;
    ervr::SafeRead((void*)(fl + 0x18), cnt, 8);
    ervr::SafeRead((void*)(fl + 0x20), &tab, 8);
    ervr::SafeRead((void*)(fl + 0x30), &names, 8);
    LOGI("[dmy] npc %d flverdata %p: +0x18 = %d, +0x1C = %d, table %p, names %p", npc, (void*)fl, cnt[0], cnt[1], (void*)tab, (void*)names);
    // names: consecutive NUL-terminated UTF-16 strings
    if (PtrLike(names)) {
        std::vector<wchar_t> w(0x4000);
        if (ervr::SafeRead((void*)names, w.data(), w.size() * 2)) {
            std::string line; int idx = 0; size_t i = 0;
            while (i < w.size() && idx < 400) {
                std::string nm;
                while (i < w.size() && w[i]) { nm += (char)(w[i] < 128 ? w[i] : '?'); ++i; }
                ++i;
                if (nm.empty()) break;
                line += std::to_string(idx++) + ":" + nm + " ";
                if (line.size() > 1500) { LOGI("[dmy]   names %s", line.c_str()); line.clear(); }
            }
            if (!line.empty()) LOGI("[dmy]   names %s", line.c_str());
            LOGI("[dmy]   %d names", idx);
        }
    }
    const int n = cnt[0] > 0 && cnt[0] <= 2000 ? cnt[0] : 0;
    for (int k = 0; k < n && PtrLike(tab); ++k) {
        float f[12];
        if (!ervr::SafeRead((void*)(tab + (uintptr_t)k * 0x30), f, sizeof(f))) break;
        uintptr_t ptr; std::memcpy(&ptr, &f[8], 8);
        int16_t h[4]; std::memcpy(h, &f[10], 8);
        // what the pointer holds: 8 int32 and as UTF-16 text
        int32_t pv[8] = {}; std::string txt;
        if (PtrLike(ptr) && ervr::SafeRead((void*)ptr, pv, sizeof(pv))) {
            const wchar_t* wc = (const wchar_t*)pv;
            for (int j = 0; j < 16 && wc[j] >= 32 && wc[j] < 127; ++j) txt += (char)wc[j];
        }
        LOGI("[dmy]   e%d a (%.4f %.4f %.4f %.3g) b (%.4f %.4f %.4f %.3g) p %p [%d %d %d %d %d %d %d %d '%s'] i16 %d %d %d %d", k, f[0], f[1], f[2], f[3],
             f[4], f[5], f[6], f[7], (void*)ptr, pv[0], pv[1], pv[2], pv[3], pv[4], pv[5], pv[6], pv[7], txt.c_str(), h[0], h[1], h[2], h[3]);
    }
}

// Which weapon mesh is shown. Evidence to collect:
//  - the NpcParam row's modelDispMask0..31 (bits at 0x14A..0x14D, eldenring-rs NPC_PARAM_ST bits_14a..);
//  - every UTF-16 text table CSFD4FlverModelData points at (material / mesh names carry "#NN#" mask ids);
//  - the small int tables it points at (mesh -> node), dumped whole;
//  - where in CSFD4ModelItem / CSChrModelIns / ChrIns the same 32-bit mask sits at run time.
uint32_t DumpMeshMask(uint8_t* chr, uintptr_t item, uintptr_t fl, int npc) {
    uint32_t mask = 0; bool haveMask = false;
    for (int pi = 0; pi < 200; ++pi) {
        const char* sn = ervr::ParamsStructName(pi);
        if (!sn || std::strcmp(sn, "NPC_PARAM_ST") != 0) continue;
        uint32_t sz = 0;
        int32_t paramId = -1;
        ervr::SafeRead(chr + 0x60, &paramId, 4);   // ChrIns::npc_param_id (eldenring-rs); +0x64 is the model number
        uint8_t* row = paramId > 0 ? ervr::ParamsRow(pi, (uint32_t)paramId, &sz) : nullptr;
        LOGI("[dmy] npc %d: NpcParam id %d", npc, paramId);
        if (row && sz > 0x14E && ervr::SafeRead(row + 0x14A, &mask, 4)) haveMask = true;
        LOGI("[dmy] npc %d: NpcParam (holder %d) row %p size %u, modelDispMask bits 0x%08x%s", npc, pi, (void*)row, sz, mask, haveMask ? "" : " (unread)");
        break;
    }
    if (!haveMask) LOGI("[dmy] npc %d: NpcParam not found", npc);
    // the mask at run time
    auto findMask = [&](const char* what, uintptr_t base, size_t len) {
        if (!haveMask || !PtrLike(base)) return;
        std::vector<uint8_t> b(len);
        if (!ervr::SafeRead((void*)base, b.data(), len)) return;
        for (size_t o = 0; o + 4 <= len; ++o) {
            uint32_t v; std::memcpy(&v, &b[o], 4);
            if (v == mask && mask != 0) LOGI("[dmy] npc %d: mask value at %s+0x%zx", npc, what, o);
        }
        // or as 32 bools (one byte per bit)
        for (size_t o = 0; o + 32 <= len; ++o) {
            bool same = true;
            for (int k = 0; k < 32 && same; ++k) same = b[o + k] == ((mask >> k) & 1u);
            if (same && mask != 0) LOGI("[dmy] npc %d: mask as 32 bools at %s+0x%zx", npc, what, o);
        }
    };
    uintptr_t mi = 0; ervr::SafeRead(chr + 0x50, &mi, 8);
    findMask("chr", (uintptr_t)chr, 0x2000);
    findMask("modelins", mi, 0x1000);
    findMask("item", item, 0x1000);
    findMask("flverdata", fl, 0x200);
    // (one level down from the model instance and item: nothing there either, 2026-11-05)
    return haveMask ? mask : 0u;
}

// The mesh table behind CSFD4FlverModelData (+0x88, 0x40-byte entries; the node each mesh hangs on is the int16
// at +0x36): one line per mesh with the node's name and the entry's bytes after its first float4, to see which
// field (if any) carries the modelDispMask bit of the weapon meshes.
void DumpMeshTable(uintptr_t fl, int npc, int param, uint32_t mask) {
    if (!PtrLike(fl)) return;
    int32_t n = 0;
    ervr::SafeRead((void*)(fl + 0x18), &n, 4);
    uintptr_t names = 0, mt = 0;
    ervr::SafeRead((void*)(fl + 0x30), &names, 8);
    ervr::SafeRead((void*)(fl + 0x88), &mt, 8);
    int32_t hdr[8] = {};
    ervr::SafeRead((void*)(fl + 0x80), hdr, sizeof(hdr));
    LOGI("[dmy] meshes npc %d param %d mask 0x%08x: %d nodes, table %p, flverdata+0x80..0x9f ints %d %d %d %d %d %d %d %d", npc, param, mask, n, (void*)mt,
         hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7]);
    if (n < 1 || n > 4000 || !PtrLike(names) || !PtrLike(mt)) return;
    std::vector<uint16_t> w(0x8000);
    size_t got = 0;
    for (size_t c = 0x8000; c >= 0x400; c /= 2) if (ervr::SafeRead((void*)names, w.data(), c * 2)) { got = c; break; }
    std::vector<std::string> nm;
    for (size_t k = 0; (int)nm.size() < n && k < got;) {
        std::string x;
        while (k < got && w[k]) { x += (char)(w[k] < 128 ? w[k] : '?'); ++k; }
        ++k; nm.push_back(x);
    }
    for (int i = 0; i < 400; ++i) {
        uint8_t e[0x40];
        if (!ervr::SafeRead((void*)(mt + (uintptr_t)i * 0x40), e, sizeof(e))) break;
        int16_t node; std::memcpy(&node, e + 0x36, 2);
        if (node < 0 || node >= n) { LOGI("[dmy] meshes npc %d param %d: %d entries (entry %d has node %d)", npc, param, i, i, node); break; }
        std::string hex;
        for (int b = 0x00; b < 0x40; ++b) { char h[4]; snprintf(h, sizeof(h), "%02x", e[b]); hex += h; if ((b & 3) == 3) hex += ' '; }
        LOGI("[dmy] mesh %d param %d #%d node %d %s: %s", npc, param, i, node, node < (int)nm.size() ? nm[node].c_str() : "?", hex.c_str());
    }
}

// Material names: FLVER material names carry the modelDispMask index as #NN# (soulsmodding FLVER notes; full-width
// signs in some models). Walk
// two pointer levels from the model objects and log every UTF-16 string with "#<digits>#" or a material path.
bool WideMatch(const uint16_t* w, size_t n, std::string& out) {
    out.clear();
    // c4311's material names use full-width signs (U+FF03 for #; kh-er zdraw.rs disp_group): read them as '#'.
    for (size_t k = 0; k < n && w[k]; ++k) {
        if (w[k] == 0xFF03) { out += '#'; continue; }
        if (w[k] >= 0xFF10 && w[k] <= 0xFF19) { out += (char)('0' + (w[k] - 0xFF10)); continue; }   // full-width digits
        if (w[k] < 32) return false;
        out += w[k] <= 126 ? (char)w[k] : '?';
    }
    if (out.size() < 4 || out.size() > 200) return false;
    for (size_t k = 0; k + 2 < out.size(); ++k)
        if (out[k] == '#' && isdigit((unsigned char)out[k + 1])) {
            size_t e = k + 1; while (e < out.size() && isdigit((unsigned char)out[e])) ++e;
            if (e < out.size() && out[e] == '#') return true;
        }
    return out.find(".matxml") != std::string::npos || out.find(".mtd") != std::string::npos;
}

void ScanStrings(uintptr_t at, const std::string& path, std::set<std::string>& seen, int& lines) {
    if (!PtrLike(at) || lines >= 300) return;
    static uint16_t w[0x800];
    size_t got = 0;
    for (size_t c = 0x800; c >= 0x40; c /= 2) if (ervr::SafeRead((void*)at, w, c * 2)) { got = c; break; }
    for (size_t k = 0; k < got && lines < 300; ++k) {
        if (k && w[k - 1]) continue;   // strings start after a 0 (or at the target)
        std::string x;
        if (!WideMatch(w + k, got - k, x) || seen.count(x)) continue;
        seen.insert(x); ++lines;
        LOGI("[dmy] material? %s+0x%zx: \"%s\"", path.c_str(), k * 2, x.c_str());
        k += x.size();
    }
}

void DumpMaterialNames(int npc, int param, const uintptr_t* roots, const char* const* rootNames, int nRoots) {
    std::set<std::string> seen;
    std::set<uintptr_t> visited;
    int lines = 0;
    for (int r = 0; r < nRoots; ++r) {
        uintptr_t q1[0x80] = {};
        if (!PtrLike(roots[r]) || !ervr::SafeRead((void*)roots[r], q1, sizeof(q1))) continue;
        for (int a = 0; a < 0x80; ++a) {
            if (!PtrLike(q1[a]) || !visited.insert(q1[a]).second) continue;
            char pa[96]; snprintf(pa, sizeof(pa), "%s+0x%x", rootNames[r], a * 8);
            ScanStrings(q1[a], pa, seen, lines);
            uintptr_t q2[0x40] = {};
            if (!ervr::SafeRead((void*)q1[a], q2, sizeof(q2))) continue;
            for (int b = 0; b < 0x40; ++b) {
                if (!PtrLike(q2[b]) || !visited.insert(q2[b]).second) continue;
                char pb[128]; snprintf(pb, sizeof(pb), "%s->+0x%x", pa, b * 8);
                ScanStrings(q2[b], pb, seen, lines);
            }
        }
    }
    LOGI("[dmy] material scan npc %d param %d: %d string(s) with #NN# or a material path, %zu object(s) read", npc, param, lines, visited.size());
}

// Snapshots of each probed enemy's memory: offsets whose value is the same for every enemy with the same
// modelDispMask and differs between masks are where the shown meshes may be kept.
struct Snap { int param; uint32_t mask; std::vector<std::vector<uint8_t>> reg; int npc = 0; };
std::vector<Snap> g_snaps;
const char* kRegName[5] = {"chr", "modelins", "item", "flverdata", "meshtable(flverdata+0x88)"};
const size_t kRegLen[5] = {0x2000, 0x1000, 0x2000, 0x200, 0x4000};

void SnapAndDiff(uint8_t* chr, uintptr_t mi, uintptr_t item, uintptr_t fl, int param, uint32_t mask, int npc) {
    Snap sn{param, mask, {}, npc};
    uintptr_t mt = 0;
    if (PtrLike(fl)) ervr::SafeRead((void*)(fl + 0x88), &mt, 8);
    const uintptr_t base[5] = {(uintptr_t)chr, mi, item, fl, mt};
    for (int r = 0; r < 5; ++r) {
        std::vector<uint8_t> b(kRegLen[r], 0);
        if (!PtrLike(base[r]) || !ervr::SafeRead((void*)base[r], b.data(), b.size())) b.clear();
        sn.reg.push_back(std::move(b));
    }
    LOGI("[dmy] npc %d param %d: flverdata %p, mesh table %p", npc, param, (void*)fl, (void*)mt);
    g_snaps.push_back(std::move(sn));
    // compare enemies of the same model only
    std::vector<Snap> allSnaps = g_snaps;
    struct Restore { std::vector<Snap>& to; std::vector<Snap> keep; ~Restore() { to = std::move(keep); } } restore{g_snaps, std::move(allSnaps)};
    g_snaps.erase(std::remove_if(g_snaps.begin(), g_snaps.end(), [&](const Snap& x) { return x.npc != npc; }), g_snaps.end());
    std::set<uint32_t> masks;
    for (const auto& x : g_snaps) masks.insert(x.mask);
    if (masks.size() < 2) return;
    for (int r = 0; r < 5; ++r) {
        size_t len = kRegLen[r];
        bool all = true;
        for (const auto& x : g_snaps) if (x.reg[r].size() != len) all = false;
        if (!all) continue;
        int shown = 0;
        std::string line;
        for (size_t o = 0; o < len && shown < 80; ++o) {
            bool ok = true, differs = false;
            for (size_t i = 0; i < g_snaps.size() && ok; ++i)
                for (size_t j = i + 1; j < g_snaps.size() && ok; ++j) {
                    const bool same = g_snaps[i].reg[r][o] == g_snaps[j].reg[r][o];
                    if (g_snaps[i].mask == g_snaps[j].mask && !same) ok = false;
                    if (g_snaps[i].mask != g_snaps[j].mask && !same) differs = true;
                    if (g_snaps[i].mask != g_snaps[j].mask && same) ok = false;
                }
            if (!ok || !differs) continue;
            char e[32]; snprintf(e, sizeof(e), " +0x%zx:", o);
            line += e;
            for (const auto& x : g_snaps) { char v[8]; snprintf(v, sizeof(v), "%02x/", x.reg[r][o]); line += v; }
            ++shown;
        }
        std::string who;
        for (const auto& x : g_snaps) { char v[48]; snprintf(v, sizeof(v), " %d(0x%08x)", x.param, x.mask); who += v; }
        LOGI("[dmy] mask diff %s over%s: %d byte(s) follow the mask:%s", kRegName[r], who.c_str(), shown, line.c_str());
    }
}
}  // namespace

// Test of a reading (not used by the game logic): each weapon mesh's int16 at mesh entry +0x32 as "group*10 + part"
// against the mask the enemy holds at run time (CSChrModelIns+0x138, the NpcParam modelDispMask copy; TAE
// ChangeChrDrawMask events can change it, e.g. drawing a weapon). Logged per enemy, and again when its mask changes.
static void MaskReadingFrame() {
    static std::map<uintptr_t, uint32_t> last;
    static int lines = 0;
    if (lines > 400) return;
    for (const TrackedChr& t : TrackerList()) {
        if (!t.chr || t.distance > 8.f || t.boneCount < 20) continue;
        uintptr_t mi = 0, item = 0, fl = 0, mt = 0, names = 0;
        uint32_t mask = 0; int32_t paramId = -1, n = 0;
        ervr::SafeRead(t.chr + 0x60, &paramId, 4);
        ervr::SafeRead(t.chr + 0x50, &mi, 8);
        if (!PtrLike(mi) || !ervr::SafeRead((void*)(mi + 0x138), &mask, 4)) continue;
        auto it = last.find((uintptr_t)t.chr);
        if (it != last.end() && it->second == mask) continue;
        const bool changed = it != last.end();
        const uint32_t was = changed ? it->second : 0;
        last[(uintptr_t)t.chr] = mask;
        ervr::SafeRead((void*)(mi + 0x10), &item, 8);
        if (PtrLike(item)) ervr::SafeRead((void*)(item + 0x68), &fl, 8);
        if (!PtrLike(fl)) continue;
        ervr::SafeRead((void*)(fl + 0x18), &n, 4);
        ervr::SafeRead((void*)(fl + 0x30), &names, 8);
        ervr::SafeRead((void*)(fl + 0x88), &mt, 8);
        if (n < 1 || n > 4000 || !PtrLike(names) || !PtrLike(mt)) continue;
        static std::vector<uint16_t> w(0x8000);
        size_t got = 0;
        for (size_t c = 0x8000; c >= 0x400; c /= 2) if (ervr::SafeRead((void*)names, w.data(), c * 2)) { got = c; break; }
        std::vector<std::string> nm;
        for (size_t k = 0; (int)nm.size() < n && k < got;) { std::string x; while (k < got && w[k]) { x += (char)(w[k] < 128 ? w[k] : '?'); ++k; } ++k; nm.push_back(x); }
        std::string shown, hidden;
        for (int i = 0; i < 400; ++i) {
            uint8_t e[0x40];
            if (!ervr::SafeRead((void*)(mt + (uintptr_t)i * 0x40), e, sizeof(e))) break;
            int16_t node; uint16_t grp; std::memcpy(&node, e + 0x36, 2); std::memcpy(&grp, e + 0x32, 2);
            if (node < 0 || node >= n) break;
            if (node >= (int)nm.size() || nm[node].find("Weapon") == std::string::npos) continue;
            const int g = grp / 10;
            char b[96]; snprintf(b, sizeof(b), " %s(%u->bit %d, sec %02x)", nm[node].c_str(), grp, g, e[0x34]);
            ((g < 32 && (mask >> g) & 1) ? shown : hidden) += b;
        }
        ++lines;
        char wasS[32] = "";
        if (changed) snprintf(wasS, sizeof(wasS), " (was 0x%08x)", was);
        LOGI("[mask] chr %p param %d dist %.1f m: mask 0x%08x%s | weapon meshes whose bit is set:%s | not set:%s", (void*)t.chr, paramId, t.distance,
             mask, wasS, shown.empty() ? " none" : shown.c_str(), hidden.empty() ? " none" : hidden.c_str());
    }
}

void DummyProbeFrame() {
    if (!ervr::Cfg().physicsDummyProbe) return;
    MaskReadingFrame();
    if (g_dmyDone.size() >= 12) return;
    for (const TrackedChr& t : TrackerList()) {
        if (!t.chr || t.distance > 6.f || t.boneCount < 20) continue;
        // once per NpcParam row: enemies of one model with different weapons have different rows
        int32_t paramId = -1;
        ervr::SafeRead(t.chr + 0x60, &paramId, 4);
        if (g_dmyDone.count(paramId)) continue;
        g_dmyDone.insert(paramId);
        uintptr_t mi = 0, res = 0, item = 0;
        ervr::SafeRead(t.chr + 0x50, &mi, 8);
        ervr::SafeRead(t.chr + 0x28, &res, 8);
        if (PtrLike(mi)) ervr::SafeRead((void*)(mi + 0x10), &item, 8);
        LOGI("[dmy] npc %d: model ins %p (%s), model item %p (%s), chr res %p (%s)", t.npcId, (void*)mi, NameAt(mi).c_str(), (void*)item,
             NameAt(item).c_str(), (void*)res, NameAt(res).c_str());
        if (PtrLike(item)) {
            uintptr_t fl = 0, dm = 0;
            ervr::SafeRead((void*)(item + 0x68), &fl, 8);
            ervr::SafeRead((void*)(item + 0x650), &dm, 8);
            const uint32_t mask = DumpMeshMask(t.chr, item, fl, t.npcId);
            DumpMeshTable(fl, t.npcId, paramId, mask);
            static std::set<int> matDone;   // the material names are per model
            if (matDone.insert(t.npcId).second) {
                const uintptr_t roots[4] = {fl, item, mi, res};
                const char* const rn[4] = {"flverdata", "item", "modelins", "chrres"};
                DumpMaterialNames(t.npcId, paramId, roots, rn, 4);
            }
            SnapAndDiff(t.chr, mi, item, fl, paramId, mask, t.npcId);
        }
        return;   // one per frame
    }
}

}  // namespace kinetic
