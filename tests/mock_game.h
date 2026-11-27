#pragma once
// Headless mock for exercising the production vehicle assignment code.
// This intentionally doesn't pretend to implement GTA III's audio engine.
#include <map>
struct CVehicle { int m_nModelIndex; int handle; };
struct MockPool { int m_nSize = 3; };
struct CPools {
    static inline MockPool pool;
    static inline MockPool* ms_pVehiclePool = &pool;
    static inline std::map<int, CVehicle*> live;
    static int GetVehicleRef(CVehicle* v) { return v ? v->handle : -1; }
    static CVehicle* GetVehicle(int ref) {
        auto it = live.find(ref);
        return it == live.end() ? nullptr : it->second;
    }
};
struct MockMusic {
    bool UsesPoliceRadio(CVehicle* v) { return v->m_nModelIndex == 116; }
};
static MockMusic gMusicManager;
namespace plugin {
struct MockEvent { template<class F> void Add(F) {} };
struct Events { static inline MockEvent initGameEvent; };
}
