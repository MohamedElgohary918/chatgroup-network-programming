#ifndef UI_CHAT_H
#define UI_CHAT_H

#include <gtk/gtk.h>

// ─────────────────────────────────────────────────────────────────────────────
// ui_chat.h
// Owns the chat message list (GtkListBox) and everything needed to
// display a message bubble: avatar, sender name, timestamp, bubble label.
// ─────────────────────────────────────────────────────────────────────────────

// Builds the chat panel widget (scrolled list + nothing else).
// Returns a GtkBox that main.c packs into the left side of the window.
// Also initialises the internal chat_list and chat_scroll globals
// so append_bubble() works immediately after this call.
GtkWidget *ui_chat_create(void);

// Inserts one message row into the chat list.
//   sender     — display name shown above the bubble (NULL for system msgs)
//   text       — the message body
//   is_me      — 1 → green bubble on the RIGHT
//   is_private — 1 → purple bubble with lock icon
//   is_system  — 1 → centered gray italic line, no avatar
void append_bubble(const char *sender, const char *text,
                   int is_me, int is_private, int is_system);

// Convenience wrappers — both show a centered gray system line.
void append_chat(const char *msg);
void append_system_msg(const char *msg);

#endif // UI_CHAT_H
