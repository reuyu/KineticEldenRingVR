#include "sword_rig.h"

#include "combat.h"
#include "config.h"
#include "enemy_contact.h"
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
bool SwordRigHoldStrike(int) { return false; }
void SwordRigPinStamina() {}

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
PxMaterial* g_material = nullptr;
bool g_failed = false;          // engine creation failed or a step faulted: stay off until restart
std::atomic<int> g_resetMask{3};

// M2.5: enemy capsules (enemy_contact), converted into this hand's tracking frame.
constexpr int kMaxParts = 48;
struct PlayPart { uint32_t key; int kind; PxTransform pose; float halfHeight, radius; };

// A blade meeting a body part for the first time (TOUCH_FOUND), to be classified after the sub-step.
struct Impact { int slot; PxVec3 point; };
constexpr int kMaxImpacts = 8;

// Contact impulses collected inside simulate(): per pool slot, the impulse on the enemy part and its moment.
struct ContactAcc { PxVec3 impulse{0.f}; PxVec3 moment{0.f}; float weight = 0.f; };

// Shape filter words: word0 = what the shape is.
constexpr PxU32 kShapeBlade = 1, kShapeEnemy = 2;

PxFilterFlags ContactFilter(PxFilterObjectAttributes, PxFilterData f0, PxFilterObjectAttributes, PxFilterData f1,
                            PxPairFlags& pairFlags, const void*, PxU32) {
    // Only the blade against an enemy part makes contact (enemy parts are kinematic among themselves anyway).
    if (!((f0.word0 == kShapeBlade && f1.word0 == kShapeEnemy) || (f0.word0 == kShapeEnemy && f1.word0 == kShapeBlade)))
        return PxFilterFlag::eSUPPRESS;
    pairFlags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT | PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_TOUCH_PERSISTS |
                PxPairFlag::eNOTIFY_CONTACT_POINTS;
    return PxFilterFlag::eDEFAULT;
}

class ContactSink : public PxSimulationEventCallback {
public:
    ContactAcc* acc = nullptr;   // the rig being stepped
    int* hits = nullptr;
    Impact* impacts = nullptr; int* nImpacts = nullptr;
    void onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 nb) override {
        if (!acc) return;
        for (PxU32 i = 0; i < nb; ++i) {
            const PxContactPair& cp = pairs[i];
            if (cp.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1)) continue;
            // Which side is the enemy part? Its actor's userData is slot + 1.
            const int s0 = (int)(size_t)header.actors[0]->userData, s1 = (int)(size_t)header.actors[1]->userData;
            const int slot = s0 > 0 ? s0 - 1 : s1 > 0 ? s1 - 1 : -1;
            if (slot < 0 || slot >= kMaxParts) continue;
            const float sign = s0 > 0 ? 1.f : -1.f;   // the impulse is the one applied to actor 0
            PxContactPairPoint pts[16];
            const PxU32 n = cp.extractContacts(pts, 16);
            for (PxU32 k = 0; k < n; ++k) {
                const PxVec3 j = pts[k].impulse * sign;
                const float m = j.magnitude();
                if (m <= 0.f) continue;
                acc[slot].impulse += j;
                acc[slot].moment += pts[k].position * m;
                acc[slot].weight += m;
            }
            if (hits) ++*hits;
            if ((cp.events & PxPairFlag::eNOTIFY_TOUCH_FOUND) && impacts && *nImpacts < kMaxImpacts && n > 0)
                impacts[(*nImpacts)++] = Impact{slot, pts[0].position};
        }
    }
    void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
    void onWake(PxActor**, PxU32) override {}
    void onSleep(PxActor**, PxU32) override {}
    void onTrigger(PxTriggerPair*, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, const PxU32) override {}
};
ContactSink g_sink;

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
    float strFactor;            // STR scaling of a blow's energy (1 at STR 16)
    bool shield;                // left hand holds a shield: a broad, immovable collider
    float enemyKg, enemyForce, enemyTorque;   // the enemy weapon as a physics body and the arm that drives it
    float shieldR;
    PlayPart parts[kMaxParts];  // enemy capsules this frame (tracking frame)
    int nParts;
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
    // M2.5
    PxShape* blade = nullptr;                    // the weapon's collision capsule
    PxRigidDynamic* enemy[kMaxParts]{};          // kinematic enemy capsules, a fixed pool
    PxShape* enemyShape[kMaxParts]{};
    uint32_t slotKey[kMaxParts]{};               // which part a slot holds (0 = free)
    PxTransform slotFrom[kMaxParts], slotTo[kMaxParts];
    bool slotActive[kMaxParts]{};
    ContactAcc acc[kMaxParts];
    int contacts = 0;                            // contact pairs reported (for the log)
    PxVec3 lastTip{0.f}; bool haveTip = false;   // swing stamina
    bool lastShield = false;
    // Body contact: what kind each slot holds, slots the blade passes through (until simTime), the embedded blade.
    int slotKind[kMaxParts]{};
    double ghostUntil[kMaxParts]{};
    double simTime = 0;
    Impact impacts[kMaxImpacts]; int nImpacts = 0;
    PxD6Joint* embed = nullptr; int embedSlot = -1; uint32_t embedKey = 0; double embedSince = 0;
    int cuts = 0, sticks = 0, pierces = 0, glances = 0; float energyMax = 0;
    int hand = 0;
    int parries = 0, armPushes = 0, armHits = 0;
    // The enemy weapon is a physics body (not a kinematic wall) driven toward the animation by a force-limited
    // joint: the enemy's arm strength against yours decides whether your blade holds it off.
    PxRigidDynamic* wAnchor[kMaxParts]{};       // kinematic: where the animation wants the weapon
    PxD6Joint* wJoint[kMaxParts]{};
    bool wDyn[kMaxParts]{};                     // configured as a driven weapon (joint built)
    bool wFree[kMaxParts]{};                    // currently a dynamic body (touched); kinematic otherwise
    float wHalf[kMaxParts]{};
    double wContactUntil[kMaxParts]{};          // simTime: free (not snapped to the animation) until then
    float wDevMax = 0.f; int wHeld = 0;         // statistics
    double contactUntil = 0;                     // simTime: the blade touched something; it does not snap through
    int snaps = 0, bodyTouch = 0;
    int kindTouch[7]{};                          // sub-steps the blade touched each kind of part
};
Rig g_rig[2];

// Strike gate (combat.cpp asks SwordRigHoldStrike every game frame): when this hand's blade last struck for real
// (a body part cut, stuck or pierced, an arm hit hard), last met an enemy weapon or was pushed off an arm, and
// last was at an enemy part at all. Wall-clock seconds.
std::atomic<double> g_struckAt[2] = {-10.0, -10.0};
std::atomic<double> g_blockedAt[2] = {-10.0, -10.0};
std::atomic<double> g_nearAt[2] = {-10.0, -10.0};
std::atomic<int> g_held[2] = {0, 0};

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
    g_material = g_physics->createMaterial(0.4f, 0.3f, 0.05f);
    if (!g_material) { g_failed = true; LOGE("[phys] sword: no material"); return false; }
    LOGI("[phys] sword: PhysX %d.%d.%d ready (CPU, inline)", PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX);
    return true;
}

bool RigCreate(Rig& r) {
    if (r.scene) return true;
    PxSceneDesc desc(g_physics->getTolerancesScale());
    desc.gravity = PxVec3(0.f, -9.81f, 0.f);    // ERVR's game frame is Y-up
    desc.cpuDispatcher = g_dispatcher;
    desc.filterShader = ContactFilter;
    desc.simulationEventCallback = &g_sink;
    desc.flags |= PxSceneFlag::eENABLE_CCD;
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
    // A blade that starts inside an enemy part is eased out, not shot out.
    r.body->setMaxDepenetrationVelocity(3.f);
    // Swept contacts: a fast enemy weapon must not pass through the blade between two sub-steps.
    r.body->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
    r.body->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD, true);
    r.blade = PxRigidActorExt::createExclusiveShape(*r.body, PxCapsuleGeometry(0.035f, 0.1f), *g_material);
    if (!r.blade) return false;
    r.blade->setSimulationFilterData(PxFilterData(kShapeBlade, 0, 0, 0));
    r.blade->setContactOffset(0.02f); r.blade->setRestOffset(0.f);
    r.blade->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);   // until a weapon with reach is held
    for (int i = 0; i < kMaxParts; ++i) {
        PxRigidDynamic* a = g_physics->createRigidDynamic(PxTransform(PxVec3(0.f, -1000.f - 10.f * i, 0.f)));
        if (!a) return false;
        a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        a->userData = (void*)(size_t)(i + 1);
        PxShape* s = PxRigidActorExt::createExclusiveShape(*a, PxCapsuleGeometry(0.05f, 0.1f), *g_material);
        if (!s) return false;
        s->setSimulationFilterData(PxFilterData(kShapeEnemy, 0, 0, 0));
        s->setContactOffset(0.02f); s->setRestOffset(0.f);
        r.enemy[i] = a; r.enemyShape[i] = s;
        r.slotFrom[i] = a->getGlobalPose(); r.slotTo[i] = r.slotFrom[i];
        r.scene->addActor(*a);
    }
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
    same = same && r.lastShield == p.shield;
    if (same) return;
    r.lastShield = p.shield;
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
    // The blade's collision capsule: from just past the hand to the tip.
    if (r.blade && p.shield) {
        r.blade->setGeometry(PxCapsuleGeometry(std::fmax(p.shieldR, 0.05f), 0.02f));
        r.blade->setLocalPose(PxTransform(s * 0.08f, rot));
        r.blade->setFlag(PxShapeFlag::eSIMULATION_SHAPE, true);
    } else if (r.blade) {
        const float from = 0.08f, len = p.reach - from;
        if (len > 0.1f) {
            r.blade->setGeometry(PxCapsuleGeometry(0.035f, len * 0.5f));
            r.blade->setLocalPose(PxTransform(s * (from + len * 0.5f), rot));
            r.blade->setFlag(PxShapeFlag::eSIMULATION_SHAPE, true);
        } else {
            r.blade->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
        }
    }
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

void ReleaseEmbed(Rig& r, float ghostSeconds);
void Place(Rig& r, const PxTransform& t) {
    ReleaseEmbed(r, 0.3f);
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

// Assign this frame's enemy parts to pool slots (a part keeps its slot while it exists, so its motion is continuous),
// resize their capsules, and park the free slots far away.
void WeaponKinematic(Rig& r, int k) {
    if (!r.wDyn[k]) return;
    if (r.wJoint[k]) { r.wJoint[k]->release(); r.wJoint[k] = nullptr; }
    r.enemy[k]->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    r.wDyn[k] = false; r.wFree[k] = false;
}

void WeaponDynamic(Rig& r, const Params& p, int k, float half) {
    if (!r.wAnchor[k]) {
        r.wAnchor[k] = g_physics->createRigidDynamic(r.slotTo[k]);
        if (!r.wAnchor[k]) return;
        r.wAnchor[k]->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        r.scene->addActor(*r.wAnchor[k]);
    }
    PxRigidDynamic* b = r.enemy[k];
    if (!r.wDyn[k]) {
        r.wAnchor[k]->setGlobalPose(r.slotTo[k]);
        b->setGlobalPose(r.slotTo[k]);
        b->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, true);
        b->setSleepThreshold(0.f);
        b->setSolverIterationCounts(16, 4);
        b->setMaxDepenetrationVelocity(2.f);
        b->setLinearVelocity(PxVec3(0.f)); b->setAngularVelocity(PxVec3(0.f));
        r.wDyn[k] = true; r.wFree[k] = false; r.wContactUntil[k] = 0; r.wHalf[k] = -1.f;
    }
    if (std::fabs(half - r.wHalf[k]) > 0.01f || !r.wJoint[k]) {
        r.wHalf[k] = half;
        const float m = std::fmax(p.enemyKg, 0.3f), L = 2.f * half;
        b->setMass(m);
        b->setCMassLocalPose(PxTransform(PxIdentity));
        const float perp = m * L * L / 12.f + 0.01f;
        b->setMassSpaceInertiaTensor(PxVec3(0.01f, perp, perp));
        if (r.wJoint[k]) { r.wJoint[k]->release(); r.wJoint[k] = nullptr; }
        // Driven at the grip: the enemy's hand holds the hilt; the blade swings about it.
        const PxTransform grip(PxVec3(-half, 0.f, 0.f));
        PxD6Joint* j = PxD6JointCreate(*g_physics, r.wAnchor[k], grip, b, grip);
        if (!j) return;
        for (PxD6Axis::Enum a : {PxD6Axis::eX, PxD6Axis::eY, PxD6Axis::eZ, PxD6Axis::eTWIST, PxD6Axis::eSWING1, PxD6Axis::eSWING2})
            j->setMotion(a, PxD6Motion::eFREE);
        j->setConstraintFlag(PxConstraintFlag::eDRIVE_LIMITS_ARE_FORCES, true);
        // Stiff and critically damped (about 40 rad/s), so it follows the animation closely until a force
        // larger than the arm's limit holds it off.
        const float w = 40.f, Ig = perp + m * half * half;
        const PxD6JointDrive lin(m * w * w, 2.f * m * w, std::fmax(p.enemyForce, 10.f), false);
        j->setDrive(PxD6Drive::eX, lin); j->setDrive(PxD6Drive::eY, lin); j->setDrive(PxD6Drive::eZ, lin);
        j->setDrive(PxD6Drive::eSLERP, PxD6JointDrive(Ig * w * w, 2.f * Ig * w, std::fmax(p.enemyTorque, 1.f), false));
        j->setDrivePosition(PxTransform(PxIdentity));
        j->setDriveVelocity(PxVec3(0.f), PxVec3(0.f));
        r.wJoint[k] = j;
    }
}

void SyncEnemies(Rig& r, const Params& p) {
    bool used[kMaxParts] = {};
    int slotOf[kMaxParts];
    for (int i = 0; i < p.nParts; ++i) {
        slotOf[i] = -1;
        for (int k = 0; k < kMaxParts; ++k) if (r.slotActive[k] && r.slotKey[k] == p.parts[i].key) { slotOf[i] = k; break; }
        if (slotOf[i] >= 0) used[slotOf[i]] = true;
    }
    for (int i = 0; i < p.nParts; ++i) {
        if (slotOf[i] < 0) for (int k = 0; k < kMaxParts; ++k) if (!used[k] && !r.slotActive[k]) { slotOf[i] = k; used[k] = true; break; }
        if (slotOf[i] < 0) for (int k = 0; k < kMaxParts; ++k) if (!used[k]) { slotOf[i] = k; used[k] = true; r.slotActive[k] = false; break; }
        const int k = slotOf[i];
        if (k < 0) continue;
        const PlayPart& pp = p.parts[i];
        r.enemyShape[k]->setGeometry(PxCapsuleGeometry(pp.radius, pp.halfHeight > 0.001f ? pp.halfHeight : 0.001f));
        if (!r.slotActive[k]) {
            // New part: put it there outright (no sweep from the parking spot).
            r.enemy[k]->setGlobalPose(pp.pose);
            r.slotFrom[k] = pp.pose;
        } else {
            r.slotFrom[k] = r.slotTo[k];
        }
        if (r.slotKey[k] != pp.key) r.ghostUntil[k] = 0;
        r.slotTo[k] = pp.pose; r.slotKey[k] = pp.key; r.slotKind[k] = pp.kind; r.slotActive[k] = true;
        if (pp.kind == 2) WeaponDynamic(r, p, k, pp.halfHeight > 0.001f ? pp.halfHeight : 0.001f); else WeaponKinematic(r, k);
    }
    for (int k = 0; k < kMaxParts; ++k) {
        if (used[k] || !r.slotActive[k]) continue;
        r.slotActive[k] = false; r.slotKey[k] = 0;
        WeaponKinematic(r, k);
        r.enemy[k]->setGlobalPose(PxTransform(PxVec3(0.f, -1000.f - 10.f * k, 0.f)));
    }
}

void ReleaseEmbed(Rig& r, float ghostSeconds) {
    if (!r.embed) return;
    r.embed->release();
    r.embed = nullptr;
    if (r.embedSlot >= 0) r.ghostUntil[r.embedSlot] = r.simTime + ghostSeconds;   // let the blade leave cleanly
    r.embedSlot = -1; r.embedKey = 0;
}

// Joint the blade to the part it went into, along the blade: it can slide deeper or back out against friction,
// tilt a little, and nothing else. Pulled back past the entry point, it comes free.
void Embed(Rig& r, const Params& p, int slot, const PxVec3& point, const PxVec3& shaftW, float depth) {
    ReleaseEmbed(r, 0.f);
    const PxVec3 x(1.f, 0.f, 0.f);
    const float d = x.dot(shaftW);
    PxQuat rot(PxIdentity);
    if (d < -0.9999f) rot = PxQuat(PxPi, PxVec3(0.f, 1.f, 0.f));
    else if (d < 0.9999f) { const PxVec3 cr = x.cross(shaftW); rot = PxQuat(cr.x, cr.y, cr.z, 1.f + d).getNormalized(); }
    const PxTransform at(point, rot);
    PxRigidDynamic* part = r.enemy[slot];
    PxD6Joint* j = PxD6JointCreate(*g_physics, part, part->getGlobalPose().getInverse() * at, r.body, r.body->getGlobalPose().getInverse() * at);
    if (!j) return;
    j->setMotion(PxD6Axis::eX, PxD6Motion::eLIMITED);
    j->setMotion(PxD6Axis::eY, PxD6Motion::eLOCKED);
    j->setMotion(PxD6Axis::eZ, PxD6Motion::eLOCKED);
    j->setMotion(PxD6Axis::eTWIST, PxD6Motion::eFREE);
    j->setMotion(PxD6Axis::eSWING1, PxD6Motion::eLIMITED);
    j->setMotion(PxD6Axis::eSWING2, PxD6Motion::eLIMITED);
    j->setLinearLimit(PxD6Axis::eX, PxJointLinearLimitPair(g_physics->getTolerancesScale(), -0.05f, depth));
    j->setSwingLimit(PxJointLimitCone(0.25f, 0.25f));
    j->setDrive(PxD6Drive::eX, PxD6JointDrive(0.f, std::fmax(ervr::Cfg().physicsBodyFriction, 0.f), PX_MAX_F32, false));
    j->setDrivePosition(PxTransform(PxIdentity));
    j->setDriveVelocity(PxVec3(0.f), PxVec3(0.f));
    // A hard enough yank tears it out.
    j->setBreakForce(std::fmax(p.force * 3.f, 500.f), std::fmax(p.torque * 3.f, 150.f));
    r.embed = j; r.embedSlot = slot; r.embedKey = r.slotKey[slot]; r.embedSince = r.simTime;
}

// Classify each new body contact by the blow's energy and direction: cut through, stick in, pierce, or glance off.
void ClassifyImpacts(Rig& r, const Params& p, const PxVec3& preLin, const PxVec3& preAng, float h) {
    const auto& c = ervr::Cfg();
    const double wall = NowSec();
    for (int i = 0; i < r.nImpacts; ++i) {
        const Impact& im = r.impacts[i];
        const int k = im.slot;
        if (k < 0 || k >= kMaxParts || !r.slotActive[k] || k == r.embedSlot || r.simTime < r.ghostUntil[k]) continue;
        const PxTransform pose = r.body->getGlobalPose();
        const PxVec3 com = pose.transform(r.body->getCMassLocalPose().p);
        PxVec3 v = preLin + preAng.cross(im.point - com);
        v -= (r.slotTo[k].p - r.slotFrom[k].p) * (1.f / std::fmax(p.dt, 1e-3f));   // relative to the moving part
        const float speed = v.magnitude();
        const float E = 0.5f * r.body->getMass() * speed * speed * p.strFactor;
        r.energyMax = std::fmax(r.energyMax, E);
        const int kind = r.slotKind[k];
        if (kind == 2) { g_blockedAt[r.hand].store(wall); ++r.parries; continue; }   // blade on blade: a parry, never a hit
        if (kind < 2) {                                                               // an arm: pushed, or hit hard
            if (E >= c.physicsArmHitEnergy) { g_struckAt[r.hand].store(wall); ++r.armHits; }
            else { g_blockedAt[r.hand].store(wall); ++r.armPushes; }
            continue;
        }
        const PxVec3 s = pose.q.rotate(p.shaft);
        const PxVec3 tip = pose.p + s * p.reach;
        const bool thrust = speed > 0.3f && v.dot(s) / speed > 0.82f && (im.point - tip).magnitude() < 0.2f;
        if (thrust && E >= c.physicsBodyPierceEnergy) {
            Embed(r, p, k, im.point, s, std::fmax(p.reach * 0.6f, 0.05f));
            ++r.pierces; g_struckAt[r.hand].store(wall);
        } else if (!thrust && E >= c.physicsBodyCutEnergy) {
            // Through: the part lets the blade pass for a moment; cutting through costs the blow some of its speed.
            r.ghostUntil[k] = r.simTime + 0.3;
            r.body->setLinearVelocity(preLin * 0.6f);
            r.body->setAngularVelocity(preAng * 0.6f);
            ++r.cuts; g_struckAt[r.hand].store(wall);
        } else if (!thrust && E >= c.physicsBodyStickEnergy) {
            Embed(r, p, k, im.point, s, 0.04f);
            ++r.sticks; g_struckAt[r.hand].store(wall);
        } else {
            ++r.glances;
        }
    }
    r.nImpacts = 0;
    (void)h;
}

// Closest distance between segments p1-q1 and p2-q2.
float SegSeg(const PxVec3& p1, const PxVec3& q1, const PxVec3& p2, const PxVec3& q2) {
    const PxVec3 d1 = q1 - p1, d2 = q2 - p2, rr = p1 - p2;
    const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(rr);
    auto cl = [](float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; };
    float s = 0.f, t = 0.f;
    if (a <= 1e-8f && e <= 1e-8f) return rr.magnitude();
    if (a <= 1e-8f) t = cl(f / e);
    else {
        const float cc = d1.dot(rr);
        if (e <= 1e-8f) s = cl(-cc / a);
        else {
            const float b = d1.dot(d2), den = a * e - b * b;
            s = den > 1e-8f ? cl((b * f - cc * e) / den) : 0.f;
            t = (b * s + f) / e;
            if (t < 0.f) { t = 0.f; s = cl(-cc / a); } else if (t > 1.f) { t = 1.f; s = cl((b - cc) / a); }
        }
    }
    return ((p1 + d1 * s) - (p2 + d2 * t)).magnitude();
}

// Keep the embedded blade honest: free it when its part is gone, it was pulled out, the joint broke, or too long.
void TendEmbed(Rig& r) {
    if (!r.embed) return;
    const bool gone = r.embedSlot < 0 || !r.slotActive[r.embedSlot] || r.slotKey[r.embedSlot] != r.embedKey;
    const bool broken = (r.embed->getConstraintFlags() & PxConstraintFlag::eBROKEN);
    const bool out = r.embed->getRelativeTransform().p.x < -0.03f;
    if (gone || broken || out || r.simTime - r.embedSince > 4.0) ReleaseEmbed(r, 0.15f);
}

// The simulation proper. No C++ objects with destructors: called inside an SEH guard.
bool StepCore(Rig* rp, const Params* pp, PxTransform* out) {
    Rig& r = *rp; const Params& p = *pp;
    if (!RigCreate(r)) return false;
    UpdateMass(r, p);
    UpdateDrives(r, p);
    if (!r.placed || (r.body->getGlobalPose().p - p.target.p).magnitude() > p.snap) { if (r.placed) ++r.snaps; Place(r, p.target); }
    // Move the controller anchor smoothly across sub-steps (<= 1/240 s each) so stiff drives stay stable.
    // 480 Hz: an enemy weapon swung at 20 m/s moves 4 cm per sub-step, less than the two capsules' radii.
    const int n = (int)std::ceil(p.dt * 480.f) < 1 ? 1 : (int)std::ceil(p.dt * 480.f) > 16 ? 16 : (int)std::ceil(p.dt * 480.f);
    const float h = p.dt / (float)n;
    SyncEnemies(r, p);
    for (int i = 0; i < kMaxParts; ++i) r.acc[i] = ContactAcc{};
    g_sink.acc = r.acc; g_sink.hits = &r.contacts;
    r.nImpacts = 0; g_sink.impacts = r.impacts; g_sink.nImpacts = &r.nImpacts;
    const PxTransform from = r.lastTarget;
    PxQuat qTo = p.target.q;
    if (from.q.dot(qTo) < 0.f) qTo = -qTo;
    for (int i = 1; i <= n; ++i) {
        const float t = (float)i / (float)n;
        const PxVec3 pos = from.p * (1.f - t) + p.target.p * t;
        const PxQuat rot = (from.q * (1.f - t) + qTo * t).getNormalized();
        r.anchor->setKinematicTarget(PxTransform(pos, rot));
        for (int k = 0; k < kMaxParts; ++k) {
            if (!r.slotActive[k]) continue;
            PxQuat a = r.slotFrom[k].q, b = r.slotTo[k].q;
            if (a.dot(b) < 0.f) b = -b;
            const PxTransform tk(r.slotFrom[k].p * (1.f - t) + r.slotTo[k].p * t, (a * (1.f - t) + b * t).getNormalized());
            if (!r.wDyn[k]) { r.enemy[k]->setKinematicTarget(tk); continue; }
            r.wAnchor[k]->setKinematicTarget(tk);
            // Untouched, the weapon is kinematic and moves exactly as the animation has it (swept, so a fast swing
            // still meets the blade); a blade on it makes it a driven body that can be held off.
            const PxTransform cur = r.enemy[k]->getGlobalPose();
            const float off = (cur.transform(PxVec3(r.wHalf[k], 0.f, 0.f)) - tk.transform(PxVec3(r.wHalf[k], 0.f, 0.f))).magnitude();
            if (off > 0.8f) r.wContactUntil[k] = 0;   // pushed this far, the blade slipped past it
            const bool touched = r.simTime < r.wContactUntil[k];
            if (!touched) {
                if (r.wFree[k]) { r.enemy[k]->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true); r.wFree[k] = false; r.enemy[k]->setGlobalPose(tk); }
                r.enemy[k]->setKinematicTarget(tk);
            } else if (!r.wFree[k]) {
                r.enemy[k]->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, false);
                r.enemy[k]->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD, true);
                r.enemy[k]->setLinearVelocity((r.slotTo[k].p - r.slotFrom[k].p) * (1.f / std::fmax(p.dt, 1e-3f)));
                r.enemy[k]->setAngularVelocity(PxVec3(0.f));
                r.wFree[k] = true;
            }
        }
        // Body parts the blade is passing through (or stuck in) do not collide with it.
        for (int k = 0; k < kMaxParts; ++k) {
            const bool solid = r.slotActive[k] && r.simTime >= r.ghostUntil[k] && k != r.embedSlot;
            if (((r.enemyShape[k]->getFlags() & PxShapeFlag::eSIMULATION_SHAPE) ? true : false) != solid)
                r.enemyShape[k]->setFlag(PxShapeFlag::eSIMULATION_SHAPE, solid);
        }
        const PxVec3 preLin = r.body->getLinearVelocity(), preAng = r.body->getAngularVelocity();
        float wBefore[kMaxParts];
        for (int k = 0; k < kMaxParts; ++k) wBefore[k] = r.acc[k].weight;
        r.scene->simulate(h);
        r.scene->fetchResults(true);
        r.simTime += h;
        for (int k = 0; k < kMaxParts; ++k)
            if (r.acc[k].weight > wBefore[k]) {
                r.contactUntil = r.simTime + 0.3;
                if (r.slotKind[k] >= 3) ++r.bodyTouch;
                if (r.slotKind[k] >= 0 && r.slotKind[k] < 7) ++r.kindTouch[r.slotKind[k]];
                if (r.wDyn[k]) r.wContactUntil[k] = r.simTime + 0.3;
            }
        ClassifyImpacts(r, p, preLin, preAng, h);
        TendEmbed(r);
        ConstrainOrientation(r, p, PxTransform(pos, rot));
    }
    g_sink.acc = nullptr; g_sink.hits = nullptr; g_sink.impacts = nullptr; g_sink.nImpacts = nullptr;
    r.lastTarget = p.target;
    *out = r.body->getGlobalPose();
    return true;
}

bool StepGuarded(Rig* r, const Params* p, PxTransform* out) {
    __try { return StepCore(r, p, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_failed = true; return false; }
}

// Swing stamina (both hands share the bar): fractional drain carried between frames, the bar as last read, and
// statistics for the log.
float g_stamDebt = 0.f;
int g_stam = -1, g_stamMax = -1;
double g_stamReadAt = -10;
float g_stamSpent = 0.f, g_tiredMin = 1.f;
std::atomic<double> g_regenHoldUntil{0.0};   // the game may not regenerate stamina before this (wall clock)
std::atomic<int> g_stamHeld{-1};              // the bar as our last drain left it

// Arm strength multiplier from how tired the player is (1 = fresh).
float TiredFactor() {
    const auto& c = ervr::Cfg();
    if (!c.physicsSwingStamina) return 1.f;
    const double now = NowSec();
    // While the regeneration is held the bar is read and pinned on every step (a 20 Hz pin let the game's
    // regeneration show through as a flicker).
    if (now - g_stamReadAt > 0.05) { g_stamReadAt = now; g_stam = ervr::CombatPlayerStamina(&g_stamMax); }
    if (g_stam < 0 || g_stamMax <= 0) return 1.f;
    const float frac = (float)g_stam / (float)g_stamMax, below = std::fmax(c.physicsTiredBelow, 0.01f);
    if (frac >= below) return 1.f;
    const float lo = std::fmin(std::fmax(c.physicsTiredStrength, 0.05f), 1.f);
    return lo + (1.f - lo) * std::fmax(frac, 0.f) / below;
}

// A swing costs stamina while the blade tip moves fast, whether or not it lands.
void DrainSwing(float tipSpeed, float weaponKg, float dt) {
    const auto& c = ervr::Cfg();
    if (!c.physicsSwingStamina || weaponKg <= 0.f || tipSpeed <= c.physicsSwingStaminaSpeed) return;
    const float v = tipSpeed / 8.f;
    g_stamDebt += std::fmax(c.physicsSwingStaminaPerSec, 0.f) * v * v * (0.5f + weaponKg / 3.f) * dt;
    if (g_stamDebt < 1.f) return;
    const int take = (int)g_stamDebt;
    g_stamDebt -= (float)take;
    int maxS = -1;
    const int cur = ervr::CombatPlayerStamina(&maxS);
    if (cur <= 0) { g_stamDebt = 0.f; return; }
    ervr::CombatSetPlayerStamina(cur - take);
    g_stam = cur - take < 0 ? 0 : cur - take; g_stamMax = maxS;
    g_stamHeld.store(g_stam);
    g_regenHoldUntil.store(NowSec() + std::fmax(c.physicsSwingRegenDelay, 0.f));
    g_stamSpent += (float)take;
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

void SwordRigPinStamina() {
    // Right after a swing the game's regeneration would refill the bar at once and the tired arm would never be
    // felt: the bar stays where the swings left it for SwingRegenDelay (it may still fall). Called from every
    // point the game updates characters (pose hook, before and after model-pos), so no regenerated frame shows.
    if (!ervr::Cfg().physicsSwingStamina || NowSec() >= g_regenHoldUntil.load()) return;
    const int held = g_stamHeld.load();
    if (held < 0) return;
    int maxS = -1;
    const int cur = ervr::CombatPlayerStamina(&maxS);
    if (cur < 0) return;
    if (cur > held) ervr::CombatSetPlayerStamina(held);
    else if (cur < held) g_stamHeld.store(cur);
}

bool SwordRigHoldStrike(int h) {
    const auto& c = ervr::Cfg();
    if (h < 0 || h > 1 || !c.physicsGateStrikes || !c.physicsContact || !SwordRigActive()) return false;
    const double now = NowSec();
    if (now - g_struckAt[h].load() < 0.35) return false;   // it really struck: the game's collision may deal it
    const bool hold = now - g_blockedAt[h].load() < 0.4 || now - g_nearAt[h].load() < 0.15;
    if (hold) g_held[h].fetch_add(1);
    return hold;
}

bool SwordRigStep(int h, ervr::Vec3& pos, ervr::Quat& q, float dt, float weight, float reach, float com,
                  float strength, ervr::Vec3 shaftAxis) {
    if (h < 0 || h > 1 || !(dt > 0.f && dt <= 0.1f) || !SwordRigActive()) return false;
    if (!EngineCreate()) return false;
    Rig& r = g_rig[h];
    r.hand = h;
    if (g_resetMask.load() & (1 << h)) { g_resetMask.fetch_and(~(1 << h)); r.placed = false; r.haveTip = false; }

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
    p.strFactor = std::fmin(std::fmax((float)str / 16.f, 0.5f), 3.f);
    const float tired = TiredFactor();
    g_tiredMin = std::fmin(g_tiredMin, tired);
    p.shield = h == 0 && c.physicsHitJudge && ervr::CombatLeftIsShield();
    p.shieldR = c.physicsShieldRadius;
    p.enemyKg = c.physicsEnemyWeaponKg; p.enemyForce = c.physicsEnemyArmForce; p.enemyTorque = c.physicsEnemyArmTorque;
    // A raised shield does not give: the arm behind it is braced (x4), so blows stop on it.
    const float k = std::fmax(strength, 0.1f) * tired * (p.shield ? 4.f : 1.f);
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

    // M2.5: the enemy parts, from the world into this hand's tracking frame (play = rot^-1 (world - origin) / scale).
    ervr::Vec3 origin; ervr::Quat w2p; float scale = 1.f;
    const bool mapped = c.physicsContact && ervr::GameERPlayToWorld(h, &origin, &w2p, &scale) && scale > 0.05f;
    p.nParts = 0;
    if (mapped) {
        ContactPart parts[kMaxParts];
        const int n = ContactParts(parts, kMaxParts);
        const ervr::Quat inv = w2p.Conj();
        for (int i = 0; i < n; ++i) {
            const ervr::Vec3 a = inv.Rotate(parts[i].a - origin) * (1.f / scale);
            const ervr::Vec3 b = inv.Rotate(parts[i].b - origin) * (1.f / scale);
            const ervr::Vec3 d = b - a;
            const float len = d.Length();
            if (len < 1e-3f) continue;
            PlayPart& pp = p.parts[p.nParts++];
            pp.key = parts[i].key;
            pp.kind = parts[i].kind;
            pp.halfHeight = len * 0.5f;
            pp.radius = parts[i].radius / scale;
            const PxVec3 x(1.f, 0.f, 0.f), dir = ToPx(d * (1.f / len));
            const float dt2 = x.dot(dir);
            PxQuat rot(PxIdentity);
            if (dt2 < -0.9999f) rot = PxQuat(PxPi, PxVec3(0.f, 1.f, 0.f));
            else if (dt2 < 0.9999f) { const PxVec3 cr = x.cross(dir); rot = PxQuat(cr.x, cr.y, cr.z, 1.f + dt2).getNormalized(); }
            pp.pose = PxTransform(ToPx((a + b) * 0.5f), rot);
        }
    }

    PxTransform out(PxIdentity);
    if (!StepGuarded(&r, &p, &out)) {
        if (g_failed) { LOGE("[phys] sword: PhysX step faulted or the rig could not be built; the B&S sword is off until restart (ERVR's own weapon model takes over)"); }
        return false;
    }
    if (!out.isValid()) { r.placed = false; r.haveTip = false; return false; }

    // Hand the impulses to the enemies they hit, in the world.
    if (mapped) {
        // No more than the blade's own momentum can give (a kinematic arm pushed into the blade otherwise reports
        // the solver's penetration fixes as huge impulses, and the arm jerked to its limit).
        const float jMax = r.body->getMass() * (r.body->getLinearVelocity().magnitude() + r.body->getAngularVelocity().magnitude() * p.reach + 1.f);
        for (int k = 0; k < kMaxParts; ++k) {
            const ContactAcc& a = r.acc[k];
            if (!r.slotActive[k]) continue;
            if (r.wDyn[k]) {
                if (r.simTime < r.wContactUntil[k]) {
                    // The physics weapon, held off its path by your blade: its arm carries it there.
                    const PxTransform wp = r.enemy[k]->getGlobalPose(), tp = r.slotTo[k];
                    const PxVec3 g = wp.transform(PxVec3(-r.wHalf[k], 0.f, 0.f)), tip = wp.transform(PxVec3(r.wHalf[k], 0.f, 0.f));
                    const float dev = (tip - tp.transform(PxVec3(r.wHalf[k], 0.f, 0.f))).magnitude() * scale;
                    ContactWeaponSim(r.slotKey[k], origin + w2p.Rotate(ervr::Vec3{g.x, g.y, g.z} * scale),
                                     origin + w2p.Rotate(ervr::Vec3{tip.x, tip.y, tip.z} * scale), dev);
                    r.wDevMax = std::fmax(r.wDevMax, dev); ++r.wHeld;
                }
                continue;
            }
            if (a.weight <= 0.f) continue;
            const PxVec3 pt = a.moment * (1.f / a.weight);
            PxVec3 J = a.impulse;
            const float jm = J.magnitude();
            if (jm > jMax && jm > 0.f) J *= jMax / jm;
            const ervr::Vec3 pw = origin + w2p.Rotate(ervr::Vec3{pt.x, pt.y, pt.z} * scale);
            const ervr::Vec3 jw = w2p.Rotate(ervr::Vec3{J.x, J.y, J.z});
            ContactImpulse(r.slotKey[k], pw, jw);
        }
    }

    // Strike gate: is the blade at an enemy part right now (within a few cm)?
    if (mapped) {
        const PxVec3 g0 = out.p, g1 = out.p + out.q.rotate(p.shaft) * p.reach;
        const float margin = 0.08f / scale;
        for (int k = 0; k < kMaxParts; ++k) {
            if (!r.slotActive[k]) continue;
            const PxTransform& t = r.slotTo[k];
            const PxVec3 ax = t.q.rotate(PxVec3(1.f, 0.f, 0.f));
            const PxCapsuleGeometry& g = static_cast<const PxCapsuleGeometry&>(r.enemyShape[k]->getGeometry());
            const float d = SegSeg(g0, g1, t.p - ax * g.halfHeight, t.p + ax * g.halfHeight) - g.radius - 0.035f;
            if (d < margin) { g_nearAt[h].store(NowSec()); break; }
        }
    }

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
            LOGI("[phys] sword %s: %.1f kg (weight %.1f), arm %.0f N / %.0f N*m (STR %d%s), load %.2f -> overshoot cap %.0f deg, lag avg %.1f cm max %.1f cm, angle avg %.1f max %.1f deg, capped %d/%d sub-steps, roll removed max %.0f deg, enemy contacts %d, swing stamina spent %.0f, arm strength min x%.2f (tired); body: cut %d, stuck %d, pierced %d, glanced %d, largest blow %.0f J; parries %d, arm pushes %d, arm hits %d, game strikes held back %d frame(s); enemy weapon held off %d frame(s) (largest %.0f cm off its path); blade on a body part %d sub-step(s) (touch sub-steps: upper arm %d, forearm %d, weapon %d, torso %d, head %d, thigh %d, calf %d); snapped back %d",
                 h == 1 ? "R" : "L", r.lastMass, weight, r.lastForce, r.lastTorque, str, strength > 1.01f ? ", two hands" : "", r.lastLoad, r.lastCapDeg,
                 100.f * r.lagSum / r.statN, 100.f * r.lagMax, r.angSum / r.statN, r.angMax, r.capHits, r.subSteps, r.twistMax, r.contacts, g_stamSpent, g_tiredMin, r.cuts, r.sticks, r.pierces, r.glances, r.energyMax, r.parries, r.armPushes, r.armHits, g_held[h].exchange(0), r.wHeld, 100.f * r.wDevMax, r.bodyTouch, r.kindTouch[0], r.kindTouch[1], r.kindTouch[2], r.kindTouch[3], r.kindTouch[4], r.kindTouch[5], r.kindTouch[6], r.snaps);
        r.parries = r.armPushes = r.armHits = 0; r.wHeld = 0; r.wDevMax = 0.f; r.snaps = 0; r.bodyTouch = 0; for (int& t : r.kindTouch) t = 0;
        r.statSince = now; r.lagSum = r.lagMax = r.angSum = r.angMax = 0; r.statN = 0; r.capHits = r.subSteps = 0; r.twistMax = 0; r.contacts = 0; r.cuts = r.sticks = r.pierces = r.glances = 0; r.energyMax = 0;
        if (h == 1) { g_stamSpent = 0.f; g_tiredMin = 1.f; }
    }

    // Swing stamina from the simulated blade tip.
    {
        const PxVec3 tip = out.p + out.q.rotate(p.shaft) * p.reach;
        const float v = (tip - r.lastTip).magnitude() / dt;
        if (r.haveTip && v < 30.f) DrainSwing(v, p.weaponKg, dt);   // faster = a snap or teleport, not a swing
        r.lastTip = tip; r.haveTip = true;
    }

    pos = {out.p.x, out.p.y, out.p.z};
    q = ervr::Quat{out.q.x, out.q.y, out.q.z, out.q.w}.Normalized();
    return true;
}

#endif

}  // namespace kinetic
