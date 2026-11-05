#include "game_shapes.h"

#include "chr_tracker.h"
#include "game_er.h"
#include "game_memory.h"
#include "rtti.h"

#include <windows.h>
#include <cmath>
#include <cstring>

// Layout, from the "[hurt]" probe of fe61b8e (enemy 4311 and the player):
//   ChrCtrl+0x28 CS::CSRagdollIns: +0x68 / +0x70 hkaSkeletonMapper (ragdoll->anim, anim->ragdoll), +0x78 hknpRagdoll
//   hknpRagdoll: +0x18 hknpRagdollData, +0x50 hkaSkeleton (the ragdoll skeleton, 18-24 bones)
//   hknpRagdollData: +0x38 hkArray<hknpCapsuleShape*> (one per body), +0x70 hkArray<int> bone -> body (-1: none)
//   hknpCapsuleShape: +0x20 radius, +0x60 / +0x70 end points A / B (body frame)
//   hkaSkeleton: +0x30 hkArray<hkaBone> (16 bytes, name pointer first)
//   hkaSkeletonMapper: +0x20 skeleton A, +0x28 skeleton B, +0x60 hkArray<SimpleMapping> (0x40 bytes: int16 boneA,
//     int16 boneB, hkQsTransform aFromB at +0x10; pose of B = pose of A * aFromB, model space)

namespace kinetic {

namespace {

using ervr::Quat;
using ervr::Vec3;

uint8_t* ModuleBase() { static uint8_t* b = (uint8_t*)GetModuleHandleW(nullptr); return b; }
bool InModule(uintptr_t p) { return p >= (uintptr_t)ModuleBase() && p < (uintptr_t)ModuleBase() + 0x10000000ull; }
bool PtrLike(uintptr_t v) { return v >= 0x10000 && v < 0x7FFFFFFFFFFFull && !(v & 7); }

uintptr_t Ptr(uintptr_t at) {
    uintptr_t v = 0;
    if (!PtrLike(at) || !ervr::ProbeRead((void*)at, &v, 8) || !PtrLike(v)) return 0;
    return v;
}

std::string NameAt(uintptr_t obj) {
    uintptr_t vt = 0, col = 0;
    if (!PtrLike(obj) || !ervr::ProbeRead((void*)obj, &vt, 8) || !InModule(vt)) return {};
    if (!ervr::ProbeRead((void*)(vt - 8), &col, 8) || !InModule(col)) return {};
    return ervr::RttiNameOfVTable((void**)vt);
}

bool Is(uintptr_t obj, const char* name) { return obj && NameAt(obj) == name; }

// hkArray {T* data; int size; int capacityAndFlags}
bool Array(uintptr_t at, uintptr_t& data, int& n, int maxN) {
    data = Ptr(at);
    n = 0;
    if (!data || !ervr::ProbeRead((void*)(at + 8), &n, 4)) return false;
    return n > 0 && n <= maxN;
}

std::string Str(uintptr_t p) {
    p &= ~(uintptr_t)1;   // hkStringPtr keeps a flag in bit 0
    char s[64] = {};
    if (!PtrLike(p & ~(uintptr_t)7) || !ervr::ProbeRead((void*)p, s, sizeof(s) - 1)) {
        // the tail may cross into an unreadable page: try shorter
        if (!ervr::ProbeRead((void*)p, s, 16)) return {};
        s[16] = 0;
    }
    s[sizeof(s) - 1] = 0;
    return s;
}

std::vector<std::string> BoneNames(uintptr_t skel) {
    std::vector<std::string> out;
    uintptr_t d; int n;
    if (!Array(skel + 0x30, d, n, 1024)) return out;
    std::vector<uintptr_t> raw((size_t)n * 2);
    if (!ervr::ProbeRead((void*)d, raw.data(), raw.size() * 8)) return out;
    out.resize(n);
    for (int i = 0; i < n; ++i) out[i] = Str(raw[(size_t)i * 2]);
    return out;
}

bool Finite(float f) { return std::isfinite(f) && std::fabs(f) < 100.f; }

}  // namespace

uintptr_t GameShapesSource(uint8_t* chr) {
    uint8_t* ctrl = chr ? ervr::GameERChrCtrl(chr) : nullptr;
    if (!ctrl) return 0;
    const uintptr_t rag = Ptr((uintptr_t)ctrl + 0x28);
    const uintptr_t np = rag ? Ptr(rag + 0x78) : 0;
    return np ? Ptr(np + 0x18) : 0;
}

bool GameShapesRead(uint8_t* chr, uint8_t* hkb, std::vector<GameCapsule>& out, std::string& why) {
    out.clear();
    uint8_t* ctrl = chr ? ervr::GameERChrCtrl(chr) : nullptr;
    if (!ctrl || !hkb) { why = "no ChrCtrl"; return false; }
    const uintptr_t rag = Ptr((uintptr_t)ctrl + 0x28);
    if (!Is(rag, "CS::CSRagdollIns")) { why = "no CSRagdollIns at ChrCtrl+0x28"; return false; }
    const uintptr_t np = Ptr(rag + 0x78);
    if (!Is(np, "hknpRagdoll")) { why = "no hknpRagdoll at +0x78"; return false; }
    const uintptr_t data = Ptr(np + 0x18), rskel = Ptr(np + 0x50);
    if (!Is(data, "hknpRagdollData") || !Is(rskel, "hkaSkeleton")) { why = "no ragdoll data / skeleton"; return false; }

    uintptr_t shp; int nb;
    if (!Array(data + 0x38, shp, nb, 64)) { why = "no shape array"; return false; }
    std::vector<uintptr_t> shapes(nb);
    if (!ervr::ProbeRead((void*)shp, shapes.data(), (size_t)nb * 8)) { why = "shape array unreadable"; return false; }
    const std::vector<std::string> rnames = BoneNames(rskel);
    const int nr = (int)rnames.size();
    if (nr < 1) { why = "ragdoll skeleton has no bones"; return false; }

    // bone -> body; without it, bone i is body i
    std::vector<int> boneBody(nr, -1);
    {
        uintptr_t d; int n; bool ok = false;
        if (Array(data + 0x70, d, n, 1024) && n == nr) {
            std::vector<int> m(n);
            if (ervr::ProbeRead((void*)d, m.data(), (size_t)n * 4)) {
                ok = true;
                for (int v : m) if (v < -1 || v >= nb) { ok = false; break; }
                if (ok) boneBody = m;
            }
        }
        if (!ok) for (int i = 0; i < nr && i < nb; ++i) boneBody[i] = i;
    }

    // ragdoll bone -> animation bone and the offset between them, from the mapper that has the ragdoll on one side
    struct Map { int anim = -1; Quat q; Vec3 t; std::string name; };
    std::vector<Map> map(nr);
    int mapped = 0;
    for (int off : {0x70, 0x68}) {
        const uintptr_t m = Ptr(rag + off);
        if (!Is(m, "hkaSkeletonMapper")) continue;
        const uintptr_t sa = Ptr(m + 0x20), sb = Ptr(m + 0x28);
        const bool animToRag = sb == rskel, ragToAnim = sa == rskel;
        if (!animToRag && !ragToAnim) continue;
        const std::vector<std::string> anames = BoneNames(animToRag ? sa : sb);
        uintptr_t d; int n;
        if (anames.empty() || !Array(m + 0x60, d, n, 1024)) continue;
        std::vector<float> raw((size_t)n * 16);
        if (!ervr::ProbeRead((void*)d, raw.data(), raw.size() * 4)) continue;
        for (int i = 0; i < n; ++i) {
            const float* e = &raw[(size_t)i * 16];
            int16_t ba, bb; std::memcpy(&ba, e, 2); std::memcpy(&bb, (const char*)e + 2, 2);
            const int rb = animToRag ? bb : ba, ab = animToRag ? ba : bb;
            if (rb < 0 || rb >= nr || ab < 0 || ab >= (int)anames.size() || map[rb].anim >= 0) continue;
            const int idx = TrackerBoneIndex(hkb, anames[ab]);
            if (idx < 0) continue;
            Vec3 t{e[4], e[5], e[6]}; Quat q = Quat{e[8], e[9], e[10], e[11]}.Normalized();
            if (!Finite(t.x) || !Finite(t.y) || !Finite(t.z)) continue;
            if (ragToAnim) { q = q.Conj(); t = q.Rotate(-t); }   // anim = rag * aFromB  ->  rag = anim * aFromB^-1
            map[rb].anim = idx; map[rb].q = q; map[rb].t = t; map[rb].name = anames[ab];
            ++mapped;
        }
        if (mapped > 0) break;
    }
    // bones the mapper did not give: the animation bone of the same name, no offset
    for (int rb = 0; rb < nr; ++rb)
        if (map[rb].anim < 0) { map[rb].anim = TrackerBoneIndex(hkb, rnames[rb]); map[rb].name = rnames[rb]; }

    int skipped = 0;
    for (int rb = 0; rb < nr; ++rb) {
        const int body = boneBody[rb];
        if (body < 0 || body >= nb || map[rb].anim < 0) { if (body >= 0) ++skipped; continue; }
        const uintptr_t s = shapes[body];
        if (!Is(s, "hknpCapsuleShape")) { ++skipped; continue; }
        float f[32];
        if (!ervr::ProbeRead((void*)s, f, sizeof(f))) { ++skipped; continue; }
        GameCapsule c;
        c.radius = f[8];
        c.a = Vec3{f[24], f[25], f[26]};
        c.b = Vec3{f[28], f[29], f[30]};
        if (!(c.radius > 0.005f && c.radius < 1.5f) || !Finite(c.a.x) || !Finite(c.a.y) || !Finite(c.a.z) || !Finite(c.b.x) ||
            !Finite(c.b.y) || !Finite(c.b.z) || c.a.Length() > 2.f || c.b.Length() > 2.f) { ++skipped; continue; }
        c.animBone = map[rb].anim; c.q = map[rb].q; c.t = map[rb].t;
        c.ragName = rnames[rb]; c.animName = map[rb].name;
        out.push_back(c);
    }
    if (out.empty()) { why = "no capsule could be placed (" + std::to_string(nb) + " bodies, " + std::to_string(mapped) + " mapped)"; return false; }
    why = std::to_string(out.size()) + " of " + std::to_string(nb) + " bodies, " + std::to_string(mapped) + " via the mapper, " + std::to_string(skipped) + " skipped";
    return true;
}

}  // namespace kinetic
