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

// GTA III CPed::m_ePedState at +0x224. Exiting/dragged/arrested states
// precede bInVehicle clearing; death and entry transitions are not seated.
enum PedRadioState {
    ExitTrain = 42, Dying = 48, Dead = 49, Carjack = 50, Dragged = 51,
    EnterCar = 52, StealCar = 53, ExitCar = 54, Arrested = 56, EnterTrain = 41
};
inline bool SeatAllowsRadio(bool inVehicle, bool hasVehicle, int state) {
    if (!inVehicle || !hasVehicle) return false;
    switch (state) {
    case ExitTrain: case Dying: case Dead: case Carjack: case Dragged:
    case EnterCar: case StealCar: case ExitCar: case Arrested: case EnterTrain:
        return false;
    default: return true;
    }
}
// Consume only our wheel actions, once, and leave every other mouse/control
// action untouched. The caller must gate this on active vehicle gameplay.
inline bool CaptureWheel(bool allowed, bool& up, bool& down, bool& next,
                         bool& prev, bool& fromWheel) {
    if (!allowed || (!up && !down)) return false;
    next = up;
    prev = !up && down;
    fromWheel = true;
    up = down = false;
    return true;
}
// Mouse wheels are always quiet, even when a legacy INI opts into switching SFX.
inline bool PlaySwitchClick(bool enabled, bool fromWheel) {
    return enabled && !fromWheel;
}

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
