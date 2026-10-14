# IW7-VR

A fork of [iw7-mod](https://github.com/auroramod/iw7-mod) that adds OpenXR VR support to the *Call of Duty: Infinite Warfare* campaign, in the spirit of CircuitLord's Titanfall 2 VR (which is built on Northstar the same way). You need to own the game on Steam.

The build output is `iw7-vr.exe`, so it can live next to the official `iw7-mod.exe`. The updater keeps iw7-mod's data files current but never replaces the exe.

## Status

| Milestone | State |
|---|---|
| 1. Virtual screen: the game on a big screen in the headset | in testing |
| 2. Head-tracked camera | next (needs reverse engineering) |
| 3. Stereo 3D | planned |
| 4. Motion controls and controller-aimed weapons | planned |
| 5. Menus, HUD and cutscenes in VR | planned |
| 6. Comfort options, Jackal cockpit, polish | planned |

## Playing a test build

1. Copy `iw7-vr.exe` into your Infinite Warfare folder.
2. Start Steam. Connect the headset and make sure its app is the active OpenXR runtime: Meta Quest Link, SteamVR or Virtual Desktop (VDXR).
3. Run `iw7-vr.exe`. Turn off V-Sync in the game's graphics settings, so the headset rather than your monitor sets the frame rate.

Logs are written to `iw7-vr.log` next to the exe; the previous run is kept as `iw7-vr.old.log`.

## Console (`~`)

| Command / dvar | |
|---|---|
| `vr_status` | runtime, headset and session state |
| `vr_recenter` | put the screen in front of you |
| `vr_restart` | restart VR, e.g. after fixing a runtime problem |
| `vr_screen_distance` / `vr_screen_width` / `vr_screen_height` | screen placement in metres |
| `vr_screen_follow_head` | lock the screen to your head |
| `vr_screen_srgb` | colour fix: off if the image looks too dark, on if washed out |
| `vr_dump_game` | developer: writes the decrypted game image to `iw7_ship.dump.exe` for analysis |
| `-novr` | launch flag that starts with VR off |

## Code

- `src/client/component/vr/`: the VR component
  - `xr_runtime.*`: OpenXR instance, session, swapchains, frame loop
  - `vr.cpp`: dvars, commands, start/retry, D3D locking
  - `vr_log.*`: logging
  - `re_tools.cpp`: reverse-engineering helpers
- `deps/OpenXR-SDK` + `deps/premake/openxr.lua`: the OpenXR loader, linked statically
- Hooks into upstream code: `component/gui/gui.cpp` (present hook calls `vr::on_present`), `component/updater.cpp` (no exe updates)

Each frame, the present hook on the render thread calls `xrWaitFrame` without holding the game's D3D11 lock. It then takes the lock for `xrBeginFrame`, the back-buffer copy and `xrEndFrame`.

## Building

Same as iw7-mod: `git clone --recurse-submodules`, run `generate.bat`, then build `build\iw7-mod.sln` in Visual Studio 2022 (x64, Release). GitHub Actions builds every push and uploads `iw7-vr.exe` as the "Release binaries" artifact.
