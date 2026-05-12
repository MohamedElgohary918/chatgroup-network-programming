#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/socket.h>
#include <gtk/gtk.h>
#include "ui_input.h"
#include "ui_chat.h"
#include "protocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// ui_input.c
// Builds and manages the bottom input bar.
// Handles the Send button (text messages) and File button (file upload).
// Knows about the socket and protocol, but not about the sidebar.
// ─────────────────────────────────────────────────────────────────────────────

// Module-private state
static GtkWidget *entry_msg;     // the text input field
static int       *sock_ref;      // pointer to the socket fd (owned by main.c)
static char       my_name[MAX_NAME]; // copy of username for packet headers

// File-sending state (read by the select loop in main.c)
int  file_fd   = -1;   // fd of the file being sent (-1 = not sending)
int  is_sending = 0;   // 1 while a file upload is in progress
char current_sending_filename[128]; // base name of the file being sent

// ── on_send_clicked ───────────────────────────────────────────────────────────
// Called when Send button is clicked OR Enter is pressed in the text field.
// Reads the text, builds a PacketHeader, sends header + text to the server,
// then shows the message as our own bubble in the chat.
void on_send_clicked(GtkWidget *btn, gpointer data)
{
    const char *msg = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (strlen(msg) == 0 || *sock_ref == -1)
        return; // nothing to send or not connected

    // Build packet header
    PacketHeader h;
    h.type         = TYPE_CHAT;
    h.payload_size = strlen(msg);
    strncpy(h.sender_name, my_name, sizeof(h.sender_name) - 1);
    h.sender_name[sizeof(h.sender_name) - 1] = '\0';
    memset(h.filename, 0, sizeof(h.filename));

    // Detect private message: must start with "@name:"
    int is_private = (msg[0] == '@' && strchr(msg, ':'));
    if (is_private) h.type = TYPE_PRIVATE;

    // Send header first, then the message text as payload
    if (send(*sock_ref, &h, sizeof(h), 0) < 0 ||
        send(*sock_ref, msg, h.payload_size, 0) < 0)
    {
        append_system_msg("❌ Failed to send message.");
        return;
    }

    // Show the message immediately as our own bubble (right side, green)
    append_bubble(my_name, msg, 1, is_private, 0);
    gtk_entry_set_text(GTK_ENTRY(entry_msg), ""); // clear the input field
}

// ── on_file_clicked ───────────────────────────────────────────────────────────
// Opens a file chooser dialog. If the user picks a file, opens it and
// sets is_sending = 1 so the select loop in main.c starts sending chunks.
static void on_file_clicked(GtkWidget *btn, gpointer win)
{
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Select File to Send", GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Send",   GTK_RESPONSE_ACCEPT,
        NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        file_fd = open(filename, O_RDONLY);
        if (file_fd >= 0) {
            is_sending = 1;
            char *base = basename(filename);
            strncpy(current_sending_filename, base, 127);
            current_sending_filename[127] = '\0';
            append_system_msg("📤 Starting file upload...");
        } else {
            append_system_msg("❌ Could not open file.");
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

// ── ui_input_create ───────────────────────────────────────────────────────────
// Builds the input bar: [text entry] [📁 File] [Send ➤]
// Connects button signals and stores references for later use.
GtkWidget *ui_input_create(GtkWidget *win, int *sock_ptr, const char *username)
{
    sock_ref = sock_ptr; // store pointer — we read *sock_ref when sending
    strncpy(my_name, username, MAX_NAME - 1);
    my_name[MAX_NAME - 1] = '\0';

    // Horizontal box: the whole input bar in one row
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "input-bar");
    // "input-bar" → .input-bar in APP_CSS → border-top, background, padding

    // Text entry — where the user types their message
    entry_msg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_msg), "Type a message...");
    gtk_style_context_add_class(gtk_widget_get_style_context(entry_msg), "msg-entry");
    // pressing Enter triggers on_send_clicked (same as clicking the button)
    g_signal_connect(entry_msg, "activate", G_CALLBACK(on_send_clicked), NULL);

    // File button
    GtkWidget *file_btn = gtk_button_new_with_label("📁  File");
    gtk_style_context_add_class(gtk_widget_get_style_context(file_btn), "file-btn");
    g_signal_connect(file_btn, "clicked", G_CALLBACK(on_file_clicked), win);

    // Send button
    GtkWidget *send_btn = gtk_button_new_with_label("Send ➤");
    gtk_style_context_add_class(gtk_widget_get_style_context(send_btn), "send-btn");
    g_signal_connect(send_btn, "clicked", G_CALLBACK(on_send_clicked), NULL);

    // Pack: entry takes all remaining space; buttons are fixed width
    gtk_box_pack_start(GTK_BOX(bar), entry_msg, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(bar), file_btn,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), send_btn,  FALSE, FALSE, 0);

    return bar;
}

// ── ui_input_get_entry ────────────────────────────────────────────────────────
// Returns the entry widget so ui_sidebar.c can write "@name: " into it.
GtkWidget *ui_input_get_entry(void)
{
    return entry_msg;
}
