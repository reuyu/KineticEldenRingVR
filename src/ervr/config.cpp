#include "config.h"
#include "log.h"
#include "vrmath.h"
#include <windows.h>
#include <xinput.h>
#include <cctype>

namespace ervr {

namespace {
Config g_cfg;

std::string Upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

std::string GetS(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, const std::string& def) {
    wchar_t buf[512]{};
    std::wstring wdef(def.begin(), def.end());
    GetPrivateProfileStringW(sec, key, wdef.c_str(), buf, 512, ini);
    std::string out;
    for (wchar_t* p = buf; *p; ++p) out.push_back((char)*p);
    while (!out.empty() && isspace((unsigned char)out.back())) out.pop_back();
    size_t i = 0; while (i < out.size() && isspace((unsigned char)out[i])) ++i;
    return out.substr(i);
}
float GetF(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def) {
    std::string s = GetS(ini, sec, key, "");
    if (s.empty()) return def;
    try { return std::stof(s); } catch (...) { return def; }
}
int GetI(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, int def) {
    std::string s = GetS(ini, sec, key, "");
    if (s.empty()) return def;
    try { return (int)std::stol(s, nullptr, 0); } catch (...) { return def; }
}
bool GetB(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, bool def) {
    std::string s = Upper(GetS(ini, sec, key, ""));
    if (s.empty()) return def;
    return s == "1" || s == "TRUE" || s == "YES" || s == "ON";
}
} // namespace

uint16_t ParseXInputButton(const std::string& raw) {
    std::string n = Upper(raw);
    if (n == "A") return XINPUT_GAMEPAD_A;
    if (n == "B") return XINPUT_GAMEPAD_B;
    if (n == "X") return XINPUT_GAMEPAD_X;
    if (n == "Y") return XINPUT_GAMEPAD_Y;
    if (n == "LB" || n == "L1") return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (n == "RB" || n == "R1") return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (n == "LS" || n == "L3") return XINPUT_GAMEPAD_LEFT_THUMB;
    if (n == "RS" || n == "R3") return XINPUT_GAMEPAD_RIGHT_THUMB;
    if (n == "START" || n == "MENU") return XINPUT_GAMEPAD_START;
    if (n == "BACK" || n == "SELECT" || n == "VIEW") return XINPUT_GAMEPAD_BACK;
    if (n == "DPAD_UP" || n == "UP") return XINPUT_GAMEPAD_DPAD_UP;
    if (n == "DPAD_DOWN" || n == "DOWN") return XINPUT_GAMEPAD_DPAD_DOWN;
    if (n == "DPAD_LEFT" || n == "LEFT") return XINPUT_GAMEPAD_DPAD_LEFT;
    if (n == "DPAD_RIGHT" || n == "RIGHT") return XINPUT_GAMEPAD_DPAD_RIGHT;
    if (n == "LT" || n == "L2") return VP_LT;
    if (n == "RT" || n == "R2") return VP_RT;
    return 0;
}

const char* XInputButtonName(uint16_t m) {
    switch (m) {
    case XINPUT_GAMEPAD_A: return "A"; case XINPUT_GAMEPAD_B: return "B";
    case XINPUT_GAMEPAD_X: return "X"; case XINPUT_GAMEPAD_Y: return "Y";
    case XINPUT_GAMEPAD_LEFT_SHOULDER: return "LB"; case XINPUT_GAMEPAD_RIGHT_SHOULDER: return "RB";
    case XINPUT_GAMEPAD_LEFT_THUMB: return "LS"; case XINPUT_GAMEPAD_RIGHT_THUMB: return "RS";
    case XINPUT_GAMEPAD_START: return "START"; case XINPUT_GAMEPAD_BACK: return "BACK";
    case XINPUT_GAMEPAD_DPAD_UP: return "DPAD_UP"; case XINPUT_GAMEPAD_DPAD_DOWN: return "DPAD_DOWN";
    case XINPUT_GAMEPAD_DPAD_LEFT: return "DPAD_LEFT"; case XINPUT_GAMEPAD_DPAD_RIGHT: return "DPAD_RIGHT";
    case VP_LT: return "LT"; case VP_RT: return "RT";
    default: return "none";
    }
}

void Config::Load(const std::wstring& path) {
    this->iniPath = path;
    const wchar_t* ini = path.c_str();
    logLevel   = GetS(ini, L"General", L"LogLevel", logLevel);
    vrRuntime  = GetS(ini, L"General", L"VRRuntime", vrRuntime);
    enabled    = GetB(ini, L"General", L"Enabled", enabled);
    pollHz     = GetI(ini, L"General", L"PollHz", pollHz);

    vrStereoMode     = GetS(ini, L"VR", L"StereoMode", vrStereoMode);
    vrRenderScale    = GetF(ini, L"VR", L"RenderScale", vrRenderScale);
    vrWorldScale     = GetF(ini, L"VR", L"WorldScale", vrWorldScale);
    vrPairRateHz     = GetI(ini, L"VR", L"PairRateHz", vrPairRateHz);
    vrCinemaDistance = GetF(ini, L"VR", L"CinemaDistance", vrCinemaDistance);
    vrCinemaWidth    = GetF(ini, L"VR", L"CinemaWidth", vrCinemaWidth);
    vrMirrorWindow   = GetB(ini, L"VR", L"MirrorWindow", vrMirrorWindow);
    vrHeadsetRefreshHz = GetI(ini, L"VR", L"HeadsetRefreshHz", vrHeadsetRefreshHz);
    vrLimiterFollowsHeadset = GetB(ini, L"VR", L"LimiterFollowsHeadset", vrLimiterFollowsHeadset);
    vrXrDebug        = GetB(ini, L"VR", L"XrDebug", vrXrDebug);
    vrFovDeg         = GetF(ini, L"VR", L"FovDeg", vrFovDeg);
    vrFovAxis        = GetS(ini, L"VR", L"FovAxis", vrFovAxis);
    vrGameRasterRefHeight = GetF(ini, L"VR", L"GameRasterRefHeight", vrGameRasterRefHeight);
    vrIpdMm          = GetF(ini, L"VR", L"IpdMm", vrIpdMm);
    vrColorFix       = GetS(ini, L"VR", L"ColorFix", vrColorFix);
    vrGameRes        = GetS(ini, L"VR", L"GameRes", vrGameRes);
    vrGameResScale   = GetF(ini, L"VR", L"GameResScale", vrGameResScale);
    vrGameResOversize = GetB(ini, L"VR", L"GameResOversize", vrGameResOversize);
    vrGameResScan    = GetB(ini, L"VR", L"GameResScan", vrGameResScan);
    vrGameResScanSeconds = GetF(ini, L"VR", L"GameResScanSeconds", vrGameResScanSeconds);
    vrGameResAspectFix = GetB(ini, L"VR", L"GameResAspectFix", vrGameResAspectFix);
    vrPass2Keep      = GetS(ini, L"VR", L"Pass2Keep", vrPass2Keep);
    vrPass2FullFrame = GetB(ini, L"VR", L"Pass2FullFrame", vrPass2FullFrame);
    vrPass2Dt        = GetF(ini, L"VR", L"Pass2Dt", vrPass2Dt);
    vrPass2FullFrameKeep = GetS(ini, L"VR", L"Pass2FullFrameKeep", vrPass2FullFrameKeep);
    vrPass2SafeKeep = GetS(ini, L"VR", L"Pass2SafeKeep", vrPass2SafeKeep);
    // An installed ERVR.ini outlives updates, and 0.6.0's spelled both keep sets out with the event
    // manager (25) gated: doors, levers, lifts and new graces stopped answering the interact button.
    // That value, and the 16-25 workaround posted for it (which brought the stuck popup back), give way
    // to the current default; any other value written there is the player's own and stays.
    {
        static const char* const kRetiredFull[] = {
            "0-7,10-14,16-21,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-125,127-132,141-142,144-160,164,168",
            "0-7,10-14,16-25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-125,127-132,141-142,144-160,164,168",
        };
        static const char* const kRetiredSafe[] = {
            "0-7,10-14,16-21,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-117,119-122,127-132,141-142,144-160,164,168",
            "0-7,10-14,16-25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-117,119-122,127-132,141-142,144-160,164,168",
        };
        const Config defaults;
        pass2KeepNote.clear();
        for (const char* s : kRetiredFull)
            if (vrPass2FullFrameKeep == s) { vrPass2FullFrameKeep = defaults.vrPass2FullFrameKeep; pass2KeepNote = "Pass2FullFrameKeep"; }
        for (const char* s : kRetiredSafe)
            if (vrPass2SafeKeep == s) {
                vrPass2SafeKeep = defaults.vrPass2SafeKeep;
                pass2KeepNote += pass2KeepNote.empty() ? "Pass2SafeKeep" : " and Pass2SafeKeep";
            }
    }
    vrPass2SafeSeconds = GetF(ini, L"VR", L"Pass2SafeSeconds", vrPass2SafeSeconds);
    vrPass2SpawnGraceSeconds = GetF(ini, L"VR", L"Pass2SpawnGraceSeconds", vrPass2SpawnGraceSeconds);
    vrPass2MenuGraceSeconds = GetF(ini, L"VR", L"Pass2MenuGraceSeconds", vrPass2MenuGraceSeconds);
    vrPass2WarpGraceSeconds = GetF(ini, L"VR", L"Pass2WarpGraceSeconds", vrPass2WarpGraceSeconds);
    vrPass2CoopGraceSeconds = GetF(ini, L"VR", L"Pass2CoopGraceSeconds", vrPass2CoopGraceSeconds);
    vrPass2CoopSafe = GetB(ini, L"VR", L"Pass2CoopSafe", vrPass2CoopSafe);
    vrPass2FinalizeReplay = GetB(ini, L"VR", L"Pass2FinalizeReplay", vrPass2FinalizeReplay);
    vrPass2SafeOnA = GetF(ini, L"VR", L"Pass2SafeOnA", vrPass2SafeOnA);
    vrPass2KeepActionRequest = GetB(ini, L"VR", L"Pass2KeepActionRequest", vrPass2KeepActionRequest);
    vrReportFinalCamera = GetB(ini, L"VR", L"ReportFinalCamera", vrReportFinalCamera);
    vrPass2Steps     = GetI(ini, L"VR", L"Pass2Steps", vrPass2Steps);
    vrPass2EatAsserts = GetB(ini, L"VR", L"Pass2EatAsserts", vrPass2EatAsserts);
    vrFadeGuard      = GetB(ini, L"VR", L"FadeGuard", vrFadeGuard);
    vrPresentLag     = GetI(ini, L"VR", L"PresentLag", vrPresentLag);
    vrGpuSync        = GetI(ini, L"VR", L"GpuSync", vrGpuSync);
    debugKeys        = GetB(ini, L"VR", L"DebugKeys", debugKeys);
    vrDebugPass2Pitch = GetF(ini, L"VR", L"DebugPass2Pitch", vrDebugPass2Pitch);

    hudMode         = GetS(ini, L"HUD", L"Mode", hudMode);
    hudTarget       = GetS(ini, L"HUD", L"Target", hudTarget);
    hudFirstDraw    = GetI(ini, L"HUD", L"FirstDraw", hudFirstDraw);
    hudProbe        = GetB(ini, L"HUD", L"Probe", hudProbe);
    hudLock         = GetS(ini, L"HUD", L"Lock", hudLock);
    hudDistance     = GetF(ini, L"HUD", L"Distance", hudDistance);
    hudWidth        = GetF(ini, L"HUD", L"Width", hudWidth);
    hudHeightOffset = GetF(ini, L"HUD", L"HeightOffset", hudHeightOffset);
    hudYawDeg       = GetF(ini, L"HUD", L"YawDeg", hudYawDeg);
    hudPitchDeg     = GetF(ini, L"HUD", L"PitchDeg", hudPitchDeg);
    hudOpacity      = GetF(ini, L"HUD", L"Opacity", hudOpacity);
    hudKey          = GetS(ini, L"HUD", L"Key", hudKey);
    hudRedirectWithoutVR = GetB(ini, L"HUD", L"RedirectWithoutVR", hudRedirectWithoutVR);
    hudMirrorPanel  = GetB(ini, L"HUD", L"MirrorPanel", hudMirrorPanel);
    hudFollowDeg    = GetF(ini, L"HUD", L"FollowDeg", hudFollowDeg);
    hudFollowRadius = GetF(ini, L"HUD", L"FollowRadius", hudFollowRadius);

    btnA = GetS(ini, L"Buttons", L"A", btnA); btnB = GetS(ini, L"Buttons", L"B", btnB);
    btnX = GetS(ini, L"Buttons", L"X", btnX); btnY = GetS(ini, L"Buttons", L"Y", btnY);
    btnLThumb = GetS(ini, L"Buttons", L"LeftStickClick", btnLThumb);
    btnRThumb = GetS(ini, L"Buttons", L"RightStickClick", btnRThumb);
    btnMenu   = GetS(ini, L"Buttons", L"Menu", btnMenu);
    btnLTrigger = GetS(ini, L"Buttons", L"LeftTrigger", btnLTrigger);
    btnRTrigger = GetS(ini, L"Buttons", L"RightTrigger", btnRTrigger);
    btnLGrip  = GetS(ini, L"Buttons", L"LeftGrip", btnLGrip);
    btnRGrip  = GetS(ini, L"Buttons", L"RightGrip", btnRGrip);
    triggerThreshold = GetF(ini, L"Buttons", L"TriggerThreshold", triggerThreshold);
    gripThreshold    = GetF(ini, L"Buttons", L"GripThreshold", gripThreshold);
    menuHoldForBack  = GetF(ini, L"Buttons", L"MenuHoldForBack", menuHoldForBack);
    leftGripPlusStickIsDpad = GetB(ini, L"Buttons", L"LeftGripPlusStickIsDpad", leftGripPlusStickIsDpad);
    btnLeftPad  = GetS(ini, L"Buttons", L"LeftPad", btnLeftPad);
    btnRightPad = GetS(ini, L"Buttons", L"RightPad", btnRightPad);
    padForce    = GetF(ini, L"Buttons", L"PadForce", padForce);
    indexGripForce = GetF(ini, L"Buttons", L"IndexGripForce", indexGripForce);

    stickDeadzone = GetF(ini, L"Sticks", L"Deadzone", stickDeadzone);
    swapSticks    = GetB(ini, L"Sticks", L"Swap", swapSticks);
    rightStickSensitivity = GetF(ini, L"Sticks", L"RightSensitivity", rightStickSensitivity);
    stickGlitchFilter = GetB(ini, L"Sticks", L"GlitchFilter", stickGlitchFilter);
    stickGlitchSweepDegPerSec = GetF(ini, L"Sticks", L"GlitchSweepDegPerSec", stickGlitchSweepDegPerSec);
    stickGlitchRisePerSec = GetF(ini, L"Sticks", L"GlitchRisePerSec", stickGlitchRisePerSec);
    stickGlitchFallPerSec = GetF(ini, L"Sticks", L"GlitchFallPerSec", stickGlitchFallPerSec);

    gesturesEnabled  = GetB(ini, L"Gestures", L"Enabled", gesturesEnabled);
    lightAttackSpeed = GetF(ini, L"Gestures", L"LightAttackSpeed", lightAttackSpeed);
    heavyAttackSpeed = GetF(ini, L"Gestures", L"HeavyAttackSpeed", heavyAttackSpeed);
    leftSwingSpeed   = GetF(ini, L"Gestures", L"LeftSwingSpeed", leftSwingSpeed);
    lightAttackButton = GetS(ini, L"Gestures", L"LightAttackButton", lightAttackButton);
    heavyAttackButton = GetS(ini, L"Gestures", L"HeavyAttackButton", heavyAttackButton);
    leftSwingButton   = GetS(ini, L"Gestures", L"LeftSwingButton", leftSwingButton);
    gestureCooldown   = GetF(ini, L"Gestures", L"Cooldown", gestureCooldown);
    gesturePressDuration = GetF(ini, L"Gestures", L"PressDuration", gesturePressDuration);
    gestureRequiresGrip  = GetB(ini, L"Gestures", L"RequireGrip", gestureRequiresGrip);
    gestureHaptics       = GetB(ini, L"Gestures", L"Haptics", gestureHaptics);

    handRelativeMovement = GetB(ini, L"Movement", L"HandRelative", handRelativeMovement);
    moveForward = GetS(ini, L"Movement", L"Forward", handRelativeMovement ? std::string("hand") : moveForward);
    rightStick = GetS(ini, L"Movement", L"RightStick", rightStick);
    cameraTurnErvr  = GetB(ini, L"Movement", L"CameraTurnByErvr", cameraTurnErvr);
    turnSpeedDegPerSec = GetF(ini, L"Movement", L"TurnSpeed", turnSpeedDegPerSec);
    snapTurnDegrees = GetF(ini, L"Movement", L"SnapTurnDegrees", snapTurnDegrees);
    moveRelativeToCamera = GetB(ini, L"Movement", L"RelativeToCamera", moveRelativeToCamera);
    snapTurn        = GetB(ini, L"Movement", L"SnapTurn", snapTurn);
    snapTurnPulseSeconds  = GetF(ini, L"Movement", L"SnapTurnPulseSeconds", snapTurnPulseSeconds);
    snapTurnRepeatSeconds = GetF(ini, L"Movement", L"SnapTurnRepeatSeconds", snapTurnRepeatSeconds);

    rumbleToControllers = GetB(ini, L"Haptics", L"RumbleToControllers", rumbleToControllers);
    rumbleScale = GetF(ini, L"Haptics", L"RumbleScale", rumbleScale);
    bhapticsLevel    = GetI(ini, L"bHaptics", L"Level", bhapticsLevel);
    bhapticsCombat   = GetB(ini, L"bHaptics", L"Combat", bhapticsCombat);
    bhapticsDamage   = GetB(ini, L"bHaptics", L"Damage", bhapticsDamage);
    bhapticsMovement = GetB(ini, L"bHaptics", L"Movement", bhapticsMovement);
    bhapticsRumble   = GetB(ini, L"bHaptics", L"Rumble", bhapticsRumble);

    combatEnabled = GetB(ini, L"Combat", L"Enabled", combatEnabled);
    combatMode = GetS(ini, L"Combat", L"Mode", combatMode);
    swingOnSpeed = GetF(ini, L"Combat", L"SwingOnSpeed", swingOnSpeed);
    swingOffSpeed = GetF(ini, L"Combat", L"SwingOffSpeed", swingOffSpeed);
    heavySwingSpeed = GetF(ini, L"Combat", L"HeavySwingSpeed", heavySwingSpeed);
    minHandSpeed = GetF(ini, L"Combat", L"MinHandSpeed", minHandSpeed);
    weaponReachRight = GetF(ini, L"Combat", L"WeaponReachRight", weaponReachRight);
    weaponReachLeft = GetF(ini, L"Combat", L"WeaponReachLeft", weaponReachLeft);
    weaponGeometry = GetS(ini, L"Combat", L"WeaponGeometry", weaponGeometry);
    kineticRefReach = GetF(ini, L"Combat", L"KineticRefReach", kineticRefReach);
    swingArmWindow = GetF(ini, L"Combat", L"ArmWindow", swingArmWindow);
    swingMaxSeconds = GetF(ini, L"Combat", L"MaxSwingSeconds", swingMaxSeconds);
    swingCooldown = GetF(ini, L"Combat", L"Cooldown", swingCooldown);
    combatLeftHand = GetB(ini, L"Combat", L"LeftHand", combatLeftHand);
    combatRequireStamina = GetB(ini, L"Combat", L"RequireStamina", combatRequireStamina);
    combatMinStamina = GetI(ini, L"Combat", L"MinStamina", combatMinStamina);
    lightJudgeId = GetI(ini, L"Combat", L"LightJudgeId", lightJudgeId);
    heavyJudgeId = GetI(ini, L"Combat", L"HeavyJudgeId", heavyJudgeId);
    leftJudgeId = GetI(ini, L"Combat", L"LeftJudgeId", leftJudgeId);
    combatLearn = GetB(ini, L"Combat", L"Learn", combatLearn);
    combatSaveLearned = GetB(ini, L"Combat", L"SaveLearned", combatSaveLearned);
    combatBlockAttackButtons = GetB(ini, L"Combat", L"BlockAttackButtons", combatBlockAttackButtons);
    combatAttackIndexRotate = GetI(ini, L"Combat", L"AttackIndexRotate", combatAttackIndexRotate);
    combatExtraStamina[0] = GetI(ini, L"Combat", L"ExtraStaminaLight", combatExtraStamina[0]);
    combatExtraStamina[1] = GetI(ini, L"Combat", L"ExtraStaminaHeavy", combatExtraStamina[1]);
    combatExtraStamina[2] = GetI(ini, L"Combat", L"ExtraStaminaLeft", combatExtraStamina[2]);
    combatSuppressAnimAttacks = GetB(ini, L"Combat", L"SuppressAnimAttacks", combatSuppressAnimAttacks);
    rideAttacks = GetS(ini, L"Combat", L"RideAttacks", rideAttacks);
    cheatSetAllStats = GetI(ini, L"Cheats", L"SetAllStats", cheatSetAllStats);
    cheatSetLevel = GetI(ini, L"Cheats", L"SetLevel", cheatSetLevel);
    cheatInfiniteStamina = GetB(ini, L"Cheats", L"InfiniteStamina", cheatInfiniteStamina);
    combatDebug = GetB(ini, L"Combat", L"Debug", combatDebug);
    combatMenuGate = GetB(ini, L"Combat", L"MenuGate", combatMenuGate);
    kineticEnabled = GetB(ini, L"Combat", L"Kinetic", kineticEnabled);
    kineticRefSpeed = GetF(ini, L"Combat", L"KineticRefSpeed", kineticRefSpeed);
    kineticArcRef = GetF(ini, L"Combat", L"KineticArc", kineticArcRef);
    kineticDamageScale = GetF(ini, L"Combat", L"KineticDamageScale", kineticDamageScale);
    kineticContinuous = GetB(ini, L"Combat", L"KineticContinuous", kineticContinuous);
    kineticContinuousMult = GetF(ini, L"Combat", L"KineticContinuousMult", kineticContinuousMult);
    kineticStaminaMode = GetS(ini, L"Combat", L"KineticStamina", kineticStaminaMode);
    kineticHitPause = GetF(ini, L"Combat", L"KineticHitPause", kineticHitPause);
    kineticHitStamina = GetI(ini, L"Combat", L"KineticHitStamina", kineticHitStamina);
    kineticMinMult = GetF(ini, L"Combat", L"KineticMinMult", kineticMinMult);
    kineticMaxMult = GetF(ini, L"Combat", L"KineticMaxMult", kineticMaxMult);
    kineticHeavyQuality = GetF(ini, L"Combat", L"KineticHeavyQuality", kineticHeavyQuality);
    kineticAccel = GetF(ini, L"Combat", L"KineticAccel", kineticAccel);
    kineticAccelWeight = GetF(ini, L"Combat", L"KineticAccelWeight", kineticAccelWeight);
    kineticInertia = GetB(ini, L"Combat", L"Inertia", kineticInertia);
    contactOnSpeed = Clamp(GetF(ini, L"Combat", L"ContactOnSpeed", contactOnSpeed), 0.1f, 10.f);
    contactOffSpeed = Clamp(GetF(ini, L"Combat", L"ContactOffSpeed", contactOffSpeed), 0.05f, contactOnSpeed * 0.9f);
    kineticSlices = GetB(ini, L"Combat", L"KineticSlices", kineticSlices);
    contactValleyFrac = Clamp(GetF(ini, L"Combat", L"ContactValley", contactValleyFrac), 0.f, 0.9f);
    contactSpentSeconds = Clamp(GetF(ini, L"Combat", L"ContactSpentSeconds", contactSpentSeconds), 0.1f, 10.f);
    inertiaAngAccel = GetF(ini, L"Combat", L"InertiaAngAccel", inertiaAngAccel);
    inertiaSpringHz = GetF(ini, L"Combat", L"InertiaSpringHz", inertiaSpringHz);
    inertiaComRef = GetF(ini, L"Combat", L"InertiaComRef", inertiaComRef);
    inertiaBrake = GetF(ini, L"Combat", L"InertiaBrake", inertiaBrake);
    inertiaTwoHandTorque = GetF(ini, L"Combat", L"InertiaTwoHandTorque", inertiaTwoHandTorque);
    twoHandAim = GetB(ini, L"Combat", L"TwoHandAim", twoHandAim);
    if (inertiaTwoHandTorque < 0.1f) inertiaTwoHandTorque = 0.1f;
    inertiaMaxLagAngle = GetF(ini, L"Combat", L"InertiaMaxLagAngle", inertiaMaxLagAngle);
    if (inertiaMaxLagAngle < 0.1f) inertiaMaxLagAngle = 0.1f; if (inertiaMaxLagAngle > 2.9f) inertiaMaxLagAngle = 2.9f;
    kineticArmPerWeight = GetF(ini, L"Combat", L"KineticArmPerWeight", kineticArmPerWeight);
    kineticStaminaScale = GetF(ini, L"Combat", L"KineticStaminaScale", kineticStaminaScale);
    kineticStaminaPerStrike = GetF(ini, L"Combat", L"KineticStaminaPerStrike", kineticStaminaPerStrike);
    kineticFatigueStart = GetF(ini, L"Combat", L"KineticFatigueStart", kineticFatigueStart);
    kineticFatigueFloor = GetF(ini, L"Combat", L"KineticFatigueFloor", kineticFatigueFloor);
    kineticPoiseScales = GetB(ini, L"Combat", L"KineticPoiseScales", kineticPoiseScales);
    kineticStaminaDamageScales = GetB(ini, L"Combat", L"KineticStaminaDamageScales", kineticStaminaDamageScales);
    combatEnemyWatch = GetB(ini, L"Combat", L"EnemyWatch", combatEnemyWatch);
    combatEnemyWatchRadius = GetF(ini, L"Combat", L"EnemyWatchRadius", combatEnemyWatchRadius);
    combatEnemyWatchOwnHits = GetB(ini, L"Combat", L"EnemyWatchOwnHits", combatEnemyWatchOwnHits);
    kineticAlignVariation = GetB(ini, L"Combat", L"KineticAlignVariation", kineticAlignVariation);
    kineticSwingBudget = GetF(ini, L"Combat", L"KineticSwingBudget", kineticSwingBudget);
    kineticFlinchFirstHitOnly = GetB(ini, L"Combat", L"KineticFlinchFirstHitOnly", kineticFlinchFirstHitOnly);
    kineticHitRadiusScale = GetF(ini, L"Combat", L"KineticHitRadiusScale", kineticHitRadiusScale);
    blockEnabled = GetB(ini, L"Combat", L"Block", blockEnabled);
    guardButton = GetS(ini, L"Combat", L"GuardButton", guardButton);
    shieldWepTypes = GetS(ini, L"Combat", L"ShieldWepTypes", shieldWepTypes);
    blockReleaseSeconds = GetF(ini, L"Combat", L"BlockReleaseSeconds", blockReleaseSeconds);
    blockMinCut = GetF(ini, L"Combat", L"BlockMinCut", blockMinCut);
    blockMinStability = GetF(ini, L"Combat", L"BlockMinStability", blockMinStability);
    shieldNormalPitchDeg = GetF(ini, L"Combat", L"ShieldNormalPitch", shieldNormalPitchDeg);
    deflectSpeed = GetF(ini, L"Combat", L"DeflectSpeed", deflectSpeed);
    blockImpactStamina = GetI(ini, L"Combat", L"BlockImpactStamina", blockImpactStamina);
    twoHandMode = GetS(ini, L"Combat", L"TwoHand", twoHandMode);
    twoHandModifierButton = GetS(ini, L"Combat", L"TwoHandModifierButton", twoHandModifierButton);
    twoHandChordLead = GetF(ini, L"Combat", L"TwoHandChordLead", twoHandChordLead);
    twoHandChordPress = GetF(ini, L"Combat", L"TwoHandChordPress", twoHandChordPress);
    twoHandChordTail = GetF(ini, L"Combat", L"TwoHandChordTail", twoHandChordTail);
    twoHandAxis = GetS(ini, L"Combat", L"TwoHandAxis", twoHandAxis);
    twoHandGrabRadius = GetF(ini, L"Combat", L"TwoHandGrabRadius", twoHandGrabRadius);
    twoHandGrabMin = GetF(ini, L"Combat", L"TwoHandGrabMin", twoHandGrabMin);
    twoHandGrabMax = GetF(ini, L"Combat", L"TwoHandGrabMax", twoHandGrabMax);
    twoHandPinHand = GetB(ini, L"Combat", L"TwoHandPinHand", twoHandPinHand);
    twoHandPinOrient = GetB(ini, L"Combat", L"TwoHandPinOrient", twoHandPinOrient);
    twoHandPinSideOffset = GetF(ini, L"Combat", L"TwoHandPinSideOffset", twoHandPinSideOffset);
    twoHandLightJudgeId = GetI(ini, L"Combat", L"TwoHandLightJudgeId", twoHandLightJudgeId);
    twoHandHeavyJudgeId = GetI(ini, L"Combat", L"TwoHandHeavyJudgeId", twoHandHeavyJudgeId);
    twoHandSkill = GetS(ini, L"Combat", L"TwoHandSkill", twoHandSkill);
    twoHandSkillButton = GetS(ini, L"Combat", L"TwoHandSkillButton", twoHandSkillButton);
    skillButton = GetS(ini, L"Combat", L"SkillButton", skillButton);
    twoHandSkillRelearnHold = GetF(ini, L"Combat", L"TwoHandSkillRelearnHold", twoHandSkillRelearnHold);
    skillReplayEvents = GetS(ini, L"Combat", L"SkillReplayEvents", skillReplayEvents);
    skillRecordSeconds = GetF(ini, L"Combat", L"SkillRecordSeconds", skillRecordSeconds);
    skillSwingWindow = GetF(ini, L"Combat", L"SkillSwingWindow", skillSwingWindow);
    skillChargeFp = GetB(ini, L"Combat", L"SkillChargeFP", skillChargeFp);
    castAim = GetB(ini, L"Combat", L"CastAim", castAim);
    castAimHand = GetS(ini, L"Combat", L"CastAimHand", castAimHand);
    castAimSeconds = GetF(ini, L"Combat", L"CastAimSeconds", castAimSeconds);
    castAimAxis = GetS(ini, L"Combat", L"CastAimAxis", castAimAxis);
    castAimPitchOffsetDeg = GetF(ini, L"Combat", L"CastAimPitchOffsetDeg", castAimPitchOffsetDeg);
    castAimTurnBody = GetB(ini, L"Combat", L"CastAimTurnBody", castAimTurnBody);
    castAimLimitDeg = GetF(ini, L"Combat", L"CastAimLimitDeg", castAimLimitDeg);
    castAimForceLock = GetB(ini, L"Combat", L"CastAimForceLock", castAimForceLock);
    castAimWeaponTypes = GetS(ini, L"Combat", L"CastAimWeaponTypes", castAimWeaponTypes);

    wheelEnabled       = GetB(ini, L"WeaponWheel", L"Enabled", wheelEnabled);
    wheelHands         = GetS(ini, L"WeaponWheel", L"Hands", wheelHands);
    wheelHoldSeconds   = GetF(ini, L"WeaponWheel", L"HoldSeconds", wheelHoldSeconds);
    wheelStillSpeed    = GetF(ini, L"WeaponWheel", L"StillSpeed", wheelStillSpeed);
    wheelSize          = GetF(ini, L"WeaponWheel", L"Size", wheelSize);
    wheelDeadZone      = GetF(ini, L"WeaponWheel", L"DeadZone", wheelDeadZone);
    wheelOpacity       = GetF(ini, L"WeaponWheel", L"Opacity", wheelOpacity);
    wheelHaptics       = GetB(ini, L"WeaponWheel", L"Haptics", wheelHaptics);
    wheelSwitchTimeout = GetF(ini, L"WeaponWheel", L"SwitchTimeout", wheelSwitchTimeout);
    wheelSwitch        = GetS(ini, L"WeaponWheel", L"Switch", wheelSwitch);

    beltEnabled        = GetB(ini, L"Belt", L"Enabled", beltEnabled);
    beltHands          = GetS(ini, L"Belt", L"Hands", beltHands);
    beltUseButton      = GetS(ini, L"Belt", L"UseButton", beltUseButton);
    beltDrop           = GetF(ini, L"Belt", L"Drop", beltDrop);
    beltRadius         = GetF(ini, L"Belt", L"Radius", beltRadius);
    beltAngles         = GetS(ini, L"Belt", L"Angles", beltAngles);
    beltSize           = GetF(ini, L"Belt", L"Size", beltSize);
    beltGrabRadius     = GetF(ini, L"Belt", L"GrabRadius", beltGrabRadius);
    beltFollowDeg      = GetF(ini, L"Belt", L"FollowDegrees", beltFollowDeg);
    beltOpacity        = GetF(ini, L"Belt", L"Opacity", beltOpacity);
    beltHaptics        = GetB(ini, L"Belt", L"Haptics", beltHaptics);
    beltPouchButton    = GetS(ini, L"Belt", L"PouchButton", beltPouchButton);
    beltDirections     = GetS(ini, L"Belt", L"Directions", beltDirections);
    beltChordLead      = GetF(ini, L"Belt", L"ChordLead", beltChordLead);
    beltChordPress     = GetF(ini, L"Belt", L"ChordPress", beltChordPress);
    beltChordTail      = GetF(ini, L"Belt", L"ChordTail", beltChordTail);

    fpEnabled = GetB(ini, L"FirstPerson", L"Enabled", fpEnabled);
    fpEyeHeight = GetF(ini, L"FirstPerson", L"EyeHeight", fpEyeHeight);
    fpCameraAnchor = GetS(ini, L"FirstPerson", L"CameraAnchor", fpCameraAnchor);
    fpCameraAnchorSmoothing = GetF(ini, L"FirstPerson", L"CameraAnchorSmoothing", fpCameraAnchorSmoothing);
    fpCameraBob = GetF(ini, L"FirstPerson", L"CameraBob", fpCameraBob);
    locomotion = GetS(ini, L"Locomotion", L"Mode", locomotion);
    locoRunSpeed = GetF(ini, L"Locomotion", L"RunSpeed", locoRunSpeed);
    locoSprintSpeed = GetF(ini, L"Locomotion", L"SprintSpeed", locoSprintSpeed);
    locoFeedStick = GetB(ini, L"Locomotion", L"FeedStick", locoFeedStick);
    fpCameraMode = GetS(ini, L"FirstPerson", L"CameraMode", fpCameraMode);
    fpCameraRotation = GetS(ini, L"FirstPerson", L"CameraRotation", fpCameraRotation);
    fpCameraYawSource = GetS(ini, L"FirstPerson", L"CameraYawSource", fpCameraYawSource);
    fpOwnChrCam = GetB(ini, L"FirstPerson", L"OwnChrCam", fpOwnChrCam);
    fpRecenter = GetB(ini, L"FirstPerson", L"Recenter", fpRecenter);
    fpRecenterSeconds = GetF(ini, L"FirstPerson", L"RecenterSeconds", fpRecenterSeconds);
    fpRecenterMaxRadius = GetF(ini, L"FirstPerson", L"RecenterMaxRadius", fpRecenterMaxRadius);
    fpRecenterSnapRadius = GetF(ini, L"FirstPerson", L"RecenterSnapRadius", fpRecenterSnapRadius);
    fpRecenterHeight = GetB(ini, L"FirstPerson", L"RecenterHeight", fpRecenterHeight);
    fpFaceCamera = GetB(ini, L"FirstPerson", L"FaceCamera", fpFaceCamera);
    fpPoseMode = GetS(ini, L"FirstPerson", L"PoseMode", fpPoseMode);
    {   // "auto" resolves against PoseMode on every (re)load so hot-editing PoseMode keeps them consistent
        std::string fcm = GetS(ini, L"FirstPerson", L"FaceCameraMode", "auto");
        fpFaceCameraMode = (fcm == "auto") ? (fpPoseMode == "vr" ? std::string("strafe") : std::string("lock")) : fcm;
    }
    fpLockTargetDistance = GetF(ini, L"FirstPerson", L"LockTargetDistance", fpLockTargetDistance);
    fpArmRootSource = GetS(ini, L"FirstPerson", L"ArmRootSource", fpArmRootSource);
    fpPinBodyFacing = GetB(ini, L"FirstPerson", L"PinBodyFacing", fpPinBodyFacing);
    fpPoseAfterHavok = GetB(ini, L"FirstPerson", L"PoseAfterHavok", fpPoseAfterHavok);
    fpLatchVRPerFrame = GetB(ini, L"FirstPerson", L"LatchVRPerFrame", fpLatchVRPerFrame);
    fpCameraFinalWrite = GetB(ini, L"FirstPerson", L"CameraFinalWrite", fpCameraFinalWrite);
    fpCameraAfterModelPos = GetB(ini, L"FirstPerson", L"CameraAfterModelPos", fpCameraAfterModelPos);
    fpArmSolveEveryCall = GetB(ini, L"FirstPerson", L"ArmSolveEveryCall", fpArmSolveEveryCall);
    fpHideHead = GetB(ini, L"FirstPerson", L"HideHead", fpHideHead);
    fpHideSheathed = GetB(ini, L"FirstPerson", L"HideSheathedWeapons", fpHideSheathed);
    fpNearDither = GetF(ini, L"FirstPerson", L"NearDither", fpNearDither);
    fpWriteAim = GetB(ini, L"FirstPerson", L"WriteAim", fpWriteAim);
    handTracking = GetB(ini, L"FirstPerson", L"HandTracking", handTracking);
    handOrigin = GetS(ini, L"FirstPerson", L"HandOrigin", handOrigin);
    handBackOffset = GetF(ini, L"FirstPerson", L"HandBackOffset", handBackOffset);
    handRotOffX = GetF(ini, L"FirstPerson", L"HandRotOffsetX", handRotOffX);
    handRotOffY = GetF(ini, L"FirstPerson", L"HandRotOffsetY", handRotOffY);
    handRotOffZ = GetF(ini, L"FirstPerson", L"HandRotOffsetZ", handRotOffZ);
    elbowPoleDown = GetF(ini, L"FirstPerson", L"ElbowPoleDown", elbowPoleDown);
    elbowPoleOut = GetF(ini, L"FirstPerson", L"ElbowPoleOut", elbowPoleOut);
    elbowPoleBack = GetF(ini, L"FirstPerson", L"ElbowPoleBack", elbowPoleBack);
    armStretch = GetF(ini, L"FirstPerson", L"ArmStretch", armStretch);
    handScale = GetF(ini, L"FirstPerson", L"HandScale", handScale);
    bodyYawSource = GetS(ini, L"FirstPerson", L"BodyYaw", bodyYawSource);
    bodyFacingOffsetDeg = GetF(ini, L"FirstPerson", L"BodyFacingOffsetDeg", bodyFacingOffsetDeg);
    bodyYawFollowSeconds = GetF(ini, L"FirstPerson", L"BodyYawFollowSeconds", bodyYawFollowSeconds);
    bodyYawDeadzoneDeg = GetF(ini, L"FirstPerson", L"BodyYawDeadzoneDeg", bodyYawDeadzoneDeg);
    rideFreeLookDeg = Clamp(GetF(ini, L"FirstPerson", L"RideFreeLookDeg", rideFreeLookDeg), -1.f, 170.f);
    rideTurnsWithHead = GetB(ini, L"FirstPerson", L"RideTurnsWithHead", rideTurnsWithHead);
    rideCameraSmoothing = Clamp(GetF(ini, L"FirstPerson", L"RideCameraSmoothing", rideCameraSmoothing), 0.f, 2.f);
    rideCameraMaxDeviation = Clamp(GetF(ini, L"FirstPerson", L"RideCameraMaxDeviation", rideCameraMaxDeviation), 0.f, 0.5f);
    torsoFaceCamera = GetB(ini, L"FirstPerson", L"TorsoFaceCamera", torsoFaceCamera);
    bodyFollowsHead = GetB(ini, L"FirstPerson", L"BodyFollowsHead", bodyFollowsHead);
    bodyAnchor = GetS(ini, L"FirstPerson", L"BodyAnchor", bodyAnchor);
    bodyRotateToHead = GetB(ini, L"FirstPerson", L"BodyRotate", bodyRotateToHead);
    bodyFollowMaxHorizontal = GetF(ini, L"FirstPerson", L"BodyFollowMaxHorizontal", bodyFollowMaxHorizontal);
    bodyFollowMaxDown = GetF(ini, L"FirstPerson", L"BodyFollowMaxDown", bodyFollowMaxDown);
    bodyFollowMaxUp = GetF(ini, L"FirstPerson", L"BodyFollowMaxUp", bodyFollowMaxUp);
    bodyBackOffset = GetF(ini, L"FirstPerson", L"BodyBackOffset", bodyBackOffset);
    boneTorso = GetS(ini, L"FirstPerson", L"BoneTorso", boneTorso);
    boneHead = GetS(ini, L"FirstPerson", L"BoneHead", boneHead);
    boneLUpperArm = GetS(ini, L"FirstPerson", L"BoneLeftUpperArm", boneLUpperArm);
    boneLForearm = GetS(ini, L"FirstPerson", L"BoneLeftForearm", boneLForearm);
    boneLHand = GetS(ini, L"FirstPerson", L"BoneLeftHand", boneLHand);
    boneRUpperArm = GetS(ini, L"FirstPerson", L"BoneRightUpperArm", boneRUpperArm);
    boneRForearm = GetS(ini, L"FirstPerson", L"BoneRightForearm", boneRForearm);
    boneRHand = GetS(ini, L"FirstPerson", L"BoneRightHand", boneRHand);
    dumpSkeleton = GetB(ini, L"FirstPerson", L"DumpSkeleton", dumpSkeleton);
    fpPoseScan = GetB(ini, L"FirstPerson", L"PoseWriterScan", fpPoseScan);
    fpDebug = GetB(ini, L"FirstPerson", L"Debug", fpDebug);
    fpForceFovDeg = GetF(ini, L"FirstPerson", L"ForceFovDeg", fpForceFovDeg);
    fpForcePitchDeg = GetF(ini, L"FirstPerson", L"ForcePitchDeg", fpForcePitchDeg);

    menuNavEnabled = GetB(ini, L"MenuNav", L"Enabled", menuNavEnabled);
    menuNavThreshold = GetF(ini, L"MenuNav", L"Threshold", menuNavThreshold);
    menuNavFollowStart = GetB(ini, L"MenuNav", L"FollowStartButton", menuNavFollowStart);
    menuNavToggleHold = GetF(ini, L"MenuNav", L"ToggleHold", menuNavToggleHold);
    menuNavHaptics = GetB(ini, L"MenuNav", L"Haptics", menuNavHaptics);
    menuSource = GetS(ini, L"MenuNav", L"Source", menuSource);
    menuAob =GetS(ini, L"MenuNav", L"MenuAOB", menuAob);
    menuAobOffset = GetI(ini, L"MenuNav", L"MenuAOBOffset", menuAobOffset);
    menuAobInstrLen = GetI(ini, L"MenuNav", L"MenuAOBInstrLen", menuAobInstrLen);
    menuChain = GetS(ini, L"MenuNav", L"MenuChain", menuChain);
    menuScan = GetB(ini, L"MenuNav", L"Scan", menuScan);
    menuScanSize = GetI(ini, L"MenuNav", L"ScanSize", menuScanSize);

    handAimEnabled = GetB(ini, L"Experimental", L"HandAim", handAimEnabled);
    cameraAob      = GetS(ini, L"Experimental", L"CameraAOB", cameraAob);
    cameraAobOffset = GetI(ini, L"Experimental", L"CameraAOBOffset", cameraAobOffset);
    cameraAobInstrLen = GetI(ini, L"Experimental", L"CameraAOBInstrLen", cameraAobInstrLen);
    cameraChain    = GetS(ini, L"Experimental", L"CameraChain", cameraChain);

    physicsEnabled = GetB(ini, L"Physics", L"Enabled", physicsEnabled);
    physicsTrackRadius = GetF(ini, L"Physics", L"TrackRadius", physicsTrackRadius);
    physicsDumpBones = GetB(ini, L"Physics", L"DumpBones", physicsDumpBones);
    physicsTestBone = GetS(ini, L"Physics", L"TestBone", physicsTestBone);
    physicsTestAxis = GetS(ini, L"Physics", L"TestAxis", physicsTestAxis);
    physicsTestDeg = GetF(ini, L"Physics", L"TestDeg", physicsTestDeg);
    physicsTestRadius = GetF(ini, L"Physics", L"TestRadius", physicsTestRadius);
    physicsSword = GetB(ini, L"Physics", L"Sword", physicsSword);
    physicsSwordKgPerWeight = GetF(ini, L"Physics", L"SwordKgPerWeight", physicsSwordKgPerWeight);
    physicsSwordHandKg = GetF(ini, L"Physics", L"SwordHandKg", physicsSwordHandKg);
    physicsSwordForceBase = GetF(ini, L"Physics", L"SwordForceBase", physicsSwordForceBase);
    physicsSwordForcePerStr = GetF(ini, L"Physics", L"SwordForcePerStr", physicsSwordForcePerStr);
    physicsSwordTorqueBase = GetF(ini, L"Physics", L"SwordTorqueBase", physicsSwordTorqueBase);
    physicsSwordTorquePerStr = GetF(ini, L"Physics", L"SwordTorquePerStr", physicsSwordTorquePerStr);
    physicsSwordLinearSpring = GetF(ini, L"Physics", L"SwordLinearSpring", physicsSwordLinearSpring);
    physicsSwordLinearDamper = GetF(ini, L"Physics", L"SwordLinearDamper", physicsSwordLinearDamper);
    physicsSwordAngularSpring = GetF(ini, L"Physics", L"SwordAngularSpring", physicsSwordAngularSpring);
    physicsSwordAngularDamper = GetF(ini, L"Physics", L"SwordAngularDamper", physicsSwordAngularDamper);
    physicsSwordMinAngleDeg = GetF(ini, L"Physics", L"SwordMinAngleDeg", physicsSwordMinAngleDeg);
    physicsSwordMaxAngleDeg = GetF(ini, L"Physics", L"SwordMaxAngleDeg", physicsSwordMaxAngleDeg);
    physicsSwordLoadForMax = GetF(ini, L"Physics", L"SwordLoadForMax", physicsSwordLoadForMax);
    physicsSwordSnapDistance = GetF(ini, L"Physics", L"SwordSnapDistance", physicsSwordSnapDistance);
    physicsSwordLog = GetB(ini, L"Physics", L"SwordLog", physicsSwordLog);
    physicsContact = GetB(ini, L"Physics", L"Contact", physicsContact);
    physicsContactRadius = GetF(ini, L"Physics", L"ContactRadius", physicsContactRadius);
    physicsEnemyArmInertia = GetF(ini, L"Physics", L"EnemyArmInertia", physicsEnemyArmInertia);
    physicsEnemyArmStiffness = GetF(ini, L"Physics", L"EnemyArmStiffness", physicsEnemyArmStiffness);
    physicsEnemyArmDamping = GetF(ini, L"Physics", L"EnemyArmDamping", physicsEnemyArmDamping);
    physicsEnemyArmMaxDeg = GetF(ini, L"Physics", L"EnemyArmMaxDeg", physicsEnemyArmMaxDeg);
    physicsEnemyWeaponBone = GetS(ini, L"Physics", L"EnemyWeaponBone", physicsEnemyWeaponBone);
    physicsEnemyWeaponAxis = GetS(ini, L"Physics", L"EnemyWeaponAxis", physicsEnemyWeaponAxis);
    physicsEnemyWeaponLength = GetF(ini, L"Physics", L"EnemyWeaponLength", physicsEnemyWeaponLength);
    physicsEnemyWeaponKg = GetF(ini, L"Physics", L"EnemyWeaponKg", physicsEnemyWeaponKg);
    physicsEnemyArmForce = GetF(ini, L"Physics", L"EnemyArmForce", physicsEnemyArmForce);
    physicsEnemyArmTorque = GetF(ini, L"Physics", L"EnemyArmTorque", physicsEnemyArmTorque);
    physicsTorsoSway = GetB(ini, L"Physics", L"TorsoSway", physicsTorsoSway);
    physicsTorsoInertia = GetF(ini, L"Physics", L"TorsoInertia", physicsTorsoInertia);
    physicsTorsoStiffness = GetF(ini, L"Physics", L"TorsoStiffness", physicsTorsoStiffness);
    physicsTorsoDamping = GetF(ini, L"Physics", L"TorsoDamping", physicsTorsoDamping);
    physicsTorsoMaxDeg = GetF(ini, L"Physics", L"TorsoMaxDeg", physicsTorsoMaxDeg);
    physicsHurtboxProbe = GetB(ini, L"Physics", L"HurtboxProbe", physicsHurtboxProbe);
    physicsContactLog = GetB(ini, L"Physics", L"ContactLog", physicsContactLog);
    physicsBodyContact = GetB(ini, L"Physics", L"BodyContact", physicsBodyContact);
    physicsBodyScale = GetF(ini, L"Physics", L"BodyScale", physicsBodyScale);
    physicsGameShapes = GetB(ini, L"Physics", L"GameBodyShapes", physicsGameShapes);
    physicsBodyCutEnergy = GetF(ini, L"Physics", L"BodyCutEnergy", physicsBodyCutEnergy);
    physicsBodyStickEnergy = GetF(ini, L"Physics", L"BodyStickEnergy", physicsBodyStickEnergy);
    physicsBodyPierceEnergy = GetF(ini, L"Physics", L"BodyPierceEnergy", physicsBodyPierceEnergy);
    physicsBodyFriction = GetF(ini, L"Physics", L"BodyFriction", physicsBodyFriction);
    physicsGateStrikes = GetB(ini, L"Physics", L"GateStrikes", physicsGateStrikes);
    physicsArmHitEnergy = GetF(ini, L"Physics", L"ArmHitEnergy", physicsArmHitEnergy);
    physicsHitJudge = GetB(ini, L"Physics", L"HitJudge", physicsHitJudge);
    physicsHitWindow = GetF(ini, L"Physics", L"HitWindow", physicsHitWindow);
    physicsBodyRadius = GetF(ini, L"Physics", L"BodyRadius", physicsBodyRadius);
    physicsShieldRadius = GetF(ini, L"Physics", L"ShieldRadius", physicsShieldRadius);
    physicsShieldStaminaPerDamage = GetF(ini, L"Physics", L"ShieldStaminaPerDamage", physicsShieldStaminaPerDamage);
    physicsEnemyWeaponRefSpeed = GetF(ini, L"Physics", L"EnemyWeaponRefSpeed", physicsEnemyWeaponRefSpeed);
    physicsGrazeMin = GetF(ini, L"Physics", L"GrazeMin", physicsGrazeMin);
    physicsSwingStamina = GetB(ini, L"Physics", L"SwingStamina", physicsSwingStamina);
    physicsSwingStaminaSpeed = GetF(ini, L"Physics", L"SwingStaminaSpeed", physicsSwingStaminaSpeed);
    physicsSwingStaminaPerSec = GetF(ini, L"Physics", L"SwingStaminaPerSec", physicsSwingStaminaPerSec);
    physicsSwingRegenDelay = GetF(ini, L"Physics", L"SwingRegenDelay", physicsSwingRegenDelay);
    physicsTiredBelow = GetF(ini, L"Physics", L"TiredBelow", physicsTiredBelow);
    physicsTiredStrength = GetF(ini, L"Physics", L"TiredStrength", physicsTiredStrength);
    cameraYawOffset   = GetI(ini, L"Experimental", L"CameraYawOffset", cameraYawOffset);
    cameraPitchOffset = GetI(ini, L"Experimental", L"CameraPitchOffset", cameraPitchOffset);
    handAimStrength = GetF(ini, L"Experimental", L"HandAimStrength", handAimStrength);
    patchFpsTarget = GetI(ini, L"Patches", L"FpsTarget", patchFpsTarget);
    patchDpiAware  = GetB(ini, L"Patches", L"DpiAware", patchDpiAware);

    camWatch        = GetB(ini, L"Experimental", L"CamWatch", camWatch);
    camWatchDelay   = GetF(ini, L"Experimental", L"CamWatchDelay", camWatchDelay);
    camWatchSeconds = GetF(ini, L"Experimental", L"CamWatchSeconds", camWatchSeconds);
    camWatchDt      = GetB(ini, L"Experimental", L"CamWatchDt", camWatchDt);
    camWatchIgt     = GetB(ini, L"Experimental", L"CamWatchIgt", camWatchIgt);
    camWatchDst     = GetB(ini, L"Experimental", L"CamWatchDst", camWatchDst);
    igtTrace        = GetB(ini, L"Experimental", L"IgtTrace", igtTrace);
    m2TaskProbe    = GetB(ini, L"Experimental", L"TaskProbe", m2TaskProbe);
    m2ForcePass2   = GetB(ini, L"Experimental", L"ForcePass2", m2ForcePass2);
    m2KillPlayerAfter = GetI(ini, L"Experimental", L"KillPlayerAfter", m2KillPlayerAfter);
    m2Pass2NoGuards = GetB(ini, L"Experimental", L"Pass2NoGuards", m2Pass2NoGuards);
    m2Pass2NoDeadGuard = GetB(ini, L"Experimental", L"Pass2NoDeadGuard", m2Pass2NoDeadGuard);
    taskRegisterTest  = GetB(ini, L"Experimental", L"TaskRegisterTest", taskRegisterTest);
    taskQueueProbe    = GetB(ini, L"Experimental", L"TaskQueueProbe", taskQueueProbe);
    taskDumpLists     = GetB(ini, L"Experimental", L"TaskDumpLists", taskDumpLists);
    taskDoublePumpTest = GetB(ini, L"Experimental", L"TaskDoublePumpTest", taskDoublePumpTest);
    taskContentProbe   = GetB(ini, L"Experimental", L"TaskContentProbe", taskContentProbe);
    taskToggle        = GetS(ini, L"Experimental", L"TaskToggle", taskToggle);
    taskToggleDelay   = GetF(ini, L"Experimental", L"TaskToggleDelay", taskToggleDelay);
    taskToggleSeconds = GetF(ini, L"Experimental", L"TaskToggleSeconds", taskToggleSeconds);
    taskToggleCycles  = GetI(ini, L"Experimental", L"TaskToggleCycles", taskToggleCycles);
    taskToggleMode    = GetS(ini, L"Experimental", L"TaskToggleMode", taskToggleMode);
    autoPilotSeconds = GetI(ini, L"Experimental", L"AutoPilotSeconds", autoPilotSeconds);
    expDumpBackbufferAt = GetF(ini, L"Experimental", L"DumpBackbufferAt", expDumpBackbufferAt);
    autoPilotDelay   = GetF(ini, L"Experimental", L"AutoPilotDelay", autoPilotDelay);
    autoPilotPeriod  = GetF(ini, L"Experimental", L"AutoPilotPeriod", autoPilotPeriod);
    autoPilotButton  = GetS(ini, L"Experimental", L"AutoPilotButton", autoPilotButton);
    autoPilotWiggleSeconds = GetI(ini, L"Experimental", L"AutoPilotWiggleSeconds", autoPilotWiggleSeconds);
    autoPilotMenuSeconds = GetI(ini, L"Experimental", L"AutoPilotMenuSeconds", autoPilotMenuSeconds);
    autoPilotMenuPeriod  = GetF(ini, L"Experimental", L"AutoPilotMenuPeriod", autoPilotMenuPeriod);
    autoPilotMenuButton  = GetS(ini, L"Experimental", L"AutoPilotMenuButton", autoPilotMenuButton);
}

Config& Cfg() { return g_cfg; }

void ConfigLogNotes() {
    if (!g_cfg.pass2KeepNote.empty())
        LOGI("[config] %s in ERVR.ini is 0.6.0's value, which kept doors, levers and new graces from opening: "
             "using ERVR's own (the event manager back in the second eye's pass). Delete the line to stop this note.",
             g_cfg.pass2KeepNote.c_str());
}
}
