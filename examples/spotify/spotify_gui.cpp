#include "spotify_gui.h"
#include "Arduino.h"
#include "lvgl.h"
#include "pin_config.h"
#include "time.h"

#define COLOR_BG      lv_color_black()
#define COLOR_CARD    lv_color_hex(0x282828)
#define COLOR_GREEN   lv_color_hex(0x1DB954)
#define COLOR_DIM     lv_color_hex(0x0B4D23)
#define COLOR_TEXT    lv_color_white()
#define COLOR_SUBTEXT lv_color_hex(0xB3B3B3)
#define COLOR_TRACK   lv_color_hex(0x535353)
#define COLOR_ALERT   lv_color_hex(0xF15E6C)

#define MARGIN    10
#define COL_X     (MARGIN + SPOTIFY_ART_SIZE + MARGIN)
#define COL_W     (EXAMPLE_LCD_H_RES - COL_X - MARGIN)
#define COL_MID   (COL_X + COL_W / 2 - EXAMPLE_LCD_H_RES / 2) /* column center, as offset from screen center */
#define VIZ_Y     76
#define VIZ_H     48
#define VIZ_BARS  20
#define VIZ_BAR_W 5
#define ROW_Y     140
#define NOTICE_MS 4000

static lv_obj_t *art_img, *header_label, *notice_label, *clock_label, *title_label, *artist_label, *viz;
static lv_obj_t *progress_bar, *elapsed_label, *duration_label, *prev_icon, *play_icon, *next_icon;
static lv_img_dsc_t art_dsc;
static spotify_state_t state; /* latest copy from the Spotify task */
static uint32_t art_version;
static uint32_t notice_version;
static uint32_t notice_until;
static float bar_level[VIZ_BARS];
static float bar_target[VIZ_BARS];

static void set_text(lv_obj_t *label, const char *text)
{
    /* Re-setting the same text would restart the scroll animation */
    if (strcmp(lv_label_get_text(label), text) != 0)
        lv_label_set_text(label, text);
}

static void set_visible(lv_obj_t *obj, bool visible)
{
    if (visible)
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void format_time(char *buf, size_t size, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(buf, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(buf, size, "%u:%02u", s / 60, s % 60);
}

static lv_obj_t *make_label(const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    if (w > 0) {
        lv_obj_set_width(label, w);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *make_icon(const char *symbol, lv_coord_t dx)
{
    lv_obj_t *icon = make_label(&lv_font_montserrat_16, COLOR_TEXT, 0, 0, 0);
    lv_label_set_text(icon, symbol);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, COL_MID + dx, ROW_Y - 2);
    return icon;
}

/* ---------------------------------------------------------------- visualizer */

/* Spotify no longer exposes audio analysis to new apps, so the bars are a stylised animation:
 * random levels shaped like a spectrum, plus a ~120 BPM pulse on the low end. */
static void viz_timer_cb(lv_timer_t *t)
{
    bool playing = state.status == SPOTIFY_ACTIVE && state.is_playing;
    float beat = 1.0f - (float)(millis() % 500) / 500.0f;
    beat *= beat;
    bool moving = false;
    for (int i = 0; i < VIZ_BARS; i++) {
        float pos = (float)i / (VIZ_BARS - 1); /* 0 = bass, 1 = treble */
        if (playing && random(100) < 35)
            bar_target[i] = (0.85f - 0.45f * pos) * (0.3f + 0.7f * random(1000) / 1000.0f);
        float target = playing ? LV_MIN(1.0f, bar_target[i] + 0.35f * (1.0f - pos) * beat) : 0.0f;
        float speed = target > bar_level[i] ? 0.6f : 0.15f; /* fast attack, slow release */
        float next = bar_level[i] + (target - bar_level[i]) * speed;
        moving |= fabsf(next - bar_level[i]) > 0.002f;
        bar_level[i] = next;
    }
    if (moving)
        lv_obj_invalidate(viz);
}

static void viz_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    bool playing = state.status == SPOTIFY_ACTIVE && state.is_playing;
    lv_coord_t gap = (lv_obj_get_width(obj) - VIZ_BARS * VIZ_BAR_W) / (VIZ_BARS - 1);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 2;
    for (int i = 0; i < VIZ_BARS; i++) {
        lv_coord_t h = LV_MAX(3, (lv_coord_t)(bar_level[i] * VIZ_H));
        dsc.bg_color = playing ? lv_color_mix(COLOR_GREEN, COLOR_DIM, (uint8_t)(80 + bar_level[i] * 175)) : COLOR_TRACK;
        lv_area_t a;
        a.x1 = obj->coords.x1 + i * (VIZ_BAR_W + gap);
        a.x2 = a.x1 + VIZ_BAR_W - 1;
        a.y2 = obj->coords.y2;
        a.y1 = a.y2 - h + 1;
        lv_draw_rect(draw_ctx, &dsc, &a);
    }
}

/* ---------------------------------------------------------------- updates */

static void apply_state(void)
{
    bool active = state.status == SPOTIFY_ACTIVE;
    bool alert = state.status == SPOTIFY_ERROR || state.status == SPOTIFY_LOGIN_NEEDED || state.status == SPOTIFY_NOT_CONFIGURED;

    set_text(title_label, state.title);
    set_text(artist_label, state.artist);
    lv_obj_set_style_text_color(title_label, alert ? COLOR_ALERT : COLOR_TEXT, 0);

    /* "Spotify" attribution plus the device that is playing; '#' would break the recolor syntax */
    char header[80] = "#1DB954 Spotify#";
    if (active && state.device[0]) {
        size_t n = strlen(header);
        snprintf(header + n, sizeof(header) - n, "  %s", state.device);
        for (char *c = header + n; *c; c++)
            if (*c == '#')
                *c = ' ';
    }
    set_text(header_label, header);

    if (state.notice_version != notice_version) {
        notice_version = state.notice_version;
        notice_until = millis() + NOTICE_MS;
        lv_label_set_text(notice_label, state.notice);
        set_visible(notice_label, true);
    }

    set_visible(progress_bar, active);
    set_visible(elapsed_label, active);
    set_visible(duration_label, active);
    bool controls = active || state.status == SPOTIFY_IDLE;
    set_visible(prev_icon, controls);
    set_visible(play_icon, controls);
    set_visible(next_icon, controls);
    set_text(play_icon, state.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    char buf[16];
    format_time(buf, sizeof(buf), state.duration_ms);
    set_text(duration_label, buf);

    if (spotify_get_art((lv_color_t *)art_dsc.data, &art_version)) {
        lv_img_cache_invalidate_src(&art_dsc);
        lv_obj_invalidate(art_img);
    }
    set_visible(art_img, state.has_art);
    lv_obj_invalidate(viz);
}

static void update_progress(void)
{
    if (state.status != SPOTIFY_ACTIVE)
        return;
    uint32_t p = state.progress_ms + (state.is_playing ? millis() - state.sampled_at : 0);
    p = LV_MIN(p, state.duration_ms);
    lv_bar_set_value(progress_bar, state.duration_ms ? (int32_t)((uint64_t)p * 1000 / state.duration_ms) : 0, LV_ANIM_OFF);
    char buf[16];
    format_time(buf, sizeof(buf), p);
    set_text(elapsed_label, buf);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    if (spotify_get_state(&state))
        apply_state();
    update_progress();
    if (!lv_obj_has_flag(notice_label, LV_OBJ_FLAG_HIDDEN) && (int32_t)(millis() - notice_until) >= 0)
        set_visible(notice_label, false);
}

static void clock_timer_cb(lv_timer_t *t)
{
    struct tm now;
    if (getLocalTime(&now, 0)) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", now.tm_hour, now.tm_min);
        set_text(clock_label, buf);
    }
}

static void restore_icons_cb(lv_timer_t *t)
{
    lv_obj_set_style_text_color(prev_icon, COLOR_TEXT, 0);
    lv_obj_set_style_text_color(play_icon, COLOR_TEXT, 0);
    lv_obj_set_style_text_color(next_icon, COLOR_TEXT, 0);
}

void ui_button_feedback(spotify_cmd_t cmd)
{
    lv_obj_t *icon = cmd == SPOTIFY_CMD_PREVIOUS ? prev_icon : cmd == SPOTIFY_CMD_NEXT ? next_icon : play_icon;
    lv_obj_set_style_text_color(icon, COLOR_GREEN, 0);
    lv_timer_t *t = lv_timer_create(restore_icons_cb, 250, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ---------------------------------------------------------------- layout */

void ui_begin(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Album art, with a note icon underneath while there is none */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SPOTIFY_ART_SIZE, SPOTIFY_ART_SIZE);
    lv_obj_set_pos(card, MARGIN, (EXAMPLE_LCD_V_RES - SPOTIFY_ART_SIZE) / 2);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_clip_corner(card, true, 0);

    lv_obj_t *note = lv_label_create(card);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(note, COLOR_TRACK, 0);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_center(note);

    art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    art_dsc.header.w = SPOTIFY_ART_SIZE;
    art_dsc.header.h = SPOTIFY_ART_SIZE;
    art_dsc.data_size = SPOTIFY_ART_SIZE * SPOTIFY_ART_SIZE * sizeof(lv_color_t);
    art_dsc.data = (const uint8_t *)ps_malloc(art_dsc.data_size);
    art_img = lv_img_create(card);
    lv_img_set_src(art_img, &art_dsc);
    lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);

    /* Header: Spotify attribution + device, clock on the right */
    header_label = make_label(&lv_font_montserrat_12, COLOR_SUBTEXT, COL_X, 8, COL_W - 40);
    lv_label_set_long_mode(header_label, LV_LABEL_LONG_DOT);
    lv_label_set_recolor(header_label, true);
    clock_label = make_label(&lv_font_montserrat_12, COLOR_SUBTEXT, 0, 0, 0);
    lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -MARGIN, 8);
    notice_label = make_label(&lv_font_montserrat_12, COLOR_ALERT, COL_X, 8, COL_W);
    lv_obj_set_style_bg_color(notice_label, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(notice_label, LV_OPA_COVER, 0);
    lv_obj_add_flag(notice_label, LV_OBJ_FLAG_HIDDEN);

    title_label = make_label(&lv_font_montserrat_20, COLOR_TEXT, COL_X, 26, COL_W);
    artist_label = make_label(&lv_font_montserrat_16, COLOR_SUBTEXT, COL_X, 52, COL_W);

    viz = lv_obj_create(scr);
    lv_obj_remove_style_all(viz);
    lv_obj_set_size(viz, COL_W, VIZ_H);
    lv_obj_set_pos(viz, COL_X, VIZ_Y);
    lv_obj_add_event_cb(viz, viz_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    progress_bar = lv_bar_create(scr);
    lv_obj_set_size(progress_bar, COL_W, 4);
    lv_obj_set_pos(progress_bar, COL_X, ROW_Y - 10);
    lv_bar_set_range(progress_bar, 0, 1000);
    lv_obj_set_style_bg_color(progress_bar, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, COLOR_TEXT, LV_PART_INDICATOR);

    elapsed_label = make_label(&lv_font_montserrat_12, COLOR_SUBTEXT, COL_X, ROW_Y, 0);
    duration_label = make_label(&lv_font_montserrat_12, COLOR_SUBTEXT, 0, 0, 0);
    lv_obj_align(duration_label, LV_ALIGN_TOP_RIGHT, -MARGIN, ROW_Y);
    prev_icon = make_icon(LV_SYMBOL_PREV, -26);
    play_icon = make_icon(LV_SYMBOL_PLAY, 0);
    next_icon = make_icon(LV_SYMBOL_NEXT, 26);

    lv_timer_create(refresh_timer_cb, 100, NULL);
    lv_timer_create(viz_timer_cb, 40, NULL);
    lv_timer_create(clock_timer_cb, 1000, NULL);
    refresh_timer_cb(NULL);
}
