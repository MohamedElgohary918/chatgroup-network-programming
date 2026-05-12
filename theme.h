#ifndef THEME_H
#define THEME_H

// ─────────────────────────────────────────────────────────────────────────────
// theme.h
// Color system and CSS for the whole application.
// ui_chat.c uses get_theme() to pick bubble colors.
// main.c calls theme_apply_css() once at startup.
// ─────────────────────────────────────────────────────────────────────────────

// One color theme = 3 coordinated hex colors
typedef struct
{
    const char *bg;         // bubble background    e.g. "#D1EAF8"
    const char *fg;         // text inside bubble   e.g. "#0a3a52"
    const char *name_color; // sender name above     e.g. "#185FA5"
} BubbleTheme;

// Built-in theme constants
extern const BubbleTheme ME_THEME;   // your own messages (green)
extern const BubbleTheme SYS_THEME;  // system lines (gray)
extern const BubbleTheme PVT_THEME;  // private messages (purple)

// Returns a consistent theme for a given username.
// Same name always maps to the same theme (hash-based).
const BubbleTheme *get_theme(const char *name);

// Fills buf with the current local time formatted as "08:57 PM"
void get_timestamp(char *buf, int len);

// Loads and applies the global CSS to the default GTK screen.
// Call this once at the start of main(), before creating any widgets.
void theme_apply_css(void);

#endif // THEME_H
