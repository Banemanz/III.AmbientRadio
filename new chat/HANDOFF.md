# NEW CHAT HANDOFF — III.AmbientRadio

Repository: https://github.com/Banemanz/III.AmbientRadio
Single draft PR: https://github.com/Banemanz/III.AmbientRadio/pull/1
Branch: `fix/audio-lifecycle-and-windows-ci`
Base: `main` at `74446c86c839f3dd0640e8ec8675fcb2e11fda8f`

## User requirements

- GTA III **1.0 US**, Plugin-SDK **October 25, 2025** pinned commit `624a6a49265fd7a6fc63bda1611013ceabeacb8a`.
- Windows GitHub Actions build and downloadable Win32 ASI + `bass.dll`.
- Fix radio station-switch artifact, car-exit latency, radio reading on foot, bridge announcements, mission passed tune, and NPC-vehicle radio acoustics. Improve performance and inspect other latent bugs.
- **Every source change stays on PR #1**; do not merge without request.

## Confirmed source defects and fixes in this PR

1. `Main.cpp`: `BASS_StreamCreateFile(TRUE, gLoadedBuffer.data(), ...)` borrowed memory from the staging vector. Replacing staging from the detached I/O thread could invalidate active playback. Fixed with separate `gActiveStreamBuffer` retained until `BASS_StreamFree` and generation-tracked background requests.
2. Startup loaded station 0 while the player was on foot. Removed eager `StartLoadingStation(0)`. HUD gated on player in a vehicle.
3. Car exit previously ran after queued-stream playback, announcements and station changes. Now performs exit teardown before any audio processing.
4. `ForceNativeRadioOff` called `cSampleManager::StopStreamedFile(0)` every frame and set `m_bDoTrackService=false`. The game's native streamed channel 0 also plays mission-passed and bridge audio. Removed unconditional stop and track-service suppression.
5. `Switch.cpp`: VC opcode `057D` does **not** exist in GTA III; GTA III has 0x394 mission passed, 0x3D1 parameterless PLAY_MISSION_AUDIO, 0x41E set radio, and bridge broadcasts call `cDMAudio::PlayRadioAnnouncement` at `0x57CC60` (13=COMOPEN, 14=SUBOPEN). Hook correct native function and use optional MP3s or stock WAVs if present, otherwise fall back to `cMusicManager::PlayAnnouncement`.
6. Wheel/key listeners swallowed input when on foot; gate them on actual seated player.
7. Wrong GTA:VC SDT SFX sample IDs caused static/tuning artifacts; now `RadioSwitchSfx=0` defaults off. Old INIs unchanged.
8. `AmbientCar.cpp` had borrowed-memory race and per-frame BASS_ChannelPlay. Separate ready/active buffers, cancel stale worker via generation; only resume from pause, use gentler EQ/compression, smooth volume, pan to nearby vehicles.
9. Audio files now resolve relative to `gta3.exe`, not the parent of the ASI (which can live anywhere under ModLoader).
10. Create default INI with `CreateFileA(CREATE_NEW)` not `ofstream(trunc)` to avoid overwriting an existing unreadable INI.

## Source reference verification

Verified against user-supplied original GTAIII IDB (gta3.exe 1.0 US):
- `0x57CC60` cDMAudio::PlayRadioAnnouncement(unsigned char), prologue `83 EC 08 8B 44 24 0C`
- `0x439485` script ProcessOneCommand CALL `E8 76 00 00 00`
- `0x57CE60` cDMAudio::SetRadioInCar
- `0x57E6D0` cMusicManager::DisplayRadioStationName
- `0x5680E0` cSampleManager::StopStreamedFile(0), previously called every frame.
Cross-checked with re3 MusicManager and ScriptCommands enum. 0x057D is outside GTA III supported script enum (max ~0x482).

## CI

`.github/workflows/build.yml`: Windows 2022, checkout pinned Plugin-SDK, call its `tools/generate/Visual Studio.bat`, MSBuild `plugin_iii` release, MSBuild original `IIIAmbientRadioEXP.vcxproj` Release GTA3/Win32, source regression checks, archive ASI + SDK-provided BASS runtime, upload artifact.
Local native game executable unavailable. CI compilation and regression checks MUST be consulted before calling changes build-verified.
BASS external library licenses apply.

## Next tasks

- Inspect CI run and fix any MSVC errors, repeating within this **same PR**.
- Test gameplay manually with station-change spam, ADF memory streams, large MP3 player folders, immediate car exit/re-enter, police car, mission-passed tune and bridge unlock missions. User will test actual GTA III.
- Check native radio suppression coexistence with original mission announcements (the native fallback may still require game-specific behaviour with music mode). Verify no simultaneous native radio + BASS.
- Consider improving station seeking and per-station audio caching without introducing unbounded RAM.
- Monitor `III.AmbientRadio.log` for BASS failures and async transitions.
