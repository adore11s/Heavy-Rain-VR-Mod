# Heavy Rain VR Mod

Play **Heavy Rain (PC)** in VR. Every frame is drawn in true 3D for each eye with full head tracking, and you can switch to a first-person view through the eyes of Ethan, Madison, Jayden or Shelby and use your own hands. Works with the Steam, Epic and GOG versions of the game.

> ⚠️ **Work in progress.** It is playable but not finished; expect rough edges. First person and the motion controls are still being tuned. **Tested:** on PS VR2 with SteamVR. Other headsets and the Epic and GOG copies are less tested.

## What the mod does

**In VR**

- **True 3D for each eye.** The game is drawn separately for each eye, with full head tracking.
- **Rendering method:** Native (both eyes every frame, the default), AFR (one eye per frame) or Mono (one picture for both eyes, the fastest), set in the VR settings panel.
- **A square VR picture** that matches your headset's resolution automatically (or a size you choose), whatever your monitor's resolution.
- **The window on your monitor** is 1280 x 720 while VR runs. Drag its edges to resize it; it stays 16:9. Your normal window comes back when VR ends.
- **Game settings:** no need to change them. The mod starts the game in a window by itself.
- **VR settings:** pause the game or open the game's Options menu, and a panel appears on the right. Point a controller and pull its trigger.
- **Menus:** tap all **four face buttons** for the pause screen (with the VR settings panel).
- **Xbox or PlayStation (PS VR2) controls**, with matching button icons in the game (VR settings panel). With PlayStation controls, the moves the game asks you to make with the controller are read from your right VR controller.
- **Rumble.** The game's controller vibration plays on your VR controllers.
- **3D audio:** sounds follow your head.

**First person** (only playing in third person? Skip this part: your controllers work like a gamepad)

- **First person on / off:** hold your right hand next to your head and press **○ (PS VR2) / B (Meta Quest, Index)**. Press it while walking for gameplay, or during a cutscene for cutscenes. By default both are off (also in the VR settings panel, or **F1** / **F3** on the keyboard). You can see your whole body.
- **Cutscenes from the character's eyes:** the view sits between the character's own eyes and moves with their head.
- **Motion controls:** while you walk the character, your controllers move their arms and hands. Things a scene hands your character (a pizza box, a bottle, a gun) go to your controller hand.
- **Finger detection.** Rest your fingers on the controller and the character's fingers follow: lift your index finger to point, squeeze for a fist; the thumb closes with your grip.
- **Your body follows your headset:** lean or take a step and the body moves with you, so your hands always reach.
- **Prompts in front of you:** action prompts, hold and mash prompts, tutorial texts, thoughts and dialogue choices appear in front of you instead of around the character.
- **In the VR settings panel:** smooth or snap turning (and its speed / angle); motion controls on or off (off: the controllers work like a gamepad); hide head on or off (on, the default: the character's head, hair, eyes and teeth are not drawn in first person).

## Install

1. Open your Heavy Rain folder (the one with `HeavyRain.exe`). In Steam: right-click the game → **Manage** → **Browse local files**. In the Epic Games Launcher: the game's **⋯** menu → **Manage** → the folder icon. In GOG Galaxy: the game's settings icon → **Manage installation** → **Show folder**.
2. Copy this whole **`HR_VR_MOD`** folder into it.
3. Double-click **`Install VR mod.cmd`** in the `HR_VR_MOD` folder. You only do this **once** (and again after downloading a new version).
4. From now on, put your headset on and start the game as usual, from Steam, Epic, GOG or `HeavyRain.exe`.

VR turns on by itself as soon as the game starts, and SteamVR starts with it. Using Virtual Desktop (**VDXR**) or another OpenXR runtime instead? Connect your headset to it first. The mod's settings file (`hrvr.ini`) is created by itself the first time, next to `HeavyRain.exe`; every setting is explained inside it.

## Controls

| Button | What it does |
|---|---|
| **○ (PS VR2) / B (Meta Quest, Index) next to your head** | First person on / off - press it while walking for gameplay, during a cutscene for cutscenes |
| **Tap all four face buttons** | Pause screen with the VR settings panel |

The sticks, face buttons and triggers work like a gamepad's; the grips are the bumpers (L1 / R1).

## Uninstall

Double-click **`Uninstall VR mod.cmd`** in the `HR_VR_MOD` folder and type **YES**. It moves the mod's own files to the **Recycle Bin** (nothing is deleted for good) and puts the game's display mode back. **Your saves and the game's files are never touched.**

## Something went wrong?

Open an issue on the mod's GitHub page (**Issues** → **New issue**) with a few words on what you were doing, and attach:

| What happened | Send |
|---|---|
| **The game crashed** | The newest **`crash_<date>.txt`** from **`hrvr_logs\crash_reports`** in your game folder |
| **Something looks or works wrong, the game froze, or VR did not start** | **`hrvr_logs\hrvr.log`** from your game folder, right after quitting the game (if you have started the game again since, send **`hrvr.prev.log`** too) |
| **The install or uninstall window showed an error** | A screenshot of that window |

The mod writes these files itself while the game runs (`hrvr.log` is started fresh each time; the run before is kept as `hrvr.prev.log`).

None of these files contain your user name or other personal information.

## Known bugs (fixes coming soon)

- **Holding items in hands can bug out in first person with motion controls.**

## Open source

The mod is free and open source under the **MIT license**. The full source code is in the [`source`](source) folder.
It is derived from the Beyond: Two Souls VR mod, which runs on the same Quantic Dream engine.
The mod includes MinHook, the official OpenXR loader and the Montserrat font: see **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**.

## Support the mod ☕

The mod is free for everyone, and it always will be. Donating is never required. If you enjoy it and want to help me make more VR mods, any support on **[Ko-fi](https://ko-fi.com/frenchiecore)** is very much appreciated. Thank you!

*Not affiliated with Quantic Dream or Sony Interactive Entertainment. No game files are included.*
