# III.AmbientRadio

GTA III 1.0 US ASI for custom in-car radio and nearby NPC-vehicle radio audio using [BASS](https://www.un4seen.com/bass.html).

## Build (Windows / GitHub Actions)

CI builds **Win32 x86** with Visual Studio 2022. It checks out Plugin-SDK revision `624a6a49265fd7a6fc63bda1611013ceabeacb8a` (October 25, 2025), builds `plugin_iii.lib`, then builds the original project. BASS headers, import library and runtime come from `Plugin-SDK/shared/bass`. No game executable or radio tracks are included.

The Actions artifact contains `III.AmbientRadio.asi` and `bass.dll`. For local builds set `PLUGIN_SDK_DIR` and build **Release GTA3 | Win32** using Visual Studio 2022.

## Installation

Place `III.AmbientRadio.asi` and `bass.dll` in GTA III's `scripts` folder (or another ASI-loader directory). The mod generates `III.AmbientRadio.ini` if missing and keeps existing settings. Station paths are relative to the GTA III executable folder; supply your own station audio.

`[SETTINGS]` includes ambient-radio EQ/compression/reverb options, `ScriptIntegration`, and `RadioSwitchSfx` (disabled by default to avoid old mismatched sample indices). `[CONTROLS]` configures keyboard/controller radio switching. Existing INIs remain compatible. **AmbientRadioDistance defaults to 20.0 game units** for new INIs and absent settings. If your existing INI says `AmbientRadioDistance=60.0`, change that line to `AmbientRadioDistance=20.0`; the mod intentionally never overwrites your configuration.

## What this branch changes

- Keeps BASS backing buffers alive for the lifetime of both player and ambient streams; obsolete file-loading workers cannot replace a newer station's audio.
- Stops radio audio and removes the name banner immediately when Claude leaves a vehicle, with no station preloading on foot. Radio controls no longer intercept mouse-wheel weapon switching on foot.
- Leaves GTA III's native streamed audio channel available for mission-passed music, mission dialogue and bridge broadcasts.
- Handles bridge announcements using the native GTA III dispatch; uses optional `audio/COMOPEN.MP3` and `audio/SUBOPEN.MP3` overrides or stock `audio/COMOPEN.WAV` and `audio/SUBOPEN.WAV`. If neither file exists, the game gets the original announcement call.
- Uses the verified GTA III native radio identifiers: police stream 10, OFF 11 (city ambience). Redirects the three stock game-mode tuning call sites and prevents the native F9 MP3 shortcut from starting a second radio underneath BASS. Genuine police-radio vehicles keep stock dispatch unless a usable optional `audio/police.mp3` replaces it. Native tuning and mission audio servicing remain separate.
- The off-state HUD reads **RADIO OFF**. Optional switch SFX now use GTA III sample IDs 343 (click) and 188 (crackle), not the previous mismatched VC IDs. `RadioSwitchSfx=0` remains the default; sound quality still needs a game test.
- Improves NPC-vehicle audio with gentler EQ/compression, stereo positioning, smoothed distance volume and single-shot resume from pause.
- Caches MP3 player track durations, avoids needless idle scanning locks, and queues worker diagnostics for safe game-thread logging.
- Inherits the actual NPC car's ambient station, even if the nearest audible vehicle has changed, and seeks to the shared station timeline. This takes priority over `RadioAutoTune`; that option still applies otherwise. Remembers the player's final selection (including OFF) on exit. Vehicle IDs are generation-safe and checked against the model; an MP3 folder-only station does not become an exterior radio source.
- Scans for nearby vehicles at 10 Hz, measures the current source's distance every frame, uses switching hysteresis, and changes BASS attributes only when needed. Obsolete exterior decodes are cancelled before playback, and ADF reads/decryption use bounded chunks. MP3 playlists advance at EOF instead of looping a single track indefinitely.

## In-game regression checklist

The Actions build can verify compilation, not actual GTA III audio. Test with one copy of the ASI loaded:

1. Walk on foot, scroll the mouse wheel and press the radio key: no radio starts, no HUD station text appears, and weapon switching remains functional.
2. Enter a normal car, switch stations quickly (including ADF and MP3 files), and exit immediately while a station is still loading: no playback on foot, stale station audio, or static artifacts.
3. With `RadioAutoTune` both on and off, approach a car playing a known station, pass a second car playing a different station, then enter the first car. The inherited station should match the first car, continuing the shared broadcast. Switch to another station or RADIO OFF, exit and re-enter; confirm persistence with auto-tune off.
4. While a custom station is ON in a civilian car, confirm no stock police dispatch or second station is audible; cycle all stations including MP3 and RADIO OFF. Try F9. Enter a real police car and verify legitimate dispatch, with and without a usable `audio/police.mp3`.
5. Trigger a mission-passed tune and story bridge unlock while in a normal car. Check both `COMOPEN.WAV` and `SUBOPEN.WAV` and optional MP3 overrides.
6. Test police/emergency and `[NORADIO]` vehicles, cutscenes, audio menu pause/resume, new game and save loading.
7. Stand near several NPC-driven vehicles, approach, pass, turn around and enter one. Check the 20-unit range, distance rolloff, panning, and station handoff.
8. Review `III.AmbientRadio.log` for missing BASS files, failed decoder streams, native-hook signature mismatches or unexpected reloads.

The mod only supports GTA III 1.0 US native addresses. BASS is required: if you manually install just the ASI, copy an appropriately licensed `bass.dll` alongside it.

## Limits

Built for **GTA III 1.0 US**. GitHub Actions checks compilation, source invariants, and a headless C++ vehicle assignment / ADF-loader regression test; gameplay sound, mission events, and interactions with other audio mods require in-game testing. Please check `new chat/HANDOFF.md` for technical details and test cases.

BASS is a third-party library from Un4seen; review its [license](https://www.un4seen.com/bass.html) for your distribution/use.
