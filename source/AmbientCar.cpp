#include "plugin.h"
#include "CPlayerPed.h"
#include "CPed.h"
#include "CVehicle.h"
#include "CVector.h"
#include "CMenuManager.h"
#include "CPools.h"
#include "CCutsceneMgr.h"
#include "CCamera.h"
#include "RadioPolicy.h"
#include "RadioFileLoader.h"
#include "bass\bass.h"
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>

using namespace plugin;

// Declared in Main.cpp
struct RadioStation {
    std::string name;
    std::string file;
};
extern std::vector<RadioStation> stations;
extern double gRadioTime;
extern std::string gGameFolder;
extern std::ofstream gLog;
extern void QueueRadioWorkerLog(const std::string& message);
extern bool gBassReady;
extern float gAmbientRadioDistance;
extern float gAmbientRadioFullVolumeDistance;
extern float gAmbientRadioVolume;
extern float gAmbientRadioInCarVolume;
extern bool gAmbientRadioParamEq;
extern bool gAmbientRadioCompressor;
extern bool gAmbientRadioReverb;

// Declared in RadioVehicles.cpp
bool CanVehiclePlayAmbient(CVehicle* vehicle);
int GetAmbientStationForVehicle(CVehicle* vehicle);

// Declared in Main.cpp — maps the 0..127 radio-volume slider to BASS 0.0..1.0.
float RadioVolume(unsigned int pref);
static unsigned char GetMusicVolume()
{
    int volume = CMenuManager::m_nPrefsMusicVolume;
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    return static_cast<unsigned char>(volume);
}
// Declared in Main.cpp — shared playback position; original stations start at 0,
// added stations use the randomized offset.
double StationTimelineMs(int index, bool applyStartOffset);

// Ambient stream — only touched on main thread
static HSTREAM gAmbientStream = 0;
static int gAmbientVehicleRef = -1;
static float gAmbientLastVol = -1.0f;
static DWORD gAmbientLastVolTick = 0;
static float gAmbientLastPan = 0.0f;

// Background loading
// gAmbientBuf: protected by gAmbientMutex
// gAmbientReadyStream: fully prepared stream, handed to main thread
static std::mutex gAmbientMutex;
static std::vector<BYTE> gAmbientReadyBuffer; // worker -> main handoff, under mutex
static std::vector<BYTE> gAmbientActiveBuffer; // borrowed by BASS until StreamFree
static HSTREAM gAmbientReadyStream = 0;
static std::atomic<unsigned int> gAmbientGeneration{ 0 };
static std::atomic<DWORD> gAmbientRetryAfter{ 0 };
static bool gAmbientWasPaused = false;
static std::atomic<bool> gAmbientStreamReady(false);
static std::atomic<bool> gAmbientLoadingInProgress(false);
static int gAmbientLoadingStation = -1;


static void StopAmbientStream()
{
    if (gAmbientStream) {
        BASS_ChannelStop(gAmbientStream);
        BASS_StreamFree(gAmbientStream);
        gAmbientStream = 0;
    }
    gAmbientActiveBuffer.clear(); // AFTER BASS_StreamFree
    {
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        gAmbientGeneration.fetch_add(1, std::memory_order_acq_rel);
        gAmbientStreamReady.store(false, std::memory_order_release);
        gAmbientLoadingInProgress.store(false, std::memory_order_release);
        if (gAmbientReadyStream) {
            BASS_StreamFree(gAmbientReadyStream);
            gAmbientReadyStream = 0;
        }
        gAmbientReadyBuffer.clear();
    }
    gAmbientVehicleRef = -1;
    gAmbientLoadingStation = -1;
    gAmbientLastVol = -1.0f;
    gAmbientLastVolTick = 0;
    gAmbientLastPan = 0.0f;
    gAmbientWasPaused = false;
}

static bool IsADF(const std::string& path)
{
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".adf";
}

static float DistanceSq(const CVector& a, const CVector& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

static const float AMBIENT_MAX_VOL = 1.0f;

static float CalcVolume(float distance)
{
    float hearDistance = gAmbientRadioDistance;
    float fullVolumeDistance = gAmbientRadioFullVolumeDistance;
    if (hearDistance < 5.0f) hearDistance = 5.0f;
    if (fullVolumeDistance < 1.0f) fullVolumeDistance = 1.0f;
    if (fullVolumeDistance >= hearDistance) fullVolumeDistance = hearDistance * 0.5f;

    if (distance >= hearDistance)      return 0.0f;
    if (distance <= fullVolumeDistance) return AMBIENT_MAX_VOL;
    float range = hearDistance - fullVolumeDistance;
    float t = (hearDistance - distance) / range;
    return t * t * AMBIENT_MAX_VOL;
}

// Everything heavy runs here — file I/O, decoding, stream creation, FX setup
static void AmbientLoadThread(int stationIndex, unsigned int generation, double timelineSnapshot, DWORD startedAt)
{
    if (stationIndex < 0 || stationIndex >= (int)stations.size()) return;
    auto fail = [&]() {
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        if (gAmbientGeneration.load(std::memory_order_acquire) == generation) {
            gAmbientLoadingInProgress.store(false, std::memory_order_release);
            gAmbientRetryAfter.store(GetTickCount() + 2000, std::memory_order_release);
        }
    };

    std::string fullPath = gGameFolder + stations[stationIndex].file;
    HSTREAM stream = 0;
    std::vector<BYTE> ownedAdfBuffer;
    if (IsADF(fullPath)) {
        std::vector<BYTE> buf;
        const auto cancelled = [generation]() {
            return gAmbientGeneration.load(std::memory_order_acquire) != generation;
        };
        if (!LoadDecryptedAdf(fullPath, buf, cancelled)) {
            if (!cancelled()) {
                QueueRadioWorkerLog("Ambient: could not read/decrypt " + fullPath);
                fail();
            }
            return;
        }
        stream = BASS_StreamCreateFile(TRUE, buf.data(), 0,
            (QWORD)buf.size(), BASS_SAMPLE_LOOP);

        if (!stream) {
            QueueRadioWorkerLog("Ambient: BASS error " + std::to_string(BASS_ErrorGetCode()));
            fail();
            return;
        }

        // Keep the worker's source memory alive until the ready handoff.
        ownedAdfBuffer = std::move(buf);
    }
    else {
        stream = BASS_StreamCreateFile(FALSE, fullPath.c_str(), 0, 0,
            BASS_SAMPLE_LOOP);

        if (!stream) {
            QueueRadioWorkerLog("Ambient: BASS error " + std::to_string(BASS_ErrorGetCode()));
            fail();
            return;
        }
    }

    if (gAmbientGeneration.load(std::memory_order_acquire) != generation) {
        BASS_StreamFree(stream);
        return;
    }
    // Same station clock as the interior radio, compensating for load latency.
    QWORD totalBytes = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
    double totalMs = BASS_ChannelBytes2Seconds(stream, totalBytes) * 1000.0;
    if (totalMs > 0.0) {
        double seekMs = fmod(timelineSnapshot + (DWORD)(GetTickCount() - startedAt), totalMs);
        if (seekMs < 0.0) seekMs += totalMs;
        QWORD seekBytes = BASS_ChannelSeconds2Bytes(stream, seekMs / 1000.0);
        BASS_ChannelSetPosition(stream, seekBytes, BASS_POS_BYTE);
    }

    // Optional built-in BASS DX8 effects for an exterior car/small-speaker sound.
    // These stay in bass.dll (no bass_fx add-on) and can be toggled in the INI.
    if (gAmbientRadioParamEq) {
        BASS_DX8_PARAMEQ eq;
        eq.fBandwidth = 1.6f;

        HFX hFx = BASS_ChannelSetFX(stream, BASS_FX_DX8_PARAMEQ, 1);
        eq.fCenter = 180.0f;
        eq.fGain = -5.0f;
        if (hFx) BASS_FXSetParameters(hFx, &eq);

        HFX hFx2 = BASS_ChannelSetFX(stream, BASS_FX_DX8_PARAMEQ, 2);
        eq.fCenter = 5500.0f;
        eq.fGain = -8.0f;
        if (hFx2) BASS_FXSetParameters(hFx2, &eq);
    }

    if (gAmbientRadioCompressor) {
        BASS_DX8_COMPRESSOR comp;
        comp.fGain = 0.0f;
        comp.fAttack = 8.0f;
        comp.fRelease = 120.0f;
        comp.fThreshold = -14.0f;
        comp.fRatio = 2.0f;
        comp.fPredelay = 4.0f;
        HFX hComp = BASS_ChannelSetFX(stream, BASS_FX_DX8_COMPRESSOR, 3);
        if (hComp) BASS_FXSetParameters(hComp, &comp);
    }

    if (gAmbientRadioReverb) {
        BASS_DX8_REVERB rev;
        rev.fInGain = 0.0f;
        rev.fReverbMix = -18.0f;
        rev.fReverbTime = 450.0f;
        rev.fHighFreqRTRatio = 0.25f;
        HFX hRev = BASS_ChannelSetFX(stream, BASS_FX_DX8_REVERB, 4);
        if (hRev) BASS_FXSetParameters(hRev, &rev);
    }

    // Publish the stream and its borrowed memory as a single ownership transfer.
    // An obsolete worker frees only its own stream, never the active one.
    {
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        if (gAmbientGeneration.load(std::memory_order_acquire) != generation) {
            BASS_StreamFree(stream);
            return;
        }
        gAmbientReadyBuffer = std::move(ownedAdfBuffer);
        gAmbientReadyStream = stream;
        gAmbientStreamReady.store(true, std::memory_order_release);
        gAmbientLoadingInProgress.store(false, std::memory_order_release);
    }
}

static void StartAmbientLoad(int stationIndex, int vehicleRef)
{
    if (stationIndex < 0 || stationIndex >= (int)stations.size()) return;
    if (gAmbientLoadingInProgress.load(std::memory_order_acquire)) return;
    DWORD retryAfter = gAmbientRetryAfter.load(std::memory_order_acquire);
    if (retryAfter && (LONG)(GetTickCount() - retryAfter) < 0)
        return;

    StopAmbientStream();
    unsigned int gen;
    {
        std::lock_guard<std::mutex> lock(gAmbientMutex);
        gen = gAmbientGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        gAmbientLoadingStation = stationIndex;
        gAmbientVehicleRef = vehicleRef;
        gAmbientLoadingInProgress.store(true, std::memory_order_release);
    }
    const double timeline = StationTimelineMs(stationIndex, false);
    std::thread(AmbientLoadThread, stationIndex, gen, timeline, GetTickCount()).detach();
}

extern bool gAmbientRadioEnabled;

static int sCachedVehicleRef = -1;
static DWORD sScanTick = 0;
static int sRetryVehicleRef = -1;
static bool sHasScanned = false;
static bool HasAmbientWork()
{
    return gAmbientStream || gAmbientVehicleRef >= 0 ||
        gAmbientLoadingInProgress.load(std::memory_order_acquire) ||
        gAmbientStreamReady.load(std::memory_order_acquire);
}
static void ResetAmbient()
{
    if (HasAmbientWork()) StopAmbientStream();
    sCachedVehicleRef = -1;
    sHasScanned = false;
    sRetryVehicleRef = -1;
    gAmbientRetryAfter.store(0, std::memory_order_release);
}

static CVehicle* ResolveAmbientVehicle(int handle, CVehicle* playerVehicle)
{
    if (handle < 0 || !CPools::ms_pVehiclePool) return nullptr;
    CVehicle* vehicle = CPools::GetVehicle(handle);
    return vehicle && vehicle != playerVehicle && vehicle->m_pDriver &&
        CanVehiclePlayAmbient(vehicle) ? vehicle : nullptr;
}

static void ProcessAmbientRadio()
{
    CPlayerPed* player = FindPlayerPed();
    if (!gAmbientRadioEnabled || !gBassReady || !player ||
        !CPools::ms_pVehiclePool || FrontEndMenuManager.m_bWantToLoad ||
        FrontEndMenuManager.m_bWantToRestart) {
        ResetAmbient();
        return;
    }

    const bool seated = player->m_bInVehicle && player->m_pVehicle;
    CVehicle* ownVehicle = seated ? player->m_pVehicle : nullptr;
    // An entry can complete between two scans: stop the exterior source now,
    // without erasing its persistent station assignment.
    if (gAmbientVehicleRef >= 0 && !ResolveAmbientVehicle(gAmbientVehicleRef, ownVehicle))
        StopAmbientStream();

    if (FrontEndMenuManager.m_bMenuActive || CCutsceneMgr::ms_running || TheCamera.m_bWideScreenOn) {
        if (gAmbientStream && !gAmbientWasPaused) {
            BASS_ChannelPause(gAmbientStream);
            gAmbientWasPaused = true;
        }
        return;
    }

    const CVector playerPos = player->GetPosition();
    const float rangeSq = gAmbientRadioDistance * gAmbientRadioDistance;
    DWORD now = GetTickCount();
    CVehicle* closest = ResolveAmbientVehicle(sCachedVehicleRef, ownVehicle);
    float closestSq = closest ? DistanceSq(playerPos, closest->GetPosition()) : rangeSq;
    if (closestSq >= rangeSq) { closest = nullptr; sCachedVehicleRef = -1; }
    // Re-scan at 10Hz; check the cached vehicle's live position each frame to
    // stop stale sounds immediately. A small hysteresis avoids repeated
    // decoder restarts at a busy intersection with equally distant cars.
    if (!sHasScanned || now - sScanTick >= 100 ||
        (sCachedVehicleRef >= 0 && !closest)) {
        sHasScanned = true; sScanTick = now;
        const float switchThreshold = closest ? closestSq * 0.81f : rangeSq;
        CVehicle* next = nullptr;
        float nextSq = switchThreshold;
        for (int i = 0; i < CPools::ms_pVehiclePool->m_nSize; ++i) {
            if (CPools::ms_pVehiclePool->IsFreeSlotAtIndex(i)) continue;
            CVehicle* vehicle = CPools::ms_pVehiclePool->GetAt(i);
            if (!vehicle || vehicle == ownVehicle || !vehicle->m_pDriver) continue;
            float dist = DistanceSq(playerPos, vehicle->GetPosition());
            if (dist >= nextSq || !CanVehiclePlayAmbient(vehicle)) continue;
            next = vehicle; nextSq = dist;
        }
        if (next) { closest = next; closestSq = nextSq; }
        sCachedVehicleRef = closest ? CPools::GetVehicleRef(closest) : -1;
    }
    if (!closest || GetMusicVolume() == 0 || gAmbientRadioVolume <= 0.0f ||
        (seated && gAmbientRadioInCarVolume <= 0.0f)) {
        if (HasAmbientWork()) StopAmbientStream();
        return;
    }

    const int station = GetAmbientStationForVehicle(closest);
    if (station < 0) {
        if (HasAmbientWork()) StopAmbientStream();
        sCachedVehicleRef = -1;
        return;
    }
    if (gAmbientVehicleRef != sCachedVehicleRef || gAmbientLoadingStation != station) {
        if (HasAmbientWork()) StopAmbientStream(); // invalidate old ready or loading source
        if (sRetryVehicleRef != sCachedVehicleRef) {
            sRetryVehicleRef = sCachedVehicleRef;
            gAmbientRetryAfter.store(0, std::memory_order_release);
        }
        StartAmbientLoad(station, sCachedVehicleRef);
        return;
    }

    const float distance = sqrtf(closestSq);
    const float volume = CalcVolume(distance) * gAmbientRadioVolume *
        (seated ? gAmbientRadioInCarVolume : 1.0f) * RadioVolume(GetMusicVolume());
    const CVector vehPos = closest->GetPosition();
    const CVector right = player->GetRight();
    const float dx = vehPos.x - playerPos.x, dy = vehPos.y - playerPos.y;
    const float planar = sqrtf(dx * dx + dy * dy);
    float pan = planar > 0.1f ? (dx * right.x + dy * right.y) / planar * 0.55f : 0.0f;
    pan = (std::max)(-0.55f, (std::min)(0.55f, pan));

    if (gAmbientStreamReady.load(std::memory_order_acquire) && !gAmbientStream) {
        {
            std::lock_guard<std::mutex> lock(gAmbientMutex);
            gAmbientStreamReady.store(false, std::memory_order_release);
            gAmbientStream = gAmbientReadyStream;
            gAmbientReadyStream = 0;
            gAmbientActiveBuffer = std::move(gAmbientReadyBuffer);
        }
        if (!gAmbientStream) return;
        BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_VOL, volume);
        BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_PAN, pan);
        if (!BASS_ChannelPlay(gAmbientStream, FALSE)) {
            StopAmbientStream();
            gAmbientRetryAfter.store(GetTickCount() + 2000, std::memory_order_release);
            return;
        }
        gAmbientLastVol = volume; gAmbientLastPan = pan;
        gAmbientWasPaused = false; gAmbientLastVolTick = now;
        gLog << "Ambient: vehicle ref " << sCachedVehicleRef << " playing ["
            << stations[station].name << "]" << std::endl;
        return;
    }

    if (gAmbientStream) {
        if (gAmbientWasPaused) {
            BASS_ChannelPlay(gAmbientStream, FALSE);
            gAmbientWasPaused = false;
        } else if (BASS_ChannelIsActive(gAmbientStream) == BASS_ACTIVE_STOPPED) {
            StopAmbientStream();
            gAmbientRetryAfter.store(now + 2000, std::memory_order_release);
            return;
        }
        if (now - gAmbientLastVolTick >= 100) {
            gAmbientLastVolTick = now;
            float smooth = gAmbientLastVol + (volume - gAmbientLastVol) * 0.4f;
            if (volume == 0.0f || fabsf(smooth - gAmbientLastVol) > 0.002f) {
                if (volume == 0.0f) smooth = 0.0f;
                BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_VOL, smooth);
                gAmbientLastVol = smooth;
            }
            if (fabsf(pan - gAmbientLastPan) > 0.015f) {
                BASS_ChannelSetAttribute(gAmbientStream, BASS_ATTRIB_PAN, pan);
                gAmbientLastPan = pan;
            }
        }
        return;
    }
    if (!gAmbientLoadingInProgress.load(std::memory_order_acquire))
        StartAmbientLoad(station, sCachedVehicleRef);
}

class AmbientCarPlugin
{
public:
    AmbientCarPlugin()
    {
        Events::initGameEvent.Add([]() { ResetAmbient(); });
        Events::gameProcessEvent.Add([]() { ProcessAmbientRadio(); });
    }
} ambientCarPlugin;
