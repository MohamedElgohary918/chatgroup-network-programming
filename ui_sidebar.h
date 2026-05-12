#ifndef UI_SIDEBAR_H
#define UI_SIDEBAR_H

#include <gtk/gtk.h>

// ─────────────────────────────────────────────────────────────────────────────
// ui_sidebar.h
// Owns the right-side panel: the "ACTIVE USERS" list.
// When a user double-clicks a name, the private-message prefix is
// inserted into the input field automatically.
// ─────────────────────────────────────────────────────────────────────────────

// Builds the sidebar widget.
// entry_msg is passed in so on_user_activated() can write "@name: " into it.
// Returns a GtkScrolledWindow that main.c packs into the right side.
GtkWidget *ui_sidebar_create(GtkWidget *entry_msg);

// Clears the user list and repopulates it from a comma-separated string.
// Called by network.c whenever a TYPE_LIST packet arrives.
// e.g. payload = "mark,mina,fo2sh"
void ui_sidebar_update_users(const char *comma_list);

#endif // UI_SIDEBAR_H
