/* RotorTerror's Doors entry and real-app inspection seam.
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef ROTOR_APP_H
#define ROTOR_APP_H
#include "app.h"
#include "rotor.h"
enum rt_button {
    RTB_START, RTB_PILOT, RTB_ACE, RTB_TERROR, RTB_SOUND, RTB_HELP,
    RTB_EXIT, RTB_RESUME, RTB_RETRY, RTB_PAD, RTB_ROCKET, RTB_PAUSE,
    RTB_NEXT, RTB_SHIELD, RTB_CANNON, RTB_MAGAZINE, RTB_REPAIR, RTB_MENU,
    RTB_COUNT
};
extern const struct pocketos_app app_rotorterror;
struct rt_game *rotor_app_game(void *priv);
lv_obj_t *rotor_app_button(void *priv,unsigned index);
void rotor_app_refresh(void *priv);
#endif
