/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_account.c — Account: Users and Groups (vigil's own accounts,
 * docs/api.md "Accounts and permissions"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

static ui_set_item_t g_items[2];

void ui_account_open(int first)
{
    const ui_set_item_t items[] = {
        { TR(S_USERS), &ui_icon_nav_users, NULL, NULL, NULL, NULL, NULL },
        { TR(S_GROUPS), &ui_icon_nav_groups, NULL, NULL, NULL, NULL, NULL },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open(TR(S_ACCOUNT), g_items, 2, first);
}
