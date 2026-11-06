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
float SwordRigStrikeScale(int) { return 1.f; }
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
constexpr int kMaxParts = 96;
// A box part (an enemy weapon or shield from its model's mesh): pose is the box's centre in its node's frame, half
// its half extents, grip the node's origin (where the hand holds it) and tip the end of its line along x farthest
// from the grip, both in the box's frame.
struct PlayPart { uint32_t key; int kind; PxTransform pose; float halfHeight, radius; bool box, shield; PxVec3 half, grip, tip; float worldSpeed; PxVec3 worldVel; bool haveVel; float weaponSpeed; };

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
    float shieldKg, shieldForce, shieldTorque;   // an enemy shield the same way
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
    PxShape* enemyBox[kMaxParts]{};              // the same actor's box (weapons, shields); one of the two is on
    bool slotBox[kMaxParts]{};
    float slotWorldSpeed[kMaxParts]{};           // the part's own speed in the world (m/s), for the impact log
    PxVec3 slotWorldVel[kMaxParts];              // that velocity turned into this hand's tracking frame (play units/s)
    bool slotHaveVel[kMaxParts]{};
    float slotWeaponSpeed[kMaxParts]{};          // its enemy's fastest weapon (m/s), for the impact log
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
    float embedBack = 0.02f;                     // m back along the way in (past the entry) before it comes free
    float embedPullMax = 0.f;                    // largest pull of a stuck blade on its part (N), for the 5 s log
    // A slash going into a part (TendCut): the part lets the blade through while it resists; out the other side is a
    // cut, slowed below BodyStickSpeed still inside is stuck.
    bool cutting[kMaxParts]{}, cutInside[kMaxParts]{};
    uint32_t cutKey[kMaxParts]{};
    double cutSince[kMaxParts]{};
    PxVec3 cutDir[kMaxParts];
    float cutDepth[kMaxParts]{};
    int cuts = 0, sticks = 0, pierces = 0, glances = 0; float energyMax = 0;
    int hand = 0;
    int parries = 0, armPushes = 0, armHits = 0;
    // The enemy weapon is a physics body (not a kinematic wall) driven toward the animation by a force-limited
    // joint: the enemy's arm strength against yours decides whether your blade holds it off.
    PxRigidDynamic* wAnchor[kMaxParts]{};       // kinematic: where the animation wants the weapon
    PxD6Joint* wJoint[kMaxParts]{};
    bool wDyn[kMaxParts]{};                     // configured as a driven weapon (joint built)
    bool wFree[kMaxParts]{};                    // currently a dynamic body (touched); kinematic otherwise
    PxVec3 wGrip[kMaxParts], wFar[kMaxParts];   // grip and far end in the body's frame (PlayPart)
    PxVec3 wHalfV[kMaxParts];                   // the shape the mass was built for
    bool wShield[kMaxParts]{};
    double wContactUntil[kMaxParts]{};          // simTime: free (not snapped to the animation) until then
    float wDevMax = 0.f; int wHeld = 0;         // statistics
    double contactUntil = 0;                     // simTime: the blade touched something; it does not snap through
    int snaps = 0, bodyTouch = 0;
    int impactLogN = 0;                          // impacts logged in this 5 s window
    int kindTouch[8]{};                          // sub-steps the blade touched each kind of part
    float stepMs = 0.f; int stepN = 0;           // cost of the simulation
    double checkAt = 0;
};
Rig g_rig[2];

// Strike gate (combat.cpp asks SwordRigHoldStrike every game frame): when this hand's blade last struck for real
// (a body part cut, stuck or pierced, an arm hit hard), last met an enemy weapon or was pushed off an arm, and
// last was at an enemy part at all. Wall-clock seconds.
std::atomic<double> g_struckAt[2] = {-10.0, -10.0};
std::atomic<double> g_blockedAt[2] = {-10.0, -10.0};
std::atomic<double> g_nearAt[2] = {-10.0, -10.0};
std::atomic<int> g_held[2] = {0, 0};
// A stuck or pierced blade deals its blow once: from g_embedHoldFrom on, while it is in and for a moment after it
// comes out (g_releasedAt), the game's strikes are held - pulling it out does no damage.
std::atomic<bool> g_embedded[2] = {false, false};
std::atomic<double> g_embedHoldFrom[2] = {1e18, 1e18};
std::atomic<double> g_releasedAt[2] = {-10.0, -10.0};
// A glance: the strike may land, scaled down to GlanceDamage.
std::atomic<double> g_glanceAt[2] = {-10.0, -10.0};

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
        PxShape* bx = PxRigidActorExt::createExclusiveShape(*a, PxBoxGeometry(0.05f, 0.05f, 0.05f), *g_material);
        if (!bx) return false;
        bx->setSimulationFilterData(PxFilterData(kShapeEnemy, 0, 0, 0));
        bx->setContactOffset(0.02f); bx->setRestOffset(0.f);
        bx->setFlag(PxShapeFlag::eSIMULATION_SHAPE, false);
        r.enemy[i] = a; r.enemyShape[i] = s; r.enemyBox[i] = bx;
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

void WeaponDynamic(Rig& r, const Params& p, int k, const PlayPart& pp) {
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
        r.wDyn[k] = true; r.wFree[k] = false; r.wContactUntil[k] = 0; r.wHalfV[k] = PxVec3(-1.f);
    }
    // Its shape: a box (from the model) or a capsule along x.
    const PxVec3 half = pp.box ? pp.half : PxVec3(pp.halfHeight + pp.radius, pp.radius, pp.radius);
    const PxVec3 grip = pp.box ? pp.grip : PxVec3(-pp.halfHeight, 0.f, 0.f);
    r.wFar[k] = pp.box ? pp.tip : PxVec3(pp.halfHeight, 0.f, 0.f);
    if ((half - r.wHalfV[k]).abs().maxElement() > 0.005f || (grip - r.wGrip[k]).abs().maxElement() > 0.005f || pp.shield != r.wShield[k] || !r.wJoint[k]) {
        r.wHalfV[k] = half; r.wGrip[k] = grip; r.wShield[k] = pp.shield;
        const float m = std::fmax(pp.shield ? p.shieldKg : p.enemyKg, 0.3f);
        b->setMass(m);
        b->setCMassLocalPose(PxTransform(PxIdentity));
        // A solid box of its extents about its centre.
        const PxVec3 e = half * 2.f;
        const PxVec3 I(m * (e.y * e.y + e.z * e.z) / 12.f + 0.001f, m * (e.x * e.x + e.z * e.z) / 12.f + 0.001f, m * (e.x * e.x + e.y * e.y) / 12.f + 0.001f);
        b->setMassSpaceInertiaTensor(I);
        if (r.wJoint[k]) { r.wJoint[k]->release(); r.wJoint[k] = nullptr; }
        // Driven at the grip: the enemy's hand holds it there; it swings about it.
        const PxTransform g(grip);
        PxD6Joint* j = PxD6JointCreate(*g_physics, r.wAnchor[k], g, b, g);
        if (!j) return;
        for (PxD6Axis::Enum a : {PxD6Axis::eX, PxD6Axis::eY, PxD6Axis::eZ, PxD6Axis::eTWIST, PxD6Axis::eSWING1, PxD6Axis::eSWING2})
            j->setMotion(a, PxD6Motion::eFREE);
        j->setConstraintFlag(PxConstraintFlag::eDRIVE_LIMITS_ARE_FORCES, true);
        // Stiff and critically damped (about 40 rad/s), so it follows the animation closely until a force
        // larger than the arm's limit holds it off.
        const float w = 40.f, Ig = I.maxElement() + m * grip.magnitudeSquared();
        const float fMax = pp.shield ? p.shieldForce : p.enemyForce, tMax = pp.shield ? p.shieldTorque : p.enemyTorque;
        const PxD6JointDrive lin(m * w * w, 2.f * m * w, std::fmax(fMax, 10.f), false);
        j->setDrive(PxD6Drive::eX, lin); j->setDrive(PxD6Drive::eY, lin); j->setDrive(PxD6Drive::eZ, lin);
        j->setDrive(PxD6Drive::eSLERP, PxD6JointDrive(Ig * w * w, 2.f * Ig * w, std::fmax(tMax, 1.f), false));
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
        if (pp.box) r.enemyBox[k]->setGeometry(PxBoxGeometry(PxVec3(std::fmax(pp.half.x, 0.002f), std::fmax(pp.half.y, 0.002f), std::fmax(pp.half.z, 0.002f))));
        else r.enemyShape[k]->setGeometry(PxCapsuleGeometry(pp.radius, pp.halfHeight > 0.001f ? pp.halfHeight : 0.001f));
        r.slotBox[k] = pp.box; r.slotWorldSpeed[k] = pp.worldSpeed; r.slotWorldVel[k] = pp.worldVel; r.slotHaveVel[k] = pp.haveVel; r.slotWeaponSpeed[k] = pp.weaponSpeed;
        if (!r.slotActive[k]) {
            // New part: put it there outright (no sweep from the parking spot).
            r.enemy[k]->setGlobalPose(pp.pose);
            r.slotFrom[k] = pp.pose;
        } else {
            r.slotFrom[k] = r.slotTo[k];
        }
        if (r.slotKey[k] != pp.key) r.ghostUntil[k] = 0;
        r.slotTo[k] = pp.pose; r.slotKey[k] = pp.key; r.slotKind[k] = pp.kind; r.slotActive[k] = true;
        if (pp.kind == 2) WeaponDynamic(r, p, k, pp); else WeaponKinematic(r, k);
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
    g_embedded[r.hand].store(false); g_embedHoldFrom[r.hand].store(1e18); g_releasedAt[r.hand].store(NowSec());
}

// Joint the blade to the part it went into, along the way it went in (a thrust: along the blade; a slash: along
// the blow's direction): it slides in up to depth and back out against friction (BodyFriction N*s/m: pulled out
// slowly, not all at once), tilts a little, and nothing else. Pulled back past the entry point, it comes free.
void Embed(Rig& r, const Params& p, int slot, const PxVec3& point, const PxVec3& shaftW, float depth, float back = 0.02f) {
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
    j->setLinearLimit(PxD6Axis::eX, PxJointLinearLimitPair(g_physics->getTolerancesScale(), -(back + 0.02f), depth));
    j->setSwingLimit(PxJointLimitCone(0.25f, 0.25f));
    j->setDrive(PxD6Drive::eX, PxD6JointDrive(0.f, std::fmax(ervr::Cfg().physicsBodyFriction, 0.f), PX_MAX_F32, false));
    j->setDrivePosition(PxTransform(PxIdentity));
    j->setDriveVelocity(PxVec3(0.f), PxVec3(0.f));
    // No break force (as Blade & Sorcery's penetration joint): it comes out only pulled back along the way in,
    // against BodyFriction, or when your hand gets SwordSnapDistance away from it.
    (void)p;
    r.embed = j; r.embedSlot = slot; r.embedKey = r.slotKey[slot]; r.embedSince = r.simTime; r.embedBack = std::fmax(back, 0.01f);
    // The blow itself may land (the game's collision is let through briefly), then nothing until it is out.
    g_embedded[r.hand].store(true); g_embedHoldFrom[r.hand].store(NowSec() + 0.12);
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
        const PxVec3 vBlade = preLin + preAng.cross(im.point - com);
        // Your blade as your hand moves it (the tracking frame), the part as it moves in the world: when the game
        // moves you (a blow knocks you back, the lock-on camera turns you) the blade is carried along, and that is
        // not a swing of yours. (Before: the part's motion in the tracking frame, which counted that carry.)
        const PxVec3 vPartT = (r.slotTo[k].p - r.slotFrom[k].p) * (1.f / std::fmax(p.dt, 1e-3f));
        const PxVec3 vPart = (c.physicsSwingOwnOnly && r.slotHaveVel[k]) ? r.slotWorldVel[k] : vPartT;
        PxVec3 v = vBlade - vPart;   // relative to the moving part
        const float speed = v.magnitude();
        // The mass the part really meets at that point: a blade turning about its grip hits with less than its
        // whole mass, 1 / (1/m + (r x n) I^-1 (r x n)) with n along the relative velocity (0.5-0.7 kg at the tip
        // of a 3.7 kg sword, near the whole mass in a thrust along the blade).
        float mEff = r.body->getMass();
        if (speed > 1e-3f) {
            const PxVec3 n = v * (1.f / speed), rn = (im.point - com).cross(n);
            const PxQuat qm = pose.q * r.body->getCMassLocalPose().q;
            const PxVec3 l = qm.rotateInv(rn), inv = r.body->getMassSpaceInvInertiaTensor();
            const float x = l.x * l.x * inv.x + l.y * l.y * inv.y + l.z * l.z * inv.z;
            mEff = 1.f / (1.f / std::fmax(r.body->getMass(), 1e-3f) + x);
        }
        const float E = 0.5f * mEff * speed * speed * p.strFactor;
        // Every impact's numbers (at most 30 per 5 s per hand).
        if (c.physicsContactLog && r.impactLogN < 30) {
            ++r.impactLogN;
            const PxVec3 s0 = pose.q.rotate(p.shaft);
            LOGI("[phys] impact %s: kind %d, blade %.1f m/s (lin %.1f, ang %.1f rad/s), part %.1f m/s in your tracking space (%.1f m/s in the world), relative %.1f m/s, effective mass at that point %.2f of %.1f kg (x%.2f STR) -> %.0f J; %.2f m from grip, dt %.4f s, blade lag %.1f cm; its weapon %.1f m/s in the world (fast: it is attacking)",
                 r.hand ? "R" : "L", r.slotKind[k], vBlade.magnitude(), preLin.magnitude(), preAng.magnitude(), vPartT.magnitude(), r.slotWorldSpeed[k], speed, mEff,
                 r.body->getMass(), p.strFactor, E, (im.point - pose.p).dot(s0), p.dt,
                 100.f * (pose.p - p.target.p).magnitude(), r.slotWeaponSpeed[k]);
        }
        r.energyMax = std::fmax(r.energyMax, E);
        const int kind = r.slotKind[k];
        if (kind == 2) { g_blockedAt[r.hand].store(wall); ++r.parries; continue; }   // blade on blade: a parry, never a hit
        if (c.physicsBodyStickAll) {                                                  // test: every blow sticks
            if (r.embed) continue;                                                    // one stuck blade at a time
            const PxVec3 dir = speed > 1e-3f ? v * (1.f / speed) : pose.q.rotate(p.shaft);
            const float back = std::fmax(c.physicsBodyStickAllBack, 0.02f);
            Embed(r, p, k, im.point, dir, back, back);
            ++r.sticks; g_struckAt[r.hand].store(wall);
            LOGI("[phys] stick-all %s: kind %d, %.0f J, %.1f m/s -> stuck, %.0f cm to pull out", r.hand ? "R" : "L", kind, E, speed, 100.f * back);
            continue;
        }
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
        } else if (!thrust && E >= c.physicsBodyStickEnergy) {
            // In: the part lets the blade through and resists it (TendCut): out the other side is a cut, slowed
            // down still inside is stuck. The blow lands as it goes in.
            if (!r.cutting[k]) {
                r.cutting[k] = true; r.cutInside[k] = false; r.cutKey[k] = r.slotKey[k]; r.cutSince[k] = r.simTime;
                r.cutDir[k] = speed > 1e-3f ? v * (1.f / speed) : s; r.cutDepth[k] = 0.f;
            }
            r.ghostUntil[k] = r.simTime + 5.0;
            g_struckAt[r.hand].store(wall);
        } else {
            // Glance: the blade loses its speed into the part where it touched (the impulse that stops that point,
            // with the mass it hits with there) and stays against it; the blow lands, weakly.
            if (speed > 1e-3f) {
                const PxVec3 n = v * (1.f / speed);
                const float into = v.dot(n);
                if (into > 0.f) PxRigidBodyExt::addForceAtPos(*r.body, n * (-mEff * into), im.point, PxForceMode::eIMPULSE);
            }
            ++r.glances; g_glanceAt[r.hand].store(wall);
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

// The point of segment p1-q1 closest to segment p2-q2.
PxVec3 ClosestOnFirst(const PxVec3& p1, const PxVec3& q1, const PxVec3& p2, const PxVec3& q2) {
    const PxVec3 d1 = q1 - p1, d2 = q2 - p2, rr = p1 - p2;
    const float a = d1.dot(d1), e = d2.dot(d2), f = d2.dot(rr);
    auto cl = [](float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; };
    if (a <= 1e-8f) return p1;
    float s = 0.f;
    const float cc = d1.dot(rr);
    if (e <= 1e-8f) s = cl(-cc / a);
    else {
        const float b = d1.dot(d2), den = a * e - b * b;
        s = den > 1e-8f ? cl((b * f - cc * e) / den) : 0.f;
        const float t = (b * s + f) / e;
        if (t < 0.f) s = cl(-cc / a); else if (t > 1.f) s = cl((b - cc) / a);
    }
    return p1 + d1 * s;
}

// A slash inside a part: the part resists the blade (BodyCutResistance N against its motion where it is in,
// never more than stops that point), so it loses speed the deeper it goes. Out the other side: a cut. Slower than
// BodyStickSpeed still inside: stuck there, and it comes out only pulled back the way it went in.
void TendCut(Rig& r, const Params& p, float h) {
    const auto& c = ervr::Cfg();
    if (!r.blade) return;
    const PxTransform bp = r.body->getGlobalPose();
    const PxTransform bladePose = bp * r.blade->getLocalPose();
    const PxCapsuleGeometry& bg = static_cast<const PxCapsuleGeometry&>(r.blade->getGeometry());
    const PxVec3 bax = bladePose.q.rotate(PxVec3(1.f, 0.f, 0.f));
    const PxVec3 b0 = bladePose.p - bax * bg.halfHeight, b1 = bladePose.p + bax * bg.halfHeight;
    const PxVec3 com = bp.transform(r.body->getCMassLocalPose().p);
    for (int k = 0; k < kMaxParts; ++k) {
        if (!r.cutting[k]) continue;
        if (!r.slotActive[k] || r.slotKey[k] != r.cutKey[k] || r.embed) {
            r.cutting[k] = false;
            if (k != r.embedSlot) r.ghostUntil[k] = r.simTime + 0.05;
            continue;
        }
        PxShape* sh = r.slotBox[k] ? r.enemyBox[k] : r.enemyShape[k];
        const PxTransform pp = r.enemy[k]->getGlobalPose() * sh->getLocalPose();
        const bool inside = PxGeometryQuery::overlap(bg, bladePose, sh->getGeometry(), pp);
        if (!inside) {
            if (r.cutInside[k] || r.simTime - r.cutSince[k] > 0.15) {
                if (r.cutInside[k]) ++r.cuts;   // through
                r.cutting[k] = false; r.ghostUntil[k] = r.simTime + 0.05;
            }
            continue;
        }
        r.cutInside[k] = true;
        float hh;
        if (r.slotBox[k]) hh = static_cast<const PxBoxGeometry&>(sh->getGeometry()).halfExtents.x;
        else hh = static_cast<const PxCapsuleGeometry&>(sh->getGeometry()).halfHeight;
        const PxVec3 pax = pp.q.rotate(PxVec3(1.f, 0.f, 0.f));
        const PxVec3 pt = ClosestOnFirst(b0, b1, pp.p - pax * hh, pp.p + pax * hh);
        const PxVec3 vB = r.body->getLinearVelocity() + r.body->getAngularVelocity().cross(pt - com);
        const PxVec3 vP = (c.physicsSwingOwnOnly && r.slotHaveVel[k]) ? r.slotWorldVel[k]
                                                                      : (r.slotTo[k].p - r.slotFrom[k].p) * (1.f / std::fmax(p.dt, 1e-3f));
        const PxVec3 rel = vB - vP;
        const float spd = rel.magnitude();
        if (spd < std::fmax(c.physicsBodyStickSpeed, 0.05f)) {
            // Stuck where it stopped: back out the way it came, as far as it went in.
            const float depth = r.cutDepth[k];
            r.cutting[k] = false;
            Embed(r, p, k, pt, r.cutDir[k], 0.05f, std::fmax(depth, 0.02f));
            ++r.sticks;
            for (int j = 0; j < kMaxParts; ++j) if (j != k && r.cutting[j]) { r.cutting[j] = false; r.ghostUntil[j] = r.simTime + 0.05; }
            return;
        }
        const PxVec3 dir = rel * (1.f / spd);
        r.cutDir[k] = dir;
        r.cutDepth[k] += spd * h;
        // The mass the resistance acts on at that point (as in ClassifyImpacts), so it never reverses the blade.
        const PxVec3 rn = (pt - com).cross(dir);
        const PxQuat qm = bp.q * r.body->getCMassLocalPose().q;
        const PxVec3 l = qm.rotateInv(rn), inv = r.body->getMassSpaceInvInertiaTensor();
        const float mEff = 1.f / (1.f / std::fmax(r.body->getMass(), 1e-3f) + l.x * l.x * inv.x + l.y * l.y * inv.y + l.z * l.z * inv.z);
        const float J = std::fmin(std::fmax(c.physicsBodyCutResistance, 0.f) * h, mEff * spd);
        PxRigidBodyExt::addForceAtPos(*r.body, dir * -J, pt, PxForceMode::eIMPULSE);
    }
}

// Keep the embedded blade honest: free it when its part is gone, it was pulled out, the joint broke, or too long.
void TendEmbed(Rig& r) {
    if (!r.embed) return;
    const bool gone = r.embedSlot < 0 || !r.slotActive[r.embedSlot] || r.slotKey[r.embedSlot] != r.embedKey;
    const bool broken = (r.embed->getConstraintFlags() & PxConstraintFlag::eBROKEN);
    const bool out = r.embed->getRelativeTransform().p.x < -r.embedBack;
    if (gone || broken || out || r.simTime - r.embedSince > 10.0) {
        LOGI("[phys] stuck blade %s free after %.2f s: %s, pulled %.0f N max", r.hand ? "R" : "L", r.simTime - r.embedSince,
             gone ? "its part went away" : broken ? "yanked out (joint broke)" : out ? "pulled out" : "10 s", r.embedPullMax);
        ReleaseEmbed(r, 0.15f);
    }
}

// The simulation proper. No C++ objects with destructors: called inside an SEH guard.
bool StepCore(Rig* rp, const Params* pp, PxTransform* out) {
    Rig& r = *rp; const Params& p = *pp;
    if (!RigCreate(r)) return false;
    UpdateMass(r, p);
    UpdateDrives(r, p);
    if (!r.placed || (r.body->getGlobalPose().p - p.target.p).magnitude() > p.snap) {
        if (r.placed) ++r.snaps;
        if (r.embed) LOGI("[phys] stuck blade %s free after %.2f s: your hand went %.2f m from the blade (SwordSnapDistance)", r.hand ? "R" : "L",
                          r.simTime - r.embedSince, (r.body->getGlobalPose().p - p.target.p).magnitude());
        ReleaseEmbed(r, 0.3f);   // your hand went too far from a stuck blade: it comes out (no damage)
        Place(r, p.target);
    }
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
            const float off = (cur.transform(r.wFar[k]) - tk.transform(r.wFar[k])).magnitude();
            if (off > 1.5f) r.wContactUntil[k] = 0;   // a runaway (lost contact far off its path): back to the animation
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
            const bool cap = solid && !r.slotBox[k], box = solid && r.slotBox[k];
            if (((r.enemyShape[k]->getFlags() & PxShapeFlag::eSIMULATION_SHAPE) ? true : false) != cap)
                r.enemyShape[k]->setFlag(PxShapeFlag::eSIMULATION_SHAPE, cap);
            if (((r.enemyBox[k]->getFlags() & PxShapeFlag::eSIMULATION_SHAPE) ? true : false) != box)
                r.enemyBox[k]->setFlag(PxShapeFlag::eSIMULATION_SHAPE, box);
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
                if (r.slotKind[k] >= 0 && r.slotKind[k] < 8) ++r.kindTouch[r.slotKind[k]];
                if (r.wDyn[k]) r.wContactUntil[k] = r.simTime + 0.3;
            }
        ClassifyImpacts(r, p, preLin, preAng, h);
        TendCut(r, p, h);
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
    // A stuck blade dealt its blow when it went in: nothing while it is in or being pulled out.
    if ((g_embedded[h].load() && now >= g_embedHoldFrom[h].load()) || now - g_releasedAt[h].load() < 0.4) { g_held[h].fetch_add(1); return true; }
    if (now - g_struckAt[h].load() < 0.35) return false;   // it really struck: the game's collision may deal it
    if (now - g_glanceAt[h].load() < 0.35) return false;   // a glance: it lands, scaled by SwordRigStrikeScale
    const bool hold = now - g_blockedAt[h].load() < 0.4 || now - g_nearAt[h].load() < 0.15;
    if (hold) g_held[h].fetch_add(1);
    return hold;
}

float SwordRigStrikeScale(int h) {
    const auto& c = ervr::Cfg();
    if (h < 0 || h > 1 || !c.physicsGateStrikes || !c.physicsContact || !SwordRigActive()) return 1.f;
    const double now = NowSec();
    if (now - g_struckAt[h].load() < 0.35) return 1.f;
    if (now - g_glanceAt[h].load() < 0.35) return std::fmin(std::fmax(c.physicsGlanceDamage, 0.f), 1.f);
    return 1.f;
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
    p.shieldKg = c.physicsEnemyShieldKg; p.shieldForce = c.physicsEnemyShieldForce; p.shieldTorque = c.physicsEnemyShieldTorque;
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
            pp.box = parts[i].box; pp.shield = parts[i].shield; pp.worldSpeed = parts[i].worldSpeed;
            pp.haveVel = parts[i].worldSpeed >= 0.f;
            pp.weaponSpeed = parts[i].weaponSpeed;
            pp.worldVel = ToPx(inv.Rotate(parts[i].worldVel) * (1.f / scale));
            if (pp.box) {
                // The mesh's box in its node's frame: the actor sits at the box's centre, turned like the node.
                const ervr::Vec3 lo = parts[i].lo, hi = parts[i].hi, ctr = (lo + hi) * 0.5f;
                const ervr::Vec3 cw = parts[i].boneP + parts[i].boneQ.Rotate(ctr);
                pp.half = ToPx((hi - lo) * (0.5f / scale));
                pp.grip = ToPx(ctr * (-1.f / scale));
                const float fx = std::fabs(hi.x) > std::fabs(lo.x) ? hi.x : lo.x;
                pp.tip = ToPx((ervr::Vec3{fx, 0.f, 0.f} - ctr) * (1.f / scale));
                pp.pose = PxTransform(ToPx(inv.Rotate(cw - origin) * (1.f / scale)), ToPx((inv * parts[i].boneQ).Normalized()));
                continue;
            }
            const PxVec3 x(1.f, 0.f, 0.f), dir = ToPx(d * (1.f / len));
            const float dt2 = x.dot(dir);
            PxQuat rot(PxIdentity);
            if (dt2 < -0.9999f) rot = PxQuat(PxPi, PxVec3(0.f, 1.f, 0.f));
            else if (dt2 < 0.9999f) { const PxVec3 cr = x.cross(dir); rot = PxQuat(cr.x, cr.y, cr.z, 1.f + dt2).getNormalized(); }
            pp.pose = PxTransform(ToPx((a + b) * 0.5f), rot);
        }
    }

    PxTransform out(PxIdentity);
    const double stepT0 = NowSec();
    const bool stepped = StepGuarded(&r, &p, &out);
    r.stepMs += (float)((NowSec() - stepT0) * 1000.0); ++r.stepN;
    if (!stepped) {
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
                    const PxVec3 g = wp.transform(r.wGrip[k]), ctr = wp.p;
                    const float dev = (wp.transform(r.wFar[k]) - tp.transform(r.wFar[k])).magnitude() * scale;
                    ContactWeaponSim(r.slotKey[k], origin + w2p.Rotate(ervr::Vec3{g.x, g.y, g.z} * scale),
                                     origin + w2p.Rotate(ervr::Vec3{ctr.x, ctr.y, ctr.z} * scale), dev);
                    r.wDevMax = std::fmax(r.wDevMax, dev); ++r.wHeld;
                }
                continue;
            }
            if (k == r.embedSlot && r.embed) {
                // Your blade stuck in it: what the joint holding it pulls the part with (never more than your arm's
                // force) sways its upper body or swings its arm like a blow would, as long as it stays in.
                PxVec3 f(0.f), tq(0.f);
                r.embed->getConstraint()->getForce(f, tq);
                PxVec3 J = f * p.dt;
                const float jm = J.magnitude(), jArm = std::fmax(p.force, 1.f) * p.dt;
                if (jm > jArm && jm > 0.f) J *= jArm / jm;
                const PxVec3 pt = r.enemy[k]->getGlobalPose().transform(r.embed->getLocalPose(PxJointActorIndex::eACTOR0).p);
                if (J.magnitudeSquared() > 0.f) {
                    ContactImpulse(r.slotKey[k], origin + w2p.Rotate(ervr::Vec3{pt.x, pt.y, pt.z} * scale), w2p.Rotate(ervr::Vec3{J.x, J.y, J.z}));
                    r.embedPullMax = std::fmax(r.embedPullMax, jm / std::fmax(p.dt, 1e-3f));
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
            float hh, rr;
            if (r.slotBox[k]) {
                const PxBoxGeometry& bg = static_cast<const PxBoxGeometry&>(r.enemyBox[k]->getGeometry());
                hh = bg.halfExtents.x; rr = std::fmax(bg.halfExtents.y, bg.halfExtents.z);
            } else {
                const PxCapsuleGeometry& cg = static_cast<const PxCapsuleGeometry&>(r.enemyShape[k]->getGeometry());
                hh = cg.halfHeight; rr = cg.radius;
            }
            const float d = SegSeg(g0, g1, t.p - ax * hh, t.p + ax * hh) - rr - 0.035f;
            if (d < margin) { g_nearAt[h].store(NowSec()); break; }
        }
    }

    // Body check from the rig's side (every 3 s, right hand, an enemy torso in the scene): where the simulated
    // blade is in the world next to the blade ERVR draws, and how far it is from the torso capsule in this scene
    // and whether that capsule is solid. A blade that goes through a body like air shows here why.
    if (mapped && h == 1 && c.physicsContactLog && NowSec() - r.checkAt >= 3.0) {
        int ts = -1;
        for (int k = 0; k < kMaxParts; ++k) if (r.slotActive[k] && r.slotKind[k] == 3) { ts = k; break; }
        if (ts >= 0) {
            r.checkAt = NowSec();
            const PxVec3 g0 = out.p, g1 = out.p + out.q.rotate(p.shaft) * p.reach;
            const PxTransform& t = r.slotTo[ts];
            const PxCapsuleGeometry& g = static_cast<const PxCapsuleGeometry&>(r.enemyShape[ts]->getGeometry());
            const PxVec3 ax = t.q.rotate(PxVec3(1.f, 0.f, 0.f));
            const float d = SegSeg(g0, g1, t.p - ax * g.halfHeight, t.p + ax * g.halfHeight) - g.radius - 0.035f;
            const PxTransform live = r.enemy[ts]->getGlobalPose();
            const bool solid = (r.enemyShape[ts]->getFlags() & PxShapeFlag::eSIMULATION_SHAPE) ? true : false;
            const bool bladeOn = r.blade && (r.blade->getFlags() & PxShapeFlag::eSIMULATION_SHAPE);
            auto W = [&](const PxVec3& v) { return origin + w2p.Rotate(ervr::Vec3{v.x, v.y, v.z} * scale); };
            const ervr::Vec3 wg = W(g0), wt = W(g1), wc = W(t.p);
            ervr::Vec3 dg{}, dtp{}; const bool drawn = ervr::GameERBladeWorld(1, &dg, &dtp);
            LOGI("[phys] rig check: simulated blade grip (%.2f %.2f %.2f) tip (%.2f %.2f %.2f) | drawn blade grip (%.2f %.2f %.2f) tip (%.2f %.2f %.2f)%s | torso centre (%.2f %.2f %.2f) r %.2f half %.2f, actor %.2f m from its target, solid %d, blade shape on %d, embedded slot %d, ghost %.2f s | blade to torso %.2f m (play), scale %.2f",
                 wg.x, wg.y, wg.z, wt.x, wt.y, wt.z, dg.x, dg.y, dg.z, dtp.x, dtp.y, dtp.z, drawn ? "" : " (none)",
                 wc.x, wc.y, wc.z, g.radius * scale, g.halfHeight * scale, (live.p - t.p).magnitude(), solid ? 1 : 0, bladeOn ? 1 : 0,
                 r.embedSlot, (float)std::fmax(r.ghostUntil[ts] - r.simTime, 0.0), d, scale);
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
            LOGI("[phys] sword %s: %.1f kg (weight %.1f), arm %.0f N / %.0f N*m (STR %d%s), load %.2f -> overshoot cap %.0f deg, lag avg %.1f cm max %.1f cm, angle avg %.1f max %.1f deg, capped %d/%d sub-steps, roll removed max %.0f deg, enemy contacts %d, swing stamina spent %.0f, arm strength min x%.2f (tired); body: cut %d, stuck %d, pierced %d, glanced %d, largest blow %.0f J, stuck blade pulled its part with up to %.0f N; parries %d, arm pushes %d, arm hits %d, game strikes held back %d frame(s); enemy weapon held off %d frame(s) (largest %.0f cm off its path); blade on a body part %d sub-step(s) (touch sub-steps: upper arm %d, forearm %d, weapon %d, torso %d, head %d, thigh %d, calf %d, shoulders/hips %d); snapped back %d; simulation %.2f ms per step",
                 h == 1 ? "R" : "L", r.lastMass, weight, r.lastForce, r.lastTorque, str, strength > 1.01f ? ", two hands" : "", r.lastLoad, r.lastCapDeg,
                 100.f * r.lagSum / r.statN, 100.f * r.lagMax, r.angSum / r.statN, r.angMax, r.capHits, r.subSteps, r.twistMax, r.contacts, g_stamSpent, g_tiredMin, r.cuts, r.sticks, r.pierces, r.glances, r.energyMax, r.embedPullMax, r.parries, r.armPushes, r.armHits, g_held[h].exchange(0), r.wHeld, 100.f * r.wDevMax, r.bodyTouch, r.kindTouch[0], r.kindTouch[1], r.kindTouch[2], r.kindTouch[3], r.kindTouch[4], r.kindTouch[5], r.kindTouch[6], r.kindTouch[7], r.snaps, r.stepN ? r.stepMs / r.stepN : 0.f);
        r.impactLogN = 0;
        r.parries = r.armPushes = r.armHits = 0; r.embedPullMax = 0.f; r.wHeld = 0; r.wDevMax = 0.f; r.snaps = 0; r.bodyTouch = 0; for (int& t : r.kindTouch) t = 0; r.stepMs = 0.f; r.stepN = 0;
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
