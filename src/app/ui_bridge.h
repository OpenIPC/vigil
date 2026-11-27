/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_bridge.h — fill the UI's model from the core (see ui_bridge.c). */
#ifndef UI_BRIDGE_H
#define UI_BRIDGE_H

#include "ui_model.h"

/* Every member of `m`; channels, events and settings come from the core
 * (channels_status, events, the configuration store). Returns 0 or -1. */
int ui_bridge_init(ui_model_t *m);

/* The management pages' part of the model (ui_mgmt.c). */
const ui_mgmt_t *ui_mgmt_model(void);

#endif /* UI_BRIDGE_H */
