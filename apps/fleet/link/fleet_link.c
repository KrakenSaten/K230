/*
 * PocketFleet multiplayer transport seam. See fleet_link.h.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "fleet_link.h"

const char *fleet_link_state_text(enum fleet_link_state state)
{
    switch (state) {
    case FLEET_LINK_UP:
        return "Mesh online.";
    case FLEET_LINK_CONNECTING:
        return "Looking for the mesh service.";
    case FLEET_LINK_NO_SERVICE:
        return "The mesh service is not running on this device, and multiplayer "
               "needs it. It is switched on per device.";
    case FLEET_LINK_RADIO_OFF:
        return "The radio is off or not available.";
    default:
        return "";
    }
}
