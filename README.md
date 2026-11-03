<p align="center">
  <a href="https://github.com/IlyaMez/EldenringVR/releases/latest"><img src="docs/images/readme/hero.jpg" alt="ELDEN RING in full VR. ERVR, from the maker of Spidy, recorded in the headset: the player's shield and sword in hand, and a co-op partner in gold armour crouched between them over a burning catacomb floor." width="100%"></a>
</p>

<h1 align="center">ERVR</h1>

<p align="center">
  <b>Native VR for ELDEN RING</b><br>
  Your head is the camera, the weapons are in your hands, and your swing is the attack.
</p>

<p align="center">
  <a href="https://github.com/IlyaMez/EldenringVR/releases/latest"><img src="docs/images/readme/btn-download.png" alt="Download ERVR: the latest release" width="232"></a>
  <a href="https://discord.gg/x27ZXDpdNt"><img src="docs/images/readme/btn-discord.png" alt="Join the Discord: news, help, clips" width="238"></a>
  <a href="https://ko-fi.com/ilyamezerowsky"><img src="docs/images/readme/btn-kofi.png" alt="Support on Ko-fi: keeps the updates coming" width="242"></a>
</p>

<p align="center">
  <a href="https://github.com/IlyaMez/EldenringVR/releases/latest"><img src="https://img.shields.io/github/v/release/IlyaMez/EldenringVR?label=latest&color=c9a45a&style=flat-square&labelColor=0c0b09" alt="Latest release"></a>
  <a href="https://github.com/IlyaMez/EldenringVR/releases"><img src="https://img.shields.io/github/downloads/IlyaMez/EldenringVR/total?label=downloads&color=e6c47a&style=flat-square&labelColor=0c0b09" alt="Downloads"></a>
  <a href="https://github.com/IlyaMez/EldenringVR/stargazers"><img src="https://img.shields.io/github/stars/IlyaMez/EldenringVR?label=stars&color=96753a&style=flat-square&labelColor=0c0b09" alt="Stars"></a>
  <a href="https://discord.gg/x27ZXDpdNt"><img src="https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fdiscord.com%2Fapi%2Fv10%2Finvites%2Fx27ZXDpdNt%3Fwith_counts%3Dtrue&query=%24.approximate_member_count&label=discord&suffix=%20members&color=eee6d2&style=flat-square&labelColor=0c0b09" alt="Discord members"></a>
</p>

<p align="center">
  <a href="#install-and-play">Install</a>
  ·
  <a href="#controls">Controls</a>
  ·
  <a href="#seamless-co-op">Seamless Co-op</a>
  ·
  <a href="#troubleshooting">Troubleshooting</a>
  ·
  <a href="docs/PLAYERS.md">Players' guide</a>
  ·
  <a href="CHANGELOG.md">What's new</a>
</p>

> **Kinetic Elden Ring VR** is a fork of [ERVR](https://github.com/IlyaMez/EldenringVR) by Ilya Mezerowsky.
> It adds a physics layer on top of ERVR: blades that stop on and embed in bodies, pushing, tripping,
> grabbing and throwing, decided by your strength against the enemy's weight. It is in early
> development and off by default (`[Physics] Enabled=false` in `ERVR.ini`), so a build behaves like
> upstream ERVR until it is turned on. Licensed GPL-3.0-or-later, like ERVR. Everything below is
> ERVR's own documentation.

ERVR is a fan mod by **Ilya Mezerowsky**, the maker of [Spidy](https://github.com/IlyaMez/SpidyVR), that
puts ELDEN RING in VR: an image for each eye at your headset's resolution, first person with the weapons in
your hands, and melee where the swing you make is the attack. One zip, one launcher, one **START VR** button.

<p align="center">
  <a href="https://www.youtube.com/watch?v=eFyqIWHU5nA"><img src="docs/images/readme/reel.webp" alt="Recorded in the headset during a Seamless Co-op session: the partner's flame sweeps the catacomb past the player's sword, the floor burns behind the raised shield, and the partner walks up between the shield and the sword." width="100%"></a>
  <br>
  <sub>Recorded in the headset, in Seamless Co-op with a partner on a flat screen. <a href="https://www.youtube.com/watch?v=eFyqIWHU5nA"><b>Watch the video on YouTube</b></a></sub>
</p>

> **Offline or Seamless Co-op only.** ERVR runs with Easy Anti-Cheat off, and the launcher starts the game
> that way. Never use it with EAC on or in FromSoftware's online play: that risks a ban on your account.
>
> **Early and in development.** This is ERVR's first public release. It supports game patches 1.16.2,
> 1.17.1 and 1.17.2; a new patch needs an ERVR update.

## Features

- **Full stereo at your headset's resolution.** The game renders an image for each eye from that eye's
  own camera, both at the same instant. Menus, loading screens and cutscenes go on a big screen in the
  headset.
- **First person, in the body.** Your head is the camera: look around, lean, duck. Look down and your
  character is there, the arms following your controllers.
- **Your swing is the attack.** Swing a weapon and ERVR fires the game's own attack along the blade in your
  hand. Damage, poise, status and stamina come from the game, and a faster swing hits harder. Weapons have
  weight: a head-heavy one is slower to turn than a balanced one.
- **A shield that blocks.** Raise the shield in front of you to block; move it into a blow to deflect it.
- **Two-hand by grabbing the hilt.** Put your free hand on the hilt and squeeze: the game two-hands the
  weapon, strength bonus and all. The right trigger then fires its Ash of War, the effect without the
  animation.
- **Spells and arrows go where your hand points.** Cast with a staff or a seal, or loose an arrow, and it
  flies along the casting hand.
- **A weapon wheel in each hand.** Hold a grip with the hand still: that hand's weapons open around it with
  the game's own icons. Let go over one to switch.
- **Your pouch on your belt.** The four pouch items with a shortcut hang at your waist. Grab one and press X
  to use it; let go and it goes back.
- **The HUD on its own panel.** Health, stamina and the menus float on a panel at headset resolution, where
  you want it, sharper than in the game's frame.
- **Ride Torrent.** The rider and the view move with the horse, and the right stick or turning your body turns both.
- **Seamless Co-op.** Play with friends on flat screens: they need neither ERVR nor a headset.
- **Body haptics.** A bHaptics vest, sleeves and visor play blows on the side they come from, your hits in
  your arms, and the heart at low HP.
- **One launcher.** It checks the game, the headset and your PC, sets the game up for VR and starts it with
  one button.

## Join the community

<p align="center">
  <a href="https://discord.gg/x27ZXDpdNt"><img src="docs/images/readme/card-discord.png" alt="Join the Discord: release news, help and clips. Come say hi." width="49%"></a>
  <a href="https://ko-fi.com/ilyamezerowsky"><img src="docs/images/readme/card-kofi.png" alt="Support on Ko-fi: your tip pays for updates, and the next full VR conversion." width="49%"></a>
</p>

### On Discord: where the players are

- **Release news first.** Every release is announced in `#ervr-updates`, with a ping if you ask for one.
- **Help and fixes.** Setup questions and bugs go to `#ervr-help`, where the answers stay findable.
- **Talk and clips.** Show a fight, a boss or a co-op session in `#ervr-chat`.
- **One server for both mods.** Spidy's players are there too.

**[Join the Discord](https://discord.gg/x27ZXDpdNt)**

### On Ko-fi: what keeps it going

- **ERVR is free.** Every release, the whole mod, here on GitHub. A tip unlocks nothing.
- **Tips pay for updates.** ERVR is built with a lot of AI help, and those tokens are the bill.
- **And for the next conversion.** More games in full VR, made to the same standard: native stereo, tracked
  hands, and the game's own systems in your hands.

**[Support ERVR on Ko-fi](https://ko-fi.com/ilyamezerowsky)**

## Watch and follow

<table>
<tr>
<td width="50%" align="center" valign="top"><a href="https://www.youtube.com/watch?v=eFyqIWHU5nA"><img src="https://i.ytimg.com/vi/eFyqIWHU5nA/maxresdefault.jpg" alt="ERVR crossplay: VR and a flat screen in one Seamless Co-op session"></a><br><b>VR and flat, one session</b><br><sub>Seamless Co-op with a partner on a flat screen</sub></td>
<td width="50%" align="center" valign="top"><a href="https://www.youtube.com/watch?v=krQd75OXMik"><img src="https://i.ytimg.com/vi/krQd75OXMik/maxresdefault.jpg" alt="Spidy's trailer: Spider-Man Remastered in full VR"></a><br><b>From the same maker: Spidy</b><br><sub>Spider-Man Remastered in full VR</sub></td>
</tr>
</table>

<p align="center">
  <a href="https://www.youtube.com/@lianmez"><img src="docs/images/readme/social-youtube.png" alt="YouTube: @lianmez" width="172"></a>
  <a href="https://x.com/lianmez"><img src="docs/images/readme/social-x.png" alt="X: @lianmez" width="121"></a>
  <a href="https://discord.gg/x27ZXDpdNt"><img src="docs/images/readme/social-discord.png" alt="Discord: #ervr-chat" width="172"></a>
</p>

Gameplay, release videos and guides go up on [YouTube](https://www.youtube.com/@lianmez) and
[X](https://x.com/lianmez). Made a video about ERVR? Post it on the [Discord](https://discord.gg/x27ZXDpdNt),
and put this page's link in its description so players find the real download.

## What you need

- **ELDEN RING on Steam**, patch 1.16.2, 1.17.1 or 1.17.2.
- **A PC VR headset with an OpenXR runtime:** Meta Quest through Quest Link or Air Link, Virtual Desktop, or
  SteamVR (Index, Vive, Pico, Steam Link and others). Played on Quest 3 and Quest Pro.
- **A gaming PC.** Each eye is its own render, so VR costs about twice the GPU of playing flat.
- **Windows.**
- Optional: bHaptics devices and the [bHaptics Player](https://www.bhaptics.com/software/player/).

ERVR needs no other mod and does not use R.E.A.L. VR.

## Install and play

1. Download the zip from [Releases](https://github.com/IlyaMez/EldenringVR/releases/latest) and unpack it to
   a folder you can write to, such as `Documents\ERVR`.
2. Start **ERVR Launcher.exe**. If Windows says it protected your PC, choose *More info*, then *Run anyway*.
3. Fix anything the Play tab marks: the game and its patch, the VR runtime, the headset.
4. Connect your headset and press **START VR**.

<p align="center">
  <img src="docs/images/launcher.png" alt="ERVR Launcher's Play tab: the checks of this PC (the game and its patch, the VR runtime, the headset, ERVR in the game folder, flat mods in VR's way), START VR and the session log. Its other tabs hold the options, the controls and ERVR's links." width="820">
</p>

<details>
<summary><b>What the launcher does</b></summary>

- Finds the game in your Steam libraries and checks its patch against ERVR's verified address maps.
- Finds the OpenXR runtime your headset streams through (Virtual Desktop while its streamer runs, SteamVR
  while it runs, else Windows' active one), or the one you choose, and checks the headset.
- Installs or updates ERVR in the game folder, keeping your `ERVR.ini`.
- Parks flat render mods and R.E.A.L. VR leftovers (renamed to `*.ervr-off`, never deleted).
- Turns off the game's depth of field, motion blur and ray tracing for VR, keeping the original settings file.
- Starts Steam if needed and the game with EAC off, through Seamless Co-op when it is installed, and shows
  `ERVR.log` as the game writes it.
- Its Options tab edits `ERVR.ini` in place, and ERVR takes each change within a second, mid-game too.
- *Play flat* puts your flat mods back; *Remove ERVR* takes it out of the game folder.

</details>

Like ERVR, the launcher has no network code of its own: links open in your browser, and a new version is a
new zip.

## Controls

<p align="center">
  <a href="docs/images/readme/controls-quest3.jpg"><img src="docs/images/readme/controls-quest3.jpg" alt="ERVR's controls on Quest 3 Touch Plus controllers. Your swing is the attack: swing a hand and that hand's weapon strikes; the triggers and grips never press the attack buttons. Left hand: Menu opens the menu, hold it for the map; left stick walks, click to crouch, click held and tilted is the D-pad; Y interacts, the game's event action; X uses the item in your quick slot; trigger guards; grip is the skill, and held with the hand still it opens the weapon wheel. Right hand: right stick turns, smoothly or in snaps, click to lock on; B dodges, hold B to sprint; A jumps, and in a menu A confirms and B goes back; trigger is the Ash of War while two-handing, and with a staff, seal or bow it casts; grip held with the hand still opens the weapon wheel. Your hands do the rest: swing a hand and that hand's weapon strikes, faster hits harder; a shield raised in front blocks, and moved into a hit it deflects; a free hand on the hilt with its grip two-hands the weapon, open the grip to let go; with a staff, seal or bow the spell or arrow flies where the hand points; your head is the camera, look, lean, duck." width="100%"></a>
  <br><br>
  <a href="docs/images/readme/controls-actions.jpg"><img src="docs/images/readme/controls-actions.jpg" alt="The actions, by where you are. A menu is open? Combat stands down and the left stick moves the cursor. In a fight: swing a hand and that hand's weapon strikes, faster hits harder, a full windup hardest; a shield raised in front blocks, and moving the shield into a hit deflects; left trigger guards, left grip is the skill. Two hands: a free hand on the hilt with its grip two-hands the weapon, open the grip to let go; the right trigger while two-handing is the weapon's skill, the Ash of War, without its animation; while two-handing the free hand stays on the hilt at the spot you grabbed. Spells and bows: with a staff, seal or bow the trigger casts or shoots; the spell or arrow flies where the casting hand points; locked on, the game aims at the enemy. Your weapons: a grip held with the hand still opens a wheel of that hand's weapons round the hand; let go over a weapon and the game switches to it; let go in the middle and nothing switches. Moving and menus: your head is the camera, look round, lean, duck; left stick click held and tilted is the D-pad for spells, items and weapons; left stick click held 0.5 s turns on menu control by hand, for grace sites, shops and level up." width="100%"></a>
  <br>
  <sub>Every button is a key in <code>ERVR.ini</code>. The full list, with two-handing, the weapon wheel and menu navigation, is in the <a href="docs/PLAYERS.md#controls">players' guide</a>.</sub>
</p>

| Do this | With |
|---|---|
| Look, lean, duck | Your head |
| Walk / turn | **Left stick**, where you look / **right stick**, smooth or in snaps |
| Dodge, sprint / jump | **B**, hold to sprint / **A** |
| Crouch / lock on | **Left stick click** / **right stick click** |
| Quick slots (the D-pad) | Hold the **left stick click** and tilt |
| Attack | Swing the weapon. Faster hits harder; the left hand attacks with the left weapon |
| Block, deflect | Raise the shield in front of you; move it into a blow to deflect it. **Left trigger** guards |
| Two-hand | Put your free hand on the hilt and squeeze its **grip**; open it to let go |
| Ash of War | **Right trigger** while two-handing; otherwise the **left grip** |
| Switch weapons | Hold a **grip** with the hand still, move over a weapon and let go |
| Cast, shoot | **Trigger** with a staff, seal or bow in that hand: it flies where the hand points |
| Use item / interact (open, pick up, talk) | **X** / **Y** |
| Menu / map | Tap / hold the **menu button**. In a menu the **left stick** moves the cursor, **A** confirms, **B** goes back |

## Seamless Co-op

ERVR runs inside [Seamless Co-op](https://www.nexusmods.com/eldenring/mods/510) sessions, and your partners
play the ordinary game. When Seamless Co-op is installed, START VR starts the game through it. Everyone must be
on the same game patch and the same Seamless Co-op version.

Your partners see your character with its normal animations, not your tracked arms, so a physical hit lands
without your character swinging on their screen. ERVR counts only the damage you cause, and a player joining
or leaving drops you to the big screen for a few seconds while their character loads. The details are in the
[players' guide](docs/PLAYERS.md#seamless-co-op).

## Troubleshooting

| Problem | Fix |
|---|---|
| The launcher says your patch has no address map | ERVR supports patches 1.16.2, 1.17.1 and 1.17.2. A newer patch needs an ERVR update: watch `#ervr-updates` on the Discord. |
| The headset is not found | Connect it first, pick the runtime it streams through on the Play tab, press *Check*. |
| The picture is washed out, doubled or broken | A flat render mod (RenoDX, DLSS Enabler, ReShade effects) is in the way. Start from the launcher: it parks them. |
| Low frame rate, or play feels sluggish | Lower *Resolution* in the launcher's Options and the game's own graphics settings. In Virtual Desktop pick 72, 80 or 90 Hz yourself. *Big screen* is the cheap fallback. |
| The stick does nothing in a menu | Hold the left stick click for half a second: menu navigation by hand turns on. |

More fixes are in the [players' guide](docs/PLAYERS.md#troubleshooting). When you report a problem, bring
`ELDEN RING\Game\ERVR\ERVR.log` to `#ervr-help` on the [Discord](https://discord.gg/x27ZXDpdNt).

## Building from source

Needs Visual Studio 2022 or newer with "Desktop development with C++" (CMake is bundled). The first configure
fetches MinHook, the ReShade headers, the OpenXR SDK and bHaptics' C/C++ SDK.

```powershell
.\build.ps1                          # build\dist: the launcher, dxgi.dll, dinput8.dll, ERVR\ERVR.dll ...
ctest --test-dir build -C Release    # regression tests
.\tools\package_release.ps1          # build\release\ERVR-v<version>.zip, the published layout
.\install.ps1                        # or install build\dist into the game folder by hand
```

Releases are built on GitHub: pushing a `v*` tag runs the public release workflow, which builds, attests and
publishes the zip.

## How it works

ERVR runs inside the game as a [ReShade](https://reshade.me) add-on with an OpenXR session of its own: a
small `dinput8.dll` loads `ERVR.dll`, and the bundled `dxgi.dll` is ReShade's own add-on build. Each frame the
game runs a second engine pass from the other eye's camera with the world held still, so both eyes see the
same instant. The HUD is lifted off the frame onto its own layer. Your weapon swings fire the game's own attack
events along the blade your hands are moving, and two-handing, skills and spells go through the game's own
systems, fed from your tracked hands.

| Read more | |
|---|---|
| [Players' guide](docs/PLAYERS.md) | Setup, every control, settings, co-op, body haptics, fixes (the zip's `README.md`) |
| [Changelog](CHANGELOG.md) | Every build: what changed and why |
| [Development notes](docs/DEVELOPMENT.md) | Architecture, first person, the HUD layer, physical melee, two-handing, cast aim, the offsets |
| [Third-party notices](THIRD_PARTY.md) | Bundled components, their licences and hashes, and the research ERVR builds on |

## Verifying a download

Some antivirus programs flag ERVR: `ERVR.dll` injects into a running game, and `dxgi.dll` is ReShade's add-on
build, which is unsigned by design. Both detections are generic, not a finding about this code. Each release
is built by [a public GitHub Actions workflow](.github/workflows/release.yml) from the tagged commit, with a
signed provenance attestation and its SHA-256 in the release notes:

```powershell
gh attestation verify ERVR-vX.Y.Z.zip --repo IlyaMez/EldenringVR
```

ERVR has no telemetry, no analytics, no auto-updater and no network code of its own. For body haptics it loads
bHaptics' own library, which talks to the bHaptics Player on your PC; `[bHaptics] Level=0` keeps it from
loading at all.

## Contributing

Fixes and features are welcome as pull requests, one feature each. [CONTRIBUTING.md](CONTRIBUTING.md) says
how. Bindings for headsets the author does not own and address maps for new game patches help most.

## Support ERVR

ERVR is free, and built with a lot of AI help whose tokens aren't. Two things keep it moving:

- **[A tip on Ko-fi](https://ko-fi.com/ilyamezerowsky)** pays for ERVR's updates, and for more full VR
  conversions of this quality.
- **[The Discord](https://discord.gg/x27ZXDpdNt)** is where releases are announced, problems get solved, and
  your clips and ideas shape what comes next.

<p align="center">
  <a href="https://ko-fi.com/ilyamezerowsky"><img src="docs/images/readme/btn-kofi.png" alt="Support on Ko-fi: keeps the updates coming" width="242"></a>
  <a href="https://discord.gg/x27ZXDpdNt"><img src="docs/images/readme/btn-discord.png" alt="Join the Discord: news, help, clips" width="238"></a>
</p>

No tip to spare? A star on this page, or a clip of your best fight, helps other players find ERVR.

Please share the original release zip with its credit intact, so players get working files and know where
updates come from.

<p align="center">
  <a href="https://github.com/IlyaMez/EldenringVR/releases/latest"><img src="docs/images/readme/footer.jpg" alt="ERVR: native VR for ELDEN RING. PC VR, Steam, OpenXR headsets. From Ilya. Free fan mod, not affiliated with or endorsed by FromSoftware or Bandai Namco." width="100%"></a>
</p>

## Credits

- erfps2 (Dasaav) for the first-person camera set, and [fromsoftware-rs](https://github.com/vswarte/fromsoftware-rs)
  (vswarte) for the `CSChrTaeAnimEvent` and ChrIns module layouts.
- [DSAnimStudio](https://github.com/Meowmaritus/DSAnimStudio) (Meowmaritus) for the TAE event templates, and the
  Souls Modding wiki (Muffin) for the BehaviorParam row scheme.
- [Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR) and
  [cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port) for the "controllers to engine input,
  gestures to actions" structure.
- [ReShade](https://github.com/crosire/reshade) (crosire), [MinHook](https://github.com/TsudaKageyu/minhook),
  the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK), [Dear ImGui](https://github.com/ocornut/imgui),
  bHaptics' C/C++ SDK, and the Cinzel and Barlow fonts.

## Licence

ERVR is Copyright (C) 2026 Ilya Mezerowsky, and free software under the
[GNU General Public License, version 3 or later](LICENSE). If you distribute a modified version, you publish
its source too. Third-party licences, including the bundled, unmodified ReShade binary (BSD-3), are in
[THIRD_PARTY.md](THIRD_PARTY.md).

ERVR is a free fan project, not affiliated with or endorsed by FromSoftware, Bandai Namco, Meta, Valve or the
ReShade project. ELDEN RING is a trademark of its owners; ERVR ships no game code or assets. For offline or
Seamless Co-op play only, never with Easy Anti-Cheat or FromSoftware's online play.
