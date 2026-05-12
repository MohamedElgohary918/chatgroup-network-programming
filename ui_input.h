#ifndef UI_INPUT_H
#define UI_INPUT_H

#include <gtk/gtk.h>

// ─────────────────────────────────────────────────────────────────────────────
// ui_input.h
// Owns the bottom input bar: the text entry, Send button, and File button.
// Also owns on_send_clicked and on_file_clicked.
// ─────────────────────────────────────────────────────────────────────────────

// Builds the input bar widget.
//   win      — parent window, passed to the file chooser dialog
//   sock_ptr — pointer to the socket fd so send() can be called
//   username — the logged-in user's name, put in PacketHeader.sender_name
// Returns a GtkBox to pack at the bottom of the chat column.
// Also exposes the entry widget via ui_input_get_entry().
GtkWidget *ui_input_create(GtkWidget *win, int *sock_ptr, const char *username);

// Returns the GtkEntry widget so ui_sidebar.c can write "@name: " into it.
GtkWidget *ui_input_get_entry(void);

#endif // UI_INPUT_H
