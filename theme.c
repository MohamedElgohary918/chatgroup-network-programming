#include <time.h>
#include <gtk/gtk.h>
#include "theme.h"

// ─────────────────────────────────────────────────────────────────────────────
// theme.c
// Implements the color palette, timestamp helper, and global CSS.
// Nothing in here knows about sockets or specific widgets —
// it is pure visual configuration.
// ─────────────────────────────────────────────────────────────────────────────

// ── 6 rotating themes assigned to usernames by hash ──────────────────────────
static const BubbleTheme THEMES[] = {
    { "#D1EAF8", "#0a3a52", "#185FA5" }, // blue
    { "#C8F0E0", "#083325", "#0F6E56" }, // teal
    { "#F5D0DF", "#4a0e22", "#993556" }, // pink
    { "#DDD9F8", "#1a1452", "#534AB7" }, // purple
    { "#FAD9CC", "#4a1505", "#993C1D" }, // coral
    { "#D4ECC0", "#1a3a05", "#3B6D11" }, // green
};
#define NUM_THEMES 6

// ── special-purpose themes ────────────────────────────────────────────────────
const BubbleTheme ME_THEME  = { "#1D9E75", "#ffffff", "#ffffff" }; // green
const BubbleTheme SYS_THEME = { "#F0EFE8", "#888780", "#888780" }; // gray
const BubbleTheme PVT_THEME = { "#EDE9FB", "#26215C", "#534AB7" }; // purple

// ── get_theme ─────────────────────────────────────────────────────────────────
// Hashes the username string to a number, picks one of 6 themes.
// The same name always produces the same hash → same colors every session.
const BubbleTheme *get_theme(const char *name)
{
    unsigned int h = 0;
    for (int i = 0; name[i]; i++)
        h = h * 31 + (unsigned char)name[i]; // djb2-style hash
    return &THEMES[h % NUM_THEMES];
}

// ── get_timestamp ─────────────────────────────────────────────────────────────
// Writes the current local time into buf as "08:57 PM".
void get_timestamp(char *buf, int len)
{
    time_t t        = time(NULL);
    struct tm *tm_i = localtime(&t);
    strftime(buf, len, "%I:%M %p", tm_i);
}

// ── APP_CSS ───────────────────────────────────────────────────────────────────
// All CSS rules for the application in one place.
// Class names here must match the ones added via
// gtk_style_context_add_class() in ui_chat.c, ui_sidebar.c, ui_input.c.
static const char *APP_CSS =

    // window background
    "window { background-color: #EDECEA; }"

    // ── chat area (left panel) ────────────────────────────────────────────────
    ".chat-scroll { background-color: #F7F6F3; }"
    ".chat-scroll > viewport { background-color: #F7F6F3; }"
    ".chat-list { background-color: #F7F6F3; padding: 12px 0; }"
    // rows must be transparent — only the bubble label should show color
    ".chat-list > row { background: transparent; padding: 0; }"
    ".chat-list > row:hover { background: transparent; }"
    ".chat-list > row:selected { background: transparent; }"

    // ── sidebar (right panel) ─────────────────────────────────────────────────
    ".sidebar { background-color: #E4E2DD; border-left: 1px solid #D0CEC8; }"
    ".sidebar > viewport { background-color: #E4E2DD; }"
    ".user-list { background-color: transparent; }"
    ".user-list > row { padding: 2px 0; background: transparent; }"
    ".user-list > row:hover { background-color: #D8D5CF; border-radius: 8px; }"
    ".user-list > row:selected { background-color: #C8C4BD; border-radius: 8px; }"

    // ── message bubbles ───────────────────────────────────────────────────────
    ".bubble { border-radius: 16px; padding: 8px 14px; margin: 0; }"
    // my messages: right-aligned green, tail bottom-right
    ".bubble-me { background-color: #1D9E75; border-bottom-right-radius: 4px; }"
    // others: tail bottom-left (color set per-widget in ui_chat.c)
    ".bubble-other { border-bottom-left-radius: 4px; }"
    // private: purple, tail bottom-left
    ".bubble-private { background-color: #EDE9FB; border-bottom-left-radius: 4px; }"
    // system: no background, centered
    ".bubble-system { background-color: transparent; }"

    // ── input bar ─────────────────────────────────────────────────────────────
    ".input-bar {"
    "  background-color: #EDECEA;"
    "  border-top: 1px solid #D0CEC8;"
    "  padding: 10px 12px;"
    "}"
    ".msg-entry {"
    "  border-radius: 22px; padding: 8px 16px;"
    "  background-color: #FFFFFF; border: 1px solid #CCCAC4;"
    "  font-size: 14px; color: #2C2C2A;"
    "}"
    ".msg-entry:focus { border-color: #1D9E75; }"
    ".send-btn {"
    "  background-color: #1D9E75; color: #ffffff;"
    "  border-radius: 22px; border: none;"
    "  padding: 8px 20px; font-weight: bold; font-size: 14px;"
    "}"
    ".send-btn:hover  { background-color: #0F6E56; }"
    ".send-btn:active { background-color: #085041; }"
    ".file-btn {"
    "  border-radius: 22px; padding: 8px 14px;"
    "  background-color: #FFFFFF; border: 1px solid #CCCAC4; font-size: 13px;"
    "}"
    ".file-btn:hover { background-color: #F0EFE8; }";

// ── theme_apply_css ───────────────────────────────────────────────────────────
// Creates a GTK CSS provider, loads APP_CSS into it, and registers it
// globally so every widget in the process inherits the rules.
// Must be called after gtk_init() and before any window is shown.
void theme_apply_css(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, APP_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    // gtk_style_context_add_provider_for_screen applies to ALL widgets
    // on the default screen — no need to add it per widget
}
