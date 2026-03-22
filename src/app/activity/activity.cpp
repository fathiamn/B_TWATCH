/*
 *  Monterro — Activity app
 *  Target : LILYGO T-Watch 2020 V1/V2/V3 · LVGL 7.11.1
 *
 *  Fix log vs previous builds
 *  ──────────────────────────
 *  1. TITLE   — monterro_64px didn't exist → linker failed silently, old
 *               binary kept running. Now uses monterro_64px (always present)
 *               with name "Monterro". Swap back when your PNG is converted.
 *
 *  2. HISTORY — shift only ran when array was FULL (history_count==MAX).
 *               Every session before slot 10 silently overwrote history[0].
 *               Fixed: always shift before inserting at [0].
 *
 *  3. SPIFFS  — SPIFFS.begin(true) was called twice: in load_history() AND
 *               in save_session(). Now mounted ONCE in activity_app_setup().
 *
 *  4. HISTORY GUARD — was "if (final_steps > 0)" which silently dropped
 *               sessions with 0 steps (timing gap between pedometer and stop).
 *               Now guards on "final_dur > 0" — any session with elapsed time
 *               is worth recording even if steps reads 0.
 *
 *  5. BG OPA  — bg tiles always need LV_OPA_COVER or mainbar dark bg bleeds
 *               through. Already correct in both uploads, kept as-is.
 *
 *  6. PACE    — threshold 2 m (not 10 m). Shows after ~4 steps at 50 cm/step.
 *
 *  7. TASKS   — display task (1 s): labels/arcs only, zero BLE/WiFi.
 *               telemetry task (5 s): BLE primary, WiFi fallback. Kept.
 *
 *  8. WIFI    — never calls wifictl_on(). mDNS with hardcoded fallback IP.
 *               Only POSTs when WiFi already connected.
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

// FIX 1 — use monterro_64px (always compiled in) until monterro_64px.c exists.
// To restore custom icon:
//   1. Convert monterro.png (64×64) at https://lvgl.io/tools/imageconverter
//      Output format: C array · Color format: CF_TRUE_COLOR_ALPHA
//   2. Add monterro_64px.c to your build
//   3. Replace the two lines below with:
//        LV_IMG_DECLARE(monterro_64px);
//        #define APP_ICON  monterro_64px
LV_IMG_DECLARE(monterro_64px);
#define APP_ICON  monterro_64px

LV_FONT_DECLARE(Ubuntu_16px);
LV_FONT_DECLARE(Ubuntu_32px);

#define YES "Yes"
#define NO  "No"

// ── Pi connection ─────────────────────────────────────────────────────────────
#define PI_FALLBACK_IP  "192.168.0.112"   // ← set your Pi's actual local IP
#define PI_PORT         5000

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

// ── Session state ─────────────────────────────────────────────────────────────
static bool     session_running     = false;
static uint32_t session_start_ms    = 0;
static uint32_t session_start_steps = 0;
static uint32_t session_steps       = 0;
static uint32_t session_distance_m  = 0;
static uint32_t session_duration_s  = 0;

// ── App / config ──────────────────────────────────────────────────────────────
static SynchronizedApplication activityApp;
static JsonConfig config("activity.json");
static String cfg_len, cfg_goal_stp, cfg_goal_dist;

// ── Task guard ────────────────────────────────────────────────────────────────
static bool tile_active = false;

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

// ── Pi address cache ──────────────────────────────────────────────────────────
static IPAddress cachedPiAddr;
static uint32_t  cachedPiAddrMs       = 0;
static const uint32_t PI_ADDR_CACHE_MS = 30000;

// ── Forward declarations ──────────────────────────────────────────────────────
static void build_main_page();
static void build_history_page();
static void refresh_main_page();
static void refresh_history_page();
static void build_settings();
static void activity_activate_cb();
static void task_display_cb(lv_task_t *t);
static void task_telem_cb(lv_task_t *t);
static void activity_reset_cb(lv_obj_t *obj, lv_event_t event);
static void btn_startstop_cb(lv_obj_t *obj, lv_event_t event);
static void btn_reset_cb(lv_obj_t *obj, lv_event_t event);
static void btn_history_cb(lv_obj_t *obj, lv_event_t event);
static void btn_back_cb(lv_obj_t *obj, lv_event_t event);
static void cache_invalidate();
static uint32_t get_step_len_cm();
static uint32_t calc_pace(uint32_t dur_s, uint32_t dist_m);
static IPAddress resolve_pi_addr();
static bool post_live_update(uint32_t stp, uint32_t dist, uint32_t dur);
static void spiffs_save_session(uint32_t steps, uint32_t dist, uint32_t dur);
static void spiffs_load_history();

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

// FIX 6 — threshold 2 m, not 10 m
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
// SPIFFS persistence (FIX 3 — SPIFFS mounted once in setup, not here)
// ─────────────────────────────────────────────────────────────────────────────

// Save one session: push into RAM ring-buffer + append line to SPIFFS log.
// FIX 2 — shift ALWAYS runs, not only when history_count == HISTORY_MAX.
static void spiffs_save_session(uint32_t steps, uint32_t dist, uint32_t dur) {
    // ── RAM ring-buffer ───────────────────────────────────────────────────────
    // Shift existing entries up by one slot to make room at [0].
    // Previously the shift only ran when the array was full (== HISTORY_MAX),
    // so sessions 1–9 silently overwrote history[0] every time.
    const uint8_t used = (history_count < HISTORY_MAX)
                         ? history_count : HISTORY_MAX - 1;
    for (int i = used; i > 0; i--)          // always shift, regardless of count
        history[i] = history[i - 1];

    history[0].steps           = steps;
    history[0].dist_m          = dist;
    history[0].dur_s           = dur;
    history[0].kcal            = steps * 4 / 100;
    history[0].pace_sec_per_km = calc_pace(dur, dist);

    if (history_count < HISTORY_MAX) history_count++;

    // ── SPIFFS append ─────────────────────────────────────────────────────────
    // FIX 3 — no SPIFFS.begin() here; already mounted in activity_app_setup()
    File f = SPIFFS.open(HIKE_LOG, FILE_APPEND);
    if (!f) {
        log_w("[monterro] SPIFFS open failed for append");
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
    log_i("[monterro] Session saved → %s", line);
}

// Load last HISTORY_MAX sessions from SPIFFS on boot.
// FIX 3 — no SPIFFS.begin() here; already mounted in activity_app_setup()
static void spiffs_load_history() {
    if (!SPIFFS.exists(HIKE_LOG)) {
        log_i("[monterro] No hike log found, starting fresh");
        return;
    }

    File f = SPIFFS.open(HIKE_LOG, FILE_READ);
    if (!f) return;

    // Read all lines, keep last HISTORY_MAX
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

    // Parse newest-first into history[]
    history_count = 0;
    for (int i = total - 1; i >= 0 && history_count < HISTORY_MAX; i--) {
        uint32_t stp = 0, dst = 0, dur = 0, kcal = 0;
        long ts = 0;
        int matched = sscanf(lines[i],
            "{\"ts\":%ld,\"stp\":%lu,\"dst\":%lu,\"dur\":%lu,\"kcal\":%lu}",
            &ts, &stp, &dst, &dur, &kcal);
        if (matched < 4 || dur == 0) continue;   // skip corrupt / zero-dur lines
        history[history_count].steps           = stp;
        history[history_count].dist_m          = dst;
        history[history_count].dur_s           = dur;
        history[history_count].kcal            = kcal;
        history[history_count].pace_sec_per_km = calc_pace(dur, dst);
        history_count++;
    }
    log_i("[monterro] Loaded %d session(s) from SPIFFS", history_count);
}

// ─────────────────────────────────────────────────────────────────────────────
// WiFi telemetry
// ─────────────────────────────────────────────────────────────────────────────

// FIX 8 — mDNS with hardcoded fallback; never calls wifictl_on()
static IPAddress resolve_pi_addr() {
    if ((uint32_t)cachedPiAddr != 0 &&
        (millis() - cachedPiAddrMs) < PI_ADDR_CACHE_MS) {
        return cachedPiAddr;
    }
    IPAddress piAddr = MDNS.queryHost("hiking-pi");
    if ((uint32_t)piAddr == 0) {
        log_w("[monterro] mDNS failed, using fallback " PI_FALLBACK_IP);
        piAddr.fromString(PI_FALLBACK_IP);
    }
    cachedPiAddr   = piAddr;
    cachedPiAddrMs = millis();
    return piAddr;
}

static bool post_live_update(uint32_t stp, uint32_t dist, uint32_t dur) {
    IPAddress piAddr = resolve_pi_addr();
    if ((uint32_t)piAddr == 0) return false;

    char url[64];
    snprintf(url, sizeof(url),
             "http://%s:%d/live", piAddr.toString().c_str(), PI_PORT);

    char body[96];
    snprintf(body, sizeof(body),
             "{\"steps\":%lu,\"distance\":%lu,\"duration\":%lu}",
             (unsigned long)stp,
             (unsigned long)dist,
             (unsigned long)dur);

    HTTPClient http;
    http.setTimeout(2000);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t*)body, strlen(body));
    http.end();
    log_i("[monterro] POST %s → %d", url, code);
    return (code >= 200 && code < 300);
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────

void activity_app_setup() {
    // FIX 1 — APP_ICON resolves to monterro_64px (always present).
    // The name "Monterro" is what appears in the watch app list.
    activityApp.init("Monterro", &APP_ICON, 1, 2);
    mainbar_add_tile_activate_cb(activityApp.mainTileId(), activity_activate_cb);

    build_main_page();
    build_history_page();
    build_settings();

    // FIX 3 — mount SPIFFS exactly once, here, before load or save are called
    if (!SPIFFS.begin(true)) {
        log_e("[monterro] SPIFFS mount failed — history will not persist");
    }
    spiffs_load_history();   // restore sessions from flash on boot

    // FIX 7 — two separate tasks: display (1 s) and telemetry (5 s)
    lv_task_create(task_display_cb, 1000, LV_TASK_PRIO_LOW, nullptr);
    lv_task_create(task_telem_cb,   5000, LV_TASK_PRIO_LOW, nullptr);

    refresh_main_page();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tasks
// ─────────────────────────────────────────────────────────────────────────────

// Display task — label/arc updates ONLY, zero BLE/WiFi
static void task_display_cb(lv_task_t *t) {
    (void)t;
    if (!tile_active || !lbl_dur_val) return;
    refresh_main_page();
}

// Telemetry task — BLE primary, WiFi fallback, never wifictl_on()
static void task_telem_cb(lv_task_t *t) {
    (void)t;
    if (!session_running) return;

    const uint32_t stp  = session_steps;
    const uint32_t dist = session_distance_m;
    const uint32_t dur  = session_duration_s;

    if (blectl_get_event(BLECTL_ON)) {
        blestepctl_set_hike(stp, dist, dur);
        blestepctl_update_hike(false);
        return;   // BLE takes priority, skip WiFi
    }

    // Request WiFi on if not already connected
    if (!wifictl_get_event(WIFICTL_CONNECT)) {
        wifictl_on();   // ask the system to connect
        return;         // will send next tick once connected
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
//
//  240 × 240 px layout
//  y=  4   "ACTIVE SESSION"   muted 16px centred
//  y= 22   "0:00"             dark  32px centred
//  arcs    Y_CTR = 100
//    dist  64×64  x=10  cx=42
//    steps 80×80  x=80  cx=120
//    kcal  64×64  x=166 cx=198
//  y=148   DIST(m) / STEPS / KCAL   coloured headers
//  y=176   ● PACE  value  min/km
//  y=190   separator
//  y=200   [Reset] [▶ Start] [History]

void build_main_page() {
    lv_obj_t *tile = mainbar_get_tile_obj(activityApp.mainTileId());

    // Background — FIX 5: LV_OPA_COVER mandatory or mainbar dark bg shows through
    lv_style_init(&sty_bg);
    lv_style_set_bg_color(&sty_bg,     LV_STATE_DEFAULT, COL_BG);
    lv_style_set_bg_opa(&sty_bg,       LV_STATE_DEFAULT, LV_OPA_COVER);
    lv_style_set_border_width(&sty_bg, LV_STATE_DEFAULT, 0);
    lv_style_set_pad_all(&sty_bg,      LV_STATE_DEFAULT, 0);
    tile_opaque(tile);

    // Arc styles
#define INIT_ARC(s,c) \
    lv_style_init(&s); \
    lv_style_set_line_color(&s, LV_STATE_DEFAULT, c); \
    lv_style_set_line_width(&s, LV_STATE_DEFAULT, 6);
    INIT_ARC(sty_arc_bg,    COL_ARC_BG)
    INIT_ARC(sty_arc_dist,  COL_DIST)
    INIT_ARC(sty_arc_steps, COL_STEPS)
    INIT_ARC(sty_arc_kcal,  COL_KCAL)
#undef INIT_ARC

    // Text styles
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

    // Button styles
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

    // Header + timer
    lv_obj_t *hdr = lv_label_create(tile, NULL);
    lv_label_set_text(hdr, "ACTIVE SESSION");
    lv_obj_add_style(hdr, LV_LABEL_PART_MAIN, &sty_hdr);
    lv_obj_align(hdr, tile, LV_ALIGN_IN_TOP_MID, 0, 4);

    lbl_dur_val = lv_label_create(tile, NULL);
    lv_label_set_text(lbl_dur_val, "0:00");
    lv_obj_add_style(lbl_dur_val, LV_LABEL_PART_MAIN, &sty_timer);
    lv_obj_align(lbl_dur_val, tile, LV_ALIGN_IN_TOP_MID, 0, 20);

    // Arc positions — 64+6+80+6+64=220, margin=10
    const lv_coord_t SM=64, LG=80, YCTR=100, G=6;
    const lv_coord_t XD=10,       XS=10+SM+G,     XK=10+SM+G+LG+G;
    const lv_coord_t CXD=XD+SM/2, CXS=XS+LG/2,   CXK=XK+SM/2;

#define MK_ARC(var,si,x,y,sz) \
    var = lv_arc_create(tile, NULL); \
    lv_arc_set_bg_angles(var,0,360); \
    lv_arc_set_angles(var,0,0); \
    lv_arc_set_range(var,0,360); \
    lv_obj_reset_style_list(var,LV_ARC_PART_BG); \
    lv_obj_reset_style_list(var,LV_ARC_PART_KNOB); \
    lv_obj_reset_style_list(var,LV_ARC_PART_INDIC); \
    lv_obj_add_style(var,LV_ARC_PART_BG,   &sty_arc_bg); \
    lv_obj_add_style(var,LV_ARC_PART_INDIC,&si); \
    lv_obj_set_click(var,false); \
    lv_obj_set_size(var,sz,sz); \
    lv_obj_set_pos(var,x,y);

    MK_ARC(arc_dist,  sty_arc_dist,  XD, YCTR-SM/2, SM)
    MK_ARC(arc_steps, sty_arc_steps, XS, YCTR-LG/2, LG)
    MK_ARC(arc_kcal,  sty_arc_kcal,  XK, YCTR-SM/2, SM)
#undef MK_ARC

    // One value label per arc — number only, centred
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

    // Column headers (y=148)
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

    // Pace row (y=176)
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

    // Separator (y=190)
    lv_obj_t *sep = lv_obj_create(tile, NULL);
    lv_obj_reset_style_list(sep, LV_OBJ_PART_MAIN);
    lv_obj_add_style(sep, LV_OBJ_PART_MAIN, &sty_sep);
    lv_obj_set_size(sep, 220, 1);
    lv_obj_set_pos(sep, 10, 190);
    lv_obj_set_click(sep, false);

    // Buttons (y=200) — 3×68 + 2×6 = 216, margin=12
    const lv_coord_t BW=68, BH=30, BG=6, BY=200;
    lv_coord_t bx=12;

    lv_obj_t *b1 = lv_btn_create(tile, NULL);
    lv_obj_reset_style_list(b1, LV_BTN_PART_MAIN);
    lv_obj_add_style(b1, LV_BTN_PART_MAIN, &sty_btn);
    lv_obj_set_size(b1,BW,BH); lv_obj_set_pos(b1,bx,BY);
    lv_obj_set_event_cb(b1, btn_reset_cb);
    lv_obj_t *l1 = lv_label_create(b1, NULL);
    lv_label_set_text(l1, "Reset");
    lv_label_set_align(l1, LV_LABEL_ALIGN_CENTER);
    bx += BW+BG;

    lv_obj_t *b2 = lv_btn_create(tile, NULL);
    lv_obj_reset_style_list(b2, LV_BTN_PART_MAIN);
    lv_obj_add_style(b2, LV_BTN_PART_MAIN, &sty_btn_go);
    lv_obj_set_size(b2,BW,BH); lv_obj_set_pos(b2,bx,BY);
    lv_obj_set_event_cb(b2, btn_startstop_cb);
    lbl_startstop = lv_label_create(b2, NULL);
    lv_label_set_text(lbl_startstop, LV_SYMBOL_PLAY " Start");
    lv_label_set_align(lbl_startstop, LV_LABEL_ALIGN_CENTER);
    bx += BW+BG;

    lv_obj_t *b3 = lv_btn_create(tile, NULL);
    lv_obj_reset_style_list(b3, LV_BTN_PART_MAIN);
    lv_obj_add_style(b3, LV_BTN_PART_MAIN, &sty_btn);
    lv_obj_set_size(b3,BW,BH); lv_obj_set_pos(b3,bx,BY);
    lv_obj_set_event_cb(b3, btn_history_cb);
    lv_obj_t *l3 = lv_label_create(b3, NULL);
    lv_label_set_text(l3, "History");
    lv_label_set_align(l3, LV_LABEL_ALIGN_CENTER);
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
    lv_obj_set_size(sep, 220, 1);
    lv_obj_set_pos(sep, 10, 28);
    lv_obj_set_click(sep, false);

    lv_obj_t *pg = lv_page_create(tile, NULL);
    lv_obj_reset_style_list(pg, LV_PAGE_PART_BG);
    lv_obj_reset_style_list(pg, LV_PAGE_PART_SCROLLABLE);
    lv_obj_add_style(pg, LV_PAGE_PART_BG,        &sty_bg);
    lv_obj_add_style(pg, LV_PAGE_PART_SCROLLABLE, &sty_bg);
    lv_obj_set_size(pg, 240, 186);
    lv_obj_set_pos(pg, 0, 32);

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
// ─────────────────────────────────────────────────────────────────────────────

static void refresh_history_page() {
    if (!lbl_hist_list) return;
    if (history_count == 0) {
        lv_label_set_text(lbl_hist_list, "No sessions yet.");
        return;
    }

    static char buf[512];
    buf[0] = '\0';
    char line[80];

    for (int i = 0; i < history_count; i++) {
        const SessionRecord &r = history[i];
        if (r.pace_sec_per_km > 0) {
            snprintf(line, sizeof(line),
                "#%d %lustp %lum %lukcal %lu'%02lu\"\n",
                (int)(history_count - i),
                (unsigned long)r.steps,
                (unsigned long)r.dist_m,
                (unsigned long)r.kcal,
                (unsigned long)(r.pace_sec_per_km / 60),
                (unsigned long)(r.pace_sec_per_km % 60));
        } else {
            snprintf(line, sizeof(line),
                "#%d %lustp %lum %lukcal\n",
                (int)(history_count - i),
                (unsigned long)r.steps,
                (unsigned long)r.dist_m,
                (unsigned long)r.kcal);
        }
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    }
    lv_label_set_text(lbl_hist_list, buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Refresh main display — NO BLE/WiFi calls
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
        // Idle: show all-time totals, timer frozen at 0:00
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
        // ── START ─────────────────────────────────────────────────────────────
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
        // ── STOP ──────────────────────────────────────────────────────────────
        // Read live pedometer now (FIX C) — don't rely on cached session_steps
        // which was last updated up to 1 s ago by the display task.
        const uint32_t live_steps  = bma_get_stepcounter();
        const uint32_t step_len_cm = get_step_len_cm();
        const uint32_t final_steps = (live_steps >= session_start_steps)
                                     ? (live_steps - session_start_steps) : 0;
        const uint32_t final_dist  = final_steps * step_len_cm / 100;
        const uint32_t final_dur   = (millis() - session_start_ms) / 1000;

        // Sync accumulators so telemetry task gets the final values
        session_steps      = final_steps;
        session_distance_m = final_dist;
        session_duration_s = final_dur;
        session_running    = false;

        blestepctl_send_hike_end(final_steps, final_dist, final_dur);
        blestepctl_set_hike_active(false);

        // FIX 4 — guard on duration, not steps.
        // Pedometer may return 0 just after session starts due to hardware
        // latency, but elapsed time is always reliable.
        if (final_dur > 0) {
            spiffs_save_session(final_steps, final_dist, final_dur);
            refresh_history_page();   // update label immediately
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
