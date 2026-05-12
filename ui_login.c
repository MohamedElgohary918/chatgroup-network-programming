#include <string.h>
#include <gtk/gtk.h>
#include "ui_login.h"
#include "protocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// ui_login.c
// One job: show the "Welcome to Assiut Messenger" dialog, get a username.
// No network code here — just the dialog UI.
// ─────────────────────────────────────────────────────────────────────────────

// ── ui_login_run ──────────────────────────────────────────────────────────────
// Builds and runs a modal dialog.
// Returns 1 if the user entered a name and clicked Join.
// Returns 0 if they clicked Cancel or closed the dialog.
int ui_login_run(char *out_username, int max_len)
{
    // gtk_dialog_new_with_buttons: creates a dialog with custom buttons
    // GTK_DIALOG_MODAL: blocks the rest of the UI until closed
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Assiut Messenger",  // window title
        NULL,                // no parent window yet
        GTK_DIALOG_MODAL,
        "_Join",   GTK_RESPONSE_OK,
        "_Cancel", GTK_RESPONSE_CANCEL,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 320, -1);
    // 320px wide, height auto-fits the content

    // Get the content area (the gray box above the buttons in a GtkDialog)
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 20);
    // 20px padding on all sides inside the dialog

    // Title label — large bold text
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size='large' weight='bold'>👋  Welcome to Assiut Messenger</span>");
    gtk_widget_set_margin_bottom(title, 12);

    // Subtitle — smaller instruction text
    GtkWidget *sub = gtk_label_new("Enter a username to join the chat:");
    gtk_widget_set_halign(sub, GTK_ALIGN_START); // left-align
    gtk_widget_set_margin_bottom(sub, 6);

    // Text entry for the username
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Your name...");
    // placeholder shows gray hint text before the user types
    gtk_entry_set_max_length(GTK_ENTRY(entry), max_len - 1);
    // enforce the username length limit from protocol.h
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    // pressing Enter inside the entry clicks the default button (Join)

    // Pack all three into the dialog content area
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), sub,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dlg);

    // gtk_dialog_run blocks here until the user clicks a button
    int result = gtk_dialog_run(GTK_DIALOG(dlg));

    if (result == GTK_RESPONSE_OK) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        strncpy(out_username, text, max_len - 1);
        out_username[max_len - 1] = '\0';
    }

    gtk_widget_destroy(dlg); // always destroy the dialog when done
    return (result == GTK_RESPONSE_OK && strlen(out_username) > 0);
}
