/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_i18n.c — the string table and the languages (see ui_i18n.h). */
#include <string.h>

#include "ui_i18n.h"

static const char *const k_en[S_COUNT] = {
#define UI_STR_EN(id, en) [id] = en,
    UI_STRINGS(UI_STR_EN)
#undef UI_STR_EN
};

/* A translation: its code, its own name, and its table (NULL entries
 * fall back to English). Add one here with its ui_i18n_<code>.c. */
static const struct {
    const char        *code, *name;
    const char *const *table;
} k_langs[] = {
    { "en", "English", k_en },
};

#define N_LANGS (int)(sizeof k_langs / sizeof k_langs[0])

static int g_lang;

const char *ui_tr(ui_str_t id)
{
    if ((int)id < 0 || id >= S_COUNT) return "";
    const char *t = k_langs[g_lang].table[id];
    return t ? t : k_en[id];
}

const char *ui_i18n_code(int i) { return i >= 0 && i < N_LANGS ? k_langs[i].code : NULL; }
const char *ui_i18n_name(int i) { return i >= 0 && i < N_LANGS ? k_langs[i].name : NULL; }

void ui_i18n_set(const char *code)
{
    g_lang = 0;
    for (int i = 0; code && i < N_LANGS; ++i)
        if (!strcmp(k_langs[i].code, code)) g_lang = i;
}
