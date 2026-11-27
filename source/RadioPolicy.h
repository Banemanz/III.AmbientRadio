#pragma once

// Original GTA III PC 1.0 US (not VC): stock tuning IDs 0..8, user tracks 9,
// police streamed audio 10, OFF/city ambience 11. Never use 10 for silence.
namespace RadioPolicy {
constexpr int NativeMp3 = 9;
constexpr int NativePolice = 10;
constexpr int NativeOff = 11;
constexpr int NativeGameMode = 1;
constexpr int Off = -1;
constexpr int Unassigned = -2;

inline bool ValidStation(int station, int count) {
    return station == Off || (station >= 0 && station < count);
}

// Preserve a car's assigned NPC station even when RadioAutoTune is enabled.
// Otherwise the existing optional carry-over applies before player-only memory.
inline int EntryStation(int saved, bool npcAssigned, bool autoTune, int previous, int count) {
    if (npcAssigned && ValidStation(saved, count)) return saved;
    if (autoTune && ValidStation(previous, count)) return previous;
    return ValidStation(saved, count) ? saved : Unassigned;
}

inline int MapScriptStation(int native, int count, int mp3) {
    if (native == NativeOff) return Off;
    if (native == NativeMp3) return mp3 >= 0 && mp3 < count ? mp3 : Unassigned;
    if (native >= 0 && native < NativeMp3 && native < count && native != mp3) return native;
    return Unassigned; // police/native announcements never index custom stations
}
inline bool NativeRadioTrack(int native) { return native >= 0 && native <= NativePolice; }
inline bool SuppressNativeTuning(bool noRadio, bool policeVehicle, bool customPolice) {
    return noRadio || !policeVehicle || customPolice;
}
}
