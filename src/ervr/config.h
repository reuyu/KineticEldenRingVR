#pragma once
#include <string>
#include <cstdint>

namespace ervr {

// Pseudo XInput masks used by virtual_pad for digital trigger mappings.
// (0x0400 and 0x0800 are unused by XInput; we take 0x0400 for RT and 0x0800 for LT.)
constexpr uint16_t VP_RT = 0x0400;
constexpr uint16_t VP_LT = 0x0800;

// XInput button names accepted in the ini (case-insensitive). 0 if none/unknown.
uint16_t ParseXInputButton(const std::string& name);
const char* XInputButtonName(uint16_t mask);

struct Config {
    // [General]
    std::string logLevel = "info";
    std::string vrRuntime = "openxr";      // openxr (ERVR owns its own session)
    bool enabled = true;
    int  pollHz = 250;

    // [VR]  ERVR's own OpenXR session + stereo rendering (ReShade add-on supplies D3D12).
    std::string vrStereoMode = "full";     // off | cinema (flat game on a head-locked quad) | debugalt | full
    float vrRenderScale = 1.0f;            // supersampling of the per-eye images, relative to delivered game pixels
    float vrWorldScale = 1.0f;             // 1.0 = metric; scales the per-eye separation
    int   vrPairRateHz = 0;                // 0 = headset refresh / 2
    float vrCinemaDistance = 2.5f;         // m in front of the head
    float vrCinemaWidth = 4.0f;            // m; height follows the back buffer's aspect
    bool  vrMirrorWindow = true;           // keep presenting to the monitor as well
    // Headset refresh rate requested through XR_FB_display_refresh_rate at session start.
    // 0 = keep the runtime's/streamer's rate; N = the supported rate nearest N. VDXR only
    // offers its current streaming rate, so under Virtual Desktop this is set in VD itself.
    int   vrHeadsetRefreshHz = 0;
    // Raise the game's frame limiter above the headset's refresh while a VR session runs. A
    // limiter below the refresh can never meet it and the runtime halves the game's rate
    // (FpsTarget 72 under a 90 Hz Quest 3: 45 Hz half-rate, ~52 pairs/s; at 90, 88 pairs/s).
    bool  vrLimiterFollowsHeadset = true;
    bool  vrXrDebug = false;               // enable XR_EXT_debug_utils when the runtime offers it
    // Vertical FOV in degrees to force into the camera so one image covers a whole eye.
    // The render thread crops back down to the runtime's exact FOV, so too wide only costs
    // resolution while too narrow costs field of view. 0 = derive it from the headset.
    float vrFovDeg = 0.f;
    std::string vrFovAxis = "vertical";    // which axis Cam_Fov means: vertical | horizontal
    // The reference height Cam_Fov spans, for the FOV request and the eye crop. Settled by
    // the 2026-08-23 pitch probe: WRITING Cam_Fov rebuilds the projection against the
    // CURRENT viewport height, and stereo writes it every frame - so the reference is the
    // current frame height, full stop. (The 08-22 "boot-locked frustum" A/B was real but
    // only describes frames where the fov is never written; treating it as the in-play
    // model claimed 1.9x too much angle per crop on the oversized frame = magnified world,
    // 0.53x head-rotation gain (the skew), hyper-magnified stereo (the depth regression).)
    //   0  = auto: the current frame height (the measured written-fov behavior)
    //   >0 = explicit reference height in px, for experiments only
    // Hot-reloads.
    float vrGameRasterRefHeight = 0.f;
    float vrIpdMm = 0.f;                   // override the eye separation the runtime reports; 0 = trust it
    // Back-buffer colour encoding. auto = convert when the format is an LDR UNORM (which is
    // gamma encoded) and the XR swapchain is read as linear. "none" if the picture looks
    // too dark, "srgb" to force the conversion on.
    std::string vrColorFix = "auto";       // auto | srgb | none
    // Game render resolution for VR, written into the game's own GraphicsConfig.xml (the
    // mechanism R.E.A.L. VR uses: the windowed size there is honoured even when no monitor
    // could display it, so it needs no menu step and no patching). The per-eye image is a
    // CROP of the game's frame, so this IS the VR sharpness - and per-eye pixels track the
    // frame's HEIGHT almost alone, since a wider frame just widens the rendered FOV
    // proportionally. "auto" = the headset's recommended per-eye size (cached in
    // ERVR.cache.ini so it is known at boot, before the runtime is up), "WxH" = exact
    // override, "off" = leave the player's own resolution alone.
    std::string vrGameRes = "auto";        // off | auto | WxH (e.g. 3072x3264)
    // Scales the auto target (GPU headroom knob). 0.86 = ~3200x1800 for a Quest 3 at VD's
    // 2688x2880 budget - what a 90 Hz headset gets held at on an RTX 5090 (1.0 = 3712x2088 was
    // GPU-bound there). The target lands exactly now (see EarlyLastSlotWrite), no more snap,
    // so it is made exact 16:9 first (SnapGameRes in game_res.h).
    float vrGameResScale = 0.86f;
    // Make the game accept a target LARGER than the desktop - the native alternative to
    // NVIDIA DSR, and the difference between 1440 and full headset-height frames on a
    // 3440x1440 monitor. Two parts: the settings-apply pass's own too-big-for-the-screen
    // entry check is NOPed (kPatSizeCheck in patches_er.cpp), and the Windows max-track
    // window clamp is lifted by the monitor-info hooks.
    // The auto target under it is a 16:9 frame at the headset's pixel budget (Auto169Target
    // in patches_er.cpp) - NOT the eye-shaped frame of 08-22/23, where every geometry
    // regression lived (the game's projection carries a constant 1.778 aspect; a 0.94 buffer
    // fought it). A tall 16:9 frame has exactly the fitted frame's proven geometry, denser.
    // Falls back to fit-to-desktop when the size-check pattern is missing, so ON is safe.
    bool  vrGameResOversize = true;
    // Diagnostic for the oversize hunt: PAGE_GUARD the patched resolution-table slots for the
    // first GameResScanSeconds of the run and log every reader/writer ("[reswatch]" lines):
    // rip, registers, instruction bytes. This is the census that names the code doing the
    // desktop-size check above (and would expose an Arxan repair restoring the slot as a
    // WRITE record). One-shot at startup, off for play - every touch of those pages costs an
    // exception while armed. Meaningful with GameResOversize=true (the reject path fires).
    bool  vrGameResScan = false;
    float vrGameResScanSeconds = 45.f;
    // Remove the game's 16:9 letterbox mask when GameRes is not 16:9. The game renders the
    // whole window and then masks it back to 16:9, which both wastes the pixels and leaves
    // the eye crop computing against an aspect the image does not actually have (black bars
    // plus head-motion shear, measured 2026-08-21). Set false to fall back to 16:9 targets.
    bool  vrGameResAspectFix = true;

    // [HUD]  the in-game UI, lifted out of the frame onto its own world-space quad layer.
    // See hud.h for how the UI draws are recognised and redirected.
    std::string hudMode = "quad";       // off | quad
    // Which render target the UI is painted onto. It MOVES during a session (measured on
    // 1.16.2, 2026-08-21): the title screen paints onto the back buffer, but in-game the
    // engine tonemaps into a full-size offscreen R8G8B8A8 target, draws the HUD onto that,
    // and copies it over in one draw. "auto" re-derives the target whenever the current
    // choice goes quiet, in both directions; "backbuffer" pins it.
    std::string hudTarget = "auto";     // auto | backbuffer
    // How many draws into that target belong to the FRAME rather than to the UI. On this
    // game the first one is the tonemap/output pass and everything after it is UI, so 1 -
    // verified in both places the UI lives (2 UI draws at the title, ~23 in-game).
    // Numpad 8 steps it live; [HUD] Probe prints the census that tells you the right value.
    int   hudFirstDraw = 1;
    bool  hudProbe = false;             // log the frame's render-target/draw census once a second
    // Where the panel hangs. body = world-locked with a lazy follow (default: the compositor
    // can reproject it perfectly, unlike head-locked content at one XR frame per stereo
    // pair), head = welded to the view, world = dropped in the world and left there.
    std::string hudLock = "body";       // body | head | world
    float hudDistance = 2.0f;           // metres in front of the eyes
    float hudWidth = 2.4f;              // metres; height follows the captured frame's aspect
    float hudHeightOffset = 0.f;        // metres up (+) or down (-) from eye level
    float hudYawDeg = 0.f, hudPitchDeg = 0.f;
    float hudOpacity = 1.0f;            // 0..1, multiplied into the layer's coverage
    // Where the panel's coverage comes from. ELDEN RING's UI pipelines never write the
    // alpha channel, so there is nothing to read: keyed on brightness, every black menu
    // panel and text outline came out invisible. "dual" solves for coverage instead, by
    // capturing the same UI over two known backgrounds - see BlitParams::Alpha.
    std::string hudKey = "dual";        // dual | max | alpha | luma
    // Test switch: redirect the UI even when there is no VR session to submit the panel
    // into. Normally the redirect stands down in that case, because taking the HUD out of
    // the frame with nowhere to put it just loses it - but that also makes the capture path
    // untestable in an unattended flat run, which is the only kind this repo can automate.
    bool  hudRedirectWithoutVR = false;
    // Composite the reconstructed panel back onto the game's own frame. Off for play (the
    // panel belongs in the headset, and drawing it twice is just a cost); on to see what the
    // capture actually produced - coverage, colour, whether the menu is there - without
    // putting a headset on. Pairs with RedirectWithoutVR for unattended runs.
    bool  hudMirrorPanel = false;
    float hudFollowDeg = 25.f;          // body lock: head turn that re-places the panel
    float hudFollowRadius = 0.5f;       // body lock: head travel (m) that re-places it

    // [Buttons]  Touch -> XInput. Values are XInput button names or "none".
    std::string btnA = "A", btnB = "B", btnX = "X", btnY = "Y";
    std::string btnLThumb = "LS", btnRThumb = "RS", btnMenu = "START";
    std::string btnLTrigger = "LB", btnRTrigger = "RB";
    std::string btnLGrip = "LT",  btnRGrip = "RT";
    float triggerThreshold = 0.55f;
    float gripThreshold = 0.60f;
    float menuHoldForBack = 0.45f;         // hold Menu this long -> BACK (map) instead of START
    bool  leftGripPlusStickIsDpad = true;  // hold left grip: left stick acts as D-pad (quick slots)
    // Valve Index trackpads, pressed: MENU (the Touch menu button: tap START, hold BACK), DPAD (the D-pad
    // in the direction pressed), any button name, or none.
    std::string btnLeftPad = "MENU", btnRightPad = "DPAD";
    float padForce = 0.5f;                 // trackpad force that counts as a press (released below 0.6x)
    float indexGripForce = 0.5f;           // Index grip squeeze force that reads as a full grip; 0 = its touch sensor

    // [Sticks]
    float stickDeadzone = 0.15f;
    bool  swapSticks = false;
    float rightStickSensitivity = 1.0f;
    bool  stickGlitchFilter = true;         // physicality filter on the runtime's stick samples (2026-08-25 Quest Pro / VDXR left-stick corruption)
    float stickGlitchSweepDegPerSec = 720.f;// fastest a real thumb sweeps the stick around the rim; above half deflection the direction slews at this rate
    float stickGlitchRisePerSec = 25.f;     // fastest a real thumb pushes from idle to full deflection (25/s = 40 ms)
    float stickGlitchFallPerSec = 30.f;     // fastest a real thumb releases (30/s = 33 ms full-to-idle); 0 = instant (pre-2026-08-31). Holds a REAL tilt through the runtime's dropout-to-zero corruption samples, which the instant fall turned into a permanently ~0.1 (dead) stick

    // [Gestures]
    bool  gesturesEnabled = true;
    float lightAttackSpeed = 2.2f;     // m/s (right hand)
    float heavyAttackSpeed = 4.0f;     // m/s
    float leftSwingSpeed   = 2.4f;     // m/s (left hand)
    std::string lightAttackButton = "RB";
    std::string heavyAttackButton = "RT";
    std::string leftSwingButton   = "LB";
    float gestureCooldown = 0.35f;     // s between triggered swings per hand
    float gesturePressDuration = 0.09f;// s a synthesized button is held
    bool  gestureRequiresGrip = false; // only detect swings while grip held
    bool  gestureHaptics = true;

    // [Movement]
    std::string moveForward = "head";     // head (where you look - stick forward walks toward your gaze) | camera (forward = the stick yaw, whatever the head does) | hand | body
    std::string rightStick = "camera";    // camera | body: what the right stick turns when ERVR owns the yaw
    bool  handRelativeMovement = false;   // legacy alias for MoveForward=hand
    bool  cameraTurnErvr = true;          // the turn stick drives ERVR's own view yaw (needs CameraYawSource=ervr)
    float turnSpeedDegPerSec = 120.f;     // smooth-turn rate when ERVR owns the yaw
    float snapTurnDegrees = 30.f;         // step size when SnapTurn is on and ERVR owns the yaw
    bool  moveRelativeToCamera = false;   // rotate the move stick by (view yaw - body yaw); ER's stick is camera-relative already, so leave off
    bool  snapTurn = false;               // right stick X -> discrete turns (pulses if ERVR does not own the yaw)
    float snapTurnPulseSeconds = 0.08f;
    float snapTurnRepeatSeconds = 0.30f;

    // [Haptics]
    bool  rumbleToControllers = true;     // forward XInputSetState rumble to Touch controllers
    float rumbleScale = 1.0f;

    // [bHaptics]  body haptics on a TactSuit / TactSleeves / TactVisor, through the bHaptics Player (body_haptics_er.h)
    int   bhapticsLevel = 100;            // percent; 0 = off (bhaptics_library.dll is then never loaded)
    bool  bhapticsCombat = true;          // blows landed, blocks, deflects, the two-hand grab, casts
    bool  bhapticsDamage = true;          // blows taken (on the side they came from), heals, death, the heart at low HP
    bool  bhapticsMovement = true;        // falls and landings
    bool  bhapticsRumble = true;          // the game's own rumble on the vest

    // [FirstPerson]  (ERVR takes over the game camera and the arm bones)
    bool  fpEnabled = true;
    float fpEyeHeight = 1.60f;            // meters above the character root (CameraAnchor=root, and the height for CameraAnchor=head)
    std::string fpCameraAnchor = "head";  // head = camera rides on the head bone (body connected by construction) | root = character root + EyeHeight
    float fpCameraAnchorSmoothing = 0.06f;// s: low-pass on the head-bone anchor (kills per-frame animation jitter, keeps the lean)
    float fpCameraBob = 1.f;              // 0..1: how much of the head bone's vertical motion (bob, crouch, run lean) the camera follows (1 = stay on the head)
    // [Locomotion]
    std::string locomotion = "game";      // game = the game moves the character from the stick | direct = ERVR integrates the position/facing itself (no collision from us)
    float locoRunSpeed = 4.5f, locoSprintSpeed = 6.5f;   // m/s for Locomotion=direct
    bool  locoFeedStick = true;           // direct: still hand the stick to the game so run/strafe animations play (its own motion is discarded)
    std::string fpCameraMode = "pre";     // pre | post | block | passthrough (how we hand the camera to the game)
    std::string fpCameraRotation = "hmd"; // yaw | game | hmd  -> what rotation we write into the game camera.
                                          // hmd is required now that ERVR renders: nothing else applies head rotation.
    std::string fpCameraYawSource = "ervr"; // ervr (our own yaw, stick-integrated; the game's chr-cam update is replaced so nothing else can move it) | game (read the game camera's yaw back)
    bool  fpOwnChrCam = true;             // replace the game's chr-cam (follow cam) update while first person is on, instead of writing on top of it
    bool  fpRecenter = true;              // slow play-space re-centring: without it, standing off the runtime origin makes the eye orbit the body on every stick turn
    float fpRecenterSeconds = 2.0f;       // time constant of the slow re-centring (0 = hard lock, no positional head tracking)
    float fpRecenterSnapRadius = 0.35f;   // m: residual beyond this snaps the play-space origin to your head instead of easing (a runtime origin metres away otherwise parks the body away from the camera and slides it back for seconds)
    float fpRecenterMaxRadius = 0.75f;    // m: never let the head sit further than this from the character
    bool  fpRecenterHeight = true;        // also absorb vertical drift (keeps EyeHeight whatever your real height is)
    bool  fpFaceCamera = true;           // keep the body facing the camera (strafe mode) so arms can always reach
    std::string fpFaceCameraMode = "strafe"; // strafe (Ctrl unlock flag only: no game lock-on camera) | lock (old: force Chr_IsLockedOn + fake target) | off; ini "auto" = strafe with PoseMode=vr, else lock
    float fpLockTargetDistance = 100.f;   // m: how far ahead the fake lock-on point sits (short = the game's lock-on camera drags your yaw)
    std::string fpPoseMode = "vr";        // vr = full-rig template override (no animation ever poses the body; root motion still moves it) | game = animation + IK on top
    std::string fpArmRootSource = "model";  // model = position from the ChrCtrl model matrix (the transform the renderer skins with, read fresh in the model-pos hook) + physics rotation | modelmatrix | physics
    bool  fpPinBodyFacing = true;         // strafe mode: pin walking facing to BodyYaw; dash/roll and non-cancelable actions retain native physics facing
    bool  fpCameraAfterModelPos = true;   // write the camera again once the rendered model matrix exists, so camera and body sit on the same transform (kills the body-slides-to-camera lag)
    bool  fpCameraFinalWrite = true;      // replay our camera matrix after the game's camera finalizer (which runs past every other write point and would otherwise render its own interpolated follow position)
    bool  fpLatchVRPerFrame = true;       // take ONE head/hand sample per render cycle and use it at every write point, so the body is posed and the camera placed from the identical head position
    bool  fpPoseAfterHavok = false;       // DISABLED: detouring the Havok pose fn (0x146b5e0) crashes - its ABI is not the guessed 4-arg one (crash dumps at +0x4e). Needs a register-preserving midhook instead. Weapon sway stays until then.
    bool  fpArmSolveEveryCall = true;     // solve the arms on every Havok update, not once per rendered frame
    bool  fpHideHead = true;
    bool  fpHideSheathed = false;
    // Near-camera dither fade (the game's own "objects close to the camera go see-through"): 0 = off while
    // first person runs (erfps2 behaviour - nothing fades as you lean toward it), 1 = the game's third-person
    // band untouched, in between = the fade band's distances scaled down (0.3 = fades start at 30% of the distance).
    float fpNearDither = 0.f;
    bool  fpWriteAim = true;              // write the HMD look direction into the player's aim matrix (spells/bows)
    bool  handTracking = true;            // drive arm bones from the controllers
    std::string handOrigin = "head";      // head | tracking | floor
    float handBackOffset = 0.06f;         // meters from controller grip pose back to the wrist bone
    // Degrees, controller -> hand bone calibration. X=+45 dialled in-headset on a Quest 3
    // (2026-08-21, Numpad9 x3): the raw grip pose leaves weapons pitched ~90 deg toward the
    // player. Numpad 6/9 step X live by -/+15 for re-calibration.
    float handRotOffX = 45.f, handRotOffY = 0.f, handRotOffZ = 0.f;
    float elbowPoleDown = 1.0f, elbowPoleOut = 0.6f, elbowPoleBack = 0.3f;
    float armStretch = 1.5f;              // max stretch factor of forearm+hand when the target is out of reach (1 = none)
    float handScale = 1.0f;               // scale controller offsets from the head (compensate arm length differences)
    std::string bodyYawSource = "head";   // head (shoulders follow your real head - physically turning turns the body) | camera | stick
    float bodyFacingOffsetDeg = 0.f;      // manual correction if the rendered body faces the wrong way (180 = flipped)
    float bodyYawFollowSeconds = 0.25f;   // time constant of the body chasing the head yaw (0 = instant)
    float bodyYawDeadzoneDeg = 0.f;       // look this far off before the body starts to follow
    float rideFreeLookDeg = 45.f;         // on Torrent: look this far either side without steering or turning the horse (-1 = off: the stick follows the head)
    bool  rideTurnsWithHead = true;       // on Torrent with BodyYaw=head: turning past the free look turns the horse and rider (BodyYawFollowSeconds)
    float rideCameraSmoothing = 0.15f;    // s: Torrent's gallop bounce smoothed out of the view, the rider carried with it (0 = rigid on the rider)
    float rideCameraMaxDeviation = 0.10f; // m: the smoothed view (and the rider with it) never sits further than this from the saddle's own motion
    bool  torsoFaceCamera = true;         // twist the Spine bone so the chest faces the body yaw
    bool  bodyFollowsHead = true;         // shift the whole rig so its head sits under your eye: physically stepping/leaning moves the character with you (PoseMode=vr makes this idempotent and feedback-free)
    bool  bodyRotateToHead = true;        // also yaw the whole rendered skeleton to BodyYaw (does not wait for the game to turn the character)
    std::string bodyAnchor = "head";      // head = put the head bone at the eye | root = shift by the headset offset only
    float bodyFollowMaxHorizontal = 0.6f; // m clamp for the skeleton shift (character root does not move)
    float bodyFollowMaxDown = 0.6f, bodyFollowMaxUp = 0.25f;
    float bodyBackOffset = 0.08f;         // m: park the rig this far behind the camera (along its own facing) so the chest/pauldrons stay out of the bottom of the view
    std::string boneTorso = "Spine", boneHead = "Head";
    std::string boneLUpperArm = "L_UpperArm", boneLForearm = "L_Forearm", boneLHand = "L_Hand";
    std::string boneRUpperArm = "R_UpperArm", boneRForearm = "R_Forearm", boneRHand = "R_Hand";
    bool  dumpSkeleton = true;            // log all bone names once when the player skeleton is first seen
    bool  fpPoseScan = false;             // diagnostic: page-protect the pose buffer for ~1 s and log every reader/writer RVA (one shot per false->true flip)
    bool  fpDebug = false;                // per-second debug lines with frame data
    // Flat-harness probe: write Cam_Fov to this many degrees every camera step, no VR
    // session needed. Exists for the fov-semantics A/B (does a WRITTEN fov apply across the
    // boot height or the current height?). 0 = off. Hot-reloads.
    float fpForceFovDeg = 0.f;
    // Flat-harness probe, part 2: pitch the written camera by this many degrees. A known
    // rotation turns a landmark's on-screen displacement into a direct px/deg measurement.
    // 0 = off. Hot-reloads.
    float fpForcePitchDeg = 0.f;

    // [Combat]  physical melee: swings invoke the game's AttackBehavior event instead of pressing R1/R2
    bool  combatEnabled = true;
    std::string combatMode = "auto";      // buttons | physical | auto (physical once the light attack judge id is known)
    float swingOnSpeed = 3.0f;            // m/s weapon-tip speed that starts a swing
    float swingOffSpeed = 1.5f;           // m/s tip speed below which the swing ends
    float heavySwingSpeed = 6.0f;         // m/s peak tip speed (during the arm window) that makes it a heavy attack
    float minHandSpeed = 0.8f;            // m/s controller (not tip) speed required to start (rejects wrist flicks)
    float weaponReachRight = 0.8f, weaponReachLeft = 0.5f;   // m from controller to weapon tip (lever arm for tip speed) when the weapon category is unknown
    std::string weaponGeometry;           // "wepType:length:comFraction,..." overrides of the per-category geometry table
    float kineticRefReach = 0.8f;         // lever the KineticRefSpeed is defined for; quality 1 scales with weapon length / this (0 = no scaling)
    float swingArmWindow = 0.06f;         // s to observe the peak before choosing light/heavy
    float swingMaxSeconds = 0.6f;         // s hard limit for a gated attack; continuous event-window lookahead
    float swingCooldown = 0.25f;          // s after a swing before another can start
    bool  combatLeftHand = true;          // left-hand swings attack with the left weapon (source = left)
    bool  combatRequireStamina = true;
    int   combatMinStamina = 1;
    // Judge ids observed in ER 1.16: R1 chain = 0/10/20, R2 = 100 (105 charged variant), left hand = 0 with source 2.
    int   lightJudgeId = 0, heavyJudgeId = 100, leftJudgeId = 0;
    bool  combatLearn = true;             // refine the ids from the game's own events after our button presses
    bool  combatSaveLearned = false;      // opt-in: write learned ids back into ERVR.ini
    bool  combatBlockAttackButtons = true;// while physical: never send RB/RT (trigger/grip would play attack anims)
    int   combatAttackIndexRotate = 0;    // diagnostic: rotate AttackIndex 0..N-1 per swing (0 = off)
    int   combatExtraStamina[3] = {0, 0, 0};   // stamina taken by ERVR per light/heavy/left swing (0 = only the game's own cost)
    bool  combatSuppressAnimAttacks = false;   // swallow AttackBehavior events from animations while physical
    std::string rideAttacks = "physical"; // on Torrent: physical (the blade strikes, as on foot) | buttons (a swing presses the game's own mounted attack)
    bool  combatDebug = false;
    // Kinetic: strike quality (tip speed vs a reference speed, squared, x arc) scales the attack row's
    // motion values live in the loaded params; stamina is fatigue.
    bool  kineticEnabled = true;
    float kineticRefSpeed = 8.0f;         // m/s VIRTUAL tip speed at contact = quality 1 (vanilla damage)
    float kineticArcRef = 0.5f;           // m of tip travel for a full-arc strike at weight 0 (x (1 + weight/10))
    float kineticDamageScale = 1.0f;      // global multiplier on every scaled strike (difficulty knob)
    // Continuous model (2026-08-25, user's call): every frame of contact is a hit, each sized by the tip's
    // speed right now - 0 at rest, KineticContinuousMult of a vanilla hit at KineticRefSpeed.
    bool  kineticContinuous = true;
    // Continuous contacts deliver each swing as ONE hit scaled by the swing's quality, then the strike is
    // spent. ER takes flat defense off every hit, so per-frame slices of 6-10% of a hit dealt 3-5 HP each
    // on an enemy a full hit takes hundreds from (2026-10-31). true = the per-frame slice model.
    bool  kineticSlices = false;
    float contactOnSpeed = 0.8f;         // m/s: continuous contacts open immediately, including wrist-driven arcs
    float contactOffSpeed = 0.25f;       // m/s: contact hysteresis; a resting blade does not attack
    // A strike whose swing budget is spent ends at the tip's next speed valley (below this fraction of the
    // swing's peak) or this long after it was spent, so the next swing opens a new strike. Before, it only
    // ended at rest, which a long weapon's tip rarely reaches mid-fight (2026-10-31: swings stopped registering).
    float contactValleyFrac = 0.4f;
    float contactSpentSeconds = 1.0f;
    float kineticContinuousMult = 0.2f;
    std::string kineticStaminaMode = "off";   // off (bar out of the loop, game's on-hit charge refunded) | game | full
    float kineticHitPause = 0.25f;        // gated model only: s the attack pauses after a landed hit (0 = the strike ends on its first hit)
    int   kineticHitStamina = 3;          // one-frame stamina drop during a strike that counts as a landed hit
    float kineticMinMult = 0.25f;         // damage multiplier at quality 0 (chip)
    float kineticMaxMult = 1.2f;          // multiplier at quality 1.5 (cap)
    float kineticHeavyQuality = 0.85f;    // quality (with >=80% of a full arc) that selects the heavy attack row
    float kineticAccel = 60.f;            // m/s^2 for the virtual-speed fallback when rendered motion is unavailable
    float kineticAccelWeight = 2.f;       // weight at which that acceleration halves (UGS ~23 -> 1/12; was 4, doubled 2026-08-24 on request)
    // The main grip tracks position directly. Weight and mass distribution resist
    // bending about the grip, with a monotone response and no spring overshoot.
    bool  kineticInertia = true;
    float inertiaAngAccel = 300.f;        // rad/s^2 at weight 0
    float inertiaSpringHz = 8.f;          // stiffness of the chase near the target (higher = tighter tracking)
    float inertiaTwoHandTorque = 2.f;     // two-handed: angular acceleration cap x this (a hand on the pommel)
    bool  twoHandAim = true;              // support position steers bending; main wrist owns shaft twist
    float inertiaBrake = 3.f;             // braking (and stop-distance) acceleration as a multiple of the cap: a wrist stops a weapon more easily than it starts one
    float inertiaComRef = 0.5f;           // m centre-of-mass distance at which weight counts fully for rotation (angular cap /= 1 + w/W * (com/ref)^2)
    float inertiaMaxLagAngle = 0.6f;      // rad max bending lag at the grip (hard safety cap 45 degrees)
    float kineticArmPerWeight = 0.012f;   // extra startup only for gated combat without rendered weapon motion
    float kineticStaminaScale = 1.0f;     // x the game's own BehaviorParam stamina cost per strike (x 0.5..1 by quality)
    float kineticStaminaPerStrike = 8.f;  // fallback flat drain when the behavior row is unknown, x(1 + weight/12)
    float kineticFatigueStart = 0.35f;    // stamina fraction below which strikes weaken
    float kineticFatigueFloor = 0.5f;     // multiplier at 0 stamina
    bool  kineticPoiseScales = true;      // scale poise damage too
    bool  kineticStaminaDamageScales = true;   // scale stamina damage dealt to guarding enemies too
    // 2026-08-31 consistency pass: enemy-HP hit detection, per-swing damage budget, one flinch
    // per swing, aligned behavior variation, fattened hit capsules.
    bool  combatEnemyWatch = true;        // read the game's by-distance ChrIns list: HP drops = landed hits (ground truth)
    float combatEnemyWatchRadius = 8.f;   // m around the player worth watching
    bool  combatEnemyWatchOwnHits = true; // count only drops the player caused (ChrIns last_hit_by) - co-op partners' hits are not ours
    bool  kineticAlignVariation = true;   // during a strike point the weapon's behaviorVariationId at the variation whose rows we edit
    float kineticSwingBudget = 1.0f;      // max damage per swing, in vanilla-hit units x QualityToMult (0 = unlimited)
    bool  kineticFlinchFirstHitOnly = true;   // after a strike's first landed hit: no hit reaction / hitstop / poise (no stunlock)
    float kineticHitRadiusScale = 1.3f;   // x the attack rows' hit capsule radii during a strike (a touch should touch)
    // Shields: left-hand shield pose vs the head -> block quality -> guard held + shield params scaled.
    bool  blockEnabled = true;
    std::string guardButton = "LB";
    std::string shieldWepTypes = "65,67,69";   // EquipParamWeapon wepType values that count as shields
    float blockReleaseSeconds = 0.35f;    // guard stays held this long after the shield leaves the "up" zone
    float blockMinCut = 0.4f;             // guard cut rate multiplier at block quality 0
    float blockMinStability = 0.5f;       // stability (stamina_guard_def) multiplier at quality 0
    float shieldNormalPitchDeg = -90.f;   // shield face normal = controller forward pitched by this (-90 = the controller's -Y, measured 2026-08-24)
    float deflectSpeed = 1.2f;            // m/s shield speed INTO the attack at impact = deflect (stamina refunded)
    int   blockImpactStamina = 4;         // one-frame stamina drop while guarding that counts as a blocked hit
    // Two-handing by grabbing: the free hand closes on the weapon hand's weapon shaft -> the game's own
    // two-hand chord (Y + RB / Y + LB) is sent and ChrAsm.arm_style read back as the authority.
    std::string twoHandMode = "grab";     // grab | off
    std::string twoHandModifierButton = "Y";   // the game's two-hand modifier (XInput name): Y + RB / Y + LB by default
    // The game counts the chord in ITS OWN frames: at 20 fps (GPU-bound stereo) a 250 ms lead is only
    // 5 frames - the same as the fast chord it ignored at 60 fps. Sized to survive ~15 fps.
    float twoHandChordLead = 0.35f;       // s the modifier is held before the key goes down (the game wants to see Y first)
    float twoHandChordPress = 0.25f;      // s the key is held with the modifier
    float twoHandChordTail = 0.15f;       // s the modifier stays down after the key is released
    std::string twoHandAxis = "handle";   // the hilt runs along the controller's HANDLE (its -Y, toward the pommel; 2026-08-25: the pinned hand sat behind the hand with "forward") | forward (the aim axis, -Z)
    float twoHandGrabRadius = 0.18f;      // m from the hilt axis within which the free hand takes hold (two Touch controllers side by side are ~0.12 m apart)
    float twoHandGrabMin = -0.15f;        // m along the hilt axis from the weapon hand's controller, positive = toward the pommel (where the second hand sits)
    float twoHandGrabMax = 0.45f;         // ... negative = up toward the blade
    bool  twoHandPinHand = true;          // while two-handing the free hand's arm rides the weapon shaft (follows the inertial weapon)
    bool  twoHandPinOrient = true;        // ...and keeps its grab-time wrist angle relative to the weapon (else follows its own controller's free orientation)
    float twoHandPinSideOffset = 0.03f;   // m the pinned hand sits off the hilt axis (perpendicular, so the wrist clears the shaft); negate to flip sides
    int   twoHandLightJudgeId = 200, twoHandHeavyJudgeId = -1;   // two-handed attack rows (R1 2H = 200 learned 2026-08-25; -1 = learn in auto mode, else the one-handed rows)
    // Skill (Ash of War) while two-handed: TwoHandSkillButton fires it. "effect" replays the TAE events the
    // skill's animation carries (AddSpEffect_WeaponArts 331, AddSpEffect 67/66, BulletBehavior 2, FFX) without
    // the animation; the first use per weapon/ash plays it the game's way once and records those events.
    std::string twoHandSkill = "effect";  // effect | game | off
    std::string twoHandSkillButton = "RTrigger";   // VR control: RTrigger RGrip LTrigger LGrip A B X Y RThumb LThumb none
    std::string skillButton = "LT";       // the game's skill button (what "game" presses and "effect" records from)
    float twoHandSkillRelearnHold = 1.0f; // hold the trigger this long: the game plays the skill and it is re-recorded
    std::string skillReplayEvents = "331,67,66,2,96,110,112,120";   // TAE event ids replayed by "effect"
    float skillRecordSeconds = 3.0f;      // how long after the game-driven press events are recorded
    float skillSwingWindow = 2.5f;        // s after an effect-skill during which the next swing uses the skill's attack row
    bool  skillChargeFp = true;           // "effect" charges the skill's FP (SwordArtsParam) and refuses without it
    // Cast aim: ranged spells, ranged skills and bows fly where the CASTING HAND points, not where the head looks.
    // The game aims an unlocked bullet from the player's aim view matrix (ChrIns+0x120, which [FirstPerson]
    // WriteAim fills from the head) and the lock-on target point; the cast animation's SetBulletAimAngle event
    // (TAE 238) clamps the vertical/lateral aim with limits and deadzones. With a casting hand ERVR fills the
    // matrix and the target from the controller's ray and relaxes the event (the erfps2 first-person mechanism).
    bool  castAim = true;
    std::string castAimHand = "auto";     // auto (the hand whose attack/skill button was pressed last, else the one holding a ranged weapon, else the right) | right | left
    float castAimSeconds = 0.5f;          // s the aim stays live after the animation's last aim event (238 SetBulletAimAngle / 2 BulletBehavior). Only those events arm it: a press never does (guard / grip-mapped presses forced the lock and the game turned the body after the hand, 2026-09-04)
    std::string castAimAxis = "aim";      // aim (where the controller points) | handle (up the handle: the staff's own direction)
    float castAimPitchOffsetDeg = 0.f;    // tilt added to the ray (positive = up)
    bool  castAimTurnBody = false;        // while a cast is live the BodyYaw target follows the ray. Off: the visible body stays on your head (the game turns the physics character toward the lock target underneath, invisibly under PoseMode=vr); on, the rig and the camera anchor swung with the arm and snapped back (2026-09-04)
    float castAimLimitDeg = 80.f;         // raise the animation's SetBulletAimAngle limits to at least this (0 = leave them); the deadzones are always zeroed
    bool  castAimForceLock = true;        // FaceCameraMode=strafe: during the press window also force the lock-on state onto the ray's far point (the lock target is what aims an unlocked spell - erfps2's way)
    std::string castAimWeaponTypes = "50,51,53,55,56,57,61,89,91";   // wepTypes that make a hand the passive aim hand (bows, staffs, seals, perfumes, throwing blades); their attack buttons pass the physical block
    std::wstring iniPath;                 // where Load() read from (for SaveLearned)

    // [WeaponWheel]  hold a grip with that hand still: a wheel of the hand's three weapon slots opens at the
    // hand, with the game's own item icons; move over one and let go to switch to it (weapon_wheel.h).
    bool  wheelEnabled = true;
    std::string wheelHands = "both";      // both | right | left | off
    float wheelHoldSeconds = 0.35f;       // grip held with the hand still this long opens the wheel; a shorter squeeze is a tap of the grip's [Buttons] mapping
    float wheelStillSpeed = 0.8f;         // m/s: a hand moving faster restarts the hold (a swing with the grip squeezed, reaching for the other hilt)
    float wheelSize = 0.30f;              // m across; the weapons sit 0.3 x this from the centre
    float wheelDeadZone = 0.035f;         // m from the centre: letting go inside it switches nothing
    float wheelOpacity = 1.0f;            // 0..1
    bool  wheelHaptics = true;
    float wheelSwitchTimeout = 1.0f;      // s the game gets to answer each D-pad press before it counts as ignored
    // jump  = a weapon more than one press away: the active slot is first moved to the slot before it, so the
    //         game's own switch runs once (no weapon in between shows up)
    // cycle = press the game's switch once per slot in between, as a gamepad would
    std::string wheelSwitch = "jump";

    // [Belt]  the pouch's four shortcut items hang at the waist: close a grip on one to take it, press UseButton
    // while holding it to use it (the game's own Event Action + D-pad pouch shortcut), open the hand to put it back
    // (belt.h).
    bool  beltEnabled = true;
    std::string beltHands = "both";       // both | right | left | off: the hands that take items
    std::string beltUseButton = "X";      // A | B | X | Y: uses the item in hand; with nothing taken it is the button it always was
    float beltDrop = 0.60f;               // m below the eyes (head upright) the items hang
    float beltRadius = 0.19f;             // m out from the spine
    std::string beltAngles = "-30,30,-80,80";   // degrees around the waist for pouch slots 1-4 (0 = straight ahead, positive = right)
    float beltSize = 0.09f;               // m across each item
    float beltGrabRadius = 0.11f;         // m: a grip closing this close to an item takes it
    float beltFollowDeg = 45.f;           // the head may look this far to either side before the belt turns with it
    float beltOpacity = 0.85f;            // 0..1, items no hand is at (a hand at one shows it fully)
    bool  beltHaptics = true;
    std::string beltPouchButton = "Y";    // the game's Event Action button (XInput name), held for the pouch shortcut
    std::string beltDirections = "up,right,left,down";   // the game's D-pad direction for pouch slots 1-4
    float beltChordLead = 0.40f;          // s Event Action is held before the direction (shorter reads as a plain D-pad switch)
    float beltChordPress = 0.20f;         // s the direction is held with it
    float beltChordTail = 0.15f;          // s Event Action stays down after the direction is let go

    // [Cheats]  offline convenience for testing (writes the save's PlayerGameData)
    int   cheatSetAllStats = 0;           // >0: set every stat to exactly this value (level adjusted); 0 = off
    int   cheatSetLevel = 0;              // >0: reach exactly this level (points off the highest stats / onto the lowest), once - the key clears itself
    bool  cheatInfiniteStamina = false;   // refill stamina to max every combat frame (attacks, blocks, sprint never drain). Hot-reloads.

    // [MenuNav]  left stick acts as a held D-pad while a menu is open (game auto-repeats), gestures muted
    bool  menuNavEnabled = true;
    float menuNavThreshold = 0.55f;       // stick deflection that counts as a D-pad press (hysteresis: releases at 0.6x)
    bool  menuNavFollowStart = true;      // no memory flag: enter on our START press, leave on the next START/BACK
    float menuNavToggleHold = 0.5f;       // hold left stick click (no tilt) this long -> toggle menu mode; 0 = off
    bool  menuNavHaptics = true;
    std::string menuSource = "auto";     // auto | menuman | hud | chain | start  (see game_memory.cpp GameMenuOpen)
    bool  combatMenuGate = true;         // any open menu stands physical combat down (equip menu refuses weapon changes while the game sees combat state)
    std::string menuAob ="48 8B 0D ?? ?? ?? ?? 48 8B 53 08 48 8B 92 D8 00 00 00 48 83 C4 20 5B";  // CSMenuMan* global
    int   menuAobOffset = 3, menuAobInstrLen = 7;
    std::string menuChain;                // pointer chain from the CSMenuMan* global to a byte that is nonzero while a menu is open; empty = disabled
    bool  menuScan = false;               // debug: log which bytes of the CSMenuMan object change (find MenuChain)
    int   menuScanSize = 0x400;

    // [Experimental] hand-aim: rotates the game camera toward where the right hand points
    bool  handAimEnabled = false;
    std::string cameraAob;                // AOB for a rip-relative LEA/MOV to the camera manager pointer
    int   cameraAobOffset = 3;            // byte offset of the rel32 inside the pattern
    int   cameraAobInstrLen = 7;          // total instruction length for rip-relative resolve
    std::string cameraChain;              // e.g. "0x20,0x60" pointer chain from resolved base to the camera struct
    int   cameraYawOffset = 0;            // offset of float yaw (radians) inside the camera struct
    int   cameraPitchOffset = 4;
    float handAimStrength = 1.0f;
    // M2 spike: dump the frame task-group table once at startup. Read-only, but it scans
    // process memory, so it costs a second or two and is off by default.
    // [VR] full-stereo pass 2: task-group indices kept active (ranges, e.g. "144-158,164").
    // 106 = CameraStep (our hook writes the second eye there). 118,123-125 = the MoveMap
    // post-physics family hosting the once-per-frame camera-propagation chain (CamWatch
    // census: exe+0x680bxx + CamFinalize run in group 123) - REQUIRED, or pass 2 renders
    // pass 1's captured camera regardless of what the camera objects hold. 142 =
    // ScaleformStep (UI list rebuild), 144-158 = Draw_Pre..Draw_Post, 164 = Flip.
    std::string vrPass2Keep = "106,118,123-125,144-158,164";
    // Full-frame second pass: instead of a draw-only pass with cherry-picked groups, run
    // pass 2 as a COMPLETE engine frame with the game's per-frame delta time swapped to
    // Pass2Dt (~0) for the pass - everything runs (environment lighting, UI, all producers;
    // no crashes from skipped setup) but the simulation advances by ~nothing, so both eyes
    // still share one sim instant. The draw-only sweeps could not find the environment-
    // lighting producer (2026-08-20: every candidate group failed and broad keeps crash),
    // which is exactly the case this mode exists for. Excluded even here: PadStep (input
    // edge replay), the resource lifecycle, AI GC. Numpad 5 toggles it live.
    bool  vrPass2FullFrame = false;
    // <= 0 = off (production). > 0 = experimental float-dt freeze (imm + hw-bp
    // timing-struct swap) - measured NOT to reach the sim (the float dt only becomes
    // limiter ticks); kept for future RE only. The real lever is Pass2Steps below.
    float vrPass2Dt = 0.f;
    // >= 0: master switch for the pass-2 time freeze - swaps the engine's frame-dt global
    // (the single float every stack-built FD4TaskData copies its dt from) to ~0 around
    // pass 2. 0 = pass 2 advances NOTHING = true same-sim-time stereo. -1 = off.
    int vrPass2Steps = 0;
    // Fade-out watcher for world transitions (fast travel/death): samples back-buffer luma
    // ~4x/s and suspends pass 2 while the screen is black. DEFAULT OFF - the sample is a
    // GPU readback submitted inside ReShade's present event, and EVERY variant of that
    // crashes D3D12Core.dll+0x11f85 (measured 2026-08-20 in three designs, then again
    // 2026-08-21 as a 100%-reproducible VR startup crash ~3 s after stereo starts: device
    // wedge -> XR_ERROR_RUNTIME_FAILURE flood -> game AV -> GPU video-engine reset). Do not
    // enable until the sampler is redesigned off the present path; fast travel is covered
    // by Pass2EatAsserts + the teleport guard + the player-inactive grace meanwhile.
    bool  vrFadeGuard = false;
    // Suppress DL_PANIC asserts that fire DURING pass 2 (skip the deliberate null-write;
    // the panic fn then returns cleanly). Pass-2 asserts are spurious - the second pass
    // replays the frame over transition state pass 1 never sees (fast travel's fade-out).
    // Pass-1 panics still crash with the stack logged.
    bool vrPass2EatAsserts = true;
    // Groups the FULL-FRAME pass keeps. Everything except: PadStep 9 (input edge replay),
    // the resource lifecycle (8 ResStep, 26/143 FLVER deletion window, 161-163 FileStep,
    // 165 DelayDeleteStep, 167 heap stats - deletion must tick once per REAL frame), and
    // BOTH halves of the AI Lua GC bracket 85/166. RULE learned the hard way: Begin/End
    // task pairs must be kept or gated TOGETHER - keeping AiBeginCollectGabage(85) while
    // gating AiEndCollectGabage(166) left "GC running" set and the next AI Lua call hit
    // the engine's deliberate DL_PANIC ("Running GC!", CSAiLua.cpp:642) - the crash
    // signature exe+0x1eb9999 of 2026-08-20.
    // The world-origin shift pair 11/160 is KEPT, and the boundary case of the rule: it
    // brackets the INTER-frame window (Begin=160 late in frame N, End=11 early in N+1),
    // so gating both out of pass 2 leaves pass 2 running an entire engine frame inside a
    // half-applied origin rebase whenever pass 1's group 160 triggered one - measured
    // in-headset 2026-08-20 as geometry shifting/teleporting/OOB starting ~1 min in (the
    // first time the player walks far enough to trigger a shift). Keeping both replicates
    // the vanilla order inside pass 2: its group 11 closes pass 1's shift before the pass
    // simulates, and its group 160's begin-check is a no-op right after a rebase.
    // Cloth (100-105 pre + 110-112 post, gated TOGETHER) is excluded since 2026-08-21: the
    // Havok cloth solver run twice per frame with pass 2's ~0 dt goes visibly unstable
    // ("cloth going haywire"); gating it renders pass 1's cloth state in pass 2 - the same
    // sim instant, exactly what same-sim-time stereo wants anyway.
    // The Havok RIGID-BODY solver is excluded for the same reason (2026-08-21, "dead enemy
    // ragdolls and broken fences go haywire"): 97 HavokWorldUpdate_Pre + 99
    // HavokWorldUpdate_Post (gated TOGETHER - they bracket the world step), 82
    // ChrIns_RagdollSafe, and the destructible/dynamic map-prop feed 89-91
    // LocationUpdate_PrePhysics* + 92-94 LocationUpdate_PostCloth* (each family whole).
    // Free bodies - ragdolls and broken geometry - are the one thing in the world with no
    // animation to re-drive them, so they are pure solver output: stepping the solver a
    // second time at dt~0 divides by that dt in constraint/penetration recovery and the
    // bodies get flung. Gating renders pass 1's rigid-body state in pass 2, the same sim
    // instant, which is the point. 98 RenderingSystemUpdate sits BETWEEN 97 and 99 and is
    // the renderer, not physics - it stays kept.
    // NOT gated here: the character pre/post-physics feed (77-79, 81, 113-114). It is the
    // exact analogue of the cloth gate and is offered as a Numpad 2 variant, but the arm
    // solve rides CS::NoUpdateInterface vft[10] (see game_er.cpp HkHandler) and pass 2 must
    // re-apply the VR arm pose, so the narrow gate is the default until the wide one is
    // confirmed in-headset to still solve arms in pass 2.
    // Hot-reloads; Numpad 2 cycles stabilisation variants.
    // Full keep set. Gating warp-related groups here was tried twice on 2026-08-22 for the
    // "fast travel or dying crashes the game" bug and BOTH attempts regressed the picture:
    // 10 GameFlowStep and 20 TaskLineIdx_InGame_MoveMapStep each turn out to do per-frame
    // work pass 2's render needs, and without either one pass 2's image (the RIGHT eye under
    // PresentLag=1) comes out dark with per-object flicker. Group 20 was the sole config
    // delta that produced it - measured against the pre-fix ini, with pass 2 still running
    // every frame (flat frames 0), so it was the gating and nothing else.
    // The crash is a DOUBLE-STEP of the transfer state machine, and steps=0 does not stop it
    // (the dispatch field is swapped and the machine still advances once per invocation), so
    // the fix is not to gate the group but to skip pass 2 ENTIRELY while a warp is in flight
    // - which is what the guards at the top of DoSecondPass are for.
    // The CHARACTER pipeline is gated (2026-08-22): 54-81 (WorldChrMan_Respawn through the
    // ChrIns behavior/AI chain), 113-114 + 117 (ChrIns/WorldChrMan post-physics) and
    // 127-129 (backread requests) run only in pass 1. Root cause of every warp crash that
    // week: pass 2 double-stepping character updates corrupted ChrIns state that pass 1's
    // own tasks then read - crash-handler register dumps showed CS::WorldChrManImp +
    // FD4Time + task id 0x90000052 at two different faulting sites (exe+0xc7f88b null
    // virtual call, exe+0x3f9b2b poisoned pointer). Gating the pipeline removed both in
    // the headless repro. Visually this is CORRECT for stereo, not a compromise: pass 2
    // renders the same sim instant, so characters must not advance between the eyes.
    // Also gated (same day, teardown-window crashes at exe+0x513baa WorldBlockChr and
    // exe+0x32b04a EnemyIns, both pass 2): 115-116 CSDistViewManager/silhouette-begin,
    // 118+123-125 GameFlowInGame_MoveMap_PostPhysics (map-transfer per-block work - must
    // not run twice, and pass 2 walking a half-deleted block grid was the loading-screen
    // crash), 120-122 DmgMan (damage shape-casts - re-running them doubled hit processing
    // besides crashing on deleted enemies), 130-132 HavokAi_World/formation. Validated:
    // the kill repro with only the dead-guard bypassed crashed 2/2 before, 0/2 after,
    // pass 2 pumping 72 Hz through the whole teardown.
    // MENU stepping gated too (2026-08-24): 29-31 (the InGame/Title/CommonMenu task lines),
    // 126 MenuMan and 134-136 GameFlowInGame_{InGame,Title,Common}Menu run only in pass 1.
    // PadStep (9) is gated, so the "START just pressed" edge computed in pass 1 is still set
    // when pass 2 runs the same frame - and the pause menu is a START TOGGLE: pass 1 opened
    // it, pass 2 closed it again in the same stereo pair, so it never reached a present
    // (log: pad sent 0x0010 for ~90 ms, the game polled it 5-6 times, nothing ever drew).
    // Same principle as the character pipeline: pass 2 renders the same sim instant, so
    // menus must not advance between the eyes. ScaleformStep (142) stays kept - it is the
    // UI DRAW, not the menu logic, and the HUD layer is captured from it.
    // NETWORK stepping gated too (2026-10-31, Seamless Co-op): 15 NetworkFlowStep, 28/133
    // TestNet, 32-36 FrpgNet_*, 39 FrpgNetMan, 40 NetworkUserManager, 41 SessionManager,
    // 42 BlockList, 44 RmiMan, 47 REMOTEMAN, 64 WorldChrMan_ChrNetwork, 80 NetFlushSendData,
    // 137-140 GameFlowFrpgNet_*. Idle offline, so the offline crash hunts never caught them,
    // but in a session every connect state machine and packet flush ran twice per pair - a
    // connect crashed at the transition null-call site with no other guard holding.
    // EVENT SCRIPTS gated too (2026-10-31): 22-24 EmkSystem_Pre/ConditionStatus/Post, the
    // event-script VM - run twice per stereo pair, a centre dialog raised after an item pickup
    // stayed on screen ignoring every button and menu until a respawn.
    // 25 EventMan is KEPT (2026-11-02): 0.6.0 gated it with the scripts, and doors, levers,
    // lifts and undiscovered graces stopped answering the interact button (several players:
    // the prompt shows, the press does nothing). A player's bisect on 0.6.0: 16-21,25 opens
    // the doors with no stuck popup; 16-25 opened them but brought the popup back. Likely
    // mechanism, not traced: CSEventManImp holds the object-action executor (doors, levers,
    // chests; fromsoftware-rs event_man.rs), and the player code that reads the action
    // button (it references CSActionButtonMan) runs in both passes, so a press handled in
    // pass 2 found no request from EventMan to act on.
    std::string vrPass2FullFrameKeep = "0-7,10-14,16-21,25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-125,127-132,141-142,144-160,164,168";
    // SAFE variant, active during the settle window after any pass-2 guard releases (and
    // whenever a guard holds, trivially). Differs from the full set by gating ONLY the
    // GameFlowInGame_MoveMap_PostPhysics family (118, 123-125): measured 2026-08-22, that
    // family is BOTH the pass-2 camera carrier (mms/follow/updFollow/finalize hang under
    // it - gating it is exactly the "no depth, same eye twice" bug) AND the transition
    // crasher (it walks the WorldBlockChr grid and task lists, which teardown/arrival
    // rebuilds under it - the whole exe+0xc7f88b/0x3f9b2b/0x513baa/0x32b04a family).
    // Steady-state play needs it for stereo depth; the settle window must not run it.
    std::string vrPass2SafeKeep = "0-7,10-14,16-21,25,27,37-38,43,45-46,48-63,65-79,81,83-84,86-88,95-96,98,106-109,113-117,119-122,127-132,141-142,144-160,164,168";
    // Set by Load when the ini still carried a retired keep set it replaced (logged by ConfigLogNotes).
    std::string pass2KeepNote;
    // Seconds of SAFE keep set after the last guard releases. Was 20 (timer-guess era),
    // then 8; now 1.5 (2026-08-24 pt2, user asked for 1-2 s transitions): post-load
    // streaming is not a special state for full-set pass 2 - the open world streams
    // blocks continuously during normal play and full-set stereo is stable through it.
    // The states that DID crash are all separately covered now (teardown by the
    // authoritative signals, the restore bug fixed, combat/writes stood down while
    // dying). This is the first knob to RAISE if an arrival crash ever returns.
    float vrPass2SafeSeconds = 1.5f;
    // Seconds pass 2 stays fully OFF (cinema quad) after every player-inactive -> active
    // transition. Was a hardcoded 15 s: 8 s for the spawn-fade crash (exe+0x1e8b0bc, later
    // re-attributed to the readback experiment) plus margin for the 11 s arrival crash
    // (exe+0xc7f88b, root-caused 2026-08-22 to the blanket keep-set restore, since fixed).
    // With both root causes gone, and arrival now covered by the authoritative signals
    // (loading byte holds through the load, SAFE covers the streaming tail, writes stand
    // down while dying), 1 s only needs to bridge the arrival fade-in frames while the
    // chr list populates (2026-08-24; was 6, then 2). Second knob to raise (after
    // SafeSeconds) if spawn crashes return. Hot-reloads.
    float vrPass2SpawnGraceSeconds = 1.f;
    // Seconds pass 2 stays off after a full-screen menu closes (the stash-gap detector).
    // RETIRED 2026-08-24 (default 0): this bridge existed to cover the fast-travel confirm
    // before the game's own warp flag was wired - GameMan.warp_requested now fires ON the
    // confirm frame, so closing a menu without traveling costs nothing. Set >0 only as a
    // fallback if the GameMan AOB misses on a new patch. Hot-reloads.
    float vrPass2MenuGraceSeconds = 0.f;
    // Seconds pass 2 stays off after a >100 m one-frame camera jump (fast travel / death
    // warp ARRIVAL - the jump fires when the destination places the camera). Was a
    // hardcoded 12 s sized against the 11-13 s arrival crashes, whose root cause (the
    // blanket keep-set restore) is fixed; it stacks on the spawn grace at every load
    // Since 2026-08-24 this is ONLY the warp LATCH tail (GameERWarpGuardActive /
    // GameERWorldDying): it bridges warp_requested clearing into the player-null/loading
    // holds, measured 250-700 ms across all 2026-08-23 sessions - 2 s is ~3x margin.
    // (The >100 m arrival-jump grace it used to size is a fixed 1 s in
    // GameERTeleportGraceActive - that heuristic is superseded by the signals.)
    float vrPass2WarpGraceSeconds = 2.f;
    // Seconds pass 2 stays off after another player's character appears, leaves or changes
    // load state (GameERCoopRoster). A Seamless Co-op connect raises none of the warp/loading
    // signals, so this is its transition guard. Hot-reloads.
    float vrPass2CoopGraceSeconds = 4.f;
    // While any other player is in the world (or a session exists), pass 2 uses the SAFE keep
    // set. OFF by default: measured 2026-10-31 in a real session, SAFE gave NO depth - the
    // finalize replay never put the second eye's camera on screen (census finalize n/0, the
    // user saw the same image in both eyes). The join, leave and load moments that crashed are
    // held by the session/roster guards instead; steady co-op streams characters like enemies.
    bool vrPass2CoopSafe = false;
    // Replay the game's camera finalizer in pass 2 whenever the SAFE set is live, so the
    // second-eye camera reaches the renderer without the MoveMap family. The depth path
    // for the SAFE set; see HookCameraStepImpl.
    // ON since 2026-08-22 (evening): it was benched after the 17:05 session's "novel
    // pass-1 lock-on-camera crash", but that session predates the fd4 arg-block fix (the
    // corruption that wedged 17:58's whole engine), and the same foreign-yaw/lock-on storm
    // appears in sessions that never ran the replay - the attribution does not hold. With
    // it on, the SAFE window has real depth, which is what makes the shorter graces below
    // FEEL fast: cinema for the spawn grace, then straight to 3D.
    bool  vrPass2FinalizeReplay = true;
    // Seconds of SAFE keep set after EVERY A press. RETIRED 2026-08-23 (default 0): this
    // was a heuristic stand-in for "a fast-travel confirm may just have happened" from the
    // era when no signal separated a map confirm from an in-world press. The real signal
    // exists and is now wired: GameMan.warp_requested (+0x10, set by the game itself on the
    // request frame of every warp) drives the "warp requested" guard in DoSecondPass, with
    // the loading-screen event byte holding through the teardown window. Set this back >0
    // only as a fallback if the GameMan AOB ever misses on a new exe (the boot log then
    // says "GameMan AOB missed"). History that sized it while it lived: 2 s too short
    // (10:14 DL_PANIC), 8 s read as "stereo turns off on every A", 5 was the compromise.
    // Hot-reloads.
    float vrPass2SafeOnA = 0.f;
    // Preserve scalar input buffers and bounded per-TAE queue entries across zero-step full-frame pass 2.
    // Skip restoration after animation/equipment changes, explicit input flushes/restrictions, transitions,
    // or changes to player/module/storage ownership. New-press history remains game-owned. Hot-reloads.
    bool  vrPass2KeepActionRequest = true;
    // Complete each pass's eye report with the camera the renderer REALLY used (read at
    // pass end) instead of the one we wrote. The game's step re-smooths the camera after
    // our write on ~every frame (yaw ~100%, position >5 cm ~50%, measured 20:13) - an
    // image/pose mismatch the compositor renders as head-motion skew and noisy depth.
    // Hot-reloads; turn off to compare.
    bool  vrReportFinalCamera = true;
    // 1 = each Present carries the PREVIOUS pass's render (pipelined renderer): the XR frame
    // opens at pass 2's present (left eye) and closes at pass 1's (right). MEASURED on
    // 2026-08-20 with the pitch probe: pass-2 content lands at pass-1's present (tilting
    // pass 2 tilted the LEFT eye under lag 0), so 1 is the correct value on this renderer -
    // it pairs both eyes from ONE sim frame with poses matching their content.
    int   vrPresentLag = 1;
    // Serialise the two engine passes on the GPU. The pump starts pass 2 on the CPU the
    // moment pass 1's queue_tasks returns, while the GPU is still executing pass 1's draws;
    // if the engine's per-frame upload ring does not advance between the passes, pass 2's
    // constant uploads overwrite data the GPU is mid-read on (per-object dark/flicker,
    // worst in pass 1's image). 0 = off, 1 = drain the present queue before pass 2,
    // 2 = also drain after pass 2 (protects pass 2's image from the NEXT frame's uploads).
    // DISPROVEN as the cause of the 2026-08-20 dark/flicker (drained 3.8 ms/pass, no visual
    // change) - kept for future A/B; default off.
    int   vrGpuSync = 0;
    // Numpad hotkeys for the stereo A/B experiments (see debug_keys.h). On by default: they
    // do nothing until pressed, and they are the only way to drive the tests in a headset.
    bool  debugKeys = true;
    float vrDebugPass2Pitch = 0.f;   // degrees; diagnostic only

    // [Patches] - in-process .text patches (uberhalit's EldenRingFpsUnlockAndMore, MIT)
    int   patchFpsTarget = 0;         // 0 = leave the 60 fps cap; 30..360 = unlock to this
    // Opt the process into per-monitor DPI awareness at startup. Without it Windows
    // virtualizes the desktop size for the game (a 3440x1440 screen at 150% scaling reads as
    // ~2293x960), so its windowed resolution list stops at 1680x945 and the back buffer -
    // the only pixels VR has to work with - is that small before DWM upscales it.
    bool  patchDpiAware = true;

    // Camera-capture hunt: PAGE_GUARD the render camera and log every reader's rip/regs.
    // Finds where the renderer captures its once-per-frame camera copy (see cam_watch.h).
    bool  camWatch = false;
    float camWatchDelay = 5.f;      // seconds after player-active before arming
    float camWatchSeconds = 3.f;    // how long the guard stays armed
    bool  camWatchDt = false;       // watch the frame-timing dt block instead of the camera
    bool  camWatchIgt = false;      // watch the IGT counter's writer (game-time advance)
    bool  camWatchDst = false;      // watch the task-runner's staged frame-state block
    bool  igtTrace = false;         // hook the IGT-advance fn; log caller/dt/pass

    bool  m2TaskProbe = false;
    // TEST HARNESS ONLY. Runs the second engine pass UNCONDITIONALLY, with no OpenXR session
    // and no second-eye camera. Full stereo gates pass 2 on the game having stashed an eye,
    // which needs a live headset - so without this the pass-2 crash class (warp double-step)
    // cannot be reproduced by tools/run_game.ps1 at all. Loading a save is itself a map
    // transfer through MoveMapStep, so forcing the pass reproduces it headlessly.
    // Never enable for play: it costs a full extra engine frame and renders nothing useful.
    bool  m2ForcePass2 = false;
    // TEST HARNESS ONLY. Seconds after the player becomes active to set HP to 0, firing the
    // death warp - the one crash path reachable without a headset or map-screen navigation.
    // 0 = off. Costs the held runes (recoverable at the death spot); nothing else moves.
    int   m2KillPlayerAfter = 0;
    // TEST HARNESS ONLY. With ForcePass2: skip the pass-2 guard chain so transition
    // crashes can be REPRODUCED for diagnosis (the guards otherwise cover the headless
    // crash windows). Never enable for play.
    bool  m2Pass2NoGuards = false;
    // TEST HARNESS ONLY. Skip ONLY the player-dead guard: with the spawn guards still
    // live, KillPlayerAfter then drives pass 2 straight into the death teardown - the
    // exact unguarded window a fast-travel confirm creates (no death, no stash gap, pass 2
    // running while enemies and world blocks are deleted ahead of the player object).
    bool  m2Pass2NoDeadGuard = false;
    // M2 experiment: temporarily clear the `active` byte of one named task group and watch
    // what stops happening. Empty = off. Auto-restores.
    bool  taskRegisterTest = false;   // register no-op probe tasks into FrameBegin/GameSceneDraw/DrawEnd
    bool  taskQueueProbe = false;     // hook CSTask::queue_tasks: caller stack, thread, synchronicity
    bool  taskDumpLists = false;      // dump every group's registered tasks with RTTI names (needs TaskQueueProbe)
    bool  taskDoublePumpTest = false; // A/B: run a draw-only second engine frame per frame (needs TaskQueueProbe)
    bool  taskContentProbe = false;   // flat routing/brightness probe: pitch pass 2 + read back pixels per present
    std::string taskToggle;
    float taskToggleDelay = 45.f;
    float taskToggleSeconds = 3.f;
    std::string taskToggleMode = "api";   // api = call set_task_group_state | byte = write cstg_control.active
    int   taskToggleCycles = 4;   // A/B repeats; one before/after cannot separate cause from weather
    // Unattended test runs: press a button repeatedly so the game walks itself into the world
    // without a human. 0 seconds = off, which is the default - this presses buttons for real.
    int   autoPilotSeconds = 0;
    // One-shot raw back-buffer dump N seconds in (0 = off): writes backbuffer.ppm next to
    // ERVR.ini. Blocks one present for the readback - flat-harness diagnostics only.
    float expDumpBackbufferAt = 0.f;
    float autoPilotDelay = 20.f;      // let the game reach the title screen first
    float autoPilotPeriod = 0.6f;     // press/release cycle; menus must see the release
    std::string autoPilotButton = "A";
    int   autoPilotWiggleSeconds = 0; // after spawn: pan the camera (right stick only) this long
    // Post-spawn menu probe. Once the player is in the world (and any wiggle is done), tap
    // this button every MenuPeriod seconds for MenuSeconds, so an unattended run can capture
    // what the renderer does with a FULL-SCREEN menu open - which is a different draw path
    // from the in-world HUD and needs its own census. START = equipment/inventory,
    // BACK = map. Both only open and close a menu, so unlike the A-press script this is safe
    // to run after spawn: it cannot talk to an NPC, use an item or touch a grace.
    int   autoPilotMenuSeconds = 0;
    float autoPilotMenuPeriod = 6.f;
    std::string autoPilotMenuButton = "START";

    // [Physics]  Kinetic Elden Ring VR physics layer (src/phys/). Off by default: with it off
    // the mod behaves exactly like upstream ERVR.
    bool  physicsEnabled = false;
    float physicsTrackRadius = 6.f;       // m: characters this close are tracked (and logged on entering)
    bool  physicsDumpBones = true;        // log each new skeleton's bone names once
    // M1 spike (S1): overwrite one bone of the nearest character, to see whether the game draws it and whether
    // its attack hitboxes follow. Empty = off. The bone rotates by TestDeg about its own local TestAxis (x|y|z).
    std::string physicsTestBone;
    std::string physicsTestAxis = "z";   // x runs along the bone (a twist); z swings it
    float physicsTestDeg = 90.f;
    float physicsTestRadius = 4.f;
    // M2: B&S-style sword (PhysX). The hand+weapon is one rigid body pulled toward the controller by a
    // force/torque-limited joint (src/phys/sword_rig.*); replaces [Combat] Inertia while on.
    bool  physicsSword = true;
    float physicsSwordKgPerWeight = 0.3f;   // kg of simulated mass per ER weight unit
    float physicsSwordHandKg = 1.0f;        // the hand itself
    float physicsSwordForceBase = 300.f;    // N, + ForcePerStr * STR: how hard the arm can pull the grip
    float physicsSwordForcePerStr = 10.f;
    float physicsSwordTorqueBase = 60.f;    // N*m, + TorquePerStr * STR: how hard the wrist can turn the blade
    float physicsSwordTorquePerStr = 2.5f;
    float physicsSwordLinearSpring = 4000.f;  // N/m
    float physicsSwordLinearDamper = 120.f;   // N*s/m
    float physicsSwordAngularSpring = 1500.f;  // N*m/rad
    float physicsSwordAngularDamper = 60.f;   // N*m*s/rad
    // How far the blade's aim may trail the wrist (no runaway spins), set by how heavy the weapon is for this arm:
    // load = the weapon's gravity torque about the grip / the wrist torque (STR, two hands). Light for you -> MinAngle
    // (precise), at or past SwordLoadForMax -> MaxAngle (it swings you).
    float physicsSwordMinAngleDeg = 30.f;   // 35 deg flat felt right (play test 2) for a weight-9 sword at STR 16
    float physicsSwordMaxAngleDeg = 60.f;
    float physicsSwordLoadForMax = 0.6f;
    float physicsSwordSnapDistance = 0.6f;    // m: further than this from the controller, the grip snaps back
    bool  physicsSwordLog = true;             // lag summary every 5 s
    // M2.5: the blade against enemy arms and weapons (src/phys/enemy_contact.*). Enemy upper arms, forearms and
    // right-hand weapons within ContactRadius become solid capsules for the simulated blade; each contact's impulse
    // swings that enemy arm about its shoulder (inertia, stiffness = how hard it holds its pose), and it springs back.
    bool  physicsContact = true;
    float physicsContactRadius = 4.5f;          // m
    float physicsEnemyArmInertia = 0.3f;        // kg*m^2 about the shoulder (arm + weapon)
    float physicsEnemyArmStiffness = 20.f;     // N*m/rad pulling the arm back into its animation
    float physicsEnemyArmDamping = 1.0f;       // fraction of critical damping
    float physicsEnemyArmMaxDeg = 45.f;        // the swing never exceeds this
    std::string physicsEnemyWeaponBone = "R_Weapon_Sword";   // the bone the right-hand weapon hangs off
    std::string physicsEnemyWeaponAxis = "auto"; // its local axis along the blade (x|y|z, - to flip; auto = learned from the hits you take)
    float physicsEnemyWeaponLength = 1.0f;     // m; 0 = no weapon capsule
    float physicsEnemyWeaponKg = 3.f;          // the enemy weapon as a physics body
    float physicsEnemyArmForce = 450.f;        // N the enemy's arm drives its weapon with (yours: SwordForce*)
    float physicsEnemyArmTorque = 90.f;        // N*m about its grip
    // Upper-body sway: blows on the torso and head turn the upper body about the pelvis and it springs back.
    bool  physicsTorsoSway = true;
    float physicsTorsoInertia = 4.f;           // kg*m^2 about the pelvis
    float physicsTorsoStiffness = 200.f;       // N*m/rad
    float physicsTorsoDamping = 1.0f;
    float physicsTorsoMaxDeg = 20.f;
    bool  physicsContactLog = true;
    // The body (torso, head, thighs, calves): a blow's energy at contact (0.5 m v^2 of the blade, x STR/16 scaling)
    // decides it. Slash: >= BodyCutEnergy cuts through, >= BodyStickEnergy sticks in, less glances off. Thrust (tip
    // first): >= BodyPierceEnergy goes in and can be pushed deeper against BodyFriction, and pulled back out.
    bool  physicsBodyContact = true;
    float physicsBodyScale = 1.0f;            // thickness of the enemy body capsules (1 = an armoured soldier)
    float physicsBodyCutEnergy = 500.f;       // J
    float physicsBodyStickEnergy = 120.f;     // J
    float physicsBodyPierceEnergy = 80.f;     // J
    // Strike gate: the game's own attack collision (ERVR's strikes) only counts where the simulated blade really
    // struck: a body part cut, stuck or pierced, or an arm hit with ArmHitEnergy or more. A blade on an enemy
    // weapon or arm (a parry, a push), a glance, or a blade at an enemy that has not struck yet deals no damage.
    bool  physicsGateStrikes = true;
    float physicsArmHitEnergy = 500.f;        // J
    float physicsBodyFriction = 300.f;        // N*s/m resisting a blade sliding in or out
    // Hit judge (src/phys/hit_judge.*): an enemy blow hurts only if its weapon reached your body; a shield always
    // holds and costs stamina instead. Damage the game deals with no enemy weapon near you is left alone.
    bool  physicsHitJudge = true;
    float physicsHitWindow = 0.35f;            // s of weapon history a hit is judged on
    float physicsBodyRadius = 0.22f;           // m, your body capsule (feet to eyes)
    float physicsShieldRadius = 0.35f;         // m, the shield as a sphere at the left grip
    float physicsShieldStaminaPerDamage = 1.0f;// stamina a shield block costs per HP of the blow
    float physicsEnemyWeaponRefSpeed = 6.f;    // m/s: a weapon this fast at contact does full damage
    float physicsGrazeMin = 0.15f;             // the least damage fraction a touch does
    // Swing stamina: swinging itself costs stamina (not only a landed hit), and a tired arm is weak - the
    // sword's arm force and wrist torque drop, so the blade gets sluggish and hard to accelerate.
    bool  physicsSwingStamina = true;
    float physicsSwingStaminaSpeed = 5.f;       // m/s blade-tip speed above which a swing costs stamina
    float physicsSwingStaminaPerSec = 5.5f;    // per second at 8 m/s tip speed, x (0.5 + weapon kg / 3), x (speed/8)^2
    float physicsSwingRegenDelay = 1.2f;       // s after swing stamina is spent before the game may regenerate it
    float physicsTiredBelow = 0.25f;           // stamina fraction below which the arm weakens
    float physicsTiredStrength = 0.4f;         // arm strength multiplier at 0 stamina

    void Load(const std::wstring& iniPath);
};

Config& Cfg();
// What the last Load changed in the player's values (a retired default it replaced), for the log:
// Load runs before the log is open at startup.
void ConfigLogNotes();
}
