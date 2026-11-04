#include "sword_rig.h"

#include "config.h"
#include "game_er.h"
#include "log.h"
#include "phys_layer.h"

#include <windows.h>
#include <atomic>
#include <cmath>

#if KINETIC_HAS_PHYSX
#include <PxPhysicsAPI.h>
#endif

namespace kinetic {

#if !KINETIC_HAS_PHYSX

bool SwordRigActive() { return false; }
bool SwordRigStep(int, ervr::Vec3&, ervr::Quat&, float, float, float, float, float, ervr::Vec3) { return false; }
void SwordRigReset(int) {}

#else

using namespace physx;

namespace {

class ErrorSink : public PxErrorCallback {
public:
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        LOGW("[phys] PhysX error %d: %s (%s:%d)", (int)code, message ? message : "", file ? file : "", line);
    }
};

PxDefaultAllocator g_alloc;
ErrorSink g_errors;
PxFoundation* g_foundation = nullptr;
PxPhysics* g_physics = nullptr;
PxDefaultCpuDispatcher* g_dispatcher = nullptr;
bool g_failed = false;          // engine creation failed or a step faulted: stay off until restart
std::atomic<int> g_resetMask{3};

struct Params {
    PxTransform target;         // the controller this frame
    PxVec3 shaft;               // unit, controller-local
    float dt;
    float handKg, weaponKg, reach, com;
    float force, torque;        // the arm's limits this frame
    float kLin, dLin, kAng, dAng;
    float snap;
    float maxAngle;             // rad
    float load;                 // weapon weight torque / wrist torque
};

struct Rig {
    PxScene* scene = nullptr;
    PxRigidDynamic* anchor = nullptr;   // kinematic: the controller
    PxRigidDynamic* body = nullptr;     // hand + weapon
    PxD6Joint* joint = nullptr;
    bool placed = false;
    PxTransform lastTarget{PxIdentity};
    float massKey[4] = {-1, -1, -1, -1};   // weaponKg, reach, com, handKg the mass was built for
    float driveKey[6] = {-1, -1, -1, -1, -1, -1};
    // lag statistics for the periodic log
    double statSince = 0;
    float lagSum = 0, lagMax = 0, angSum = 0, angMax = 0; int statN = 0;
    int capHits = 0, subSteps = 0; float twistMax = 0;   // ConstrainOrientation: how often the cap held the blade, largest roll removed (deg)
    float lastMass = 0, lastForce = 0, lastTorque = 0, lastLoad = 0, lastCapDeg = 0;
};
Rig g_rig[2];

double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

PxVec3 ToPx(const ervr::Vec3& v) { return {v.x, v.y, v.z}; }
PxQuat ToPx(const ervr::Quat& q) { return PxQuat(q.x, q.y, q.z, q.w).getNormalized(); }

bool EngineCreate() {
    if (g_physics) return true;
    if (g_failed) return false;
    g_foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_alloc, g_errors);
    if (!g_foundation) { g_failed = true; LOGE("[phys] sword: PxCreateFoundation failed"); return false; }
    g_physics = PxCreatePhysics(PX_PHYSICS_VERSION, *g_foundation, PxTolerancesScale(), false, nullptr);
    if (!g_physics) { g_failed = true; LOGE("[phys] sword: PxCreatePhysics failed"); return false; }
    // 0 worker threads: every simulate() runs inline on the calling (game) thread.
    g_dispatcher = PxDefaultCpuDispatcherCreate(0);
    if (!g_dispatcher) { g_failed = true; LOGE("[phys] sword: no CPU dispatcher"); return false; }
    LOGI("[phys] sword: PhysX %d.%d.%d ready (CPU, inline)", PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX);
    return true;
}

bool RigCreate(Rig& r) {
    if (r.scene) return true;
    PxSceneDesc desc(g_physics->getTolerancesScale());
    desc.gravity = PxVec3(0.f, -9.81f, 0.f);    // ERVR's game frame is Y-up
    desc.cpuDispatcher = g_dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    r.scene = g_physics->createScene(desc);
    if (!r.scene) return false;
    r.anchor = g_physics->createRigidDynamic(PxTransform(PxIdentity));
    r.anchor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    r.body = g_physics->createRigidDynamic(PxTransform(PxIdentity));
    r.body->setSleepThreshold(0.f);
    r.body->setSolverIterationCounts(16, 4);
    r.body->setLinearDamping(0.05f);
    r.body->setAngularDamping(0.05f);
    r.body->setMaxAngularVelocity(60.f);
    r.scene->addActor(*r.anchor);
    r.scene->addActor(*r.body);
    r.joint = PxD6JointCreate(*g_physics, r.anchor, PxTransform(PxIdentity), r.body, PxTransform(PxIdentity));
    if (!r.joint) return false;
    for (PxD6Axis::Enum a : {PxD6Axis::eX, PxD6Axis::eY, PxD6Axis::eZ, PxD6Axis::eTWIST, PxD6Axis::eSWING1, PxD6Axis::eSWING2})
        r.joint->setMotion(a, PxD6Motion::eFREE);
    // The drive limits are forces (N) and torques (N*m), not impulses, so they mean the same at any step size.
    r.joint->setConstraintFlag(PxConstraintFlag::eDRIVE_LIMITS_ARE_FORCES, true);
    r.joint->setDrivePosition(PxTransform(PxIdentity));
    r.joint->setDriveVelocity(PxVec3(0.f), PxVec3(0.f));
    return true;
}

// Hand (a small sphere at the grip) + weapon (a rod along the shaft, its mass centred at com).
void UpdateMass(Rig& r, const Params& p) {
    const float key[4] = {p.weaponKg, p.reach, p.com, p.handKg};
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && std::fabs(key[i] - r.massKey[i]) <= 0.01f * std::fmax(1.f, std::fabs(key[i]));
    if (same) return;
    for (int i = 0; i < 4; ++i) r.massKey[i] = key[i];
    const float mw = p.weaponKg, mh = p.handKg, M = std::fmax(mw + mh, 0.1f);
    const float cAlong = mw * p.com / M;                         // total centre of mass along the shaft
    const float handR = 0.05f;
    float perp = mw * p.reach * p.reach / 12.f + mw * (p.com - cAlong) * (p.com - cAlong)
               + 0.4f * mh * handR * handR + mh * cAlong * cAlong;
    float axial = 0.5f * mw * 0.02f * 0.02f + 0.4f * mh * handR * handR;
    perp = std::fmax(perp, 0.02f); axial = std::fmax(axial, 0.01f);
    // Principal frame: local X along the shaft.
    PxVec3 s = p.shaft; s.normalize();
    const PxVec3 x(1.f, 0.f, 0.f);
    PxQuat rot(PxIdentity);
    const float d = x.dot(s);
    if (d < -0.9999f) rot = PxQuat(PxPi, PxVec3(0.f, 1.f, 0.f));
    else if (d < 0.9999f) { const PxVec3 c = x.cross(s); rot = PxQuat(c.x, c.y, c.z, 1.f + d).getNormalized(); }
    r.body->setMass(M);
    r.body->setCMassLocalPose(PxTransform(s * cAlong, rot));
    r.body->setMassSpaceInertiaTensor(PxVec3(axial, perp, perp));
}

void UpdateDrives(Rig& r, const Params& p) {
    const float key[6] = {p.kLin, p.dLin, p.force, p.kAng, p.dAng, p.torque};
    bool same = true;
    for (int i = 0; i < 6; ++i) same = same && key[i] == r.driveKey[i];
    if (same) return;
    for (int i = 0; i < 6; ++i) r.driveKey[i] = key[i];
    const PxD6JointDrive lin(p.kLin, p.dLin, p.force, false);
    r.joint->setDrive(PxD6Drive::eX, lin);
    r.joint->setDrive(PxD6Drive::eY, lin);
    r.joint->setDrive(PxD6Drive::eZ, lin);
    r.joint->setDrive(PxD6Drive::eSLERP, PxD6JointDrive(p.kAng, p.dAng, p.torque, false));
}

void Place(Rig& r, const PxTransform& t) {
    r.anchor->setGlobalPose(t);
    r.body->setGlobalPose(t);
    r.body->setLinearVelocity(PxVec3(0.f));
    r.body->setAngularVelocity(PxVec3(0.f));
    r.lastTarget = t;
    r.placed = true;
}

// B&S keeps the weapon controllable: the wrist owns the roll about the blade, and the blade's aim can trail the
// hand only so far. 0.7.2 had neither and a 5 kg blade trailed by up to 180 degrees (a "360 spin"). After each
// sub-step: drop any roll about the shaft relative to the controller, cap the remaining swing at maxAngle, and
// when capped, take away the angular velocity that was carrying it further.
void ConstrainOrientation(Rig& r, const Params& p, const PxTransform& anchor) {
    const PxTransform b = r.body->getGlobalPose();
    PxQuat rel = anchor.q.getConjugate() * b.q;          // the body in the controller's frame
    if (rel.w < 0.f) rel = -rel;
    const PxVec3 s = p.shaft;                            // unit, controller-local
    const PxVec3 v(rel.x, rel.y, rel.z);
    const PxVec3 proj = s * v.dot(s);
    PxQuat twist(proj.x, proj.y, proj.z, rel.w);
    const float tn = twist.magnitude();
    r.twistMax = std::fmax(r.twistMax, 2.f * std::acos(std::fmin(std::fabs(rel.w) / std::fmax(tn, 1e-6f), 1.f)) * 57.2958f);
    ++r.subSteps;
    twist = tn > 1e-6f ? twist * (1.f / tn) : PxQuat(PxIdentity);
    PxQuat swing = rel * twist.getConjugate();
    if (swing.w < 0.f) swing = -swing;
    const float angle = 2.f * std::acos(std::fmin(swing.w, 1.f));
    bool capped = false;
    if (angle > p.maxAngle) {
        const float sh = std::sin(angle * 0.5f);
        const PxVec3 axis = sh > 1e-6f ? PxVec3(swing.x, swing.y, swing.z) * (1.f / sh) : PxVec3(1.f, 0.f, 0.f);
        swing = PxQuat(p.maxAngle, axis.getNormalized());
        capped = true;
        ++r.capHits;
    }
    r.body->setGlobalPose(PxTransform(b.p, (anchor.q * swing).getNormalized()));
    // Angular velocity relative to the hand: no roll about the shaft, and none at all once the cap is hit.
    const PxVec3 wa = r.anchor->getAngularVelocity();
    PxVec3 wr = r.body->getAngularVelocity() - wa;
    const PxVec3 sw = (anchor.q * swing).rotate(s);
    wr -= sw * wr.dot(sw);
    if (capped) wr = PxVec3(0.f);
    r.body->setAngularVelocity(wa + wr);
}

// The simulation proper. No C++ objects with destructors: called inside an SEH guard.
bool StepCore(Rig* rp, const Params* pp, PxTransform* out) {
    Rig& r = *rp; const Params& p = *pp;
    if (!RigCreate(r)) return false;
    UpdateMass(r, p);
    UpdateDrives(r, p);
    if (!r.placed || (r.body->getGlobalPose().p - p.target.p).magnitude() > p.snap) Place(r, p.target);
    // Move the controller anchor smoothly across sub-steps (<= 1/240 s each) so stiff drives stay stable.
    const int n = (int)std::ceil(p.dt * 240.f) < 1 ? 1 : (int)std::ceil(p.dt * 240.f) > 8 ? 8 : (int)std::ceil(p.dt * 240.f);
    const float h = p.dt / (float)n;
    const PxTransform from = r.lastTarget;
    PxQuat qTo = p.target.q;
    if (from.q.dot(qTo) < 0.f) qTo = -qTo;
    for (int i = 1; i <= n; ++i) {
        const float t = (float)i / (float)n;
        const PxVec3 pos = from.p * (1.f - t) + p.target.p * t;
        const PxQuat rot = (from.q * (1.f - t) + qTo * t).getNormalized();
        r.anchor->setKinematicTarget(PxTransform(pos, rot));
        r.scene->simulate(h);
        r.scene->fetchResults(true);
        ConstrainOrientation(r, p, PxTransform(pos, rot));
    }
    r.lastTarget = p.target;
    *out = r.body->getGlobalPose();
    return true;
}

bool StepGuarded(Rig* r, const Params* p, PxTransform* out) {
    __try { return StepCore(r, p, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_failed = true; return false; }
}

int PlayerStrength() {
    static double s_at = -10; static int s_str = 10;
    const double now = NowSec();
    if (now - s_at > 2.0) {
        s_at = now;
        const int v = ervr::GameERPlayerStrength();
        if (v >= 1 && v <= 99) s_str = v;
    }
    return s_str;
}

}  // namespace

bool SwordRigActive() {
    const auto& c = ervr::Cfg();
    return !g_failed && c.physicsSword && PhysEnabled();
}

void SwordRigReset(int h) { g_resetMask.fetch_or(h < 0 ? 3 : (1 << (h & 1))); }

bool SwordRigStep(int h, ervr::Vec3& pos, ervr::Quat& q, float dt, float weight, float reach, float com,
                  float strength, ervr::Vec3 shaftAxis) {
    if (h < 0 || h > 1 || !(dt > 0.f && dt <= 0.1f) || !SwordRigActive()) return false;
    if (!EngineCreate()) return false;
    Rig& r = g_rig[h];
    if (g_resetMask.load() & (1 << h)) { g_resetMask.fetch_and(~(1 << h)); r.placed = false; }

    const auto& c = ervr::Cfg();
    const int str = PlayerStrength();
    Params p{};
    p.target = PxTransform(ToPx(pos), ToPx(q));
    p.shaft = ToPx(shaftAxis.Normalized());
    if (p.shaft.magnitudeSquared() < 0.5f) p.shaft = PxVec3(0.f, 1.f, 0.f);
    p.dt = dt;
    p.handKg = std::fmax(c.physicsSwordHandKg, 0.1f);
    p.weaponKg = std::fmin(std::fmax(weight, 0.f) * std::fmax(c.physicsSwordKgPerWeight, 0.f), 60.f);
    p.reach = std::fmin(std::fmax(reach, 0.f), 4.f);
    p.com = std::fmin(std::fmax(com, 0.f), p.reach);
    const float k = std::fmax(strength, 0.1f);
    p.force = std::fmax(c.physicsSwordForceBase + c.physicsSwordForcePerStr * (float)str, 10.f) * k;
    p.torque = std::fmax(c.physicsSwordTorqueBase + c.physicsSwordTorquePerStr * (float)str, 1.f) * k;
    p.kLin = std::fmax(c.physicsSwordLinearSpring, 0.f); p.dLin = std::fmax(c.physicsSwordLinearDamper, 0.f);
    p.kAng = std::fmax(c.physicsSwordAngularSpring, 0.f); p.dAng = std::fmax(c.physicsSwordAngularDamper, 0.f);
    p.snap = std::fmax(c.physicsSwordSnapDistance, 0.05f);
    // Overshoot allowance from how heavy this weapon is for this arm (weight torque about the grip / wrist torque).
    p.load = p.weaponKg * 9.81f * p.com / std::fmax(p.torque, 1.f);
    {
        const float lo = std::fmin(std::fmax(c.physicsSwordMinAngleDeg, 1.f), 120.f);
        const float hi = std::fmin(std::fmax(c.physicsSwordMaxAngleDeg, lo), 120.f);
        const float t = std::fmin(std::fmax(p.load / std::fmax(c.physicsSwordLoadForMax, 0.01f), 0.f), 1.f);
        p.maxAngle = (lo + (hi - lo) * t) * 0.0174533f;
    }

    PxTransform out(PxIdentity);
    if (!StepGuarded(&r, &p, &out)) {
        if (g_failed) { LOGE("[phys] sword: PhysX step faulted or the rig could not be built; the B&S sword is off until restart (ERVR's own weapon model takes over)"); }
        return false;
    }
    if (!out.isValid()) { r.placed = false; return false; }

    // Statistics: how far the simulated grip trails the real hand.
    const float lag = (out.p - p.target.p).magnitude();
    PxQuat dq = p.target.q.getConjugate() * out.q;
    const float ang = 2.f * std::acos(std::fmin(std::fabs(dq.w), 1.f)) * 57.2958f;
    r.lagSum += lag; r.angSum += ang; r.lagMax = std::fmax(r.lagMax, lag); r.angMax = std::fmax(r.angMax, ang); ++r.statN;
    r.lastMass = p.weaponKg + p.handKg; r.lastForce = p.force; r.lastTorque = p.torque; r.lastLoad = p.load; r.lastCapDeg = p.maxAngle * 57.2958f;
    const double now = NowSec();
    if (r.statSince == 0) r.statSince = now;
    if (now - r.statSince >= 5.0) {
        if (c.physicsSwordLog && r.statN > 0)
            LOGI("[phys] sword %s: %.1f kg (weight %.1f), arm %.0f N / %.0f N*m (STR %d%s), load %.2f -> overshoot cap %.0f deg, lag avg %.1f cm max %.1f cm, angle avg %.1f max %.1f deg, capped %d/%d sub-steps, roll removed max %.0f deg",
                 h == 1 ? "R" : "L", r.lastMass, weight, r.lastForce, r.lastTorque, str, strength > 1.01f ? ", two hands" : "", r.lastLoad, r.lastCapDeg,
                 100.f * r.lagSum / r.statN, 100.f * r.lagMax, r.angSum / r.statN, r.angMax, r.capHits, r.subSteps, r.twistMax);
        r.statSince = now; r.lagSum = r.lagMax = r.angSum = r.angMax = 0; r.statN = 0; r.capHits = r.subSteps = 0; r.twistMax = 0;
    }

    pos = {out.p.x, out.p.y, out.p.z};
    q = ervr::Quat{out.q.x, out.q.y, out.q.z, out.q.w}.Normalized();
    return true;
}

#endif

}  // namespace kinetic
