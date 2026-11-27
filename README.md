# III.AmbientRadio

GTA III 1.0 US ASI for custom in-car radio and nearby NPC-vehicle radio audio using [BASS](https://www.un4seen.com/bass.html).

## Build (Windows / GitHub Actions)

CI builds **Win32 x86** with Visual Studio 2022. It checks out Plugin-SDK revision `624a6a49265fd7a6fc63bda1611013ceabeacb8a` (October 25, 2025), builds `plugin_iii.lib`, then builds the original project. BASS headers, import library and runtime come from `Plugin-SDK/shared/bass`. No game executable or radio tracks are included.

The Actions artifact contains `III.AmbientRadio.asi` and `bass.dll`. For local builds set `PLUGIN_SDK_DIR` and build **Release GTA3 | Win32** using Visual Studio 2022.

## Installation

Place `III.AmbientRadio.asi` and `bass.dll` in GTA III's `scripts` folder (or another ASI-loader directory). The mod generates `III.AmbientRadio.ini` if missing and keeps existing settings. Station paths are relative to the GTA III executable folder; supply your own station audio.

`[SETTINGS]` includes ambient-radio EQ/compression/reverb options, `ScriptIntegration`, and `RadioSwitchSfx` (disabled by default to avoid old mismatched sample indices). `[CONTROLS]` configures keyboard/controller radio switching. Existing INIs remain compatible.

## What this branch changes

- Keeps BASS backing buffers alive for the lifetime of both player and ambient streams; obsolete file-loading workers cannot replace a newer station's audio.
- Stops radio audio and removes the name banner immediately when Claude leaves a vehicle, with no station preloading on foot. Radio controls no longer intercept mouse-wheel weapon switching on foot.
- Leaves GTA III's native streamed audio channel available for mission-passed music, mission dialogue and bridge broadcasts.
- Handles bridge announcements using the native GTA III dispatch; uses optional `audio/COMOPEN.MP3` and `audio/SUBOPEN.MP3` overrides or stock `audio/COMOPEN.WAV` and `audio/SUBOPEN.WAV`. If neither file exists, the game gets the original announcement call.
- Disables mismatched old switch-static samples by default (`RadioSwitchSfx=0`). You can opt in with `RadioSwitchSfx=1`, but the original sample-number assumption still needs an in-game GTA III-specific audit.
- Improves NPC-vehicle audio with gentler EQ/compression, stereo positioning, smoothed distance volume and single-shot resume from pause.
- Caches MP3 player track durations, avoids needless idle scanning locks, and queues worker diagnostics for safe game-thread logging.
- Persists per-vehicle stations using generation-safe pool handles and avoids invalid random selection for MP3-only configurations.

## In-game regression checklist

The Actions build can verify compilation, not actual GTA III audio. Test with one copy of the ASI loaded:

1. Walk on foot, scroll the mouse wheel and press the radio key: no radio starts, no HUD station text appears, and weapon switching remains functional.
2. Enter a normal car, switch stations quickly (including ADF and MP3 files), and exit immediately while a station is still loading: no playback on foot, stale station audio, or static artifacts.
3. Re-enter the same car, then enter a different car; check saved stations and `RadioAutoTune` separately.
4. Trigger a mission-passed tune and story bridge unlock while in a normal car. Check both `COMOPEN.WAV` and `SUBOPEN.WAV` and optional MP3 overrides.
5. Test police/emergency and `[NORADIO]` vehicles, cutscenes, audio menu pause/resume, new game and save loading.
6. Stand near several NPC-driven vehicles, approach, pass, turn around and enter one. Check distance rolloff, panning, and whether the selected station matches the car you enter.
7. Review `III.AmbientRadio.log` for missing BASS files, failed decoder streams or unexpected reloads.

The mod only supports GTA III 1.0 US native addresses. BASS is required: if you manually install just the ASI, copy an appropriately licensed `bass.dll` alongside it.

## Limits

Built for **GTA III 1.0 US**. GitHub Actions checks compilation and static tests; gameplay sound, mission events, and interactions with other audio mods require in-game testing. Please check `new chat/HANDOFF.md` for technical details and test cases.

BASS is a third-party library from Un4seen; review its [license](https://www.un4seen.com/bass.html) for your distribution/use.
