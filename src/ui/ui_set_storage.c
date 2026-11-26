/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_storage.c — Storage: Basic, Schedule, Disk Manager, Disk Quota,
 * Network Storage (docs/management.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

static ui_set_item_t g_items[5];

void ui_storage_open(int first)
{
    const ui_set_item_t items[] = {
        { TR(S_BASIC), &ui_icon_nav_basic, NULL, NULL, NULL, NULL, NULL },
        { TR(S_SCHEDULE), &ui_icon_nav_schedule, NULL, NULL, NULL, NULL, NULL },
        { TR(S_DISK_MANAGER), &ui_icon_nav_disks, NULL, NULL, NULL, NULL, NULL },
        { TR(S_DISK_QUOTA), &ui_icon_nav_quota, NULL, NULL, NULL, NULL, NULL },
        { TR(S_NET_STORAGE), &ui_icon_nav_nas, NULL, NULL, NULL, NULL, NULL },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open(TR(S_STORAGE), g_items, 5, first);
}
