#!/bin/sh
# Desktop tests of small self-contained functions lifted straight out of the firmware
# source: the token scanner (abs_api.c, added by local.patch) and the battery curve.
set -e
cd "$(dirname "$0")/../.."
OUT="${TMPDIR:-/tmp}/yotwo_small_units"
{
  printf '%s\n' '#include <stdio.h>' '#include <string.h>' '#include <stdbool.h>' '#include <stddef.h>'
  sed -n '/^static bool scan_token/,/^}/p' firmware/main/abspresso/network/abs_api.c
  sed -n '/^static int percent_from/,/^}/p' firmware/main/hardware/battery.c
  printf '%s\n' 'static int s_volume_limit = 80;'
  grep '^static int output_for' firmware/main/ui/ui.c
  cat <<'C'
static int pass, fail;
#define CHECK(what, cond) do { if (cond) pass++; else { fail++; printf("FAIL: %s\n", what); } } while (0)
int main(void)
{
    char out[16];
    // The reply to a sign-in: tokens inside "user", among much else.
    const char *reply = "{\"user\":{\"id\":\"u1\",\"mediaProgress\":[{\"id\":\"p\",\"currentTime\":12.5}],"
                        "\"accessToken\":\"AAA.BBB\",\"refreshToken\":\"RRR\"},\"serverSettings\":{}}";
    CHECK("finds the access token", scan_token(reply, "accessToken", out, sizeof(out)) && !strcmp(out, "AAA.BBB"));
    CHECK("finds the refresh token", scan_token(reply, "refreshToken", out, sizeof(out)) && !strcmp(out, "RRR"));
    CHECK("a key that is not there", !scan_token(reply, "apiKey", out, sizeof(out)));
    CHECK("a null token is not a token", !scan_token("{\"refreshToken\":null}", "refreshToken", out, sizeof(out)));
    CHECK("an empty token is not a token", !scan_token("{\"accessToken\":\"\"}", "accessToken", out, sizeof(out)));
    CHECK("a token too long for the buffer is refused, not cut short",
          !scan_token("{\"accessToken\":\"0123456789ABCDEFGHIJ\"}", "accessToken", out, sizeof(out)));
    CHECK("exactly fitting is accepted", scan_token("{\"accessToken\":\"0123456789ABCDE\"}", "accessToken", out, sizeof(out)));
    CHECK("an escaped value is left to the full parser", !scan_token("{\"accessToken\":\"a\\\\b\"}", "accessToken", out, sizeof(out)));
    CHECK("an unterminated value", !scan_token("{\"accessToken\":\"abc", "accessToken", out, sizeof(out)));
    CHECK("no reply at all", !scan_token(NULL, "accessToken", out, sizeof(out)));
    CHECK("a longer key with the same ending does not match", !scan_token("{\"oldaccessTokenX\":\"zzz\"}", "accessToken", out, sizeof(out)));

    // The battery curve: monotonic, pinned at both ends.
    CHECK("empty at 3.30 V and below", percent_from(3300) == 0 && percent_from(2900) == 0);
    CHECK("full at 4.10 V and above", percent_from(4100) == 100 && percent_from(4250) == 100);
    CHECK("3.70 V is 38 %", percent_from(3700) == 38);
    int last = 0, ok = 1;
    for (int mv = 3000; mv <= 4300; mv++) { const int p = percent_from(mv); if (p < last || p < 0 || p > 100) ok = 0; last = p; }
    CHECK("never falls as the voltage rises, and stays within 0..100", ok);
    // The volume limit: the knob's 0-100 is spread over 0 to the limit.
    CHECK("top of the knob is the limit", output_for(100) == 80);
    CHECK("bottom is silence", output_for(0) == 0);
    CHECK("half way is half the limit", output_for(50) == 40);
    int rising = 1;
    for (int limit = 10; limit <= 100; limit += 10) {
        s_volume_limit = limit;
        if (output_for(100) != limit || output_for(0) != 0) rising = 0;
        for (int k = 1; k <= 100; k++) if (output_for(k) < output_for(k - 1) || output_for(k) > limit) rising = 0;
    }
    CHECK("for every limit: never above it, never falling as the knob rises", rising);
    printf("small units: %d passed, %d failed\n", pass, fail);
    return fail != 0;
}
C
} > "$OUT.c"
cc -w -o "$OUT" "$OUT.c"
"$OUT"
