/*
 *  Monterro — Activity app
 *  Target : LILYGO T-Watch 2020 V1/V2/V3 · LVGL 7.11.1
 *
 *  Based on the working original (activity_v5). Changes applied on top:
 *  ───────────────────────────────────────────────────────────────────────
 *  A. Pace threshold 2 m (kept from original)
 *  B. Display task UI-only, telemetry task separate (kept from original)
 *  C. Stop handler reads live pedometer value (kept from original)
 *  D. WiFi fallback wakes WiFi and POSTs /live (kept from original)
 *  E. BLE primary, WiFi only when BLE off (kept from original)
 *  F. SPIFFS persistence — save every session to flash on the watch
 *  G. Ring-buffer shift always runs (was broken: only ran when full)
 *  H. History guard: save if dur > 0 (not steps > 0, handles BMA warmup)
 *  I. post_session_end() — WiFi notifies Pi at session end
 *  J. History display: buf[1024], skip zero-step entries, show duration
 *  K. SPIFFS.begin(true) — always works, formats on fresh flash
 */

#include "config.h"
#include "quickglui/quickglui.h"
#include "activity.h"
#include "gui/mainbar/mainbar.h"
#include "gui/widget_styles.h"
#include "gui/app.h"
#include "hardware/motion.h"
#include "hardware/ble/blestepctl.h"
#include "hardware/motor.h"
#include "hardware/wifictl.h"
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <time.h>

#ifdef NATIVE_64BIT
#else
    #ifdef M5PAPER
    #elif defined(LILYGO_WATCH_2020_V1) || \
          defined(LILYGO_WATCH_2020_V2) || \
          defined(LILYGO_WATCH_2020_V3)
        #include <TTGO.h>
    #endif
#endif

// Swap back to monterro_64px once monterro_64px.c is in the build
LV_IMG_DECLARE(monterro_64px);
#define APP_ICON  move_64px

LV_FONT_DECLARE(Ubuntu_16px);
LV_FONT_DECLARE(Ubuntu_32px);

#define YES "Yes"
#define NO  "No"

// ── Pi connection ─────────────────────────────────────────────────────────────
#define PI_PORT  5000

// ── Palette ───────────────────────────────────────────────────────────────────
#define COL_BG        LV_COLOR_MAKE(0xF4,0xF5,0xF7)
#define COL_ARC_BG    LV_COLOR_MAKE(0xD8,0xDA,0xE0)
#define COL_TXT       LV_COLOR_MAKE(0x1A,0x1A,0x2E)
#define COL_MUTED     LV_COLOR_MAKE(0x88,0x88,0x99)
#define COL_DIST      LV_COLOR_MAKE(0x3D,0x8E,0xF0)
#define COL_STEPS     LV_COLOR_MAKE(0xE0,0x44,0x55)
#define COL_KCAL      LV_COLOR_MAKE(0x22,0xBB,0x7A)
#define COL_PACE      LV_COLOR_MAKE(0xA0,0x60,0xF8)
#define COL_BTN       LV_COLOR_MAKE(0xE2,0xE4,0xE8)
#define COL_BTN_PR    LV_COLOR_MAKE(0xC8,0xCA,0xCF)
#define COL_BTN_GO    LV_COLOR_MAKE(0x1A,0x1A,0x2E)
#define COL_BTN_GO_PR LV_COLOR_MAKE(0x33,0x33,0x55)
#define COL_SEP       LV_COLOR_MAKE(0xCC,0xCE,0xD4)

// ── SPIFFS log ────────────────────────────────────────────────────────────────
#define HIKE_LOG "/hike_log.jsonl"

// ── History ───────────────────────────────────────────────────────────────────
#define HISTORY_MAX 10
struct SessionRecord {
    uint32_t steps, dist_m, dur_s, kcal, pace_sec_per_km;
};
static SessionRecord history[HISTORY_MAX];
static uint8_t       history_count = 0;

// ── Session ───────────────────────────────────────────────────────────────────
static bool     session_running     = false;
static uint32_t session_start_ms    = 0;
static uint32_t session_start_steps = 0;
static uint32_t session_steps       = 0;
static uint32_t session_distance_m  = 0;
static uint32_t session_duration_s  = 0;

// ── Config ────────────────────────────────────────────────────────────────────
static SynchronizedApplication activityApp;
static JsonConfig config("activity.json");
static String cfg_len, cfg_goal_stp, cfg_goal_dist;

// ── Task guard ────────────────────────────────────────────────────────────────
static bool tile_active = false;

// ── SPIFFS flag ───────────────────────────────────────────────────────────────
static bool spiffs_ok = false;

// ── LVGL handles ──────────────────────────────────────────────────────────────
static lv_obj_t *arc_steps     = nullptr;
static lv_obj_t *arc_dist      = nullptr;
static lv_obj_t *arc_kcal      = nullptr;
static lv_obj_t *lbl_steps_val = nullptr;
static lv_obj_t *lbl_dist_val  = nullptr;
static lv_obj_t *lbl_kcal_val  = nullptr;
static lv_obj_t *lbl_dur_val   = nullptr;
static lv_obj_t *lbl_pace_val  = nullptr;
static lv_obj_t *lbl_startstop = nullptr;
static lv_obj_t *lbl_hist_list = nullptr;

// ── Styles ────────────────────────────────────────────────────────────────────
static lv_style_t sty_bg, sty_arc_bg;
static lv_style_t sty_arc_dist, sty_arc_steps, sty_arc_kcal;
static lv_style_t sty_val, sty_hdr, sty_timer, sty_muted;
static lv_style_t sty_pace, sty_cdist, sty_csteps, sty_ckcal;
static lv_style_t sty_btn, sty_btn_go, sty_sep;

// ── Render cache ──────────────────────────────────────────────────────────────
static uint32_t last_stp  = 0xFFFFFFFF;
static uint32_t last_dist = 0xFFFFFFFF;
static uint32_t last_dur  = 0xFFFFFFFF;
static uint32_t last_kcal = 0xFFFFFFFF;
static uint32_t last_pace = 0xFFFFFFFF;

static lv_event_cb_t default_msgbox_cb;

// ── Telemetry helpers ─────────────────────────────────────────────────────────
static IPAddress cachedPiAddr;
static uint32_t  cachedPiAddrMs = 0;
static const uint32_t PI_ADDR_CACHE_MS = 30000;

// ── Prototypes ────────────────────────────────────────────────────────────────
static void      build_main_page();
static void      build_history_page();
static void      refresh_main_page();
static void      refresh_history_page();
static void      build_settings();
static void      activity_activate_cb();
static void      task_display_cb(lv_task_t *t);
static void      task_telem_cb(lv_task_t *t);
static void      activity_reset_cb(lv_obj_t *obj, lv_event_t event);
static void      btn_startstop_cb(lv_obj_t *obj, lv_event_t event);
static void      btn_reset_cb(lv_obj_t *obj, lv_event_t event);
static void      btn_history_cb(lv_obj_t *obj, lv_event_t event);
static void      btn_back_cb(lv_obj_t *obj, lv_event_t event);
static void      cache_invalidate();
static uint32_t  get_step_len_cm();
static uint32_t  calc_pace(uint32_t dur_s, uint32_t dist_m);
static IPAddress resolve_pi_addr();
static bool      post_live_update(uint32_t stp, uint32_t dist, uint32_t dur);
static bool      post_session_end(uint32_t stp, uint32_t dist, uint32_t dur);
static void      spiffs_save_session(uint32_t steps, uint32_t dist, uint32_t dur);
static void      spiffs_load_history();

static int registed __attribute__((used)) =
    app_autocall_function(&activity_app_setup, 8);

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void cache_invalidate() {
    last_stp = last_dist = last_dur = last_kcal = last_pace = 0xFFFFFFFF;
}

static uint32_t get_step_len_cm() {
    uint32_t v = (uint32_t)atoi(cfg_len.c_str());
    return (v == 0) ? 50 : v;
}

static uint32_t calc_pace(uint32_t dur_s, uint32_t dist_m) {
    if (dist_m < 2 || dur_s == 0) return 0;
    return (dur_s * 1000u) / dist_m;
}

static void arc_set(lv_obj_t *arc, uint32_t val, uint32_t goal) {
    if (!arc || goal == 0) return;
    int32_t d = (int32_t)((val * 360u) / goal);
    lv_arc_set_value(arc, (int16_t)(d > 360 ? 360 : d));
}

static void tile_opaque(lv_obj_t *t) {
    lv_obj_reset_style_list(t, LV_OBJ_PART_MAIN);
    lv_obj_add_style(t, LV_OBJ_PART_MAIN, &sty_bg);
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi telemetry (original logic kept — wifictl_get_event compiles here)
// ─────────────────────────────────────────────────────────────────────────────

static IPAddress resolve_pi_addr() {
    if ((uint32_t)cachedPiAddr != 0 &&
        (millis() - cachedPiAddrMs) < PI_ADDR_CACHE_MS) {
        return cachedPiAddr;
    }
    IPAddress piAddr = MDNS.queryHost("hiking-pi");
    cachedPiAddr   = piAddr;
    cachedPiAddrMs = millis();
    if ((uint32_t)piAddr == 0)
        log_w("[monterro] mDNS: hiking-pi.local not found");
    else
        log_i("[monterro] hiking-pi -> %s", piAddr.toString().c_str());
    return piAddr;
}

static bool post_live_update(uint32_t stp, uint32_t dist, uint32_t dur) {
    IPAddress piAddr = resolve_pi_addr();
    if ((uint32_t)piAddr == 0) return false;

    char url[72];
    snprintf(url, sizeof(url),
             "http://%s:%d/live", piAddr.toString().c_str(), PI_PORT);
    char body[96];
    snprintf(body, sizeof(body),
             "{\"steps\":%lu,\"distance\":%lu,\"duration\":%lu}",
             (unsigned long)stp, (unsigned long)dist, (unsigned long)dur);

    HTTPClient http;
    http.setTimeout(2000);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t*)body, strlen(body));
    http.end();
    log_i("[monterro] POST /live -> %d", code);
    return (code >= 200 && code < 300);
}

// FIX I — notify Pi at session end so session_history.json is written
// even when BLE was never connected during the hike.
static bool post_session_end(uint32_t stp, uint32_t dist, uint32_t dur) {
    IPAddress piAddr = resolve_pi_addr();
    if ((uint32_t)piAddr == 0) return false;

    char url[80];
    snprintf(url, sizeof(url),
             "http://%s:%d/session-end", piAddr.toString().c_str(), PI_PORT);
    char body[96];
    snprintf(body, sizeof(body),
             "{\"steps\":%lu,\"distance\":%lu,\"duration\":%lu}",
             (unsigned long)stp, (unsigned long)dist, (unsigned long)dur);

    HTTPClient http;
    http.setTimeout(3000);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t*)body, strlen(body));
    http.end();
    log_i("[monterro] POST /session-end -> %d", code);
    return (code >= 200 && code < 300);
}

// ─────────────────────────────────────────────────────────────────────────────
// SPIFFS persistence
// FIX F — saves every session to flash regardless of WiFi/BLE
// FIX G — ring-buffer shift always runs (not only when array is full)
// ─────────────────────────────────────────────────────────────────────────────

static void spiffs_save_session(uint32_t steps, uint32_t dist, uint32_t dur) {
    // ── RAM ring-buffer ───────────────────────────────────────────────────────
    // FIX G: shift always runs — previously only ran when history_count==MAX,
    // causing slots 1-9 to silently overwrite history[0] every time.
    const uint8_t used = (history_count < HISTORY_MAX)
                         ? history_count : HISTORY_MAX - 1;
    for (int i = used; i > 0; i--)
        history[i] = history[i - 1];

    history[0].steps           = steps;
    history[0].dist_m          = dist;
    history[0].dur_s           = dur;
    history[0].kcal            = steps * 4 / 100;
    history[0].pace_sec_per_km = calc_pace(dur, dist);

    if (history_count < HISTORY_MAX) history_count++;

    log_i("[monterro] RAM saved: stp=%lu dst=%lu dur=%lu count=%d",
          (unsigned long)steps, (unsigned long)dist,
          (unsigned long)dur, (int)history_count);

    // ── SPIFFS append ─────────────────────────────────────────────────────────
    if (!spiffs_ok) {
        log_w("[monterro] SPIFFS not ready — RAM only");
        return;
    }

    File f = SPIFFS.open(HIKE_LOG, FILE_APPEND);
    if (!f) {
        log_w("[monterro] SPIFFS open failed");
        return;
    }

    time_t now;
    time(&now);
    char line[128];
    snprintf(line, sizeof(line),
        "{\"ts\":%ld,\"stp\":%lu,\"dst\":%lu,\"dur\":%lu,\"kcal\":%lu}\n",
        (long)now,
        (unsigned long)steps,
        (unsigned long)dist,
        (unsigned long)dur,
        (unsigned long)(steps * 4 / 100));
    f.print(line);
    f.close();
    log_i("[monterro] SPIFFS saved: %s", line);
}

static void spiffs_load_history() {
    if (!spiffs_ok) return;

    if (!SPIFFS.exists(HIKE_LOG)) {
        log_i("[monterro] No hike log, starting fresh");
        return;
    }

    File f = SPIFFS.open(HIKE_LOG, FILE_READ);
    if (!f) return;

    static char lines[HISTORY_MAX][128];
    int total = 0;

    while (f.available()) {
        String s = f.readStringUntil('\n');
        s.trim();
        if (s.length() == 0) continue;
        if (total < HISTORY_MAX) {
            strncpy(lines[total], s.c_str(), 127);
            lines[total][127] = '\0';
            total++;
        } else {
            for (int i = 0; i < HISTORY_MAX - 1; i++)
                memcpy(lines[i], lines[i + 1], 128);
            strncpy(lines[HISTORY_MAX - 1], s.c_str(), 127);
            lines[HISTORY_MAX - 1][127] = '\0';
        }
    }
    f.close();

    history_count = 0;
    for (int i = total - 1; i >= 0 && history_count < HISTORY_MAX; i--) {
        // Use unsigned long to match %lu — avoids -Wformat warnings on ESP32
        unsigned long stp = 0, dst = 0, dur = 0, kcal = 0;
        long ts = 0;
        int matched = sscanf(lines[i],
            "{\"ts\":%ld,\"stp\":%lu,\"dst\":%lu,\"dur\":%lu,\"kcal\":%lu}",
            &ts, &stp, &dst, &dur, &kcal);
        if (matched < 4 || dur == 0) continue;
        history[history_count].steps           = (uint32_t)stp;
        history[history_count].dist_m          = (uint32_t)dst;
        history[history_count].dur_s           = (uint32_t)dur;
        history[history_count].kcal            = (uint32_t)kcal;
        history[history_count].pace_sec_per_km = calc_pace((uint32_t)dur, (uint32_t)dst);
        history_count++;
    }
    log_i("[monterro] Loaded %d session(s) from SPIFFS", history_count);
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────

void activity_app_setup() {
    wifictl_on();
    activityApp.init("Monterro", &monterro_64px, 1, 2);
    mainbar_add_tile_activate_cb(activityApp.mainTileId(), activity_activate_cb);
    build_main_page();
    build_history_page();
    build_settings();

    // FIX K — SPIFFS.begin(true): formats on first boot, no-op if already mounted.
    // This is exactly what the original did inline — safe and always works.
    spiffs_ok = SPIFFS.begin(true);
    if (!spiffs_ok)
        log_e("[monterro] SPIFFS mount failed — history will not persist");

    spiffs_load_history();

    lv_task_create(task_display_cb, 1000, LV_TASK_PRIO_LOW, nullptr);
    lv_task_create(task_telem_cb,   5000, LV_TASK_PRIO_LOW, nullptr);

    refresh_main_page();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tasks (original logic kept verbatim — compiles cleanly)
// ─────────────────────────────────────────────────────────────────────────────

static void task_display_cb(lv_task_t *t) {
    (void)t;
    if (!tile_active || !lbl_dur_val) return;
    refresh_main_page();
}

static void task_telem_cb(lv_task_t *t) {
    (void)t;
    if (!session_running) return;

    const uint32_t stp  = session_steps;
    const uint32_t dist = session_distance_m;
    const uint32_t dur  = session_duration_s;

    // BLE primary path
    if (blectl_get_event(BLECTL_ON)) {
        blestepctl_set_hike(stp, dist, dur);
        blestepctl_update_hike(false);
        return;
    }

    // WiFi fallback — wake WiFi if not already up, then POST on next tick
    if (!wifictl_get_event(WIFICTL_CONNECT)) {
        if (!wifictl_get_event(WIFICTL_ON_REQUEST) &&
            !wifictl_get_event(WIFICTL_SCAN) &&
            !wifictl_get_event(WIFICTL_ON)) {
            log_i("[monterro] BLE off, waking WiFi");
            wifictl_on();
        }
        return;
    }

    post_live_update(stp, dist, dur);
}

static void activity_activate_cb() {
    tile_active = true;
    cache_invalidate();
    refresh_main_page();
}

// ─────────────────────────────────────────────────────────────────────────────
// Build main tile
// ─────────────────────────────────────────────────────────────────────────────

void build_main_page() {
    lv_obj_t *tile = mainbar_get_tile_obj(activityApp.mainTileId());

    lv_style_init(&sty_bg);
    lv_style_set_bg_color(&sty_bg,     LV_STATE_DEFAULT, COL_BG);
    lv_style_set_bg_opa(&sty_bg,       LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_border_width(&sty_bg, LV_STATE_DEFAULT, 0);
    lv_style_set_pad_all(&sty_bg,      LV_STATE_DEFAULT, 0);
    tile_opaque(tile);

#define INIT_ARC_STY(s,c) \
    lv_style_init(&s); \
    lv_style_set_line_color(&s, LV_STATE_DEFAULT, c); \
    lv_style_set_line_width(&s, LV_STATE_DEFAULT, 6);
    INIT_ARC_STY(sty_arc_bg,    COL_ARC_BG)
    INIT_ARC_STY(sty_arc_dist,  COL_DIST)
    INIT_ARC_STY(sty_arc_steps, COL_STEPS)
    INIT_ARC_STY(sty_arc_kcal,  COL_KCAL)
#undef INIT_ARC_STY

#define INIT_TXT(s,c,f) \
    lv_style_init(&s); \
    lv_style_set_text_color(&s, LV_STATE_DEFAULT, c); \
    lv_style_set_text_font(&s,  LV_STATE_DEFAULT, &f);
    INIT_TXT(sty_hdr,    COL_MUTED, Ubuntu_16px)
    INIT_TXT(sty_timer,  COL_TXT,   Ubuntu_32px)
    INIT_TXT(sty_val,    COL_TXT,   Ubuntu_16px)
    INIT_TXT(sty_muted,  COL_MUTED, Ubuntu_16px)
    INIT_TXT(sty_pace,   COL_PACE,  Ubuntu_16px)
    INIT_TXT(sty_cdist,  COL_DIST,  Ubuntu_16px)
    INIT_TXT(sty_csteps, COL_STEPS, Ubuntu_16px)
    INIT_TXT(sty_ckcal,  COL_KCAL,  Ubuntu_16px)
#undef INIT_TXT

    lv_style_init(&sty_btn);
    lv_style_set_bg_color(&sty_btn,     LV_STATE_DEFAULT, COL_BTN);
    lv_style_set_bg_color(&sty_btn,     LV_STATE_PRESSED, COL_BTN_PR);
    lv_style_set_bg_opa(&sty_btn,       LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_border_width(&sty_btn, LV_STATE_DEFAULT, 0);
    lv_style_set_radius(&sty_btn,       LV_STATE_DEFAULT, 10);
    lv_style_set_text_color(&sty_btn,   LV_STATE_DEFAULT, COL_TXT);
    lv_style_set_text_font(&sty_btn,    LV_STATE_DEFAULT, &Ubuntu_16px);

    lv_style_init(&sty_btn_go);
    lv_style_set_bg_color(&sty_btn_go,     LV_STATE_DEFAULT, COL_BTN_GO);
    lv_style_set_bg_color(&sty_btn_go,     LV_STATE_PRESSED, COL_BTN_GO_PR);
    lv_style_set_bg_opa(&sty_btn_go,       LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_border_width(&sty_btn_go, LV_STATE_DEFAULT, 0);
    lv_style_set_radius(&sty_btn_go,       LV_STATE_DEFAULT, 10);
    lv_style_set_text_color(&sty_btn_go,   LV_STATE_DEFAULT, LV_COLOR_WHITE);
    lv_style_set_text_font(&sty_btn_go,    LV_STATE_DEFAULT, &Ubuntu_16px);

    lv_style_init(&sty_sep);
    lv_style_set_bg_color(&sty_sep,     LV_STATE_DEFAULT, COL_SEP);
    lv_style_set_bg_opa(&sty_sep,       LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_border_width(&sty_sep, LV_STATE_DEFAULT, 0);
    lv_style_set_radius(&sty_sep,       LV_STATE_DEFAULT, 0);

    lv_obj_t *hdr = lv_label_create(tile, NULL);
    lv_label_set_text(hdr, "ACTIVE SESSION");
    lv_obj_add_style(hdr, LV_LABEL_PART_MAIN, &sty_hdr);
    lv_obj_align(hdr, tile, LV_ALIGN_IN_TOP_MID, 0, 4);

    lbl_dur_val = lv_label_create(tile, NULL);
    lv_label_set_text(lbl_dur_val, "0:00");
    lv_obj_add_style(lbl_dur_val, LV_LABEL_PART_MAIN, &sty_timer);
    lv_obj_align(lbl_dur_val, tile, LV_ALIGN_IN_TOP_MID, 0, 20);

    const lv_coord_t SM=64, LG=80, YCTR=100, G=6;
    const lv_coord_t XD=10, XS=10+SM+G, XK=10+SM+G+LG+G;
    const lv_coord_t CXD=XD+SM/2, CXS=XS+LG/2, CXK=XK+SM/2;

#define MK_ARC(var,si,x,y,sz) \
    var = lv_arc_create(tile, NULL); \
    lv_arc_set_bg_angles(var,0,360); lv_arc_set_angles(var,0,0); \
    lv_arc_set_range(var,0,360); \
    lv_obj_reset_style_list(var,LV_ARC_PART_BG); \
    lv_obj_reset_style_list(var,LV_ARC_PART_KNOB); \
    lv_obj_reset_style_list(var,LV_ARC_PART_INDIC); \
    lv_obj_add_style(var,LV_ARC_PART_BG,   &sty_arc_bg); \
    lv_obj_add_style(var,LV_ARC_PART_INDIC,&si); \
    lv_obj_set_click(var,false); \
    lv_obj_set_size(var,sz,sz); lv_obj_set_pos(var,x,y);

    MK_ARC(arc_dist,  sty_arc_dist,  XD, YCTR-SM/2, SM)
    MK_ARC(arc_steps, sty_arc_steps, XS, YCTR-LG/2, LG)
    MK_ARC(arc_kcal,  sty_arc_kcal,  XK, YCTR-SM/2, SM)
#undef MK_ARC

    lbl_dist_val = lv_label_create(tile, NULL);
    lv_label_set_text(lbl_dist_val, "0");
    lv_obj_add_style(lbl_dist_val, LV_LABEL_PART_MAIN, &sty_val);
    lv_obj_align(lbl_dist_val, arc_dist, LV_ALIGN_CENTER, 0, 0);

    lbl_steps_val = lv_label_create(tile, NULL);
    lv_label_set_text(lbl_steps_val, "0");
    lv_obj_add_style(lbl_steps_val, LV_LABEL_PART_MAIN, &sty_val);
    lv_obj_align(lbl_steps_val, arc_steps, LV_ALIGN_CENTER, 0, 0);

    lbl_kcal_val = lv_label_create(tile, NULL);
    lv_label_set_text(lbl_kcal_val, "0");
    lv_obj_add_style(lbl_kcal_val, LV_LABEL_PART_MAIN, &sty_val);
    lv_obj_align(lbl_kcal_val, arc_kcal, LV_ALIGN_CENTER, 0, 0);

    const lv_coord_t CW=76;
#define COL(txt,st,cx,y) { \
    lv_obj_t *_l=lv_label_create(tile,NULL); \
    lv_label_set_text(_l,txt); \
    lv_obj_add_style(_l,LV_LABEL_PART_MAIN,&st); \
    lv_label_set_align(_l,LV_LABEL_ALIGN_CENTER); \
    lv_obj_set_width(_l,CW); \
    lv_obj_set_pos(_l,(lv_coord_t)((cx)-CW/2),y); }

    COL("DIST (m)", sty_cdist,  CXD, 148)
    COL("STEPS",    sty_csteps, CXS, 148)
    COL("KCAL",     sty_ckcal,  CXK, 148)
#undef COL

    lv_obj_t *pt = lv_label_create(tile, NULL);
    lv_label_set_text(pt, LV_SYMBOL_BULLET " PACE");
    lv_obj_add_style(pt, LV_LABEL_PART_MAIN, &sty_pace);
    lv_obj_set_pos(pt, 10, 176);

    lbl_pace_val = lv_label_create(tile, NULL);
    lv_label_set_text(lbl_pace_val, "--");
    lv_obj_add_style(lbl_pace_val, LV_LABEL_PART_MAIN, &sty_pace);
    lv_obj_set_pos(lbl_pace_val, 90, 176);

    lv_obj_t *pu = lv_label_create(tile, NULL);
    lv_label_set_text(pu, "min/km");
    lv_obj_add_style(pu, LV_LABEL_PART_MAIN, &sty_muted);
    lv_obj_set_pos(pu, 160, 176);

    lv_obj_t *sep = lv_obj_create(tile, NULL);
    lv_obj_reset_style_list(sep, LV_OBJ_PART_MAIN);
    lv_obj_add_style(sep, LV_OBJ_PART_MAIN, &sty_sep);
    lv_obj_set_size(sep, 220, 1);
    lv_obj_set_pos(sep, 10, 190);
    lv_obj_set_click(sep, false);

    const lv_coord_t BW=68, BH=30, BG=6, BY=200;
    lv_coord_t bx=12;

    lv_obj_t *b1 = lv_btn_create(tile, NULL);
    lv_obj_reset_style_list(b1, LV_BTN_PART_MAIN);
    lv_obj_add_style(b1, LV_BTN_PART_MAIN, &sty_btn);
    lv_obj_set_size(b1,BW,BH); lv_obj_set_pos(b1,bx,BY);
    lv_obj_set_event_cb(b1, btn_reset_cb);
    lv_obj_t *l1=lv_label_create(b1,NULL);
    lv_label_set_text(l1,"Reset");
    lv_label_set_align(l1,LV_LABEL_ALIGN_CENTER);
    bx+=BW+BG;

    lv_obj_t *b2 = lv_btn_create(tile, NULL);
    lv_obj_reset_style_list(b2, LV_BTN_PART_MAIN);
    lv_obj_add_style(b2, LV_BTN_PART_MAIN, &sty_btn_go);
    lv_obj_set_size(b2,BW,BH); lv_obj_set_pos(b2,bx,BY);
    lv_obj_set_event_cb(b2, btn_startstop_cb);
    lbl_startstop=lv_label_create(b2,NULL);
    lv_label_set_text(lbl_startstop, LV_SYMBOL_PLAY " Start");
    lv_label_set_align(lbl_startstop,LV_LABEL_ALIGN_CENTER);
    bx+=BW+BG;

    lv_obj_t *b3 = lv_btn_create(tile, NULL);
    lv_obj_reset_style_list(b3, LV_BTN_PART_MAIN);
    lv_obj_add_style(b3, LV_BTN_PART_MAIN, &sty_btn);
    lv_obj_set_size(b3,BW,BH); lv_obj_set_pos(b3,bx,BY);
    lv_obj_set_event_cb(b3, btn_history_cb);
    lv_obj_t *l3=lv_label_create(b3,NULL);
    lv_label_set_text(l3,"History");
    lv_label_set_align(l3,LV_LABEL_ALIGN_CENTER);
}

// ─────────────────────────────────────────────────────────────────────────────
// Build history tile
// ─────────────────────────────────────────────────────────────────────────────

void build_history_page() {
    lv_obj_t *tile = mainbar_get_tile_obj(activityApp.mainTileId() + 1);
    tile_opaque(tile);

    lv_obj_t *t = lv_label_create(tile, NULL);
    lv_label_set_text(t, "SESSION HISTORY");
    lv_obj_add_style(t, LV_LABEL_PART_MAIN, &sty_hdr);
    lv_obj_set_pos(t, 12, 8);

    lv_obj_t *sep = lv_obj_create(tile, NULL);
    lv_obj_reset_style_list(sep, LV_OBJ_PART_MAIN);
    lv_obj_add_style(sep, LV_OBJ_PART_MAIN, &sty_sep);
    lv_obj_set_size(sep, 220, 1); lv_obj_set_pos(sep, 10, 28);
    lv_obj_set_click(sep, false);

    lv_obj_t *pg = lv_page_create(tile, NULL);
    lv_obj_reset_style_list(pg, LV_PAGE_PART_BG);
    lv_obj_reset_style_list(pg, LV_PAGE_PART_SCROLLABLE);
    lv_obj_add_style(pg, LV_PAGE_PART_BG,        &sty_bg);
    lv_obj_add_style(pg, LV_PAGE_PART_SCROLLABLE, &sty_bg);
    lv_obj_set_size(pg, 240, 186); lv_obj_set_pos(pg, 0, 32);

    lbl_hist_list = lv_label_create(pg, NULL);
    lv_label_set_long_mode(lbl_hist_list, LV_LABEL_LONG_BREAK);
    lv_obj_set_width(lbl_hist_list, 224);
    lv_label_set_text(lbl_hist_list, "No sessions yet.");
    lv_obj_add_style(lbl_hist_list, LV_LABEL_PART_MAIN, &sty_muted);
    lv_obj_set_pos(lbl_hist_list, 8, 4);

    lv_obj_t *bb = lv_btn_create(tile, NULL);
    lv_obj_reset_style_list(bb, LV_BTN_PART_MAIN);
    lv_obj_add_style(bb, LV_BTN_PART_MAIN, &sty_btn);
    lv_obj_set_size(bb, 110, 28);
    lv_obj_align(bb, tile, LV_ALIGN_IN_BOTTOM_MID, 0, -4);
    lv_obj_set_event_cb(bb, btn_back_cb);
    lv_obj_t *bl = lv_label_create(bb, NULL);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_label_set_align(bl, LV_LABEL_ALIGN_CENTER);
}

// ─────────────────────────────────────────────────────────────────────────────
// Refresh history display
// FIX J — buf[1024], skip zero-step entries, show duration, update once
// ─────────────────────────────────────────────────────────────────────────────

static void refresh_history_page() {
    if (!lbl_hist_list) return;
    if (history_count == 0) {
        lv_label_set_text(lbl_hist_list, "No sessions yet.");
        return;
    }

    static char buf[1024];
    buf[0] = '\0';
    char line[96];

    for (int i = 0; i < history_count; i++) {
        const SessionRecord &r = history[i];

        // Skip zero-data entries saved during BMA warmup
        if (r.steps == 0 && r.dist_m == 0) continue;

        char dur_str[12];
        snprintf(dur_str, sizeof(dur_str), "%lu:%02lu",
                 (unsigned long)(r.dur_s / 60),
                 (unsigned long)(r.dur_s % 60));

        if (r.pace_sec_per_km > 0) {
            snprintf(line, sizeof(line),
                "#%d %lustp %lum %lukcal %s @ %lu'%02lu\"\n",
                (int)(history_count - i),
                (unsigned long)r.steps,
                (unsigned long)r.dist_m,
                (unsigned long)r.kcal,
                dur_str,
                (unsigned long)(r.pace_sec_per_km / 60),
                (unsigned long)(r.pace_sec_per_km % 60));
        } else {
            snprintf(line, sizeof(line),
                "#%d %lustp %lum %lukcal %s\n",
                (int)(history_count - i),
                (unsigned long)r.steps,
                (unsigned long)r.dist_m,
                (unsigned long)r.kcal,
                dur_str);
        }
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    }

    lv_label_set_text(lbl_hist_list, buf);
    lv_obj_realign(lbl_hist_list);
}

// ─────────────────────────────────────────────────────────────────────────────
// Refresh main display — NO BLE / WiFi calls here
// ─────────────────────────────────────────────────────────────────────────────

void refresh_main_page() {
    if (!lbl_steps_val || !lbl_dist_val ||
        !lbl_kcal_val  || !lbl_dur_val  || !lbl_pace_val) return;

    const uint32_t step_len_cm = get_step_len_cm();
    uint32_t goal_steps  = (uint32_t)atoi(cfg_goal_stp.c_str());
    uint32_t goal_dist_m = (uint32_t)atoi(cfg_goal_dist.c_str());
    if (goal_steps  == 0) goal_steps  = 10000;
    if (goal_dist_m == 0) goal_dist_m = 5000;
    const uint32_t goal_kcal = goal_steps * 4 / 100;

    const uint32_t total_steps = bma_get_stepcounter();
    uint32_t stp, dist, dur;

    if (session_running) {
        session_steps      = (total_steps >= session_start_steps)
                             ? (total_steps - session_start_steps) : 0;
        session_distance_m = session_steps * step_len_cm / 100;
        session_duration_s = (millis() - session_start_ms) / 1000;
        stp  = session_steps;
        dist = session_distance_m;
        dur  = session_duration_s;
    } else {
        stp  = total_steps;
        dist = total_steps * step_len_cm / 100;
        dur  = 0;
    }

    char buf[24];

    if (dur != last_dur) {
        last_dur = dur;
        snprintf(buf, sizeof(buf), "%lu:%02lu",
                 (unsigned long)(dur/60), (unsigned long)(dur%60));
        lv_label_set_text(lbl_dur_val, buf);
    }
    if (stp != last_stp) {
        last_stp = stp;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)stp);
        lv_label_set_text(lbl_steps_val, buf);
        arc_set(arc_steps, stp, goal_steps);
    }
    if (dist != last_dist) {
        last_dist = dist;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)dist);
        lv_label_set_text(lbl_dist_val, buf);
        arc_set(arc_dist, dist, goal_dist_m);
    }
    const uint32_t kcal = stp * 4 / 100;
    if (kcal != last_kcal) {
        last_kcal = kcal;
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)kcal);
        lv_label_set_text(lbl_kcal_val, buf);
        arc_set(arc_kcal, kcal, goal_kcal);
    }
    const uint32_t pace = calc_pace(dur, dist);
    if (pace != last_pace) {
        last_pace = pace;
        if (pace == 0) {
            lv_label_set_text(lbl_pace_val, "--");
        } else {
            snprintf(buf, sizeof(buf), "%lu'%02lu\"",
                     (unsigned long)(pace/60), (unsigned long)(pace%60));
            lv_label_set_text(lbl_pace_val, buf);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Button callbacks
// ─────────────────────────────────────────────────────────────────────────────

static void btn_startstop_cb(lv_obj_t *obj, lv_event_t event) {
    (void)obj;
    if (event != LV_EVENT_CLICKED) return;

    if (!session_running) {
        session_start_ms    = millis();
        session_start_steps = bma_get_stepcounter();
        session_steps       = 0;
        session_distance_m  = 0;
        session_duration_s  = 0;
        session_running     = true;
        cache_invalidate();
        blestepctl_set_hike_active(true);
        if (lbl_startstop)
            lv_label_set_text(lbl_startstop, LV_SYMBOL_STOP " Stop");
        motor_vibe(20);

    } else {
        const uint32_t live_steps  = bma_get_stepcounter();
        const uint32_t step_len_cm = get_step_len_cm();
        const uint32_t final_steps = (live_steps >= session_start_steps)
                                     ? (live_steps - session_start_steps) : 0;
        const uint32_t final_dist  = final_steps * step_len_cm / 100;
        const uint32_t final_dur   = (millis() - session_start_ms) / 1000;

        session_steps      = final_steps;
        session_distance_m = final_dist;
        session_duration_s = final_dur;
        session_running    = false;

        blestepctl_send_hike_end(final_steps, final_dist, final_dur);
        blestepctl_set_hike_active(false);

        // FIX H — guard on dur > 0 (not steps > 0).
        // Always save locally first — watch is the source of truth.
        // history display skips zero-step entries so they never show up.
        if (final_dur > 0) {
            spiffs_save_session(final_steps, final_dist, final_dur);
            refresh_history_page();

            // FIX I — notify Pi via WiFi if connected (BLE already sent hike_end)
            if (!blectl_get_event(BLECTL_ON) &&
                 wifictl_get_event(WIFICTL_CONNECT)) {
                post_session_end(final_steps, final_dist, final_dur);
            }
        }

        if (lbl_startstop)
            lv_label_set_text(lbl_startstop, LV_SYMBOL_PLAY " Start");
        motor_vibe(40);
    }

    cache_invalidate();
    refresh_main_page();
}

static void btn_reset_cb(lv_obj_t *obj, lv_event_t event) {
    (void)obj;
    if (event != LV_EVENT_CLICKED) return;
    static const char *btns[] = { YES, NO, "" };
    lv_obj_t *mbox = lv_msgbox_create(lv_scr_act(), NULL);
    lv_msgbox_set_text(mbox, "Reset step counter?");
    lv_msgbox_add_btns(mbox, btns);
    lv_obj_set_width(mbox, 200);
    default_msgbox_cb = lv_obj_get_event_cb(mbox);
    lv_obj_set_event_cb(mbox, activity_reset_cb);
    lv_obj_align(mbox, NULL, LV_ALIGN_CENTER, 0, 0);
}

static void btn_history_cb(lv_obj_t *obj, lv_event_t event) {
    (void)obj;
    if (event != LV_EVENT_CLICKED) return;
    refresh_history_page();
    mainbar_jump_to_tilenumber(activityApp.mainTileId() + 1,
                               (lv_anim_enable_t)LV_ANIM_ON);
}

static void btn_back_cb(lv_obj_t *obj, lv_event_t event) {
    (void)obj;
    if (event != LV_EVENT_CLICKED) return;
    mainbar_jump_to_tilenumber(activityApp.mainTileId(),
                               (lv_anim_enable_t)LV_ANIM_ON);
}

static void activity_reset_cb(lv_obj_t *obj, lv_event_t event) {
    if (event == LV_EVENT_VALUE_CHANGED) {
        if (strcmp(lv_msgbox_get_active_btn_text(obj), YES) == 0) {
            if (session_running) {
                session_running = false;
                blestepctl_set_hike_active(false);
                if (lbl_startstop)
                    lv_label_set_text(lbl_startstop, LV_SYMBOL_PLAY " Start");
            }
            bma_reset_stepcounter();
            cache_invalidate();
            refresh_main_page();
        }
    }
    default_msgbox_cb(obj, event);
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings
// ─────────────────────────────────────────────────────────────────────────────

void build_settings() {
    config.addString("Step length (cm)", 3, "50")
          .setDigitsMode(true, "0123456789").assign(&cfg_len);
    config.addString("Step Goal", 7, "10000")
          .setDigitsMode(true, "0123456789").assign(&cfg_goal_stp);
    config.addString("Distance Goal (m)", 7, "5000")
          .setDigitsMode(true, "0123456789").assign(&cfg_goal_dist);
    activityApp.useConfig(config, false);
}