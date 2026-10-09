#include "ui_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "gfx.h"

typedef enum {
    M_STATUS, M_SETUP, M_PLAYER, M_CHAPTER, M_MENU, M_GROUPS, M_BOOKS, M_SLEEP,
    M_SETTINGS, M_LIBS, M_VIEW, M_READ, M_SPEED, M_CARD, M_CARD_MIN, M_EXT, M_CLOCK_SET, M_CLOCK_PICK, M_AUTO_OFF, M_VOL_LIMIT, M_LIGHT_SET, M_LIGHT_PICK, M_OFF
} screen_t;
typedef enum {
    OV_NONE, OV_VOLUME, OV_PLAYPAUSE, OV_SLEEP, OV_WORD, OV_MESSAGE, OV_HEART, OV_BATTERY, OV_WIFI
} overlay_t;
// Ways to look through the books. These are also the first six menu items.
typedef enum { V_LAST, V_NEW, V_FAV, V_DL, V_TITLE, V_AUTHOR, V_SERIES, V_COUNT } view_t;
// The one menu: six ways to find a book, then the sleep timer and settings. (Chapters
// are reached by turning the right knob on the player screen.)
enum { MENU_SLEEP = V_COUNT, MENU_SETTINGS, MENU_ITEMS };
// Settings: which library, cover art or text, readability mode, the card in the slot,
// and how long a finished sleep card adds when it is put back.
enum { SET_LIBRARY, SET_STYLE, SET_READABLE, SET_SPEED, SET_CARD, SET_EXTEND, SET_CLOCK, SET_AUTO_OFF, SET_VOL_LIMIT, SET_LIGHT, SET_WIFI, SET_ITEMS };

#define MS(ms)         ((ms) / UI_FRAME_MS)  // a time in ticks
#define OVERLAY_FRAMES MS(1440)
#define HOLD_FRAMES    MS(1120)  // pause before a marquee starts
#define VOLUME_STEP    6
#define NEW_LIMIT      50   // "recently added" shows this many
#define HEART_FRAMES   MS(1280)  // the favourite animation

static const char *const MENU[MENU_ITEMS] = { "LAST", "NEW", "FAV", "DOWN", "A-Z", "AUTH", "SERI", "BED", "SET" };
static const char *const SETTINGS[SET_ITEMS] = { "LIB", "VIEW", "READ", "SPD", "CARD", "EXT", "CLK", "AUTO", "VOL", "LITE", "WIFI" };
// Scroll speeds, in pixels a second. MED is what it has always been.
static const char *const SPEED_LABEL[] = { "MIN", "SLOW", "MED", "FAST" };
static const int SPEED_PX[] = { 5, 8, 12, 18 };
#define SPEED_OPTIONS 4
static const char *const CARD_ACTIONS[] = { "SLP", "CLR" };  // make it a sleep card; forget it
static const int CARD_MINUTES[] = { 5, 10, 15, 20, 25, 30, 45, 60 };
#define CARD_MINUTE_OPTIONS 8
static const int EXTEND_MINUTES[] = { 5, 10, 15, 20, 30 };
#define EXTEND_OPTIONS 5
// The clock's settings: 12 or 24 hours, when the sun starts to rise, when it has set, and
// how long each of those takes.
static const char *const CLOCK_ITEMS[] = { "HOUR", "RISE", "DUSK", "FADE" };
static const ui_clock_setting_t CLOCK_KEYS[] = { UI_CLOCK_24H, UI_CLOCK_RISE, UI_CLOCK_SET, UI_CLOCK_FADE };
#define CLOCK_ITEM_COUNT 4
static const int FADE_MINUTES[] = { 5, 10, 15, 20, 30, 45, 60 };
#define FADE_OPTIONS 7
#define RISE_FIRST (4 * 60)   // sunrise is offered from 4:00 to 10:00,
#define DUSK_FIRST (16 * 60)  // sunset from 16:00 to 22:00, in quarter hours
#define TIME_OPTIONS 25
// Switching itself off when left alone on battery: after this many minutes (0 = never).
static const int AUTO_OFF_MINUTES[] = { 15, 30, 60, 120, 0 };
#define AUTO_OFF_OPTIONS 5
// The loudest the player will go, percent of what it can do: 10 to 100 in tens. The volume
// knob and its display still run the whole way from 0 to 100, spread over that range.
#define VOL_LIMIT_OPTIONS 10
// The LED ring: the nightlight when the player lies on its face, and the wake light that
// rises with the clock's sun. Its settings, under SET > LITE.
static const char *const LIGHT_ITEMS[] = { "NITE", "COL", "BRT", "WAKE", "HOLD" };
static const ui_light_setting_t LIGHT_KEYS[] = { UI_LIGHT_NIGHT, UI_LIGHT_COLOUR, UI_LIGHT_BRIGHT, UI_LIGHT_WAKE, UI_LIGHT_HOLD };
#define LIGHT_ITEM_COUNT 5
static const char *const COLOUR_LABEL[] = { "RED", "ORNG", "YELW", "GRN", "CYAN", "BLUE", "PURP", "PINK", "WHIT" };
static const unsigned COLOUR_RGB[] = { 0xFF0000, 0xFF5A00, 0xFFC000, 0x00FF00, 0x00C8FF, 0x0000FF, 0x8C00FF, 0xFF2D90, 0xFFFFFF };
#define COLOUR_OPTIONS 9
static const int HOLD_MINUTES[] = { 5, 10, 15 };
#define HOLD_OPTIONS 3
#define WAKE_RGB 0xFF6A14  // a warm orange, sunrise
// Display brightness, percent: by day and by night (it follows the clock's sunrise and
// sunset), a quarter of that once no knob has been touched for a minute, and barely lit in
// sleep mode.
#define BRIGHT_DAY   80
#define BRIGHT_NIGHT 40
#define BRIGHT_SLEEP 3
#define DIM_IDLE     MS(60000)
#define CLOCK_IDLE MS(30000)  // with nothing playing and no knob touched, the clock comes up
static const char *const ON_OFF[] = { "ON", "OFF" };
static const char *const VIEW_STYLE[] = { "ART", "TEXT" };
static const char *const SLEEP_LABEL[] = { "OFF", "15M", "30M", "60M" };
static const int SLEEP_MINUTES[] = { 0, 15, 30, 60 };
#define SLEEP_OPTIONS 4

static screen_t s_mode;
static overlay_t s_overlay;
static unsigned s_t;          // frames since start
static unsigned s_mode_t;     // frame the current screen (or its selection) last changed
static unsigned s_overlay_until;
static char s_text[160];      // status word, setup password or message
static gfx_color_t s_text_color;
static char s_word[5];        // OV_WORD
static bool s_heart_on;       // OV_HEART: just made a favourite (else just unmade)
static ui_battery_t s_battery; // OV_BATTERY: as read when the overlay opened
static unsigned s_heart_start;
static unsigned s_message_start;

static int s_menu;
static view_t s_view;         // the one the lists below were built for
static int *s_order;          // book indices in the view's order
static int s_order_n;
static int *s_group_first;    // V_AUTHOR / V_SERIES / V_TITLE: where each group starts in s_order
static int s_group_n;
static int s_group;           // highlighted / opened group
static int s_first, s_count;  // the book list on show: s_order[s_first .. s_first + s_count)
static int s_sel;             // highlighted book within it
static int s_lib;             // highlighted library
static int s_style;           // highlighted list style: 0 = cover art, 1 = text
static int s_setting;         // highlighted settings item
static int s_readable;        // highlighted readability choice: 0 = on, 1 = off
static int s_chapter_pick;    // the chapter being offered on the chapter screen
static int s_sleep;           // selected option
static int s_sleep_sel;
static unsigned s_sleep_at;   // frame at which to pause; 0 = no timer
static int s_pick;            // highlighted item on the small settings lists
static int s_clock_item;       // which of CLOCK_ITEMS is being set
static bool s_clock_on;        // the clock has taken the display
static bool s_clock_held;      // ...because it was asked for, so playing does not dismiss it
static unsigned s_last_input;  // when a knob was last touched
static unsigned s_last_touch;  // when anything was last done to the player (for dimming)
static unsigned s_last_active; // ...or it was last playing or on external power (for switching off)
static bool s_sleep_dim;       // sleep mode has the display barely lit
static bool s_low_warned;      // the nearly-empty battery has been shown
static unsigned s_flat_at;     // when to switch off for an empty battery; 0 = not
static int s_brightness = -1;  // what the display was last set to
static bool s_offline;          // no Wi-Fi for a while: only downloaded books are on offer
static int s_light_item;       // which of LIGHT_ITEMS is being set
static bool s_face_down;       // lying on its face: display off, nightlight on
static int s_wake;             // the wake light now, 0 (off) to 256 (full); 0 when not in its time
static bool s_wake_cancelled;  // a button was pressed during this morning's wake light
static unsigned s_ring;        // what the ring was last set to (it starts dark)
static bool s_setup_from_settings;  // setup was opened from SET (so hold can leave it)

// Start-up. The animation plays for at least BOOT_MIN before the books are shown, which
// also gives Wi-Fi, the library and the covers a head start; if start-up is still going
// after BOOT_WORD it shrinks to make room for the word saying what is being waited for.
#define BOOT_MIN  MS(3000)
#define BOOT_WORD MS(12000)
static bool s_booting = true;
static bool s_books_waiting;  // the books arrived before BOOT_MIN

// Cards.
static ui_card_kind_t s_card;       // what is in the slot (as of the last card event)
static bool s_card_pending;         // a card arrived before the library had loaded
// Sleep mode, started by a sleep card: the display is dimmed and the timer runs while the
// card is in. Taking the card out pauses the book and stops the clock.
static bool s_sleep_mode;
static unsigned s_sleep_left;       // frames left on the clock while the card is out
static bool s_sleep_done;           // the timer ran out (so the next insert extends it)
static char s_sleep_word[5];        // what the sleep overlay shows, e.g. "30M"

static ui_playback_t s_pb;

static int clampi(int v, int lo, int hi) { return v > hi ? hi : v < lo ? lo : v; }
// Lists go round: past the last item is the first, and before the first is the last.
static int wrapi(int v, int n) { return n > 0 ? ((v % n) + n) % n : 0; }

static void go(screen_t mode)
{
    s_mode = mode;
    s_mode_t = s_t;
    s_overlay = OV_NONE;
}

static void overlay(overlay_t ov, int frames)
{
    s_overlay = ov;
    s_overlay_until = s_t + frames;
}

static void word(const char *w)
{
    snprintf(s_word, sizeof(s_word), "%s", w);
    overlay(OV_WORD, OVERLAY_FRAMES);
}

static bool has_book(void)
{
    return s_pb.state != UI_PLAY_IDLE;
}

/* Readability mode: reword names so that the part that tells them apart comes first. */

// "The Gruffalo" -> "Gruffalo, The".
static const char *readable_title(const char *t, char *buf, size_t len)
{
    if (!host_readable() || strncasecmp(t, "The ", 4) != 0 || !t[4]) return t;
    snprintf(buf, len, "%s, The", t + 4);
    return buf;
}

// "Chapter 12: The Wand" -> "12: The Wand"; likewise Chap, Chp, Ch, Track and Trk, with or
// without a full stop or a '#'. Only when a number follows the word.
static const char *readable_chapter(const char *t, char *buf, size_t len)
{
    static const char *const words[] = { "chapter", "chap", "chp", "ch", "track", "trk" };
    if (!host_readable()) return t;
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        const size_t n = strlen(words[i]);
        if (strncasecmp(t, words[i], n) != 0) continue;
        const char *p = t + n;
        while (*p == '.' || *p == ' ' || *p == '#') p++;
        if (*p >= '0' && *p <= '9') {
            snprintf(buf, len, "%s", p);
            return buf;
        }
    }
    return t;
}

// The player's state, with the names as they are to be shown.
static void get_playback(void)
{
    static char title[160], chapter[120];
    host_get_playback(&s_pb);
    if (s_pb.title) s_pb.title = readable_title(s_pb.title, title, sizeof(title));
    if (s_pb.chapter) s_pb.chapter = readable_chapter(s_pb.chapter, chapter, sizeof(chapter));
}

/* Views: orderings and groupings of the books */

// Authors and series are groups you open; A-Z is grouped by first letter, which is a way
// of jumping into the one long list rather than a list of its own.
static bool grouped(view_t v) { return v == V_AUTHOR || v == V_SERIES || v == V_TITLE; }
static bool opens_sublist(view_t v) { return v == V_AUTHOR || v == V_SERIES; }

// The name a book is grouped under: its author, or its (first) series without the "#3".
static void group_key(view_t v, int book, char *out, size_t len)
{
    if (v == V_TITLE) {
        // The first letter the title is filed under; everything else goes under "#".
        const char *t = host_book_sort_title(book);
        while (*t == ' ' || *t == '"' || *t == '\'' || *t == '(') t++;
        char c = *t;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        snprintf(out, len, "%c", c >= 'A' && c <= 'Z' ? c : '#');
        return;
    }
    const char *s = v == V_AUTHOR ? host_book_author(book) : host_book_series(book);
    size_t n = 0;
    if (v == V_SERIES) {
        const char *hash = strstr(s, " #"), *comma = strstr(s, ", ");
        const char *end = s + strlen(s);
        if (hash && hash < end) end = hash;
        if (comma && comma < end) end = comma;
        n = end - s;
    } else {
        n = strlen(s);
    }
    if (n >= len) n = len - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static double series_number(int book)
{
    const char *hash = strstr(host_book_series(book), " #");
    return hash ? atof(hash + 2) : 0;
}

static view_t s_sort_view;  // qsort has no context argument

static int compare(const void *pa, const void *pb)
{
    const int a = *(const int *)pa, b = *(const int *)pb;
    switch (s_sort_view) {
    case V_LAST: {
        const double d = host_book_last_played(b) - host_book_last_played(a);
        if (d != 0) return d > 0 ? 1 : -1;
        break;
    }
    case V_NEW: {
        const double d = host_book_added(b) - host_book_added(a);
        if (d != 0) return d > 0 ? 1 : -1;
        break;
    }
    case V_AUTHOR:
    case V_SERIES: {
        char ka[96], kb[96];
        group_key(s_sort_view, a, ka, sizeof(ka));
        group_key(s_sort_view, b, kb, sizeof(kb));
        const int c = strcasecmp(ka, kb);
        if (c) return c;
        if (s_sort_view == V_SERIES) {
            const double d = series_number(a) - series_number(b);
            if (d != 0) return d > 0 ? 1 : -1;
        }
        break;
    }
    default:
        break;
    }
    return a - b;  // the books arrive in title order
}

// Builds s_order (and the groups) for a view. Returns false if the view has nothing in it.
static bool build_view(view_t v)
{
    const int total = host_book_count();
    free(s_order);
    free(s_group_first);
    s_order = malloc((total ? total : 1) * sizeof(int));
    s_group_first = malloc((total ? total : 1) * sizeof(int));
    s_order_n = s_group_n = 0;
    s_view = v;
    if (!s_order || !s_group_first) return false;

    for (int i = 0; i < total; i++) {
        if (v == V_LAST && host_book_last_played(i) <= 0) continue;
        if (v == V_FAV && !host_book_favourite(i)) continue;
        // Downloaded: the books with a complete copy on the SD card. And offline, with no
        // server to play anything else from, those are all that any list shows.
        if ((v == V_DL || s_offline) && !host_book_downloaded(i)) continue;
        if (v == V_AUTHOR && !host_book_author(i)[0]) continue;
        if (v == V_SERIES && !host_book_series(i)[0]) continue;
        s_order[s_order_n++] = i;
    }
    s_sort_view = v;
    if (v != V_TITLE) qsort(s_order, s_order_n, sizeof(int), compare);
    if (v == V_NEW && s_order_n > NEW_LIMIT) s_order_n = NEW_LIMIT;

    if (grouped(v)) {
        char prev[96] = "", key[96];
        for (int i = 0; i < s_order_n; i++) {
            group_key(v, s_order[i], key, sizeof(key));
            if (i == 0 || strcasecmp(key, prev) != 0) {
                s_group_first[s_group_n++] = i;
                snprintf(prev, sizeof(prev), "%s", key);
            }
        }
    }
    return s_order_n > 0;
}

static int group_size(int g)
{
    return (g + 1 < s_group_n ? s_group_first[g + 1] : s_order_n) - s_group_first[g];
}

// The group (author, series or letter) that position `pos` of s_order falls in.
static int group_of(int pos)
{
    int g = 0;
    while (g + 1 < s_group_n && s_group_first[g + 1] <= pos) g++;
    return g;
}

// Shows the book list: one author's or series' books, or the whole view. For A-Z a group
// is a letter, and choosing it shows the whole list starting at that letter.
static void show_books(int group)
{
    const int current = host_current_book();
    if (group >= 0 && opens_sublist(s_view)) {
        s_group = group;
        s_first = s_group_first[group];
        s_count = group_size(group);
    } else {
        s_first = 0;
        s_count = s_order_n;
    }
    s_sel = 0;
    if (group >= 0 && !opens_sublist(s_view)) {
        s_group = group;
        s_sel = s_group_first[group];
    } else {
        // Start on the book that is playing (or was last), if it is in this list.
        for (int i = 0; i < s_count; i++) {
            if (s_order[s_first + i] == current) s_sel = i;
        }
    }
    go(M_BOOKS);
}

// The home screen when nothing is playing: recently played if there is any, else A-Z.
// Offline it is the downloaded books (empty or not: there is nothing else to offer).
static void show_home(void)
{
    if (s_offline) build_view(V_DL);
    else if (!build_view(V_LAST)) build_view(V_TITLE);
    s_menu = s_view;
    show_books(-1);
}

static int selected_book(void)
{
    return s_count > 0 ? s_order[s_first + s_sel] : -1;
}

/* The chapter screen: reached by turning the right knob on the player. It shows the
   number of the chapter on offer and, if the chapter has a name worth showing, scrolls it
   once. Pressing goes to that chapter; leaving it alone goes back with nothing changed. */

// Back to the player as it was, without scrolling the title and chapter past again.
static void settle_player(void)
{
    go(M_PLAYER);
    s_mode_t = s_t - 1000000;
}

// A chapter's name, without a leading "Chapter 12" (or Chap, Chp, Ch, C, Track, Trk, with or
// without a full stop or '#') since the number is already on show. NULL if nothing is left,
// or the name is only a number.
static const char *chapter_name(int index)
{
    static const char *const words[] = { "chapter", "chap", "chp", "ch", "c", "track", "trk" };
    const char *t = host_chapter_title(index);
    for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        const size_t n = strlen(words[i]);
        if (strncasecmp(t, words[i], n) != 0) continue;
        const char *p = t + n;
        while (*p == '.' || *p == ' ' || *p == '#') p++;
        if (*p < '0' || *p > '9') continue;
        while (*p >= '0' && *p <= '9') p++;
        t = p;
        break;
    }
    const char *rest = t;
    while (*rest == ' ' || *rest == ':' || *rest == '-' || *rest == '.' || *rest == ',') rest++;
    const char *digits = rest;
    while (*digits >= '0' && *digits <= '9') digits++;
    return *rest && *digits ? rest : NULL;  // something there, and not only digits
}

static unsigned minutes_to_frames(int minutes);
static unsigned scroll_ticks(int px);

// How long the chapter screen waits for a press: long enough for the name to scroll past.
static unsigned chapter_timeout(void)
{
    const char *name = chapter_name(s_chapter_pick);
    const unsigned scroll = name && gfx_text_width(name) > DISPLAY_W
                                ? HOLD_FRAMES + scroll_ticks((int)strlen(name) * GFX_CHAR_W) : 0;
    return scroll + MS(3000);
}

static int clock_options(void);
static int light_options(void);
static int daylight(int second_of_day);

/* Events */

static void set_text(const char *s);

static void end_sleep_mode(void);

static void right_turn(int d)
{
    switch (s_mode) {
    case M_PLAYER:
        // Turning offers another chapter; nothing changes unless the knob is then pressed.
        if (s_pb.chapter_count > 1) {
            s_chapter_pick = wrapi(s_pb.chapter_index + d, s_pb.chapter_count);
            go(M_CHAPTER);
        }
        return;
    case M_CHAPTER:  s_chapter_pick = wrapi(s_chapter_pick + d, s_pb.chapter_count); break;
    case M_MENU:     s_menu = wrapi(s_menu + d, MENU_ITEMS); break;
    case M_GROUPS:   s_group = wrapi(s_group + d, s_group_n); break;
    case M_BOOKS:    s_sel = wrapi(s_sel + d, s_count); break;
    case M_SLEEP:    s_sleep_sel = wrapi(s_sleep_sel + d, SLEEP_OPTIONS); break;
    case M_LIBS:     s_lib = wrapi(s_lib + d, host_library_count()); break;
    case M_VIEW:     s_style = wrapi(s_style + d, 2); break;
    case M_SETTINGS: s_setting = wrapi(s_setting + d, SET_ITEMS); break;
    case M_READ:     s_readable = wrapi(s_readable + d, 2); break;
    case M_CARD:     s_pick = wrapi(s_pick + d, 2); break;
    case M_CARD_MIN: s_pick = wrapi(s_pick + d, CARD_MINUTE_OPTIONS); break;
    case M_EXT:      s_pick = wrapi(s_pick + d, EXTEND_OPTIONS); break;
    case M_AUTO_OFF: s_pick = wrapi(s_pick + d, AUTO_OFF_OPTIONS); break;
    case M_VOL_LIMIT: s_pick = wrapi(s_pick + d, VOL_LIMIT_OPTIONS); break;
    case M_LIGHT_SET:  s_light_item = wrapi(s_light_item + d, LIGHT_ITEM_COUNT); break;
    case M_LIGHT_PICK: s_pick = wrapi(s_pick + d, light_options()); break;
    case M_SPEED:    s_pick = wrapi(s_pick + d, SPEED_OPTIONS); break;
    case M_CLOCK_SET:  s_clock_item = wrapi(s_clock_item + d, CLOCK_ITEM_COUNT); break;
    case M_CLOCK_PICK: s_pick = wrapi(s_pick + d, clock_options()); break;
    default: return;
    }
    s_mode_t = s_t;
}

static void open_menu(int item)
{
    s_menu = item;
    go(M_MENU);
}

static void menu_select(void)
{
    if (s_menu < V_COUNT) {
        if (!build_view((view_t)s_menu)) {
            word("NONE");
        } else if (grouped(s_view)) {
            // Start on the letter (or author, or series) of the book that is playing.
            s_group = 0;
            for (int i = 0; i < s_order_n; i++) {
                if (s_order[i] == host_current_book()) s_group = group_of(i);
            }
            go(M_GROUPS);
        } else {
            show_books(-1);
        }
    } else if (s_menu == MENU_SLEEP) {
        s_sleep_sel = s_sleep;
        go(M_SLEEP);
    } else {
        go(M_SETTINGS);
    }
}

static int light_options(void)
{
    switch (s_light_item) {
    case 1:  return COLOUR_OPTIONS;
    case 2:  return 10;  // 10 to 100 %
    case 4:  return HOLD_OPTIONS;
    default: return 2;   // on / off
    }
}

// The value that option `i` of the light setting being chosen stands for.
static int light_option_value(int i)
{
    switch (s_light_item) {
    case 1:  return (int)COLOUR_RGB[i];
    case 2:  return (i + 1) * 10;
    case 4:  return HOLD_MINUTES[i];
    default: return i == 0;  // ON first
    }
}

static int clock_options(void)
{
    return s_clock_item == 0 ? 2 : s_clock_item == 3 ? FADE_OPTIONS : TIME_OPTIONS;
}

// The value that option `i` of the clock setting being chosen stands for.
static int clock_option_value(int i)
{
    switch (s_clock_item) {
    case 0:  return i;  // 0 = 12-hour, 1 = 24-hour
    case 1:  return RISE_FIRST + 15 * i;
    case 2:  return DUSK_FIRST + 15 * i;
    default: return FADE_MINUTES[i];
    }
}

static void settings_select(void)
{
    if (s_setting == SET_STYLE) {
        s_style = host_cover_view() ? 0 : 1;
        go(M_VIEW);
    } else if (s_setting == SET_READABLE) {
        s_readable = host_readable() ? 0 : 1;
        go(M_READ);
    } else if (s_setting == SET_SPEED) {
        s_pick = 2;
        for (int i = 0; i < SPEED_OPTIONS; i++) {
            if (SPEED_PX[i] == host_scroll_speed()) s_pick = i;
        }
        go(M_SPEED);
    } else if (s_setting == SET_CARD) {
        s_card = host_card();
        if (s_card == UI_CARD_NONE) {
            word("NONE");  // these act on the card in the slot
        } else {
            s_pick = 0;
            go(M_CARD);
        }
    } else if (s_setting == SET_LIGHT) {
        s_light_item = 0;
        go(M_LIGHT_SET);
    } else if (s_setting == SET_VOL_LIMIT) {
        s_pick = clampi(host_volume_limit() / 10 - 1, 0, VOL_LIMIT_OPTIONS - 1);
        go(M_VOL_LIMIT);
    } else if (s_setting == SET_AUTO_OFF) {
        s_pick = 2;
        for (int i = 0; i < AUTO_OFF_OPTIONS; i++) {
            if (AUTO_OFF_MINUTES[i] == host_auto_off()) s_pick = i;
        }
        go(M_AUTO_OFF);
    } else if (s_setting == SET_CLOCK) {
        s_clock_item = 0;
        go(M_CLOCK_SET);
    } else if (s_setting == SET_WIFI) {
        // Wi-Fi, server and sign-in from a phone: the player's own network, password here.
        set_text(host_setup_start());
        s_setup_from_settings = true;
        go(M_SETUP);
    } else if (s_setting == SET_EXTEND) {
        s_pick = 0;
        for (int i = 0; i < EXTEND_OPTIONS; i++) {
            if (EXTEND_MINUTES[i] == host_sleep_extend()) s_pick = i;
        }
        go(M_EXT);
    } else if (host_library_count() > 0) {
        s_lib = clampi(host_current_library(), 0, host_library_count() - 1);
        go(M_LIBS);
    } else {
        word("NONE");
    }
}

static void right_press(void)
{
    switch (s_mode) {
    case M_PLAYER:
        open_menu(s_menu);
        break;
    case M_CHAPTER:
        host_chapter_step(s_chapter_pick - s_pb.chapter_index);
        go(M_PLAYER);  // the new chapter is introduced
        break;
    case M_MENU:
        menu_select();
        break;
    case M_GROUPS:
        show_books(s_group);
        break;
    case M_BOOKS:
        if (selected_book() >= 0) {
            host_play_book(selected_book());
            go(M_PLAYER);
        }
        break;
    case M_SLEEP:
        end_sleep_mode();  // the timer is now being set by hand
        s_sleep = s_sleep_sel;
        s_sleep_at = s_sleep ? s_t + minutes_to_frames(SLEEP_MINUTES[s_sleep]) : 0;
        snprintf(s_sleep_word, sizeof(s_sleep_word), "%s", SLEEP_LABEL[s_sleep]);
        if (has_book()) go(M_PLAYER);
        else show_home();
        overlay(OV_SLEEP, OVERLAY_FRAMES);
        break;
    case M_CARD:
        if (s_pick == 0) {
            s_pick = 5;  // 30 minutes
            go(M_CARD_MIN);
        } else {
            host_card_clear();
            s_card = host_card();
            go(M_SETTINGS);
            word("CLR");
        }
        break;
    case M_CARD_MIN:
        host_card_make_sleep(CARD_MINUTES[s_pick]);
        s_card = host_card();
        go(M_SETTINGS);
        word("SLP");
        break;
    case M_EXT:
        host_set_sleep_extend(EXTEND_MINUTES[s_pick]);
        go(M_SETTINGS);
        break;
    case M_SPEED:
        host_set_scroll_speed(SPEED_PX[s_pick]);
        go(M_SETTINGS);
        break;
    case M_AUTO_OFF:
        host_set_auto_off(AUTO_OFF_MINUTES[s_pick]);
        go(M_SETTINGS);
        break;
    case M_VOL_LIMIT:
        host_set_volume_limit((s_pick + 1) * 10);
        go(M_SETTINGS);
        break;
    case M_LIGHT_SET:
        s_pick = 0;
        for (int i = 0; i < light_options(); i++) {
            if (light_option_value(i) == host_light_setting(LIGHT_KEYS[s_light_item])) s_pick = i;
        }
        go(M_LIGHT_PICK);
        break;
    case M_LIGHT_PICK:
        host_set_light_setting(LIGHT_KEYS[s_light_item], light_option_value(s_pick));
        go(M_LIGHT_SET);
        break;
    case M_CLOCK_SET:
        s_pick = 0;
        for (int i = 0; i < clock_options(); i++) {
            if (clock_option_value(i) == host_clock_setting(CLOCK_KEYS[s_clock_item])) s_pick = i;
        }
        go(M_CLOCK_PICK);
        break;
    case M_CLOCK_PICK:
        host_set_clock_setting(CLOCK_KEYS[s_clock_item], clock_option_value(s_pick));
        go(M_CLOCK_SET);
        break;
    case M_LIBS:
        if (s_lib != host_current_library()) {
            host_select_library(s_lib);
            // Wait on the status screen; ui_core_books_changed() brings the books back.
            snprintf(s_text, sizeof(s_text), "LOAD");
            s_text_color = GFX_RGB(0x40E0D0);
            go(M_STATUS);
        } else {
            go(M_SETTINGS);
        }
        break;
    case M_SETTINGS:
        settings_select();
        break;
    case M_VIEW:
        host_set_cover_view(s_style == 0);
        go(M_SETTINGS);
        break;
    case M_READ:
        host_set_readable(s_readable == 0);
        go(M_SETTINGS);
        break;
    default:
        break;
    }
}

// Hold = back one level. Everything hangs off the one menu.
static void right_hold(void)
{
    switch (s_mode) {
    case M_CHAPTER:
        settle_player();
        break;
    case M_PLAYER: {
        // Nothing further to back out of: straight to the clock, which then stays up even
        // while the book plays, until a knob is touched.
        int now;
        if (host_time(&now)) s_clock_on = s_clock_held = true;
        else word("WAIT");  // the time has not yet come from the network
        break;
    }
    case M_MENU:
        if (has_book()) go(M_PLAYER);
        else show_home();
        break;
    case M_BOOKS:
        if (grouped(s_view)) {
            if (!opens_sublist(s_view)) s_group = group_of(s_sel);  // back to this book's letter
            go(M_GROUPS);
        } else {
            open_menu(s_view);
        }
        break;
    case M_GROUPS:   open_menu(s_view); break;
    case M_SLEEP:    open_menu(MENU_SLEEP); break;
    case M_SETTINGS: open_menu(MENU_SETTINGS); break;
    case M_LIBS:
    case M_VIEW:
    case M_READ:
    case M_SPEED:
    case M_CARD:
    case M_CLOCK_SET:
    case M_AUTO_OFF:
    case M_VOL_LIMIT:
    case M_LIGHT_SET:
    case M_EXT:      go(M_SETTINGS); break;
    case M_LIGHT_PICK: go(M_LIGHT_SET); break;
    case M_CLOCK_PICK: go(M_CLOCK_SET); break;
    case M_CARD_MIN: s_pick = 0; go(M_CARD); break;
    default:
        break;
    }
}

/* Cards */

static unsigned minutes_to_frames(int minutes)
{
    return (unsigned)minutes * 60 * 1000 / UI_FRAME_MS;
}

static void end_sleep_mode(void)
{
    if (!s_sleep_mode) return;
    s_sleep_mode = false;
    s_sleep_left = 0;
    s_sleep_done = false;
    s_sleep_at = 0;
    s_sleep_dim = false;
}

// Play: carry on with the open book, or start the one last listened to.
static void resume_or_start(void)
{
    if (has_book()) {
        host_resume();
    } else if (host_current_book() >= 0) {
        host_play_book(host_current_book());
        go(M_PLAYER);
    }
}

static void card_in(void)
{
    s_card = host_card();
    switch (s_card) {
    case UI_CARD_BOOK:
        end_sleep_mode();
        host_card_play();
        go(M_PLAYER);
        break;
    case UI_CARD_SLEEP: {
        int minutes = host_card_minutes();
        if (!s_sleep_mode) {
            s_sleep_mode = true;
            s_sleep_dim = true;
            s_sleep_at = s_t + minutes_to_frames(minutes);
        } else if (s_sleep_done) {
            minutes = host_sleep_extend();  // it had finished: a little longer
            s_sleep_at = s_t + minutes_to_frames(minutes);
        } else {
            s_sleep_at = s_t + s_sleep_left;  // back in: the clock carries on
            minutes = (int)((s_sleep_left * UI_FRAME_MS + 59999) / 60000);
        }
        s_sleep_done = false;
        s_sleep_left = 0;
        resume_or_start();
        snprintf(s_sleep_word, sizeof(s_sleep_word), "%dM", minutes);
        overlay(OV_SLEEP, OVERLAY_FRAMES);
        break;
    }
    case UI_CARD_NEW:
        word("NEW");  // press both knobs to link it to a book
        break;
    default:
        break;
    }
}

static void card_out(void)
{
    const ui_card_kind_t was = s_card;
    s_card = UI_CARD_NONE;
    if (was == UI_CARD_BOOK) {
        if (host_card_book_open()) host_pause();  // only if this card's book is what's on
    } else if (was == UI_CARD_SLEEP && s_sleep_mode) {
        host_pause();
        s_sleep_left = s_sleep_at > s_t ? s_sleep_at - s_t : 0;  // stop the clock
        s_sleep_at = 0;
    }
}

// Both knobs: mark or unmark a favourite. In a book list it is the book shown; anywhere
// else, the book that is playing.
static void toggle_favourite(void)
{
    // With a card in the slot that is not linked to anything yet, both knobs links it to
    // this book instead. (A card that is already linked leaves the gesture alone, or it
    // could never be used while a book card sits in the slot.)
    s_card = host_card();
    if (s_card == UI_CARD_NEW) {
        if (host_card_link(s_mode == M_BOOKS ? selected_book() : -1)) {
            s_card = host_card();
            word("LINK");
        } else {
            word("NONE");
        }
        return;
    }
    const int book = s_mode == M_BOOKS ? selected_book() : has_book() ? host_current_book() : -1;
    if (book < 0) {
        word("NONE");
        return;
    }
    host_toggle_favourite(book);
    const bool now = host_book_favourite(book);
    if (s_mode == M_BOOKS && s_view == V_FAV) {
        // The list being looked at has just changed.
        const int sel = s_sel;
        if (build_view(V_FAV)) {
            show_books(-1);
            s_sel = clampi(sel, 0, s_count - 1);
        } else {
            open_menu(V_FAV);
        }
    }
    s_heart_on = now;
    s_heart_start = s_t;
    overlay(OV_HEART, HEART_FRAMES);
}

void ui_core_event(const ui_event_t *ev)
{
    if (s_mode == M_OFF) return;
    if (ev->type == UI_EV_POWER_HOLD) {
        go(M_OFF);
        host_power_off();
        return;
    }
    if (s_mode == M_SETUP && s_setup_from_settings && ev->type == UI_EV_R_HOLD) {
        host_setup_stop();
        s_setup_from_settings = false;
        go(M_SETTINGS);
        return;
    }
    s_last_touch = s_last_active = s_t;  // any knob, button, card or cable
    if (s_wake > 0 && (ev->type <= UI_EV_BOTH || ev->type == UI_EV_POWER_PRESS)) {
        // Awake: any knob or button puts the ring back to its usual self, and does no more.
        s_wake_cancelled = true;
        s_wake = 0;
        return;
    }
    if (ev->type <= UI_EV_BOTH || ev->type == UI_EV_CARD_IN) {
        // A knob touched (or a card put in) while the clock is up brings the player's own
        // screen back and does nothing more, except the left press, which also plays.
        const bool was_clock = s_clock_on;
        s_clock_on = s_clock_held = false;
        s_last_input = s_t;
        if (was_clock) {
            s_mode_t = s_t;
            if (ev->type != UI_EV_L_PRESS && ev->type != UI_EV_CARD_IN) return;
        }
    }
    if (ev->type == UI_EV_POWER_PRESS || ev->type == UI_EV_PLUGGED || ev->type == UI_EV_UNPLUGGED) {
        // A look at the battery, on any screen: on a touch of the power button, and when
        // the cable goes in or comes out. Three seconds, as the original firmware gave it.
        if (s_mode == M_OFF) return;
        host_battery(&s_battery);
        overlay(OV_BATTERY, MS(3000));
        return;
    }
    if (s_mode == M_STATUS && s_booting && ev->type == UI_EV_R_HOLD) {
        // Stuck at start-up (the Wi-Fi or the server has changed, and nothing is cached to
        // show instead): a hold opens setup, the only way to put it right from here.
        set_text(host_setup_start());
        s_setup_from_settings = true;
        go(M_SETUP);
        return;
    }
    if (s_mode == M_STATUS || s_mode == M_SETUP) {
        // Nothing to operate yet, but remember a card that is already in the slot.
        if (ev->type == UI_EV_CARD_IN) s_card_pending = true;
        if (ev->type == UI_EV_CARD_OUT) s_card_pending = false;
        return;
    }

    get_playback();
    switch (ev->type) {
    case UI_EV_L_TURN:
        host_set_volume(clampi(s_pb.volume + ev->delta * VOLUME_STEP, 0, 100));
        overlay(OV_VOLUME, OVERLAY_FRAMES);
        break;
    case UI_EV_L_PRESS:
        // Pressing play with the sleep card out means sleep is over.
        if (s_sleep_mode && s_card != UI_CARD_SLEEP) end_sleep_mode();
        if (has_book()) {
            host_toggle_play();
            overlay(OV_PLAYPAUSE, MS(960));
        } else if (host_current_book() >= 0) {
            // Nothing open yet (just switched on): carry on with the last book listened to.
            host_play_book(host_current_book());
            go(M_PLAYER);
        }
        break;
    case UI_EV_R_TURN:  right_turn(ev->delta); break;
    case UI_EV_R_PRESS: right_press(); break;
    case UI_EV_R_HOLD:  right_hold(); break;
    case UI_EV_BOTH:
        toggle_favourite();
        break;
    case UI_EV_CARD_IN:  card_in(); break;
    case UI_EV_CARD_OUT: card_out(); break;
    default:
        break;
    }
}

/* Drawing */

static gfx_color_t dim(gfx_color_t c)
{
    return (gfx_color_t){ c.r / 2, c.g / 2, c.b / 2 };
}

// Text that fits is drawn as is; longer text scrolls by `off` pixels and wraps round with
// a short gap.
static void loop_text(const char *s, int y, gfx_color_t c, int off)
{
    gfx_text(s, -off, y, c);
    if (gfx_text_width(s) > DISPLAY_W && off > 0) {
        gfx_text(s, -off + (int)strlen(s) * GFX_CHAR_W + 6, y, c);
    }
}

static int scroll_len(const char *s)
{
    return s && gfx_text_width(s) > DISPLAY_W ? (int)strlen(s) * GFX_CHAR_W + 6 : 0;
}

// Scrolling runs at the chosen speed (pixels a second): how far text has moved after a
// number of ticks, and how many ticks it takes to move a distance.
static int scroll_px(unsigned ticks)
{
    return (int)(ticks * UI_FRAME_MS * (unsigned)host_scroll_speed() / 1000);
}

static unsigned scroll_ticks(int px)
{
    const int speed = host_scroll_speed();
    return px > 0 ? (unsigned)((px * 1000 + speed * UI_FRAME_MS - 1) / (speed * UI_FRAME_MS)) : 0;
}

// Two lines that scroll in turn: hold, scroll line 1, hold, scroll line 2. (The player's
// introduction of a book and chapter.)
static void two_lines(const char *l1, gfx_color_t c1, const char *l2, gfx_color_t c2)
{
    const unsigned t1 = scroll_ticks(scroll_len(l1)), t2 = l2 ? scroll_ticks(scroll_len(l2)) : 0;
    const unsigned cycle = HOLD_FRAMES + t1 + HOLD_FRAMES + t2;
    const unsigned k = (s_t - s_mode_t) % cycle;
    int o1 = 0, o2 = 0;
    if (k >= HOLD_FRAMES && k < HOLD_FRAMES + t1) o1 = scroll_px(k - HOLD_FRAMES);
    else if (k >= 2 * HOLD_FRAMES + t1) o2 = scroll_px(k - 2 * HOLD_FRAMES - t1);
    loop_text(l1, 0, c1, o1);
    if (l2) loop_text(l2, 6, c2, o2);
}

// A row of dots, one per item when they fit, otherwise a single marker showing position.
static void draw_dots(int y, int index, int count, gfx_color_t on)
{
    if (count <= 0) return;
    if (count <= 8) {
        for (int i = 0; i < count; i++) gfx_set(i * 2, y, i == index ? on : GFX_GREY);
    } else {
        for (int x = 0; x < DISPLAY_W; x++) gfx_set(x, y, GFX_DIM);
        gfx_set(index * (DISPLAY_W - 1) / (count - 1), y, on);
    }
}

/* Lists. Every list and menu is drawn the same way: something to look at on top, the
   item's name along the bottom, and on the bottom row a line showing where you are. */

#define BAND_TOP 8            // caption band: rows 8-14, text in rows 9-13; row 15 is the line
#define LINE_Y   15

static const gfx_color_t BLACK = { 0, 0, 0 };
#define LINE_TRACK  GFX_RGB(0x101010)
#define LINE_MARKER GFX_RGB(0x00D0FF)  // bright cyan: nothing else on screen uses it

// Where you are in the list: a bright marker on a nearly black track. With only a few
// items the marker is wide, like a scrollbar's thumb.
static void draw_line(int index, int count)
{
    for (int x = 0; x < DISPLAY_W; x++) gfx_set(x, LINE_Y, LINE_TRACK);
    if (count <= 0) return;
    int w = DISPLAY_W / count;
    if (w < 1) w = 1;
    const int x0 = count > 1 ? index * (DISPLAY_W - w) / (count - 1) : 0;
    for (int x = x0; x < x0 + w; x++) gfx_set(x, LINE_Y, LINE_MARKER);
}

static void draw_band(void)
{
    for (int y = BAND_TOP; y < LINE_Y; y++) {
        for (int x = 0; x < DISPLAY_W; x++) gfx_set(x, y, BLACK);
    }
}

// Small pictures for things that have no cover: the menu items, and a book as the stand-in
// for a missing cover. Up to 8 rows of 10, '#' = lit.
typedef struct { const char *rows[8]; unsigned rgb; } icon_t;
static const icon_t ICON_BOOK    = { { "..######..", ".#......#.", ".#.####.#.", ".#......#.", ".#.####.#.", ".#......#.", "..######.." }, 0x4A4A4A };
static const icon_t ICON_CLOCK   = { { "...####...", "..#..#.#..", ".#...#..#.", ".#...##.#.", ".#......#.", "..#....#..", "...####..." }, 0x3DDC84 };
static const icon_t ICON_NEW     = { { "....#.....", "....#.....", "...###....", ".#######..", "...###....", "....#.....", "....#....." }, 0xFFD166 };
static const icon_t ICON_PERSON  = { { "....##....", "...####...", "...####...", "....##....", "..######..", ".########.", ".########." }, 0xFF8A3D };
static const icon_t ICON_SERIES  = { { ".#######..", "..........", "..#######.", "..........", ".#######..", "..........", "..#######." }, 0xFF8A3D };
static const icon_t ICON_MOON    = { { "...###....", "..##......", ".##.......", ".##.......", ".##.......", "..##...#..", "...####..." }, 0x9B8CFF };
static const icon_t ICON_LIBRARY = { { ".#.#.##.#.", ".#.#.##.#.", ".#.#.##.#.", ".#.#.##.#.", ".#.#.##.#.", "##########" }, 0x9B8CFF };
static const icon_t ICON_HEART   = { { "..##..##..", ".####.###.", ".########.", ".########.", "..######..", "...####...", "....##...." }, 0xFF2D55 };
static const icon_t ICON_DOWN    = { { "....##....", "....##....", "....##....", ".########.", "..######..", "...####...", "....##...." }, 0x2D9BFF };
static const icon_t ICON_GEAR    = { { "...##.....", ".#.##.#...", "..####....", "###..###..", "###..###..", "..####....", ".#.##.#...", "...##....." }, 0xBDBDBD };
static const icon_t ICON_GLASS   = { { "..###.....", ".#...#....", ".#...#....", ".#...#....", "..###.....", ".....#....", "......#..." }, 0xFFD166 };
static const icon_t ICON_CARD    = { { ".########.", ".#......#.", ".#.##...#.", ".#.##...#.", ".#......#.", ".#......#.", ".########." }, 0x40E0D0 };
static const icon_t ICON_WIFI    = { { "..######..", ".#......#.", "...####...", "..#....#..", "....##....", "....##...." }, 0x8FD3FF };
static const icon_t ICON_POWER   = { { "....#.....", "..#.#.#...", ".#..#..#..", ".#..#..#..", ".#.....#..", "..#...#...", "...###...." }, 0xFF4D4D };
static const icon_t ICON_LAMP    = { { "...####...", "..#....#..", ".#......#.", ".#......#.", ".#......#.", "..#....#..", "...####..." }, 0xFFB347 };  // the LED ring
static const icon_t ICON_SWATCH  = { { ".########.", ".########.", ".########.", ".########.", ".########.", ".########.", ".########." }, 0xFFFFFF };
static const icon_t ICON_LOUD    = { { "....#.....", "..###..#..", "#####.#.#.", "#####.#.#.", "#####.#.#.", "..###..#..", "....#....." }, 0x3DDC84 };
static const icon_t ICON_PLUS    = { { "....##....", "....##....", "..######..", "..######..", "....##....", "....##...." }, 0x9B8CFF };
static const icon_t ICON_SPEED   = { { ".#...#....", ".##..##...", ".###.###..", ".########.", ".###.###..", ".##..##...", ".#...#...." }, 0x3DDC84 };
static const icon_t ICON_EYE     = { { "...####...", ".##....##.", "#...##...#", ".##....##.", "...####..." }, 0x40E0D0 };

static void draw_icon(const icon_t *icon)
{
    int rows = 0;
    while (rows < 8 && icon->rows[rows]) rows++;
    const int y0 = (BAND_TOP - rows) / 2;
    for (int r = 0; r < rows; r++) {
        for (int k = 0; icon->rows[r][k]; k++) {
            if (icon->rows[r][k] == '#') gfx_set(3 + k, y0 + r, GFX_RGB(icon->rgb));
        }
    }
}

// A book's copy for the SD card, in the top left corner as the heart is in the top right:
// a 3 x 3 square that fills like the battery, from its bottom left pixel rightwards and
// then up, as the copy comes in. Light grey-blue where it is still to come (all of it,
// before the download has begun); blue where it has. While the download is running the
// next pixel to fill flashes slowly; held or waiting, nothing flashes. A finished copy has
// no mark at all.
static void draw_offline(int percent, bool fetching)
{
    const gfx_color_t empty = GFX_RGB(0x56687A), full = GFX_RGB(0x2D9BFF);
    int filled = percent * 9 / 100;
    if (filled > 9) filled = 9;
    const bool lit = fetching && (s_t / MS(600)) % 2 == 0;
    for (int i = 0; i < 9; i++) {
        gfx_set(i % 3, 2 - i / 3, i < filled || (i == filled && lit) ? full : empty);
    }
}

// A favourite's mark: a small red heart in the top right corner, on a black patch so it
// shows against any picture.
static void draw_heart(void)
{
    static const char *const rows[3] = { "#.#", "###", ".#." };
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) gfx_set(13 + k, r, rows[r][k] == '#' ? GFX_RGB(0xFF2D55) : BLACK);
    }
}

// One item of a list.
//
// With cover art (`art`, when the style is ART and the cover is there): the caption is on
// the bottom band straight away, starting from its first letter, so that turning the knob
// shows what each book is. After a pause it scrolls off to the left, and then the picture
// has the whole display.
//
// Otherwise the caption band is always there, under an icon: a caption that fits just
// sits there, and a longer one scrolls round after a pause.
// Figures for times, three wide and five tall, row by row.
static const char *const DIGIT[10] = { "111101101101111", "010010010010010", "111001111100111", "111001111001111",
                                       "101101111001001", "111100111001111", "111100111101111", "111001001001001",
                                       "111101111101111", "111101111001111" };

static void draw_digit(int d, int x, int y, gfx_color_t c)
{
    for (int r = 0; r < 5; r++) {
        for (int col = 0; col < 3; col++) {
            if (DIGIT[d][r * 3 + col] == '1') gfx_set(x + col, y + r, c);
        }
    }
}

// A time being chosen in the settings: hours to the left of a colon that never moves,
// minutes to the right, so nothing shifts as the hour goes from one figure to two. Above
// it AM or PM on the 12-hour clock, the clock icon on the 24-hour one.
static void draw_time_choice(int minutes_of_day, int index, int count, bool current)
{
    const bool h24 = host_clock_setting(UI_CLOCK_24H);
    int hour = minutes_of_day / 60;
    const int minute = minutes_of_day % 60, y = BAND_TOP + 1;
    if (h24) {
        // Two full figures: the colon sits against the minutes, there being no room to spare.
        draw_icon(&ICON_CLOCK);
        if (hour >= 10) draw_digit(hour / 10, 0, y, GFX_WHITE);
        draw_digit(hour % 10, 4, y, GFX_WHITE);
        gfx_set(8, y + 1, GFX_GREY); gfx_set(8, y + 3, GFX_GREY);
    } else {
        const bool pm = hour >= 12;
        gfx_text_centered(pm ? "PM" : "AM", 1, pm ? GFX_VIOLET : GFX_YELLOW);
        hour = hour % 12 ? hour % 12 : 12;
        if (hour >= 10) { for (int r = 0; r < 5; r++) gfx_set(1, y + r, GFX_WHITE); }  // a slim 1
        draw_digit(hour % 10, 3, y, GFX_WHITE);
        gfx_set(7, y + 1, GFX_GREY); gfx_set(7, y + 3, GFX_GREY);
    }
    draw_digit(minute / 10, 9, y, GFX_WHITE);
    draw_digit(minute % 10, 13, y, GFX_WHITE);
    if (current) gfx_set(0, 0, GFX_GREEN);
    draw_line(index, count);
}

static void draw_card(const unsigned char *art, const icon_t *icon, const char *caption, gfx_color_t text,
                      int index, int count, bool current, bool favourite)
{
    const unsigned rested = s_t - s_mode_t;
    if (art) {
        gfx_image(art, 100);
        // Hold, then scroll until the last letter has left; a caption that fits is simply
        // shown for a couple of seconds.
        const bool fits = gfx_text_width(caption) <= DISPLAY_W;
        const unsigned shown = fits ? 2 * HOLD_FRAMES
                                    : HOLD_FRAMES + scroll_ticks((int)strlen(caption) * GFX_CHAR_W);
        if (rested < shown) {
            draw_band();
            if (fits) gfx_text_centered(caption, BAND_TOP + 1, text);
            else gfx_text(caption, rested < HOLD_FRAMES ? 0 : -scroll_px(rested - HOLD_FRAMES), BAND_TOP + 1, text);
        }
    } else {
        if (icon) draw_icon(icon);
        const int len = scroll_len(caption);
        if (!len) {
            gfx_text_centered(caption, BAND_TOP + 1, text);
        } else {
            const unsigned k = rested % (HOLD_FRAMES + scroll_ticks(len));
            loop_text(caption, BAND_TOP + 1, text, k < HOLD_FRAMES ? 0 : scroll_px(k - HOLD_FRAMES));
        }
    }
    if (current) gfx_set(0, 0, GFX_GREEN);
    if (favourite) draw_heart();
    draw_line(index, count);
}

static const unsigned char *art_for_book(int book)
{
    return host_cover_view() ? host_book_cover(book) : NULL;
}

static void draw_book(int book, bool current)
{
    char caption[200];
    const char *author = host_book_author(book);
    char reworded[160];
    snprintf(caption, sizeof(caption), author[0] ? "%s - %s" : "%s",
             readable_title(host_book_title(book), reworded, sizeof(reworded)), author);
    draw_card(art_for_book(book), &ICON_BOOK, caption, GFX_WHITE, s_sel, s_count, current,
              host_book_favourite(book));
    // Its copy for the SD card, while that is wanted and not yet complete.
    bool fetching = false;
    const int offline = host_book_offline(book, &fetching);
    if (offline >= 0) {
        draw_offline(offline, fetching);
        // The square has the corner the "this is the open book" dot uses: the dot moves along.
        if (current) gfx_set(4, 0, GFX_GREEN);
    }
}

static void draw_menu(void)
{
    static const icon_t *const icons[MENU_ITEMS] = {
        &ICON_CLOCK, &ICON_NEW, &ICON_HEART, &ICON_DOWN, NULL, &ICON_PERSON, &ICON_SERIES,
        &ICON_MOON, &ICON_GEAR,
    };
    if (s_menu == V_TITLE) gfx_text_centered("ABC", 2, GFX_YELLOW);
    draw_card(NULL, icons[s_menu], MENU[s_menu], GFX_WHITE, s_menu, MENU_ITEMS, false, false);
}

// The favourite animation. Made a favourite: a filled heart that swells for a beat.
// Unmade: an empty heart that shrinks a little.
static void draw_heart_animation(void)
{
    static const char *const small[] = { ".##...##.", "####.####", "#########", "#########", ".#######.",
                                         "..#####..", "...###...", "....#....", NULL };
    static const char *const normal[] = { "..##...##..", ".####.####.", "###########", "###########", "###########",
                                          ".#########.", "..#######..", "...#####...", "....###....", ".....#.....",
                                          NULL };
    static const char *const large[] = { "..###...###..", ".#####.#####.", "#############", "#############",
                                         "#############", "#############", ".###########.", "..#########..",
                                         "...#######...", "....#####....", ".....###.....", "......#......", NULL };
    const unsigned k = s_t - s_heart_start;
    const char *const *rows = s_heart_on ? (k >= MS(320) && k < MS(640) ? large : normal)
                                         : (k < MS(480) ? normal : small);
    int h = 0;
    while (rows[h]) h++;
    const int w = (int)strlen(rows[0]), x0 = (DISPLAY_W - w) / 2, y0 = (DISPLAY_H - h) / 2;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (rows[r][c] != '#') continue;
            // The empty heart is the outline: lit pixels with an unlit one beside them.
            const bool edge = r == 0 || r == h - 1 || c == 0 || c == w - 1 || rows[r - 1][c] != '#' ||
                              rows[r + 1][c] != '#' || rows[r][c - 1] != '#' || rows[r][c + 1] != '#';
            if (s_heart_on || edge) gfx_set(x0 + c, y0 + r, GFX_RGB(0xFF2D55));
        }
    }
}

// The battery, alone on the display and standing upright: an outline six pixels wide with
// four columns of level inside, filling from the bottom, red under 20 %, orange under
// 40 %, green above. On external power a white lightning bolt lies over the middle,
// whatever the level.
static void draw_battery(void)
{
    const gfx_color_t shell = GFX_RGB(0xBDBDBD);
    const int left = 5, right = 10, top = 3, bottom = 14;  // the level is columns 6-9, rows 4-13
    for (int x = left; x <= right; x++) { gfx_set(x, top, shell); gfx_set(x, bottom, shell); }
    for (int y = top; y <= bottom; y++) { gfx_set(left, y, shell); gfx_set(right, y, shell); }
    for (int x = left + 2; x <= right - 2; x++) { gfx_set(x, top - 1, shell); gfx_set(x, top - 2, shell); }
    if (!s_battery.present) return;

    const int cells = bottom - top - 1;
    const int percent = s_battery.percent;
    int lit = (percent * cells + 50) / 100;
    if (lit > cells) lit = cells;
    if (lit < 1) lit = 1;  // never quite empty to look at
    const gfx_color_t colour = percent < 20 ? GFX_RED : percent < 40 ? GFX_ORANGE : GFX_GREEN;
    for (int i = 0; i < lit; i++) {
        for (int x = left + 1; x <= right - 1; x++) gfx_set(x, bottom - 1 - i, colour);
    }
    if (s_battery.plugged) {
        static const char *const bolt[7] = { "...#", "..#.", ".##.", "####", ".##.", ".#..", "#..." };
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 4; c++) {
                if (bolt[r][c] == '#') gfx_set(left + 1 + c, top + 2 + r, GFX_WHITE);
            }
        }
    }
}

// Wi-Fi gone (offline mode begins) or back: the usual fan of arcs, in grey with a red
// cross over its corner when it has gone, in turquoise when it is back.
static void draw_wifi(bool lost)
{
    static const char *const fan[10] = {
        "...#######...", ".##.......##.", "#....###....#", "...##...##...", "..#.......#..",
        ".....###.....", "....#...#....", ".............", "......#......", "......#......",
    };
    const gfx_color_t c = lost ? GFX_GREY : GFX_RGB(0x40E0D0);
    for (int r = 0; r < 10; r++) {
        for (int k = 0; k < 13; k++) {
            if (fan[r][k] == '#') gfx_set(1 + k, 2 + r, c);
        }
    }
    if (!lost) return;
    // The cross, five by five in the bottom right corner, on a black patch.
    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 7; x++) gfx_set(9 + x, 8 + y, (gfx_color_t){ 0, 0, 0 });
    }
    for (int i = 0; i < 5; i++) {
        gfx_set(10 + i, 9 + i, GFX_RED);
        gfx_set(14 - i, 9 + i, GFX_RED);
    }
}

static void draw_volume(void)
{
    const int lit = (s_pb.volume * 8 + 50) / 100;
    gfx_text("VOL", 2, 0, GFX_WHITE);
    for (int i = 0; i < 8; i++) {
        const gfx_color_t c = i >= lit ? GFX_DIM : i < 4 ? GFX_GREEN : i < 6 ? GFX_YELLOW : GFX_RED;
        for (int h = 0; h <= i; h++) {
            gfx_set(i * 2, 15 - h, c);
        }
    }
}

static void draw_playpause(void)
{
    if (s_pb.state == UI_PLAY_PAUSED) {
        for (int y = 4; y < 12; y++) {
            gfx_set(5, y, GFX_WHITE); gfx_set(6, y, GFX_WHITE);
            gfx_set(9, y, GFX_WHITE); gfx_set(10, y, GFX_WHITE);
        }
    } else {
        for (int r = 0; r < 9; r++) {
            const int w = r <= 4 ? r + 1 : 9 - r;
            for (int x = 0; x < w; x++) gfx_set(6 + x, 3 + r, GFX_GREEN);
        }
    }
}

static void draw_spinner(int y)
{
    // Eight points round a ring, with a bright head and a fading tail.
    static const int8_t ring[8][2] = { {7,0},{10,1},{11,4},{10,7},{7,8},{4,7},{3,4},{4,1} };
    const int head = (s_t / MS(160)) % 8;
    for (int i = 0; i < 8; i++) {
        const int age = (head - i + 8) % 8;
        const int v = age == 0 ? 255 : age < 4 ? 160 >> age : 12;
        const gfx_color_t c = { v / 4, v * 7 / 8, v * 13 / 16 };  // turquoise
        gfx_set(ring[i][0], y + ring[i][1], c);
        gfx_set(ring[i][0] + 1, y + ring[i][1], c);
    }
}

static void draw_error(void)
{
    for (int i = 0; i < 7; i++) {
        gfx_set(4 + i, 1 + i, GFX_RED); gfx_set(5 + i, 1 + i, GFX_RED);
        gfx_set(10 - i, 1 + i, GFX_RED); gfx_set(11 - i, 1 + i, GFX_RED);
    }
}

// How long the title and chapter take to scroll past once.
static unsigned intro_frames(const char *chapter)
{
    return HOLD_FRAMES + scroll_ticks(scroll_len(s_pb.title)) + HOLD_FRAMES +
           (chapter ? scroll_ticks(scroll_len(chapter)) + HOLD_FRAMES : 0);
}

// The settled player screen: the cover with the chapter number over it and progress along
// the bottom. Without a cover, the chapter number stands alone.
static void draw_player_settled(bool on)
{
    const unsigned char *cover = host_cover_view() ? host_cover() : NULL;
    char num[12] = "";
    if (s_pb.chapter_index >= 0 && s_pb.chapter_count > 1) {
        snprintf(num, sizeof(num), "%d", (s_pb.chapter_index + 1) % 1000);
    }
    if (cover) {
        gfx_image(cover, on ? 70 : 30);
        if (num[0]) gfx_text_outlined(num, 1, 8, on ? GFX_WHITE : GFX_GREY);
    } else {
        gfx_text_centered("CH", 1, on ? GFX_ORANGE : dim(GFX_ORANGE));
        gfx_text_centered(num[0] ? num : "-", 7, on ? GFX_WHITE : GFX_GREY);
    }
    // Progress through the chapter on the bottom row, set off from the picture by a
    // black row above it.
    const gfx_color_t black = { 0, 0, 0 };
    const int fill = (int)(s_pb.chapter_progress * DISPLAY_W + 0.5f);
    for (int x = 0; x < DISPLAY_W; x++) {
        gfx_set(x, 14, black);
        gfx_set(x, 15, x < fill ? (on ? GFX_GREEN : dim(GFX_GREEN)) : GFX_DIM);
    }
    if (!on && s_t % MS(960) < MS(480)) gfx_set(fill > 15 ? 15 : fill, 15, GFX_WHITE);
    if (host_book_favourite(host_current_book())) draw_heart();
    if (s_sleep_at) gfx_set(15, 4, GFX_VIOLET);
}

static void draw_player(void)
{
    if (s_pb.state == UI_PLAY_LOADING) {
        draw_spinner(4);  // alone, in the middle
        return;
    }
    if (s_pb.state == UI_PLAY_ERROR) {
        draw_error();
        gfx_text_centered("ERR", 10, GFX_WHITE);
        return;
    }
    const bool on = s_pb.state == UI_PLAY_PLAYING;
    const char *chapter = s_pb.chapter && s_pb.chapter[0] ? s_pb.chapter : NULL;
    // Introduce the book and chapter by scrolling them past once, then settle.
    if (s_t - s_mode_t >= intro_frames(chapter)) {
        draw_player_settled(on);
        return;
    }
    two_lines(s_pb.title, on ? GFX_ORANGE : dim(GFX_ORANGE), chapter, on ? GFX_WHITE : GFX_GREY);
    const int fill = (int)(s_pb.chapter_progress * DISPLAY_W + 0.5f);
    for (int x = 0; x < DISPLAY_W; x++) {
        gfx_set(x, 13, x < fill ? (on ? GFX_GREEN : dim(GFX_GREEN)) : GFX_DIM);
    }
    if (!on && s_t % MS(960) < MS(480)) gfx_set(fill > 15 ? 15 : fill, 13, GFX_WHITE);
    draw_dots(15, s_pb.chapter_index, s_pb.chapter_count, GFX_WHITE);
    if (s_sleep_at) gfx_set(15, 15, GFX_VIOLET);
}

// The start-up animation: eight rainbow bars, each swinging between full height above the
// middle and full height below it (a bar turning end over end, seen edge on), every bar a
// little behind the one before, so a wave runs along them. One turn takes two seconds.
// `rows` is the height available, from the top of the display.
static void draw_boot_bars(int rows)
{
    // The loader's six colours and two more, so that eight bars two pixels wide fill the
    // display edge to edge with no gaps.
    static const unsigned colour[8] = { 0x27AAE1, 0xF05A2A, 0xFFCC00, 0x27AE60, 0xAE2727, 0x6B26B7, 0x1ABC9C,
                                        0xE84393 };
    const int half = rows / 2;                          // the middle is between rows half-1 and half
    const float amplitude = (float)(half - 1) + 0.5f;
    const float t = (float)(s_t * UI_FRAME_MS);
    for (int i = 0; i < 8; i++) {
        const float turn = (t - 100.0f * (float)(i + 1)) / 2000.0f;
        const int h = (int)lroundf(amplitude * cosf(turn * 6.2831853f));
        for (int k = 0; k < (h < 0 ? -h : h); k++) {
            const int y = h > 0 ? half - 1 - k : half + k;
            gfx_set(2 * i, y, GFX_RGB(colour[i]));
            gfx_set(2 * i + 1, y, GFX_RGB(colour[i]));
        }
    }
}

static void draw_screen(void)
{
    switch (s_mode) {
    case M_STATUS:
        if (!s_booting) {
            gfx_text_centered(s_text, 5, s_text_color);  // e.g. LOAD while switching library
        } else if (s_t < BOOT_WORD) {
            draw_boot_bars(DISPLAY_H);
        } else {
            draw_boot_bars(10);
            gfx_text_centered(s_text, 11, s_text_color);
        }
        break;
    case M_SETUP: {
        char top[5], bottom[5] = "";
        strncpy(top, s_text, 4); top[4] = 0;
        if (strlen(s_text) > 4) { strncpy(bottom, s_text + 4, 4); bottom[4] = 0; }
        if (!bottom[0]) {
            gfx_text_centered(top, 5, GFX_ORANGE);  // the setup page's four-digit code
        } else {
            gfx_text_centered(top, 2, GFX_ORANGE);
            gfx_text_centered(bottom, 9, GFX_WHITE);
        }
        break;
    }
    case M_PLAYER:
        draw_player();
        break;
    case M_CHAPTER: {
        // The number on top; the chapter's name, if it has one, along the bottom: there
        // from its first letters, then scrolling off once.
        char num[12];
        snprintf(num, sizeof(num), "%d", (s_chapter_pick + 1) % 10000);
        gfx_text_centered(num, 1, GFX_ORANGE);
        const char *name = chapter_name(s_chapter_pick);
        if (name) {
            const unsigned rested = s_t - s_mode_t;
            if (gfx_text_width(name) <= DISPLAY_W) gfx_text_centered(name, BAND_TOP + 1, GFX_WHITE);
            else gfx_text(name, rested < HOLD_FRAMES ? 0 : -scroll_px(rested - HOLD_FRAMES), BAND_TOP + 1, GFX_WHITE);
        }
        if (s_chapter_pick == s_pb.chapter_index) gfx_set(0, 0, GFX_GREEN);
        draw_line(s_chapter_pick, s_pb.chapter_count);
        break;
    }
    case M_MENU:
        draw_menu();
        break;
    case M_GROUPS: {
        char name[96];
        const int first = s_order[s_group_first[s_group]];
        group_key(s_view, first, name, sizeof(name));
        if (s_view == V_TITLE) {
            // The alphabet: one big letter.
            gfx_text_scaled(name, 5, 2, GFX_YELLOW, 2);
            draw_line(s_group, s_group_n);
        } else {
            // An author or a series, pictured by its first book.
            char reworded[100];
            const char *shown = s_view == V_SERIES ? readable_title(name, reworded, sizeof(reworded)) : name;
            draw_card(art_for_book(first), s_view == V_AUTHOR ? &ICON_PERSON : &ICON_SERIES, shown, GFX_YELLOW,
                      s_group, s_group_n, false, false);
        }
        break;
    }
    case M_BOOKS:
        if (s_count == 0) {
            gfx_text_centered("NONE", 5, GFX_GREY);
            break;
        }
        draw_book(selected_book(), has_book() && selected_book() == host_current_book());
        break;
    case M_SLEEP:
        draw_card(NULL, &ICON_MOON, SLEEP_LABEL[s_sleep_sel], GFX_WHITE, s_sleep_sel, SLEEP_OPTIONS,
                  s_sleep_sel == s_sleep, false);
        break;
    case M_LIBS:
        draw_card(NULL, &ICON_LIBRARY, host_library_name(s_lib), GFX_WHITE, s_lib, host_library_count(),
                  s_lib == host_current_library(), false);
        break;
    case M_SETTINGS: {
        static const icon_t *const icons[SET_ITEMS] = { &ICON_LIBRARY, &ICON_EYE, &ICON_GLASS, &ICON_SPEED,
                                                        &ICON_CARD, &ICON_PLUS, &ICON_CLOCK, &ICON_POWER, &ICON_LOUD, &ICON_LAMP, &ICON_WIFI };
        draw_card(NULL, icons[s_setting], SETTINGS[s_setting], GFX_WHITE, s_setting, SET_ITEMS, false, false);
        break;
    }
    case M_VIEW:
        draw_card(NULL, &ICON_EYE, VIEW_STYLE[s_style], GFX_WHITE, s_style, 2, (s_style == 0) == host_cover_view(),
                  false);
        break;
    case M_READ:
        draw_card(NULL, &ICON_GLASS, ON_OFF[s_readable], GFX_WHITE, s_readable, 2,
                  (s_readable == 0) == host_readable(), false);
        break;
    case M_SPEED:
        // The label demonstrates the speed being offered: a sample scrolls beneath the icon.
        draw_card(NULL, &ICON_SPEED, SPEED_LABEL[s_pick], GFX_WHITE, s_pick, SPEED_OPTIONS,
                  SPEED_PX[s_pick] == host_scroll_speed(), false);
        break;
    case M_CARD:
        draw_card(NULL, s_pick == 0 ? &ICON_MOON : &ICON_CARD, CARD_ACTIONS[s_pick], GFX_WHITE, s_pick, 2,
                  s_pick == 0 && s_card == UI_CARD_SLEEP, false);
        break;
    case M_CARD_MIN:
    case M_EXT: {
        const bool ext = s_mode == M_EXT;
        const int minutes = ext ? EXTEND_MINUTES[s_pick] : CARD_MINUTES[s_pick];
        char label[8];
        snprintf(label, sizeof(label), "%dM", minutes);
        draw_card(NULL, ext ? &ICON_PLUS : &ICON_MOON, label, GFX_WHITE, s_pick,
                  ext ? EXTEND_OPTIONS : CARD_MINUTE_OPTIONS,
                  minutes == (ext ? host_sleep_extend() : (s_card == UI_CARD_SLEEP ? host_card_minutes() : 0)), false);
        break;
    }
    case M_LIGHT_SET:
        draw_card(NULL, &ICON_LAMP, LIGHT_ITEMS[s_light_item], GFX_WHITE, s_light_item, LIGHT_ITEM_COUNT, false, false);
        break;
    case M_LIGHT_PICK: {
        const int v = light_option_value(s_pick);
        const bool current = v == host_light_setting(LIGHT_KEYS[s_light_item]);
        char label[16];
        icon_t icon = ICON_LAMP;
        if (s_light_item == 1) {
            snprintf(label, sizeof(label), "%s", COLOUR_LABEL[s_pick]);
            icon = ICON_SWATCH;
            icon.rgb = (unsigned)v;
        } else if (s_light_item == 2) {
            snprintf(label, sizeof(label), "%d", v);
        } else if (s_light_item == 4) {
            snprintf(label, sizeof(label), "%dM", v);
        } else {
            snprintf(label, sizeof(label), v ? "ON" : "OFF");
        }
        draw_card(NULL, &icon, label, GFX_WHITE, s_pick, light_options(), current, false);
        break;
    }
    case M_VOL_LIMIT: {
        char label[16];
        snprintf(label, sizeof(label), "%d", (s_pick + 1) * 10);
        draw_card(NULL, &ICON_LOUD, label, GFX_WHITE, s_pick, VOL_LIMIT_OPTIONS,
                  (s_pick + 1) * 10 == host_volume_limit(), false);
        break;
    }
    case M_AUTO_OFF: {
        char label[16];
        if (AUTO_OFF_MINUTES[s_pick]) snprintf(label, sizeof(label), "%dM", AUTO_OFF_MINUTES[s_pick]);
        else snprintf(label, sizeof(label), "NONE");
        draw_card(NULL, &ICON_POWER, label, GFX_WHITE, s_pick, AUTO_OFF_OPTIONS,
                  AUTO_OFF_MINUTES[s_pick] == host_auto_off(), false);
        break;
    }
    case M_CLOCK_SET:
        draw_card(NULL, &ICON_CLOCK, CLOCK_ITEMS[s_clock_item], GFX_WHITE, s_clock_item, CLOCK_ITEM_COUNT, false, false);
        break;
    case M_CLOCK_PICK: {
        const int v = clock_option_value(s_pick);
        const bool current = v == host_clock_setting(CLOCK_KEYS[s_clock_item]);
        if (s_clock_item == 1 || s_clock_item == 2) {
            draw_time_choice(v, s_pick, clock_options(), current);
            break;
        }
        char label[16];
        if (s_clock_item == 0) snprintf(label, sizeof(label), v ? "24H" : "12H");
        else snprintf(label, sizeof(label), "%dM", v);
        draw_card(NULL, &ICON_CLOCK, label, GFX_WHITE, s_pick, clock_options(), current, false);
        break;
    }
    case M_OFF:
        break;
    }
}

/* The clock: the time along the bottom, and above it the sun or the moon. */

static gfx_color_t mix(gfx_color_t a, gfx_color_t b, int t)  // t: 0 = a ... 256 = b
{
    return (gfx_color_t){ a.r + (b.r - a.r) * t / 256, a.g + (b.g - a.g) * t / 256, a.b + (b.b - a.b) * t / 256 };
}

// How far the day has come up, 0 (night) to 256 (day): rising over the fade time from
// sunrise, and falling over the same time to reach night at sunset.
static int daylight(int second_of_day)
{
    const int rise = host_clock_setting(UI_CLOCK_RISE) * 60, set = host_clock_setting(UI_CLOCK_SET) * 60;
    const int fade = host_clock_setting(UI_CLOCK_FADE) * 60;
    const int t = second_of_day;
    if (fade <= 0) return t >= rise && t < set ? 256 : 0;
    if (t < rise || t >= set) return 0;
    if (t < rise + fade) return (t - rise) * 256 / fade;
    if (t >= set - fade) return (set - t) * 256 / fade;
    return 256;
}

static void draw_clock(int second_of_day)
{
    static const gfx_color_t NIGHT = { 0, 0, 0 }, DAY = { 92, 70, 22 };  // unlit; a warm light yellow
    // Six by six. Sun: O the darker orange rim, I the lighter middle. Moon: L its light
    // face, S a speckle, D the dark edge, heavier down the right as its shadow.
    static const char *const SUN[6]  = { ".OOOO.", "OOIIOO", "OIIIIO", "OIIIIO", "OOIIOO", ".OOOO." };
    static const char *const MOON[6] = { ".DLLD.", "DLLSLD", "LSLLLD", "LLLSDD", "DLSLDD", ".DDDD." };
    const int day = daylight(second_of_day);
    // The sky dims faster than the sun sinks (by the square), and is out altogether at night.
    const gfx_color_t sky = mix(NIGHT, DAY, day * day / 256);
    for (int y = 0; y < DISPLAY_H; y++) {
        for (int x = 0; x < DISPLAY_W; x++) gfx_set(x, y, sky);
    }
    // The moon keeps its place and fades with the night.
    for (int r = 0; r < 6 && day < 256; r++) {
        for (int c = 0; c < 6; c++) {
            const char k = MOON[r][c];
            if (k == '.') continue;
            const int v = k == 'L' ? 96 : k == 'S' ? 52 : 22;  // a dim moon: it is night
            gfx_set(5 + c, 1 + r, mix((gfx_color_t){ v, v, v }, sky, day));
        }
    }
    // The sun comes up from behind the halfway line to the top, and goes back the same way.
    const int sun_top = 8 - 7 * day / 256;
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c < 6; c++) {
            if (SUN[r][c] == '.' || sun_top + r >= 8) continue;
            gfx_set(5 + c, sun_top + r, SUN[r][c] == 'I' ? GFX_RGB(0xFFB21E) : GFX_RGB(0xFF6A00));
        }
    }

    // The time. A leading 1 is drawn one pixel wide, which is what lets 12:34 fit with
    // room round the colon; 20:00 to 23:59 fit only with the colon against the minutes.
    int hour = second_of_day / 3600;
    const int minute = second_of_day / 60 % 60;
    if (!host_clock_setting(UI_CLOCK_24H)) hour = hour % 12 ? hour % 12 : 12;
    bool lit[7][DISPLAY_W] = { 0 };  // rows 9-15; the digits sit in rows 10-14
    const int lead = hour / 10;
    const int lead_w = lead == 0 ? 0 : lead == 1 ? 2 : 4;  // with its gap
    const bool tight = lead == 2;
    const int width = lead_w + 3 + (tight ? 2 : 3) + 3 + 1 + 3;
    int x = (DISPLAY_W - width + 1) / 2;
    const int digits[4] = { lead, hour % 10, minute / 10, minute % 10 };
    for (int i = 0; i < 4; i++) {
        if (i == 0 && lead == 0) continue;
        const bool thin = i == 0 && lead == 1;
        for (int r = 0; r < 5; r++) {
            for (int c = 0; c < 3; c++) {
                if (thin ? c == 0 : DIGIT[digits[i]][r * 3 + c] == '1') lit[1 + r][x + (thin ? 0 : c)] = true;
            }
        }
        x += thin ? 2 : 4;
        if (i == 1) {
            lit[2][x] = lit[4][x] = true;  // the colon
            x += tight ? 1 : 2;
        }
    }
    // A dark box round the figures, one pixel clear of them; the sky shows to either side
    // when the time is a narrow one.
    int left = (DISPLAY_W - width + 1) / 2 - 1, right = left + width + 1;
    if (left <= 1) { left = 0; right = DISPLAY_W - 1; }  // no stray column of sky at the edge
    for (int r = 0; r < 7; r++) {
        for (int c = left < 0 ? 0 : left; c <= right && c < DISPLAY_W; c++) {
            gfx_set(c, 9 + r, lit[r][c] ? GFX_WHITE : (gfx_color_t){ 0, 0, 0 });
        }
    }
}

void ui_core_tick(void)
{
    s_t++;
    get_playback();

    if (s_overlay != OV_NONE && s_t >= s_overlay_until) s_overlay = OV_NONE;
    if (s_mode == M_CHAPTER) {
        if (!has_book()) show_home();
        else if (s_overlay == OV_NONE && s_t - s_mode_t >= chapter_timeout()) settle_player();
    }
    if (s_sleep_at && s_t >= s_sleep_at) {
        s_sleep_at = 0;
        s_sleep = 0;
        if (s_sleep_mode) s_sleep_done = true;  // stays dim; out-and-in adds a little more
        host_pause();
    }
    // The clock takes the display once nothing is playing and no knob has been touched for
    // a while, and gives it back when something starts.
    const bool busy = s_pb.state == UI_PLAY_PLAYING || s_pb.state == UI_PLAY_LOADING;
    int now_s;
    if ((busy && !s_clock_held) || (s_clock_held && s_mode != M_PLAYER) || s_mode == M_STATUS || s_mode == M_SETUP || s_mode == M_OFF) {
        s_clock_on = s_clock_held = false;
        s_last_input = s_t;
    } else if (!s_clock_on && s_t - s_last_input >= CLOCK_IDLE && host_time(&now_s)) {
        s_clock_on = true;
    }
    // Brightness: the day's or the night's level, lower when left alone, lowest in sleep mode.
    int level = BRIGHT_DAY;
    if (host_time(&now_s)) level = BRIGHT_NIGHT + (BRIGHT_DAY - BRIGHT_NIGHT) * daylight(now_s) / 256;
    if (s_mode != M_STATUS && s_mode != M_SETUP && s_t - s_last_touch >= DIM_IDLE) level /= 4;
    if (s_sleep_dim) level = BRIGHT_SLEEP;
    if (level != s_brightness && s_mode != M_OFF) {
        s_brightness = level;
        host_set_brightness(level);
    }
    // Left alone on battery, with nothing playing, the player switches itself off.
    if (busy || s_mode == M_SETUP || (s_t % MS(5000) == 0 && !host_on_battery())) s_last_active = s_t;
    // A battery nearly empty is shown once; an empty one is shown and the player switches
    // off, before the cell is run down further.
    if (s_t % MS(5000) == 0 && s_mode != M_OFF && !s_flat_at) {
        const int alarm = host_battery_alarm();
        if (alarm == 0) s_low_warned = false;
        if (alarm == 2 || (alarm == 1 && !s_low_warned)) {
            s_low_warned = true;
            host_battery(&s_battery);
            overlay(OV_BATTERY, MS(3000));
            if (alarm == 2) s_flat_at = s_t + MS(3000);
        }
    }
    if (s_flat_at && s_t >= s_flat_at && s_mode != M_OFF) {
        go(M_OFF);
        host_power_off();
        return;
    }
    const int auto_off = host_auto_off();
    if (auto_off > 0 && s_mode != M_OFF && s_t - s_last_active >= (unsigned)auto_off * MS(60000)) {
        go(M_OFF);
        host_power_off();
        return;
    }
    // Offline mode comes and goes with the Wi-Fi (the host decides when it has been gone
    // long enough). Either way the lists are rebuilt; going offline also returns to the
    // home list, which is then the downloaded books, unless a book is on the player screen.
    const bool offline = host_offline();
    if (offline != s_offline && s_mode != M_STATUS && s_mode != M_SETUP && s_mode != M_OFF) {
        s_offline = offline;
        if (offline && s_mode != M_PLAYER && s_mode != M_CHAPTER && s_mode < M_SETTINGS) show_home();
        else ui_core_books_changed();
        overlay(OV_WIFI, MS(2500));
    }
    // The LED ring. In order: the wake light while the sun comes up and for a while after;
    // a preview while its colour or brightness is being chosen; the nightlight while the
    // player lies on its face; otherwise off.
    s_face_down = host_face_down() && s_mode != M_SETUP && s_mode != M_OFF;
    s_wake = 0;
    if (host_light_setting(UI_LIGHT_WAKE) && host_time(&now_s)) {
        const int rise = host_clock_setting(UI_CLOCK_RISE) * 60;
        const int fade = host_clock_setting(UI_CLOCK_FADE) * 60;
        const int up = rise + fade, until = up + host_light_setting(UI_LIGHT_HOLD) * 60;
        if (now_s < rise || now_s >= until) {
            s_wake_cancelled = false;  // ready for tomorrow
        } else if (!s_wake_cancelled) {
            s_wake = now_s >= up || fade <= 0 ? 256 : (now_s - rise) * 256 / fade;
            if (s_wake < 1) s_wake = 1;
        }
    }
    const int bright = host_light_setting(UI_LIGHT_BRIGHT);
    unsigned colour = 0;
    int percent = 0;
    if (s_wake > 0) {
        colour = WAKE_RGB;
        percent = bright * s_wake / 256;
        if (percent < 1) percent = 1;
    } else if (s_mode == M_LIGHT_PICK && (s_light_item == 1 || s_light_item == 2)) {
        colour = s_light_item == 1 ? (unsigned)light_option_value(s_pick) : (unsigned)host_light_setting(UI_LIGHT_COLOUR);
        percent = s_light_item == 2 ? light_option_value(s_pick) : bright;
    } else if (s_face_down && host_light_setting(UI_LIGHT_NIGHT)) {
        colour = (unsigned)host_light_setting(UI_LIGHT_COLOUR);
        percent = bright;
    }
    const unsigned ring = ((((colour >> 16) & 0xFF) * percent / 100) << 16) | ((((colour >> 8) & 0xFF) * percent / 100) << 8) |
                          ((colour & 0xFF) * percent / 100);
    if (ring != s_ring) {
        s_ring = ring;
        host_set_ring(ring);
    }
    if (s_mode != M_STATUS) s_booting = false;
    if (s_books_waiting && s_t >= BOOT_MIN) {
        s_books_waiting = false;
        ui_core_books_changed();
    }
    if (s_card_pending && s_mode != M_STATUS && s_mode != M_SETUP) {
        s_card_pending = false;
        card_in();
    }
    // The player screen needs a book; without one the book list is the home screen.
    if (s_mode == M_PLAYER && !has_book()) show_home();
    // A new chapter, or a book that has just finished loading, is introduced afresh.
    static int last_chapter = -2;
    static ui_play_state_t last_state;
    if (s_mode == M_PLAYER && (s_pb.chapter_index != last_chapter ||
                               (last_state == UI_PLAY_LOADING && s_pb.state != UI_PLAY_LOADING))) {
        s_mode_t = s_t;
    }
    last_chapter = s_pb.chapter_index;
    last_state = s_pb.state;

    gfx_clear();
    if (s_face_down) return;  // face-down: the display is dark and the ring does the lighting
    switch (s_overlay) {
    case OV_VOLUME:    draw_volume(); break;
    case OV_PLAYPAUSE: draw_playpause(); break;
    case OV_SLEEP:
        gfx_text_centered("BED", 1, GFX_VIOLET);
        gfx_text_centered(s_sleep_word, 9, GFX_WHITE);
        break;
    case OV_WORD:
        gfx_text_centered(s_word, 5, GFX_YELLOW);
        break;
    case OV_HEART:
        draw_heart_animation();
        break;
    case OV_BATTERY:
        draw_battery();
        break;
    case OV_WIFI:
        draw_wifi(s_offline);
        break;
    case OV_MESSAGE:
        gfx_text(s_text, DISPLAY_W - scroll_px(s_t - s_message_start), 5, GFX_WHITE);
        break;
    case OV_NONE: {
        int now;
        if (s_clock_on && host_time(&now)) draw_clock(now);
        else draw_screen();
        break;
    }
    }
}

/* System screens */

void ui_core_init(void)
{
    s_mode = M_STATUS;
    s_text[0] = 0;
}

static void set_text(const char *s)
{
    snprintf(s_text, sizeof(s_text), "%s", s ? s : "");
    for (char *p = s_text; *p; p++) {
        if (*p == '\n') *p = ' ';
    }
}

void ui_core_status(const char *word, unsigned rgb)
{
    if (s_mode != M_STATUS && s_mode != M_SETUP) return;  // the UI proper has taken over
    set_text(word);
    s_text_color = GFX_RGB(rgb);
    go(M_STATUS);
}

void ui_core_setup(const char *password)
{
    set_text(password);
    go(M_SETUP);
}

void ui_core_message(const char *msg)
{
    if (!msg) {
        if (s_overlay == OV_MESSAGE) s_overlay = OV_NONE;
        return;
    }
    if (s_mode == M_SETUP) return;
    if (s_mode == M_STATUS) return;  // the status word shares the text buffer
    if (s_overlay == OV_BATTERY) return;  // a brief look at the battery is not to be cut short
    set_text(msg);
    s_message_start = s_t;
    // Once across: in from the right edge and fully out on the left.
    overlay(OV_MESSAGE, (int)scroll_ticks((int)strlen(s_text) * GFX_CHAR_W + DISPLAY_W));
}

void ui_core_word(const char *text, unsigned seconds)
{
    if (s_mode == M_OFF) return;
    snprintf(s_word, sizeof(s_word), "%s", text);
    overlay(OV_WORD, MS(seconds * 1000));
    s_last_touch = s_t;  // and at full brightness
}

void ui_core_books_changed(void)
{
    // Every index held is now stale: rebuild, and stay as close to where the user was as
    // makes sense.
    get_playback();
    const screen_t mode = s_mode;
    if (mode == M_SETUP || mode == M_OFF) return;
    if (host_book_count() == 0 && mode == M_STATUS) return;  // still nothing to show
    if (mode == M_STATUS && s_booting && s_t < BOOT_MIN) {
        s_books_waiting = true;  // let the start-up animation have its moment
        return;
    }
    const view_t view = s_view;
    if (mode == M_STATUS || !build_view(view)) {
        if (has_book() && mode != M_STATUS) { build_view(V_TITLE); go(M_PLAYER); }
        else show_home();
        return;
    }
    if (mode == M_GROUPS || mode == M_BOOKS) {
        s_group = clampi(s_group, 0, s_group_n > 0 ? s_group_n - 1 : 0);
        if (mode == M_BOOKS) {
            const int sel = s_sel;
            show_books(opens_sublist(view) ? s_group : -1);
            s_sel = clampi(sel, 0, s_count > 0 ? s_count - 1 : 0);
        }
    }
}
