/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_theme.c — styles from the design tokens (see ui_theme.h). */
#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

lv_style_t ui_st_screen, ui_st_panel, ui_st_btn, ui_st_btn_hover, ui_st_btn_pressed,
    ui_st_btn_checked, ui_st_btn_disabled, ui_st_chip, ui_st_focus, ui_st_focus_in;
static lv_style_t st_accent, st_accent_hover;

static int g_init, g_focus_visible;

void ui_theme_focus_visible(int on)
{
    on = !!on;
    if (g_init && on == g_focus_visible) return;
    g_focus_visible = on;
    lv_style_reset(&ui_st_focus);
    lv_style_reset(&ui_st_focus_in);
    if (on) {
        /* Opaque: over video a blended pixel is a fringe in the key colour. */
        lv_style_set_outline_color(&ui_st_focus, UI_C(FOCUS));
        lv_style_set_outline_opa(&ui_st_focus, LV_OPA_COVER);
        lv_style_set_outline_width(&ui_st_focus, VG_SIZE_FOCUS_RING);
        lv_style_set_outline_pad(&ui_st_focus, VG_SIZE_FOCUS_GAP);
        lv_style_set_border_color(&ui_st_focus_in, UI_C(FOCUS));
        lv_style_set_border_opa(&ui_st_focus_in, LV_OPA_COVER);
        lv_style_set_border_width(&ui_st_focus_in, VG_SIZE_FOCUS_RING);
        lv_style_set_border_side(&ui_st_focus_in, LV_BORDER_SIDE_FULL);
    } else {
        /* LVGL's own theme marks a focused widget too: nothing, as here. */
        lv_style_set_outline_width(&ui_st_focus, 0);
    }
    if (g_init) {
        lv_obj_report_style_change(&ui_st_focus);
        lv_obj_report_style_change(&ui_st_focus_in);
    }
}

void ui_focus_style(lv_obj_t *o)
{
    lv_obj_add_style(o, &ui_st_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(o, &ui_st_focus, LV_STATE_FOCUS_KEY);
}

void ui_theme_init(lv_display_t *disp)
{
    lv_theme_t *th = lv_theme_default_init(disp, UI_C(ACCENT), UI_C(OK), 1, VG_FONT_MD_LV);
    lv_display_set_theme(disp, th);
    if (g_init) return;
    lv_style_init(&ui_st_focus);
    lv_style_init(&ui_st_focus_in);
    ui_theme_focus_visible(0);
    g_init = 1;

    lv_style_init(&ui_st_screen);
    lv_style_set_bg_color(&ui_st_screen, UI_C(BG));
    lv_style_set_bg_opa(&ui_st_screen, LV_OPA_COVER);
    lv_style_set_text_color(&ui_st_screen, UI_C(TEXT));
    lv_style_set_text_font(&ui_st_screen, VG_FONT_MD_LV);

    lv_style_init(&ui_st_panel);
    lv_style_set_bg_color(&ui_st_panel, UI_C(PANEL));
    lv_style_set_bg_opa(&ui_st_panel, LV_OPA_COVER);
    lv_style_set_border_color(&ui_st_panel, UI_C(LINE));
    lv_style_set_border_width(&ui_st_panel, 1);
    lv_style_set_radius(&ui_st_panel, VG_RADIUS_MD);
    lv_style_set_pad_all(&ui_st_panel, VG_SPACE_LG);
    lv_style_set_text_color(&ui_st_panel, UI_C(TEXT));

    lv_style_init(&ui_st_btn);
    lv_style_set_bg_color(&ui_st_btn, UI_C(PANEL2));
    lv_style_set_bg_opa(&ui_st_btn, LV_OPA_COVER);
    lv_style_set_radius(&ui_st_btn, VG_RADIUS_SM);
    lv_style_set_border_color(&ui_st_btn, UI_C(LINE));
    lv_style_set_border_width(&ui_st_btn, 1);
    lv_style_set_pad_hor(&ui_st_btn, VG_SPACE_MD);
    lv_style_set_pad_ver(&ui_st_btn, VG_SPACE_SM);
    lv_style_set_pad_column(&ui_st_btn, VG_SPACE_SM);
    lv_style_set_min_height(&ui_st_btn, VG_SIZE_BUTTON);
    lv_style_set_text_color(&ui_st_btn, UI_C(TEXT));
    lv_style_set_text_font(&ui_st_btn, VG_FONT_MD_LV);

    lv_style_init(&ui_st_btn_hover);
    lv_style_set_bg_color(&ui_st_btn_hover, UI_C(HOVER));
    lv_style_set_border_color(&ui_st_btn_hover, UI_C(ACCENT));

    lv_style_init(&ui_st_btn_pressed);
    lv_style_set_bg_color(&ui_st_btn_pressed, lv_color_darken(UI_C(ACCENT), LV_OPA_30));
    lv_style_set_border_color(&ui_st_btn_pressed, UI_C(ACCENT));

    lv_style_init(&ui_st_btn_checked);
    lv_style_set_bg_color(&ui_st_btn_checked, UI_C(ACCENT));
    lv_style_set_border_color(&ui_st_btn_checked, UI_C(ACCENT));
    lv_style_set_text_color(&ui_st_btn_checked, lv_color_white());

    lv_style_init(&st_accent);
    lv_style_set_bg_color(&st_accent, UI_C(ACCENT));
    lv_style_set_border_color(&st_accent, UI_C(ACCENT));
    lv_style_set_text_color(&st_accent, lv_color_white());
    lv_style_init(&st_accent_hover);
    lv_style_set_bg_color(&st_accent_hover, lv_color_lighten(UI_C(ACCENT), LV_OPA_20));
    lv_style_set_border_color(&st_accent_hover, lv_color_white());

    lv_style_init(&ui_st_btn_disabled);
    lv_style_set_bg_color(&ui_st_btn_disabled, UI_C(PANEL));
    lv_style_set_border_color(&ui_st_btn_disabled, UI_C(LINE));
    lv_style_set_text_color(&ui_st_btn_disabled, UI_C(TEXT2));

    lv_style_init(&ui_st_chip);
    lv_style_set_bg_color(&ui_st_chip, UI_C(CHIP));
    lv_style_set_bg_opa(&ui_st_chip, LV_OPA_COVER);
    /* Square: a rounded corner's soft edge over a tile would blend with
     * the key colour. */
    lv_style_set_radius(&ui_st_chip, 0);
    lv_style_set_pad_hor(&ui_st_chip, VG_SPACE_SM);
    lv_style_set_pad_ver(&ui_st_chip, VG_SPACE_XS);
    lv_style_set_pad_column(&ui_st_chip, VG_SPACE_SM);
    lv_style_set_text_color(&ui_st_chip, UI_C(TEXT));
    lv_style_set_text_font(&ui_st_chip, VG_FONT_SM_LV);
}

static void ring_room_cb(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, VG_SIZE_FOCUS_RING + VG_SIZE_FOCUS_GAP);
}

void ui_ring_room(lv_obj_t *o)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(o, ring_room_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(o);
}

void ui_plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

void ui_scrollbar(lv_obj_t *o)
{
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(o, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(o, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(o, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(o, UI_C(TEXT2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
}

lv_obj_t *ui_btn(lv_obj_t *parent, const char *symbol, const char *text)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &ui_st_btn, 0);
    lv_obj_add_style(b, &ui_st_btn_hover, LV_STATE_HOVERED);
    lv_obj_add_style(b, &ui_st_btn_checked, LV_STATE_CHECKED);
    lv_obj_add_style(b, &ui_st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(b, &ui_st_btn_disabled, LV_STATE_DISABLED);
    lv_obj_add_style(b, &ui_st_focus, LV_STATE_FOCUSED);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(b, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    char buf[96];
    if (symbol && text && *text)
        snprintf(buf, sizeof buf, "%s  %s", symbol, text);
    else
        snprintf(buf, sizeof buf, "%s", symbol ? symbol : text ? text : "");
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, buf);
    return b;
}

void ui_btn_accent(lv_obj_t *btn)
{
    lv_obj_add_style(btn, &st_accent, 0);
    lv_obj_add_style(btn, &st_accent_hover, LV_STATE_HOVERED);
    lv_obj_add_style(btn, &ui_st_btn_pressed, LV_STATE_PRESSED);
}

lv_obj_t *ui_btn_label(lv_obj_t *btn) { return lv_obj_get_child(btn, 0); }

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img, uint32_t rgb)
{
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, img);
    /* A8: the colour is the recolour */
    lv_obj_set_style_image_recolor(i, lv_color_hex(rgb), 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    return i;
}

/* ---- text over the video ------------------------------------------------------------- */

#define HARD_FONTS 6
static struct {
    const lv_font_t *base;
    lv_font_t        font;
} g_hard[HARD_FONTS];

/* Above this a glyph pixel is drawn, fully: 4-bpp anti-aliasing levels
 * cut at a little under half keep the strokes as thick as they look. */
#define HARD_CUT 0x70

static const void *hard_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    /* fmt_txt fills `buf` with A8 (whatever the font's bpp) and returns it. */
    lv_draw_buf_t *out = (lv_draw_buf_t *)lv_font_get_bitmap_fmt_txt(g, buf);
    if (!out) return NULL;
    uint32_t stride = out->header.stride;
    uint8_t *p = out->data;
    for (uint32_t y = 0; y < g->box_h; ++y)
        for (uint32_t x = 0; x < g->box_w; ++x) p[y * stride + x] = p[y * stride + x] >= HARD_CUT ? 0xFF : 0;
    return out;
}

const lv_font_t *ui_hard_font(const lv_font_t *base)
{
    int i;
    for (i = 0; i < HARD_FONTS && g_hard[i].base; ++i)
        if (g_hard[i].base == base) return &g_hard[i].font;
    if (i == HARD_FONTS) return base;
    g_hard[i].base = base;
    g_hard[i].font = *base;
    g_hard[i].font.get_glyph_bitmap = hard_bitmap;
    g_hard[i].font.fallback = NULL;
    return &g_hard[i].font;
}

/* Digits of equal width (tabular figures), for a clock: Montserrat's are
 * proportional ("1" narrower than "8"), so a clock that changes every
 * second also changes its width, and one placed by its right edge or its
 * centre moves sideways. Every digit here takes the widest one's advance,
 * its glyph centred in it, and nothing is kerned (a digit next to ':'
 * would be by a different amount). A copy of `base` (a hard font too)
 * with its own glyph description, so it serves every style. */
#define TNUM_FONTS 12
static struct {
    const lv_font_t *base;
    lv_font_t        font;
    uint16_t         adv;               /* the widest digit's */
} g_tnum[TNUM_FONTS];

static bool tnum_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t letter, uint32_t next)
{
    int i;
    for (i = 0; i < TNUM_FONTS && &g_tnum[i].font != font; ++i) {}
    if (i == TNUM_FONTS) return false;
    /* The base's own description of the glyph, from the copy's tables. */
    if (!g_tnum[i].base->get_glyph_dsc(font, g, letter, next)) return false;
    if (letter >= '0' && letter <= '9' && g->adv_w < g_tnum[i].adv) {
        g->ofs_x += (int16_t)((g_tnum[i].adv - g->adv_w) / 2);
        g->adv_w = g_tnum[i].adv;
    }
    return true;
}

const lv_font_t *ui_tnum_font(const lv_font_t *base)
{
    int i;
    for (i = 0; i < TNUM_FONTS && g_tnum[i].base; ++i)
        if (g_tnum[i].base == base || &g_tnum[i].font == base) return &g_tnum[i].font;
    if (i == TNUM_FONTS || !base->get_glyph_dsc) return base;
    uint16_t adv = 0;
    for (uint32_t d = '0'; d <= '9'; ++d) {
        lv_font_glyph_dsc_t g;
        memset(&g, 0, sizeof g);
        if (base->get_glyph_dsc(base, &g, d, 0) && g.adv_w > adv) adv = g.adv_w;
    }
    g_tnum[i].base = base;
    g_tnum[i].adv = adv;
    g_tnum[i].font = *base;
    g_tnum[i].font.get_glyph_dsc = tnum_glyph_dsc;
    g_tnum[i].font.kerning = LV_FONT_KERNING_NONE;
    return &g_tnum[i].font;
}

void ui_tnum(lv_obj_t *label)
{
    lv_obj_set_style_text_font(label, ui_tnum_font(lv_obj_get_style_text_font(label, LV_PART_MAIN)), 0);
}

/* Eight dark copies a pixel away in each direction, drawn before the
 * label draws its text: one object, not nine (64 tiles of them would not
 * fit LVGL's heap). */
static const signed char k_around[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 },
                                            { 1, 0 },   { -1, 1 }, { 0, 1 },  { 1, 1 } };

static void outline_draw_cb(lv_event_t *e)
{
    lv_obj_t *l = lv_event_get_current_target(e);
    const char *text = lv_label_get_text(l);
    if (!text || !*text) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    lv_obj_init_draw_label_dsc(l, LV_PART_MAIN, &d);
    d.color = UI_C(OUTLINE);
    d.text = text;
    if (lv_label_get_long_mode(l) == LV_LABEL_LONG_WRAP) d.flag |= LV_TEXT_FLAG_NONE;
    else d.flag |= LV_TEXT_FLAG_EXPAND;
    lv_area_t a;
    lv_obj_get_content_coords(l, &a);
    for (int i = 0; i < 8; ++i) {
        lv_area_t b = a;
        lv_area_move(&b, k_around[i][0], k_around[i][1]);
        lv_draw_label(layer, &d, &b);
    }
}

static void outline_ext_cb(lv_event_t *e) { lv_event_set_ext_draw_size(e, 1); }

lv_obj_t *ui_outlined(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb)
{
    return ui_osd_text(parent, font, rgb, UI_OSD_OUTLINE);
}

lv_obj_t *ui_osd_text(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb, ui_osd_bg_t bg)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (bg == UI_OSD_BOX) {
        /* On its own opaque chip the text keeps its soft edges: they
         * blend with the chip, never with the key. */
        lv_obj_add_style(l, &ui_st_chip, 0);
        lv_obj_set_style_text_font(l, font, 0);
        return l;
    }
    lv_obj_set_style_text_font(l, ui_hard_font(font), 0);
    if (bg == UI_OSD_OUTLINE) {
        lv_obj_add_event_cb(l, outline_draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        lv_obj_add_event_cb(l, outline_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_refresh_ext_draw_size(l);
    }
    return l;
}

/* The icons with hard edges, as the hard fonts: every pixel opaque or not
 * drawn. Made once per icon. */
#define HARD_ICONS 8
static struct {
    const lv_image_dsc_t *base;
    uint8_t              *px;
    lv_image_dsc_t        img;
} g_hard_icon[HARD_ICONS];

static const lv_image_dsc_t *hard_icon(const lv_image_dsc_t *base)
{
    int i;
    for (i = 0; i < HARD_ICONS && g_hard_icon[i].base; ++i)
        if (g_hard_icon[i].base == base) return &g_hard_icon[i].img;
    if (i == HARD_ICONS || base->header.cf != LV_COLOR_FORMAT_A8) return base;
    uint32_t w = base->header.w, h = base->header.h, stride = base->header.stride ? base->header.stride : w;
    if (!(g_hard_icon[i].px = lv_malloc((size_t)stride * h))) return base;
    for (uint32_t k = 0; k < stride * h; ++k) g_hard_icon[i].px[k] = base->data[k] >= HARD_CUT ? 0xFF : 0;
    g_hard_icon[i].base = base;
    g_hard_icon[i].img = *base;
    g_hard_icon[i].img.data = g_hard_icon[i].px;
    g_hard_icon[i].img.data_size = stride * h;
    return &g_hard_icon[i].img;
}

static void icon_outline_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_current_target(e);
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = lv_image_get_src(o);
    d.recolor = UI_C(OUTLINE);
    d.recolor_opa = LV_OPA_COVER;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    for (int i = 0; i < 8; ++i) {
        lv_area_t b = a;
        lv_area_move(&b, k_around[i][0], k_around[i][1]);
        lv_draw_image(lv_event_get_layer(e), &d, &b);
    }
}

lv_obj_t *ui_osd_icon(lv_obj_t *parent, const lv_image_dsc_t *img, uint32_t rgb, ui_osd_bg_t bg)
{
    if (bg == UI_OSD_BOX) return ui_icon(parent, img, rgb);
    lv_obj_t *i = ui_icon(parent, hard_icon(img), rgb);
    lv_obj_set_user_data(i, (void *)img);
    if (bg == UI_OSD_OUTLINE) {
        lv_obj_add_event_cb(i, icon_outline_draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        lv_obj_add_event_cb(i, outline_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_refresh_ext_draw_size(i);
    }
    return i;
}

lv_obj_t *ui_outlined_label(lv_obj_t *o) { return o; }

void ui_outlined_set_text(lv_obj_t *o, const char *text)
{
    if (strcmp(lv_label_get_text(o), text)) lv_label_set_text(o, text);
}

void ui_outlined_set_color(lv_obj_t *o, lv_color_t c)
{
    lv_obj_set_style_text_color(o, c, 0);
}

/* Discs with hard edges, made once per size. */
#define DISCS_MAX 4
static struct {
    int            d;
    uint8_t       *px;
    lv_image_dsc_t img;
} g_discs[DISCS_MAX];

static const lv_image_dsc_t *disc_image(int d)
{
    int i;
    for (i = 0; i < DISCS_MAX && g_discs[i].d; ++i)
        if (g_discs[i].d == d) return &g_discs[i].img;
    if (i == DISCS_MAX || !(g_discs[i].px = lv_malloc((size_t)d * (size_t)d))) return NULL;
    /* Opaque where the pixel's centre is inside the circle, else nothing. */
    long r2 = (long)d * d;
    for (int y = 0; y < d; ++y)
        for (int x = 0; x < d; ++x) {
            long dx = 2 * x + 1 - d, dy = 2 * y + 1 - d;
            g_discs[i].px[y * d + x] = dx * dx + dy * dy <= r2 ? 0xFF : 0;
        }
    g_discs[i].d = d;
    lv_image_dsc_t *img = &g_discs[i].img;
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_A8;
    img->header.w = (uint32_t)d;
    img->header.h = (uint32_t)d;
    img->header.stride = (uint32_t)d;
    img->data = g_discs[i].px;
    img->data_size = (uint32_t)(d * d);
    return img;
}

static void disc_hover_cb(lv_event_t *e)
{
    lv_obj_t *disc = lv_event_get_user_data(e);
    int over = lv_event_get_code(e) == LV_EVENT_HOVER_OVER;
    lv_obj_set_style_image_recolor(disc, over ? UI_C(ACCENT) : UI_C(CHIP), 0);
}

lv_obj_t *ui_disc_button(lv_obj_t *parent, int d)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    const lv_image_dsc_t *img = disc_image(d);
    if (img) {
        lv_obj_t *disc = ui_icon(b, img, VG_COLOR_CHIP);
        lv_obj_center(disc);
        lv_obj_add_event_cb(b, disc_hover_cb, LV_EVENT_HOVER_OVER, disc);
        lv_obj_add_event_cb(b, disc_hover_cb, LV_EVENT_HOVER_LEAVE, disc);
    }
    return b;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}
