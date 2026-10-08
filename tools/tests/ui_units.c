// Unit tests for the pure functions inside firmware/main/ui/ui_core.c: built as one file
// with the UI core and the simulator's stand-in for the player, so the statics are in reach.
// Names the two files both use for their own statics: the simulator's get a prefix.
#define main sim_main
#define s_readable sim_s_readable
#include "../ui_sim/sim.c"
#undef main
#undef s_readable
#include "../../firmware/main/ui/ui_core.c"

static int s_pass, s_fail;
#define CHECK(what, cond) do { if (cond) s_pass++; else { s_fail++; printf("FAIL: %s (line %d)\n", what, __LINE__); } } while (0)
#define CHECK_STR(what, got, want) do { const char *g_ = (got); if (g_ && strcmp(g_, (want)) == 0) s_pass++; \
    else { s_fail++; printf("FAIL: %s: got \"%s\", wanted \"%s\"\n", what, g_ ? g_ : "(null)", (want)); } } while (0)

int main(void)
{
    char buf[160];

    // wrapi: lists go round in both directions, and an empty list is safe.
    CHECK("wrap forward", wrapi(5, 5) == 0);
    CHECK("wrap back", wrapi(-1, 5) == 4);
    CHECK("wrap far back", wrapi(-11, 5) == 4);
    CHECK("wrap empty", wrapi(3, 0) == 0);

    // daylight: 7:00 to 19:00 with 15 minutes each way (the defaults).
    CHECK("night before sunrise", daylight(6 * 3600 + 59 * 60 + 59) == 0);
    CHECK("sunrise begins at zero", daylight(7 * 3600) == 0);
    CHECK("half risen", daylight(7 * 3600 + 450) == 128);
    CHECK("fully up after the fade", daylight(7 * 3600 + 900) == 256);
    CHECK("day", daylight(12 * 3600) == 256);
    CHECK("still up before the sunset fade", daylight(19 * 3600 - 901) == 256);
    CHECK("half set", daylight(19 * 3600 - 450) == 128);
    CHECK("set at sunset", daylight(19 * 3600) == 0);
    CHECK("night", daylight(23 * 3600) == 0);
    CHECK("midnight", daylight(0) == 0);
    for (int t = 0; t < 86400; t += 7) {
        const int d = daylight(t);
        if (d < 0 || d > 256) { CHECK("daylight stays within 0..256", false); break; }
    }
    s_clock[UI_CLOCK_FADE] = 0;
    CHECK("no fade: day switches on at sunrise", daylight(7 * 3600) == 256 && daylight(7 * 3600 - 1) == 0);
    s_clock[UI_CLOCK_FADE] = 15;

    // Readability mode.
    sim_s_readable = true;
    CHECK_STR("The moves to the end", readable_title("The Gruffalo", buf, sizeof(buf)), "Gruffalo, The");
    CHECK_STR("the, any case", readable_title("the BFG", buf, sizeof(buf)), "BFG, The");
    CHECK_STR("Theatre is not The", readable_title("Theatre Shoes", buf, sizeof(buf)), "Theatre Shoes");
    CHECK_STR("a bare The is left", readable_title("The ", buf, sizeof(buf)), "The ");
    CHECK_STR("Chapter 12: name", readable_chapter("Chapter 12: The Wand", buf, sizeof(buf)), "12: The Wand");
    CHECK_STR("Chp. 2", readable_chapter("Chp. 2", buf, sizeof(buf)), "2");
    CHECK_STR("Track #3", readable_chapter("Track #3", buf, sizeof(buf)), "3");
    CHECK_STR("a word that only starts with ch", readable_chapter("Charlie arrives", buf, sizeof(buf)), "Charlie arrives");
    CHECK_STR("Chapter without a number", readable_chapter("Chapter the First", buf, sizeof(buf)), "Chapter the First");
    sim_s_readable = false;
    CHECK_STR("off: titles untouched", readable_title("The Gruffalo", buf, sizeof(buf)), "The Gruffalo");
    CHECK_STR("off: chapters untouched", readable_chapter("Chapter 12", buf, sizeof(buf)), "Chapter 12");

    // Chapter names on the chapter screen (the number is shown separately).
    s_book = 5;  // Room on the Broom: "Chapter 1: The Hat" ...
    CHECK_STR("name after the number", chapter_name(0), "The Hat");
    s_book = 6;  // Stick Man: "Chapter 1", "Chp. 2", "Track #3"
    CHECK("only a number: no name", chapter_name(0) == NULL && chapter_name(1) == NULL && chapter_name(2) == NULL);
    CHECK("no such chapter: no name", chapter_name(9) == NULL);
    s_book = -1;

    // Scrolling: the time to scroll a distance, and the distance scrolled in that time.
    static const int speeds[] = { 5, 8, 12, 18 };
    for (int i = 0; i < 4; i++) {
        s_speed = speeds[i];
        for (int px = 1; px < 400; px += 13) {
            const unsigned t = scroll_ticks(px);
            if (scroll_px(t) < px || (t > 0 && scroll_px(t - 1) >= px)) { CHECK("scroll_ticks is the first tick that reaches the distance", false); break; }
        }
    }
    s_speed = 12;
    CHECK("no distance, no time", scroll_ticks(0) == 0);

    // Grouping: series names lose their number; titles file under a letter or '#'.
    group_key(V_SERIES, 0, buf, sizeof(buf));
    CHECK_STR("series without its number", buf, "Charlie Bucket");
    group_key(V_TITLE, 2, buf, sizeof(buf));
    CHECK_STR("The Gruffalo files under G", buf, "G");
    CHECK("series order by number", series_number(1) > series_number(0));

    // Views: A-Z has every book; recently played only those played, newest first.
    CHECK("A-Z has all the books", build_view(V_TITLE) && s_order_n == 8);
    CHECK("recently played, newest first", build_view(V_LAST) && s_order_n == 3 && s_order[0] == 1 && s_order[2] == 5);
    CHECK("recently added, newest first", build_view(V_NEW) && s_order[0] == 1);
    CHECK("no favourites: empty view", !build_view(V_FAV));
    CHECK("authors: two groups", build_view(V_AUTHOR) && s_group_n == 2);

    // Minutes to frames at the UI's tick.
    CHECK("a minute of frames", minutes_to_frames(1) == 60000 / UI_FRAME_MS);

    printf("ui units: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail != 0;
}
