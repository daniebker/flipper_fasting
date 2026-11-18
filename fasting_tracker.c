#include <furi.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <datetime/datetime.h>

#include <stdio.h>
#include <string.h>

#define TAG "FastingTracker"

#define STATE_PATH     APP_DATA_PATH("state.bin")
#define STATE_MAGIC    0x46415354UL /* "FAST" */
#define STATE_VERSION  1
#define HISTORY_MAX    30
#define HISTORY_ROWS   4
#define ADJUST_STEP_S  (15 * 60)
#define MIN_RECORD_S   60
#define SECONDS_IN_DAY (24UL * 60UL * 60UL)
#define MAX_BACKDATE_S (7UL * SECONDS_IN_DAY)

typedef struct {
    const char* name;
    uint8_t fast_hours;
} FastingPlan;

static const FastingPlan plans[] = {
    {"13:11", 13},
    {"16:8", 16},
    {"18:6", 18},
    {"20:4", 20},
    {"OMAD 23:1", 23},
    {"36h fast", 36},
    {"48h fast", 48},
};
#define PLAN_COUNT (sizeof(plans) / sizeof(plans[0]))
#define DEFAULT_PLAN 1

typedef struct {
    uint32_t start;
    uint32_t end;
    uint8_t goal_hours;
    uint8_t reserved[3];
} FastRecord;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t fasting;
    uint8_t plan;
    uint8_t goal_notified;
    uint32_t fast_start;
    uint32_t last_end;
    uint8_t history_count;
    uint8_t reserved[3];
    FastRecord history[HISTORY_MAX]; /* newest first */
} PersistState;

typedef enum {
    ScreenMain,
    ScreenPickStart,
    ScreenConfirmEnd,
    ScreenHistory,
} Screen;

typedef struct {
    PersistState data;
    Screen screen;
    uint8_t history_scroll;
    uint8_t history_sel;
    uint32_t pick_ts; /* time chosen in start/end picker; 0 = "now" */
    FuriMutex* mutex;
} FastingApp;

/* ---------- helpers ---------- */

static uint32_t now_ts(void) {
    return furi_hal_rtc_get_timestamp();
}

static uint32_t goal_seconds(const FastingApp* app) {
    return (uint32_t)plans[app->data.plan].fast_hours * 3600UL;
}

static uint32_t elapsed_since(uint32_t ts) {
    uint32_t now = now_ts();
    return now > ts ? now - ts : 0;
}

static void format_hms(char* buf, size_t len, uint32_t secs) {
    snprintf(
        buf,
        len,
        "%02lu:%02lu:%02lu",
        (unsigned long)(secs / 3600UL),
        (unsigned long)((secs / 60UL) % 60UL),
        (unsigned long)(secs % 60UL));
}

static void format_hm(char* buf, size_t len, uint32_t secs) {
    snprintf(
        buf,
        len,
        "%luh%02lum",
        (unsigned long)(secs / 3600UL),
        (unsigned long)((secs / 60UL) % 60UL));
}

static void format_datetime(char* buf, size_t len, uint32_t ts) {
    DateTime dt;
    datetime_timestamp_to_datetime(ts, &dt);
    snprintf(
        buf,
        len,
        "%02u/%02u %02u:%02u",
        (unsigned)dt.day,
        (unsigned)dt.month,
        (unsigned)dt.hour,
        (unsigned)dt.minute);
}

static void format_clock(char* buf, size_t len, uint32_t ts) {
    DateTime dt;
    datetime_timestamp_to_datetime(ts, &dt);
    snprintf(buf, len, "%02u:%02u", (unsigned)dt.hour, (unsigned)dt.minute);
}

/* ---------- time picker (0 = now, otherwise an absolute timestamp) ---------- */

static uint32_t pick_resolve(uint32_t pick) {
    return pick ? pick : now_ts();
}

/* Step to the previous quarter hour, not going below min_ts. */
static void pick_earlier(uint32_t* pick, uint32_t min_ts) {
    uint32_t base = pick_resolve(*pick);
    if(base <= min_ts) return;
    uint32_t next = ((base - 1) / ADJUST_STEP_S) * ADJUST_STEP_S;
    if(next < min_ts) next = min_ts;
    *pick = next;
}

/* Step to the next quarter hour; reaching the present snaps back to "now". */
static void pick_later(uint32_t* pick) {
    if(*pick == 0) return;
    uint32_t next = (*pick / ADJUST_STEP_S + 1) * ADJUST_STEP_S;
    *pick = next >= now_ts() ? 0 : next;
}

/* Earliest allowed start: not before the previous fast ended, and at most 7 days back. */
static uint32_t earliest_start(const FastingApp* app) {
    uint32_t now = now_ts();
    uint32_t min_ts = now > MAX_BACKDATE_S ? now - MAX_BACKDATE_S : 0;
    if(app->data.last_end > min_ts && app->data.last_end <= now) min_ts = app->data.last_end;
    return min_ts;
}

/* Earliest allowed end: long enough after the start for the fast to be logged. */
static uint32_t earliest_end(const FastingApp* app) {
    return app->data.fast_start + MIN_RECORD_S;
}

/* ---------- persistence ---------- */

static void state_defaults(PersistState* s) {
    memset(s, 0, sizeof(*s));
    s->magic = STATE_MAGIC;
    s->version = STATE_VERSION;
    s->plan = DEFAULT_PLAN;
}

static void state_load(PersistState* s) {
    state_defaults(s);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, STATE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        PersistState tmp;
        if(storage_file_read(file, &tmp, sizeof(tmp)) == sizeof(tmp) &&
           tmp.magic == STATE_MAGIC && tmp.version == STATE_VERSION) {
            if(tmp.plan >= PLAN_COUNT) tmp.plan = DEFAULT_PLAN;
            if(tmp.history_count > HISTORY_MAX) tmp.history_count = HISTORY_MAX;
            *s = tmp;
        } else {
            FURI_LOG_W(TAG, "State file invalid, using defaults");
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void state_save(const PersistState* s) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, STATE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        if(storage_file_write(file, s, sizeof(*s)) != sizeof(*s)) {
            FURI_LOG_E(TAG, "Failed to write state");
        }
    } else {
        FURI_LOG_E(TAG, "Failed to open state file");
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

/* ---------- actions (call with mutex held) ---------- */

static void fast_start(FastingApp* app, uint32_t start) {
    app->data.fasting = 1;
    app->data.fast_start = start;
    app->data.goal_notified = 0;
}

static void fast_end(FastingApp* app, uint32_t end) {
    uint32_t start = app->data.fast_start;
    if(end > start && end - start >= MIN_RECORD_S) {
        PersistState* d = &app->data;
        uint8_t count = d->history_count < HISTORY_MAX ? d->history_count + 1 : HISTORY_MAX;
        memmove(&d->history[1], &d->history[0], sizeof(FastRecord) * (count - 1));
        d->history[0].start = start;
        d->history[0].end = end;
        d->history[0].goal_hours = plans[d->plan].fast_hours;
        d->history_count = count;
    }
    app->data.fasting = 0;
    app->data.last_end = end;
    app->data.goal_notified = 0;
}

static void fast_adjust_start(FastingApp* app, int32_t delta) {
    uint32_t now = now_ts();
    uint32_t start = app->data.fast_start;
    if(delta < 0) {
        uint32_t d = (uint32_t)(-delta);
        uint32_t min_ts = earliest_start(app);
        start = start > min_ts + d ? start - d : min_ts;
    } else {
        start += (uint32_t)delta;
        if(start > now) start = now;
    }
    app->data.fast_start = start;
    if(elapsed_since(start) < goal_seconds(app)) app->data.goal_notified = 0;
}

/* ---------- drawing ---------- */

static void draw_main(Canvas* canvas, FastingApp* app) {
    char buf[96];
    const PersistState* d = &app->data;
    const FastingPlan* plan = &plans[d->plan];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, plan->name);
    canvas_draw_str_aligned(
        canvas,
        126,
        10,
        AlignRight,
        AlignBottom,
        d->fasting ? "FASTING" : (d->last_end ? "EATING" : "READY"));
    canvas_draw_line(canvas, 0, 12, 127, 12);

    uint32_t elapsed;
    uint32_t target;
    if(d->fasting) {
        elapsed = elapsed_since(d->fast_start);
        target = goal_seconds(app);
    } else if(d->last_end) {
        elapsed = elapsed_since(d->last_end);
        target = plan->fast_hours < 24 ? (24UL - plan->fast_hours) * 3600UL : 0;
    } else {
        elapsed = 0;
        target = 0;
    }

    /* Big timer */
    format_hms(buf, sizeof(buf), elapsed);
    canvas_set_font(canvas, strlen(buf) <= 8 ? FontBigNumbers : FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 23, AlignCenter, AlignCenter, buf);

    /* Progress bar */
    const uint8_t bx = 4, by = 33, bw = 120, bh = 7;
    canvas_draw_rframe(canvas, bx, by, bw, bh, 2);
    if(target > 0) {
        uint32_t fill = elapsed >= target ? (bw - 4) : (uint32_t)(((uint64_t)elapsed * (bw - 4)) / target);
        if(fill > 0) canvas_draw_box(canvas, bx + 2, by + 2, fill, bh - 4);
    }

    /* Status line */
    canvas_set_font(canvas, FontSecondary);
    char hm[32];
    if(d->fasting) {
        if(elapsed >= target) {
            format_hm(hm, sizeof(hm), elapsed - target);
            snprintf(buf, sizeof(buf), "Goal hit! +%s", hm);
        } else {
            format_hm(hm, sizeof(hm), target - elapsed);
            snprintf(buf, sizeof(buf), "%s to goal", hm);
        }
    } else if(d->last_end && target > 0) {
        if(elapsed >= target) {
            snprintf(buf, sizeof(buf), "Eating window over");
        } else {
            format_hm(hm, sizeof(hm), target - elapsed);
            snprintf(buf, sizeof(buf), "Eat window: %s left", hm);
        }
    } else {
        snprintf(buf, sizeof(buf), "Press OK to start fast");
    }
    canvas_draw_str_aligned(canvas, 64, 47, AlignCenter, AlignBottom, buf);

    elements_button_left(canvas, "Plan");
    elements_button_center(canvas, d->fasting ? "End" : "Start");
    elements_button_right(canvas, "Log");
}

static void draw_time_picker(Canvas* canvas, const char* title, uint32_t pick) {
    char buf[96];
    char hm[32];
    uint32_t ts = pick_resolve(pick);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, title);

    format_clock(buf, sizeof(buf), ts);
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, buf);

    /* up/down arrows hint either side of the time */
    canvas_draw_triangle(canvas, 22, 20, 7, 4, CanvasDirectionBottomToTop);
    canvas_draw_triangle(canvas, 22, 28, 7, 4, CanvasDirectionTopToBottom);
    canvas_draw_triangle(canvas, 105, 20, 7, 4, CanvasDirectionBottomToTop);
    canvas_draw_triangle(canvas, 105, 28, 7, 4, CanvasDirectionTopToBottom);

    canvas_set_font(canvas, FontSecondary);
    if(pick == 0) {
        snprintf(buf, sizeof(buf), "Now");
    } else {
        format_hm(hm, sizeof(hm), elapsed_since(ts));
        snprintf(buf, sizeof(buf), "%s ago", hm);
    }
    canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignBottom, buf);
}

static void draw_pick_start(Canvas* canvas, FastingApp* app) {
    char buf[96];
    char clock[16];
    draw_time_picker(canvas, "Start fast at", app->pick_ts);

    uint32_t goal_at = pick_resolve(app->pick_ts) + goal_seconds(app);
    format_clock(clock, sizeof(clock), goal_at);
    snprintf(buf, sizeof(buf), "%s goal at %s", plans[app->data.plan].name, clock);
    canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignBottom, buf);

    elements_button_left(canvas, "Cancel");
    elements_button_center(canvas, "Start");
}

static void draw_confirm(Canvas* canvas, FastingApp* app) {
    char buf[96];
    char hm[32];
    draw_time_picker(canvas, "End fast at", app->pick_ts);

    uint32_t end = pick_resolve(app->pick_ts);
    uint32_t start = app->data.fast_start;
    uint32_t fasted = end > start ? end - start : 0;
    format_hm(hm, sizeof(hm), fasted);
    snprintf(
        buf,
        sizeof(buf),
        "Fasted %s %s",
        hm,
        fasted >= goal_seconds(app) ? "- goal met" : "- short");
    canvas_draw_str_aligned(canvas, 64, 51, AlignCenter, AlignBottom, buf);

    elements_button_left(canvas, "Cancel");
    elements_button_center(canvas, "End");
}

static void draw_history(Canvas* canvas, FastingApp* app) {
    char buf[96];
    char hm[32];
    const PersistState* d = &app->data;

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "History");

    canvas_set_font(canvas, FontSecondary);
    if(d->history_count == 0) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "No fasts logged yet");
        return;
    }

    uint32_t total = 0;
    uint8_t hits = 0;
    for(uint8_t i = 0; i < d->history_count; i++) {
        uint32_t dur = d->history[i].end - d->history[i].start;
        total += dur;
        if(dur >= (uint32_t)d->history[i].goal_hours * 3600UL) hits++;
    }
    format_hm(hm, sizeof(hm), total / d->history_count);
    snprintf(buf, sizeof(buf), "avg %s %u/%u", hm, (unsigned)hits, (unsigned)d->history_count);
    canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, buf);
    canvas_draw_line(canvas, 0, 12, 127, 12);

    for(uint8_t row = 0; row < HISTORY_ROWS; row++) {
        uint8_t i = app->history_scroll + row;
        if(i >= d->history_count) break;
        const FastRecord* r = &d->history[i];
        uint32_t dur = r->end - r->start;
        bool hit = dur >= (uint32_t)r->goal_hours * 3600UL;
        char when[64];
        format_datetime(when, sizeof(when), r->start);
        format_hm(hm, sizeof(hm), dur);
        int y = 23 + row * 11;
        bool sel = i == app->history_sel;
        if(sel) {
            canvas_draw_box(canvas, 0, y - 9, 125, 11);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, 2, y, when);
        canvas_draw_str_aligned(canvas, 112, y, AlignRight, AlignBottom, hm);
        if(hit) {
            /* check mark */
            canvas_draw_line(canvas, 116, y - 4, 118, y - 2);
            canvas_draw_line(canvas, 118, y - 2, 122, y - 7);
        } else {
            canvas_draw_str(canvas, 117, y, "-");
        }
        canvas_set_color(canvas, ColorBlack);
    }

    if(d->history_count > HISTORY_ROWS) {
        elements_scrollbar_pos(
            canvas, 127, 14, 50, app->history_sel, d->history_count);
    }
}

static void draw_callback(Canvas* canvas, void* ctx) {
    FastingApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    switch(app->screen) {
    case ScreenMain:
        draw_main(canvas, app);
        break;
    case ScreenPickStart:
        draw_pick_start(canvas, app);
        break;
    case ScreenConfirmEnd:
        draw_confirm(canvas, app);
        break;
    case ScreenHistory:
        draw_history(canvas, app);
        break;
    }
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

/* ---------- input handling; returns false to exit ---------- */

static bool handle_input(FastingApp* app, const InputEvent* ev) {
    bool press = ev->type == InputTypeShort;
    bool repeatable = press || ev->type == InputTypeRepeat;
    bool changed = false;

    switch(app->screen) {
    case ScreenMain:
        if(press && ev->key == InputKeyBack) return false;
        if(press && ev->key == InputKeyOk) {
            app->pick_ts = 0;
            app->screen = app->data.fasting ? ScreenConfirmEnd : ScreenPickStart;
        } else if(press && ev->key == InputKeyLeft) {
            app->data.plan = (app->data.plan + 1) % PLAN_COUNT;
            if(app->data.fasting && elapsed_since(app->data.fast_start) < goal_seconds(app)) {
                app->data.goal_notified = 0;
            }
            changed = true;
        } else if(press && ev->key == InputKeyRight) {
            app->history_scroll = 0;
            app->history_sel = 0;
            app->screen = ScreenHistory;
        } else if(repeatable && app->data.fasting && ev->key == InputKeyUp) {
            fast_adjust_start(app, -ADJUST_STEP_S); /* started earlier -> more elapsed */
            changed = true;
        } else if(repeatable && app->data.fasting && ev->key == InputKeyDown) {
            fast_adjust_start(app, ADJUST_STEP_S);
            changed = true;
        }
        break;

    case ScreenPickStart:
    case ScreenConfirmEnd: {
        bool is_start = app->screen == ScreenPickStart;
        if(press && ev->key == InputKeyOk) {
            uint32_t ts = pick_resolve(app->pick_ts);
            if(is_start) {
                fast_start(app, ts);
            } else {
                fast_end(app, ts);
            }
            changed = true;
            app->screen = ScreenMain;
        } else if(press && (ev->key == InputKeyLeft || ev->key == InputKeyBack)) {
            app->screen = ScreenMain;
        } else if(repeatable && ev->key == InputKeyUp) {
            pick_earlier(&app->pick_ts, is_start ? earliest_start(app) : earliest_end(app));
        } else if(repeatable && ev->key == InputKeyDown) {
            pick_later(&app->pick_ts);
        }
        break;
    }

    case ScreenHistory:
        if(press && (ev->key == InputKeyBack || ev->key == InputKeyLeft)) {
            app->screen = ScreenMain;
        } else if(repeatable && ev->key == InputKeyUp) {
            if(app->history_sel > 0) app->history_sel--;
        } else if(repeatable && ev->key == InputKeyDown) {
            if(app->history_sel + 1 < app->data.history_count) app->history_sel++;
        } else if(ev->type == InputTypeLong && ev->key == InputKeyOk) {
            /* Hold OK to delete the selected entry */
            PersistState* d = &app->data;
            uint8_t i = app->history_sel;
            if(i < d->history_count) {
                memmove(
                    &d->history[i],
                    &d->history[i + 1],
                    sizeof(FastRecord) * (d->history_count - i - 1));
                d->history_count--;
                if(app->history_sel >= d->history_count && app->history_sel > 0) {
                    app->history_sel--;
                }
                changed = true;
            }
        }
        /* Keep selection visible */
        if(app->history_sel < app->history_scroll) app->history_scroll = app->history_sel;
        if(app->history_sel >= app->history_scroll + HISTORY_ROWS) {
            app->history_scroll = app->history_sel - HISTORY_ROWS + 1;
        }
        if(app->history_scroll > 0 &&
           app->history_scroll + HISTORY_ROWS > app->data.history_count) {
            app->history_scroll = app->data.history_count > HISTORY_ROWS ?
                                      app->data.history_count - HISTORY_ROWS :
                                      0;
        }
        break;
    }

    if(changed) state_save(&app->data);
    return true;
}

/* ---------- entry point ---------- */

int32_t fasting_tracker_app(void* p) {
    UNUSED(p);

    FastingApp* app = malloc(sizeof(FastingApp));
    memset(app, 0, sizeof(FastingApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->screen = ScreenMain;
    state_load(&app->data);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, app);
    view_port_input_callback_set(view_port, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    bool running = true;
    InputEvent event;
    while(running) {
        if(furi_message_queue_get(queue, &event, 500) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            running = handle_input(app, &event);
            furi_mutex_release(app->mutex);
        }

        /* Goal alert */
        bool notify = false;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(app->data.fasting && !app->data.goal_notified &&
           elapsed_since(app->data.fast_start) >= goal_seconds(app)) {
            app->data.goal_notified = 1;
            state_save(&app->data);
            notify = true;
        }
        furi_mutex_release(app->mutex);
        if(notify) {
            notification_message(notifications, &sequence_display_backlight_on);
            notification_message(notifications, &sequence_success);
        }

        view_port_update(view_port);
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
