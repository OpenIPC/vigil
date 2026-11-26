/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_system.c — System: General (device name, language, date and
 * time, time zone with DST, NTP, formats) and the HTTP API's settings. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

typedef struct {
    ui_form_t *form;
} api_t;

static void *api_open(ui_set_t *s, lv_obj_t *pane)
{
    api_t *a = calloc(1, sizeof *a);
    if (a) a->form = ui_set_form(s, pane, "api");
    return a;
}
static int api_dirty(void *st) { return ui_form_embed_dirty(((api_t *)st)->form); }
static int api_save(void *st) { return ui_form_embed_save(((api_t *)st)->form); }
static void api_cancel(void *st) { ui_form_embed_reset(((api_t *)st)->form); }
static void api_close(void *st)
{
    ui_form_embed_free(((api_t *)st)->form);
    free(st);
}

static ui_set_item_t g_items[2];

void ui_sysset_open(int first)
{
    const ui_set_item_t items[] = {
        { TR(S_GENERAL), &ui_icon_nav_general, NULL, NULL, NULL, NULL, NULL },
        { "HTTP API", &ui_icon_nav_ethernet, api_open, api_dirty, api_save, api_cancel, api_close },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open(TR(S_SYSTEM), g_items, 2, first);
}
