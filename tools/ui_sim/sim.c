// Desktop simulator for the Yoto UI: runs firmware/main/ui/ui_core.c against a fake library
// and prints frames as text, or writes them as an image strip.
//
//   tools/ui_sim/run.sh <command>...
//
// Commands:  boot        library loaded (leaves the start-up status screen)
//            status:W    show a start-up status word      setup:12345678
//            msg:TEXT    scrolling message (use _ for spaces)
//            l+ l- lp lh left knob: turn right/left, press, hold
//            r+ r- rp rh right knob
//            both pw       both knobs (favourite), power hold
//            pwr plug unplug bat:N   power button touch; cable in or out; set level
//            card:X nocard a card put in (A = a book card, S = a sleep card, else new), taken out
//            time:H:M[:S]  set the time of day (the clock shows after 30 s idle)
//            dl:N dlrun dlhold  the first book's copy: N % arrived (-1 none), being fetched or held
//            face:1 face:0  lying on its face, or not
//            wake:1        the wake light on
//            wifi:0 wifi:1 Wi-Fi gone (offline mode) or back; books 0 and 5 are downloaded
//            endless       books play on for ever instead of ending
//            mN            advance N minutes
//            tN          advance N ticks (UI_FRAME_MS, 40 ms each)
//            show        print the frame
//            ppm:FILE    start recording every shown frame to FILE (a horizontal strip)
//
// In the text output a letter is a lit pixel: W white, O orange, G green, Y yellow, R red,
// V violet, T turquoise, lower case = dim, '.' = off.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "ui_core.h"

typedef struct {
    const char *title, *author, *series;
    double played, added;
    const char *chapters[6];
    int n;
} book_t;
// In title order, as the player's books are.
static const book_t LIB_KIDS[] = {
    { "Charlie and the Chocolate Factory", "Roald Dahl", "Charlie Bucket #1", 0, 50, { 0 }, 0 },
    { "Charlie and the Great Glass Elevator", "Roald Dahl", "Charlie Bucket #2", 300, 90, { 0 }, 0 },
    { "The Gruffalo", "Julia Donaldson", "Gruffalo #1", 200, 40, { "Chapter 1: The Fox", "Chapter 2: The Owl", "Chapter 3: The Snake" }, 3 },
    { "The Gruffalo's Child", "Julia Donaldson", "Gruffalo #2", 0, 80, { 0 }, 0 },
    { "Matilda", "Roald Dahl", "", 0, 20, { 0 }, 0 },
    { "Room on the Broom", "Julia Donaldson", "", 100, 10, { "Chapter 1: The Hat", "Chapter 2: The Bow", "Chapter 3: The Wand", "Chapter 4: The Dragon" }, 4 },
    { "Stick Man", "Julia Donaldson", "", 0, 30, { "Chapter 1", "Chp. 2", "Track #3" }, 3 },
    { "Zog", "Julia Donaldson", "", 0, 70, { 0 }, 0 },
};
static const book_t LIB_GROWNUPS[] = {
    { "Dune", "Frank Herbert", "Dune #1", 0, 5, { 0 }, 0 },
    { "Emma", "Jane Austen", "", 0, 6, { 0 }, 0 },
};
static const struct { const char *name; const book_t *books; int n; } LIBS[] = {
    { "Kids", LIB_KIDS, 8 }, { "Grown-ups", LIB_GROWNUPS, 2 },
};
static int s_library;
#define BOOKS (LIBS[s_library].books)
#define NBOOKS (LIBS[s_library].n)
#define CHAPTER_FRAMES 520

static int s_book = -1, s_chapter, s_pos, s_volume = 42, s_loading;
static bool s_endless;  // the book never reaches its end (for tests of long spans)
static ui_play_state_t s_state = UI_PLAY_IDLE;

int host_book_count(void) { return NBOOKS; }
const char *host_book_title(int i) { return BOOKS[i].title; }
const char *host_book_sort_title(int i) { const char *t = BOOKS[i].title; return strncmp(t, "The ", 4) ? t : t + 4; }
// Cards: "A" is a book card for The Gruffalo, "S" a 30-minute sleep card, anything else new.
static char s_slot[8], s_last_kind;      // card in the slot; kind of the last one seen
static struct { char uid[8]; char kind; int book, minutes; } s_cards[8] = { { "A", 'b', 2, 0 }, { "S", 's', 0, 30 } };
static int s_ncards = 2, s_card_book = -1, s_extend = 5;
static int card_slot(void) { for (int i = 0; i < s_ncards; i++) if (!strcmp(s_cards[i].uid, s_slot)) return i; return -1; }
ui_card_kind_t host_card(void)
{
    if (!s_slot[0]) return UI_CARD_NONE;
    const int i = card_slot();
    s_last_kind = i < 0 ? 'n' : s_cards[i].kind;
    if (i >= 0 && s_cards[i].kind == 'b') s_card_book = s_cards[i].book;
    return i < 0 ? UI_CARD_NEW : s_cards[i].kind == 's' ? UI_CARD_SLEEP : UI_CARD_BOOK;
}
int host_card_minutes(void) { const int i = card_slot(); return i < 0 ? 0 : s_cards[i].minutes; }
bool host_card_book_open(void) { return s_last_kind == 'b' && s_book >= 0 && s_book == s_card_book && s_state != UI_PLAY_IDLE; }
void host_resume(void) { if (s_state == UI_PLAY_PAUSED) { s_state = UI_PLAY_PLAYING; printf("  [host] resume\n"); } }
void host_play_book(int i);
void host_card_play(void) { if (host_card_book_open()) host_resume(); else host_play_book(s_card_book); }
static int card_put(char kind) { int i = card_slot(); if (i < 0) { i = s_ncards++; strcpy(s_cards[i].uid, s_slot); } s_cards[i].kind = kind; return i; }
bool host_card_link(int book)
{
    if (book < 0) book = s_state != UI_PLAY_IDLE ? s_book : -1;
    if (book < 0) return false;
    s_cards[card_put('b')].book = book;
    printf("  [host] card %s linked to %s\n", s_slot, BOOKS[book].title);
    return true;
}
void host_card_make_sleep(int m) { s_cards[card_put('s')].minutes = m; printf("  [host] card %s is a %d-minute sleep card\n", s_slot, m); }
void host_card_clear(void) { const int i = card_slot(); if (i >= 0) { s_cards[i] = s_cards[--s_ncards]; printf("  [host] card %s forgotten\n", s_slot); } }
void host_set_brightness(int percent) { printf("  [host] brightness %d\n", percent); }
static int s_vol_limit = 80;
static bool s_face;
static int s_light[5] = { 1, 0xFF0000, 50, 0, 10 };
void host_set_ring(unsigned rgb) { printf("  [host] ring %06X\n", rgb); }
bool host_face_down(void) { return s_face; }
int host_light_setting(ui_light_setting_t w) { return s_light[w]; }
void host_set_light_setting(ui_light_setting_t w, int v) { s_light[w] = v; printf("  [host] light setting %d = %d\n", w, v); }
static int s_bright[2] = { 80, 40 };
int host_bright_setting(ui_bright_t which) { return s_bright[which]; }
void host_set_bright_setting(ui_bright_t which, int percent) { s_bright[which] = percent; printf("  [host] bright setting %d = %d\n", which, percent); }
int host_volume_limit(void) { return s_vol_limit; }
void host_set_volume_limit(int percent) { s_vol_limit = percent; printf("  [host] volume limit %d\n", percent); }
static int s_auto_off = 60;
int host_auto_off(void) { return s_auto_off; }
void host_set_auto_off(int minutes) { s_auto_off = minutes; printf("  [host] auto-off %d\n", minutes); }
int host_sleep_extend(void) { return s_extend; }
void host_set_sleep_extend(int m) { s_extend = m; printf("  [host] sleep extension %d min\n", m); }
static ui_battery_t s_bat = { true, 66, false };
const char *host_setup_start(void) { printf("  [host] setup started\n"); return "4821"; }
bool host_setup_stop(void) { printf("  [host] setup stopped\n"); return true; }
void host_battery(ui_battery_t *out) { *out = s_bat; }
bool host_on_battery(void) { return !s_bat.plugged; }
int host_battery_alarm(void) { return s_bat.plugged ? 0 : s_bat.percent <= 2 ? 2 : s_bat.percent <= 10 ? 1 : 0; }
// Offline mode: wifi:0 / wifi:1. Books 0 and 5 have complete copies.
static bool s_no_wifi;
bool host_offline(void) { return s_no_wifi; }
bool host_book_downloaded(int index) { return index == 0 || index == 5; }
// The copy of book 1 (the first in the home list): dl:N sets how much has arrived (-1 = none
// wanted or complete), dlrun / dlhold whether it is being fetched.
static int s_dl = -1;
static bool s_dl_run;
int host_book_offline(int index, bool *fetching) { *fetching = index == 1 && s_dl_run; return index == 1 ? s_dl : -1; }
static int s_time = -1;                           // seconds since midnight; -1 = not known
static int s_clock[4] = { 0, 7 * 60, 19 * 60, 15 };
bool host_time(int *second_of_day) { if (s_time < 0) return false; *second_of_day = s_time; return true; }
int host_clock_setting(ui_clock_setting_t which) { return s_clock[which]; }
void host_set_clock_setting(ui_clock_setting_t which, int value) { s_clock[which] = value; printf("  [host] clock setting %d = %d\n", which, value); }
static int s_speed = 12;
int host_scroll_speed(void) { return s_speed; }
void host_set_scroll_speed(int px) { s_speed = px; printf("  [host] scroll speed %d px/s\n", px); }
static bool s_readable;
bool host_readable(void) { return s_readable; }
void host_set_readable(bool on) { s_readable = on; printf("  [host] readability %s\n", on ? "on" : "off"); }
static bool s_cover_view = true;
bool host_cover_view(void) { return s_cover_view; }
void host_set_cover_view(bool on) { s_cover_view = on; printf("  [host] cover view %s\n", on ? "on" : "off"); }
const char *host_book_author(int i) { return BOOKS[i].author; }
const char *host_book_series(int i) { return BOOKS[i].series; }
double host_book_last_played(int i) { return BOOKS[i].played; }
double host_book_added(int i) { return BOOKS[i].added; }
const char *host_chapter_title(int i) { return s_book >= 0 && i >= 0 && i < BOOKS[s_book].n ? BOOKS[s_book].chapters[i] : ""; }
int host_library_count(void) { return 2; }
const char *host_library_name(int i) { return LIBS[i].name; }
int host_current_library(void) { return s_library; }
void host_select_library(int i) { printf("  [host] library -> %s\n", LIBS[i].name); s_library = i; s_book = -1; s_state = UI_PLAY_IDLE; }
int host_current_book(void)
{
    if (s_book >= 0) return s_book;
    int best = -1;
    for (int i = 0; i < NBOOKS; i++) if (BOOKS[i].played > 0 && (!s_no_wifi || host_book_downloaded(i)) && (best < 0 || BOOKS[i].played > BOOKS[best].played)) best = i;
    return best;
}
void host_play_book(int i) { s_book = i; s_chapter = 0; s_pos = 0; s_state = UI_PLAY_LOADING; s_loading = 24; printf("  [host] play \"%s\"\n", BOOKS[i].title); }
// A made-up cover: a sun over hills, tinted per book. "Zog" has none, to show the fallback.
const unsigned char *host_book_cover(int book)
{
    static unsigned char px[768];
    if (book < 0 || !strcmp(BOOKS[book].title, "Zog")) return NULL;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        unsigned char *p = px + (y * 16 + x) * 3;
        const int sun = (x - 11) * (x - 11) + (y - 4) * (y - 4) < 10, hill = y > 9 + ((x * 5 + book * 3) % 7) / 3;
        p[0] = sun ? 255 : hill ? 20 : 30 + book * 40; p[1] = sun ? 210 : hill ? 150 - y * 5 : 60; p[2] = sun ? 60 : hill ? 40 : 200 - y * 8;
    }
    return px;
}
const unsigned char *host_cover(void) { return host_book_cover(s_book); }

void host_toggle_play(void) { s_state = s_state == UI_PLAY_PLAYING ? UI_PLAY_PAUSED : UI_PLAY_PLAYING; printf("  [host] %s\n", s_state == UI_PLAY_PLAYING ? "play" : "pause"); }
void host_pause(void) { if (s_state == UI_PLAY_PLAYING) { s_state = UI_PLAY_PAUSED; printf("  [host] pause\n"); } }
void host_set_volume(int v) { s_volume = v; printf("  [host] volume %d\n", v); }
void host_chapter_step(int d) { if (s_book >= 0 && d) { s_chapter += d; s_pos = 0; printf("  [host] chapter -> %d\n", s_chapter + 1); } }
static bool s_fav[2][16];
bool host_book_favourite(int i) { return i >= 0 && s_fav[s_library][i]; }
void host_toggle_favourite(int i) { s_fav[s_library][i] = !s_fav[s_library][i]; printf("  [host] favourite %s: %s\n", s_fav[s_library][i] ? "on" : "off", BOOKS[i].title); }
void host_power_off(void) { printf("  [host] power off\n"); }

void host_get_playback(ui_playback_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->volume = s_volume;
    out->chapter_index = -1;
    out->title = out->chapter = "";
    if (s_book < 0) return;
    const book_t *b = &BOOKS[s_book];
    out->title = b->title;
    out->chapter_count = b->n;
    if (b->n) { out->chapter_index = s_chapter; out->chapter = b->chapters[s_chapter]; }
    out->chapter_progress = (float)s_pos / CHAPTER_FRAMES;
}

static void host_tick(void)
{
    if (s_state == UI_PLAY_LOADING && --s_loading <= 0) s_state = UI_PLAY_PLAYING;
    if (s_state != UI_PLAY_PLAYING) return;
    if (++s_pos >= CHAPTER_FRAMES) {
        s_pos = 0;
        if (s_chapter < BOOKS[s_book].n - 1) s_chapter++;
        else if (!s_endless) s_state = UI_PLAY_PAUSED;
    }
}

static char letter(display_px_t p)
{
    const int max = p.r > p.g ? (p.r > p.b ? p.r : p.b) : (p.g > p.b ? p.g : p.b);
    if (max < 8) return '.';
    char c;
    if (p.r > 200 && p.g > 200 && p.b > 180) c = 'W';
    else if (abs(p.r - p.g) < 24 && abs(p.g - p.b) < 24) c = 'W';
    else if (p.b >= p.r && p.b >= p.g) c = p.r > p.g ? 'V' : 'T';
    else if (p.g >= p.r) c = p.b * 5 > p.g * 4 ? 'T' : 'G';
    else if (p.g > p.r * 3 / 4) c = 'Y';
    else if (p.g > p.r * 2 / 5) c = 'O';
    else c = 'R';
    return max < 130 ? c + 32 : c;
}

static FILE *s_ppm;
static display_px_t *s_strip;
static int s_frames;

static void show(const char *label)
{
    const display_px_t *f = gfx_frame();
    printf("%s\n", label);
    for (int y = 0; y < DISPLAY_H; y++) {
        printf("    ");
        for (int x = 0; x < DISPLAY_W; x++) putchar(letter(f[y * DISPLAY_W + x]));
        putchar('\n');
    }
    if (s_ppm) {
        s_strip = realloc(s_strip, (s_frames + 1) * 256 * sizeof(display_px_t));
        memcpy(s_strip + s_frames * 256, f, 256 * sizeof(display_px_t));
        s_frames++;
    }
}

// Frames side by side, each pixel as a 10 x 10 cell with a dark gap, like the LED matrix.
static void write_ppm(void)
{
    const int cell = 10, pad = 12, fw = 16 * cell + pad, W = s_frames * fw + pad, H = 16 * cell + 2 * pad;
    fprintf(s_ppm, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned char rgb[3] = { 6, 6, 6 };
            const int fx = (x - pad) / fw, lx = (x - pad) - fx * fw, ly = y - pad;
            if (x >= pad && fx < s_frames && lx < 16 * cell && ly >= 0 && ly < 16 * cell &&
                lx % cell < cell - 2 && ly % cell < cell - 2) {
                const display_px_t p = s_strip[fx * 256 + (ly / cell) * 16 + lx / cell];
                rgb[0] = p.r < 22 ? 22 : p.r; rgb[1] = p.g < 22 ? 22 : p.g; rgb[2] = p.b < 22 ? 22 : p.b;
            }
            fwrite(rgb, 1, 3, s_ppm);
        }
    }
    fclose(s_ppm);
}

static void tick(int n)
{
    for (int i = 0; i < n; i++) { host_tick(); ui_core_tick(); }
}

int main(int argc, char **argv)
{
    ui_core_init();
    tick(1);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        ui_event_t ev = { 0 };
        int is_event = 1;
        if (!strcmp(a, "l+")) ev = (ui_event_t){ UI_EV_L_TURN, 1 };
        else if (!strcmp(a, "l-")) ev = (ui_event_t){ UI_EV_L_TURN, -1 };
        else if (!strcmp(a, "lp")) ev.type = UI_EV_L_PRESS;
        else if (!strcmp(a, "lh")) ev.type = UI_EV_L_HOLD;
        else if (!strcmp(a, "r+")) ev = (ui_event_t){ UI_EV_R_TURN, 1 };
        else if (!strcmp(a, "r-")) ev = (ui_event_t){ UI_EV_R_TURN, -1 };
        else if (!strcmp(a, "rp")) ev.type = UI_EV_R_PRESS;
        else if (!strcmp(a, "rh")) ev.type = UI_EV_R_HOLD;
        else if (!strcmp(a, "both")) ev.type = UI_EV_BOTH;
        else if (!strcmp(a, "pw")) ev.type = UI_EV_POWER_HOLD;
        else if (!strncmp(a, "card:", 5)) { snprintf(s_slot, sizeof(s_slot), "%s", a + 5); ev.type = UI_EV_CARD_IN; }
        else if (!strcmp(a, "nocard")) { s_slot[0] = 0; ev.type = UI_EV_CARD_OUT; }
        else if (!strcmp(a, "pwr")) ev.type = UI_EV_POWER_PRESS;
        else if (!strcmp(a, "plug")) { s_bat.plugged = true; ev.type = UI_EV_PLUGGED; }
        else if (!strcmp(a, "unplug")) { s_bat.plugged = false; ev.type = UI_EV_UNPLUGGED; }
        else if (!strcmp(a, "endless")) { s_endless = true; continue; }
        else if (!strncmp(a, "face:", 5)) { s_face = a[5] == '1'; continue; }
        else if (!strncmp(a, "wake:", 5)) { s_light[3] = a[5] == '1'; continue; }
        else if (!strncmp(a, "wifi:", 5)) { s_no_wifi = a[5] == '0'; continue; }
        else if (!strncmp(a, "dl:", 3)) { s_dl = atoi(a + 3); continue; }
        else if (!strcmp(a, "dlrun")) { s_dl_run = true; continue; }
        else if (!strcmp(a, "dlhold")) { s_dl_run = false; continue; }
        else if (!strncmp(a, "bat:", 4)) { s_bat.percent = atoi(a + 4); continue; }
        else if (!strncmp(a, "time:", 5)) { int h = 0, m = 0, sec = 0; sscanf(a + 5, "%d:%d:%d", &h, &m, &sec); s_time = h * 3600 + m * 60 + sec; continue; }
        else is_event = 0;
        if (is_event) { ui_core_event(&ev); tick(1); continue; }

        if (!strcmp(a, "boot")) { ui_core_books_changed(); tick(1); }
        else if (!strncmp(a, "status:", 7)) { ui_core_status(a + 7, 0xFFD166); tick(1); }
        else if (!strncmp(a, "setup:", 6)) { ui_core_setup(a + 6); tick(1); }
        else if (!strncmp(a, "msg:", 4)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "%s", a + 4);
            for (char *p = buf; *p; p++) if (*p == '_') *p = ' ';
            ui_core_message(buf); tick(1);
        }
        else if (a[0] == 't' && a[1] >= '0' && a[1] <= '9') tick(atoi(a + 1));
        else if (a[0] == 'm' && a[1] >= '0' && a[1] <= '9') tick(atoi(a + 1) * (60000 / UI_FRAME_MS));
        else if (!strcmp(a, "show")) { char l[64]; snprintf(l, sizeof(l), "-- after: %s", i > 1 ? argv[i - 1] : "start"); show(l); }
        else if (!strncmp(a, "ppm:", 4)) s_ppm = fopen(a + 4, "wb");
        else { fprintf(stderr, "unknown command '%s'\n", a); return 2; }
    }
    if (s_ppm) write_ppm();
    return 0;
}
