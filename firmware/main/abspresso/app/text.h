#pragma once

// Accent-insensitive text helpers for sorting and A-Z grouping of library titles (UTF-8).
// Accented Latin letters count as their base letter (É as E, ß as s, Ł as L).

// Case- and accent-insensitive compare, like strcasecmp. Letters outside Latin sort after Z.
int text_cmp(const char *a, const char *b);
// The A-Z group for a title: its first letter upper-cased with accents removed, or '#'.
char text_initial(const char *s);
