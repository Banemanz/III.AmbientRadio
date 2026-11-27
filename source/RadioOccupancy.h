#pragma once
#include "CPed.h"
#include "RadioPolicy.h"

// Validate the portable policy against the pinned game's SDK, not VC states.
static_assert(int(PEDSTATE_EXIT_CAR) == RadioPolicy::ExitCar &&
    int(PEDSTATE_DRAG_FROM_CAR) == RadioPolicy::Dragged &&
    int(PEDSTATE_EXIT_TRAIN) == RadioPolicy::ExitTrain &&
    int(PEDSTATE_ENTER_TRAIN) == RadioPolicy::EnterTrain &&
    int(PEDSTATE_CARJACK) == RadioPolicy::Carjack &&
    int(PEDSTATE_ENTER_CAR) == RadioPolicy::EnterCar &&
    int(PEDSTATE_STEAL_CAR) == RadioPolicy::StealCar &&
    int(PEDSTATE_ARRESTED) == RadioPolicy::Arrested &&
    int(PEDSTATE_DIE) == RadioPolicy::Dying &&
    int(PEDSTATE_DEAD) == RadioPolicy::Dead, "GTA III ped-state mismatch");

inline bool IsRadioSeated(const CPed* ped)
{
    return ped && RadioPolicy::SeatAllowsRadio(ped->m_bInVehicle,
        ped->m_pVehicle != nullptr, int(ped->m_ePedState));
}
