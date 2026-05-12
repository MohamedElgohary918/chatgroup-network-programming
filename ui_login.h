#ifndef UI_LOGIN_H
#define UI_LOGIN_H

// ─────────────────────────────────────────────────────────────────────────────
// ui_login.h
// Shows the login dialog and returns the chosen username.
// This is the only GTK code that runs before the main window exists.
// ─────────────────────────────────────────────────────────────────────────────

// Shows a modal dialog asking for a username.
// Writes the entered name into `out_username` (must be at least `max_len` bytes).
// Returns 1 if the user clicked Join, 0 if they clicked Cancel or closed.
int ui_login_run(char *out_username, int max_len);

#endif // UI_LOGIN_H
