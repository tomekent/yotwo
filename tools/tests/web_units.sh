#!/bin/sh
# Desktop tests of two pieces of the player's web server (firmware/main/web/web.c), lifted
# out of the source: the network name made from the player's name, and the check on paths
# asked for from the SD card (nothing outside the card may be named).
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/yotwo_web_units"
{
  cat <<'C'
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __linux__
static size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }
#endif
static const char *s_name;
static const char *ui_player_name(void) { return s_name; }
// Stand-ins for the web server's request: the query string is given directly.
typedef struct { const char *query; } httpd_req_t;
#define ESP_OK 0
static int httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t len) { if (!r->query) return -1; strlcpy(buf, r->query, len); return ESP_OK; }
static int httpd_query_key_value(const char *q, const char *key, char *val, size_t len)
{
    const char *p = strstr(q, "path=");
    if (!p) return -1;
    p += 5;
    size_t n = strcspn(p, "&");
    if (n >= len) n = len - 1;
    memcpy(val, p, n); val[n] = 0;
    return ESP_OK;
}
C
  sed -n '/^void web_hostname/,/^}/p' firmware/main/web/web.c
  sed -n '/^static bool sd_path/,/^}/p' firmware/main/web/web.c
  sed -n '/^typedef struct {$/,/^} asker_t;/p' firmware/main/web/web.c
  sed -n '/^static bool same_host/,/^}/p' firmware/main/web/web.c
  sed -n '/^static bool host_is_ours/,/^}/p' firmware/main/web/web.c
  sed -n '/^static const char \*refusal/,/^}/p' firmware/main/web/web.c
  cat <<'C'
static int pass, fail;
static void host(const char *name, const char *want)
{
    char out[48];
    s_name = name;
    web_hostname(out, sizeof(out));
    if (!strcmp(out, want)) pass++; else { fail++; printf("FAIL: name \"%s\": got \"%s\", wanted \"%s\"\n", name, out, want); }
}
static void path(const char *query, const char *want)  // want NULL = refused
{
    char out[280] = "";
    httpd_req_t r = { query };
    const bool ok = sd_path(&r, out, sizeof(out));
    if (want ? (ok && !strcmp(out, want)) : !ok) pass++;
    else { fail++; printf("FAIL: path query \"%s\": got %s \"%s\", wanted %s\n", query ? query : "(none)", ok ? "ok" : "refused", out, want ? want : "refused"); }
}
int main(void)
{
    host("Tom's Yotwo", "toms-yotwo");
    host("Yotwo", "yotwo");
    host("  Kitchen   Player 2 ", "kitchen-player-2");
    host("Zoë's", "zo-s");                 // letters outside a-z are dropped like punctuation
    host("---", "yotwo");                  // nothing usable
    host("", "yotwo");
    host("UPPER_case.name", "upper-case-name");
    host("A very long name indeed for a small player that goes on", "a-very-long-name-indeed-for-a-small-player-tha");

    path(NULL, "/sdcard");
    path("path=/", "/sdcard");
    path("path=/abs", "/sdcard/abs");
    path("path=/abs/", "/sdcard/abs");
    path("path=%2Fabs%2Fdl", "/sdcard/abs/dl");
    path("path=/abs/My+Book%20One", "/sdcard/abs/My Book One");
    path("path=/abs/../../etc", NULL);
    path("path=%2E%2E/secret", NULL);       // ".." after decoding, and not absolute
    path("path=/abs/%2e%2e/x", NULL);
    path("path=abs", NULL);                 // must be absolute
    path("path=", NULL);
    // Who may ask. The player is "toms-yotwo" at 192.168.1.18.
#define ASK(what, allowed, ...) do { const asker_t a_ = { __VA_ARGS__ }; const char *why_ = refusal(&a_, "toms-yotwo", "192.168.1.18"); \
        if ((why_ == NULL) == (allowed)) pass++; else { fail++; printf("FAIL: %s: %s\n", what, why_ ? why_ : "allowed"); } } while (0)
    const char *H = "toms-yotwo.local", *O = "http://toms-yotwo.local";
    // The page's own requests, as a current browser sends them.
    ASK("page typed into the address bar", true, .host = H, .fetch_site = "none", .page = true);
    ASK("page by IP address", true, .host = "192.168.1.18", .fetch_site = "none", .page = true);
    ASK("page with port and trailing dot", true, .host = "Toms-Yotwo.local.:80", .page = true);
    ASK("page from a link on another site", true, .host = H, .fetch_site = "cross-site", .page = true);
    ASK("the page reading its data", true, .host = H, .fetch_site = "same-origin", .marked = true, .data = true);
    ASK("the page saving settings", true, .host = H, .origin = O, .fetch_site = "same-origin", .marked = true, .json = true, .data = true, .change = true);
    ASK("the page by IP saving settings", true, .host = "192.168.1.18", .origin = "http://192.168.1.18", .fetch_site = "same-origin", .marked = true, .json = true, .data = true, .change = true);
    ASK("a cover in the page", true, .host = H, .fetch_site = "same-origin");
    ASK("a file opened from the page", true, .host = H, .fetch_site = "same-origin");
    ASK("an older browser (no Sec-Fetch-Site) saving", true, .host = H, .origin = O, .marked = true, .json = true, .data = true, .change = true);
    ASK("a script on the home network with the right headers", true, .host = H, .marked = true, .json = true, .data = true, .change = true);
    // What must be refused.
    ASK("a bare POST (no header, as from curl or a form)", false, .host = H, .json = true, .data = true, .change = true);
    ASK("a form on another site", false, .host = H, .origin = "http://evil.example", .fetch_site = "cross-site", .data = true, .change = true);
    ASK("another site's script with the header (after a preflight it should never pass)", false, .host = H, .origin = "http://evil.example", .fetch_site = "cross-site", .marked = true, .json = true, .data = true, .change = true);
    ASK("another site, older browser: Origin alone gives it away", false, .host = H, .origin = "https://evil.example", .marked = true, .json = true, .data = true, .change = true);
    ASK("an origin of null (sandboxed frame, file)", false, .host = H, .origin = "null", .marked = true, .json = true, .data = true, .change = true);
    ASK("a name pointed at the player's address (DNS rebinding)", false, .host = "evil.example", .fetch_site = "none", .page = true);
    ASK("rebinding, reading data as if same-origin", false, .host = "evil.example", .origin = "http://evil.example", .fetch_site = "same-origin", .marked = true, .data = true);
    ASK("no Host header at all", false, .fetch_site = "none", .page = true);
    ASK("data without the page's header", false, .host = H, .fetch_site = "same-origin", .data = true);
    ASK("another site embedding a cover", false, .host = H, .fetch_site = "cross-site");
    ASK("another site linking straight to a file", false, .host = H, .fetch_site = "cross-site");
    ASK("a save that is not JSON", false, .host = H, .origin = O, .fetch_site = "same-origin", .marked = true, .data = true, .change = true);
    ASK("a save from another port's page", false, .host = H, .origin = "http://toms-yotwo.local:8080", .fetch_site = "same-site", .marked = true, .json = true, .data = true, .change = true);
    ASK("the page uploading firmware (a file, not JSON)", true, .host = H, .origin = O, .fetch_site = "same-origin", .marked = true, .data = true, .change = true, .upload = true);
    ASK("firmware from another site", false, .host = H, .origin = "http://evil.example", .fetch_site = "cross-site", .marked = true, .data = true, .change = true, .upload = true);
    ASK("firmware posted bare (no header)", false, .host = H, .data = true, .change = true, .upload = true);
    ASK("firmware to a rebound name", false, .host = "evil.example", .origin = "http://evil.example", .fetch_site = "same-origin", .marked = true, .data = true, .change = true, .upload = true);
    ASK("a neighbour's player name", false, .host = "yotwo.local", .fetch_site = "none", .page = true);
    ASK("the player asking its own page (trial check)", true, .host = H, .marked = true, .data = true);
    ASK("going back, from the page", true, .host = H, .origin = O, .fetch_site = "same-origin", .marked = true, .json = true, .data = true, .change = true);
    ASK("going back, from another site", false, .host = H, .origin = "http://evil.example", .fetch_site = "cross-site", .marked = true, .json = true, .data = true, .change = true);
    printf("web units: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
C
} > "$OUT.c"
cc -w -o "$OUT" "$OUT.c"
"$OUT"

# Every route must fit in the server's table: one too many and it is silently not registered.
routes=$(sed -n '/^void web_start/,/^}/p' firmware/main/web/web.c | grep -c '{ .uri = ')
limit=$(sed -n 's/.*cfg.max_uri_handlers = \([0-9]*\);.*/\1/p' firmware/main/web/web.c)
if [ "$routes" -le "$limit" ]; then
  echo "web routes: $routes of $limit"
else
  echo "FAIL: $routes web routes but room for $limit (cfg.max_uri_handlers)"
  exit 1
fi
