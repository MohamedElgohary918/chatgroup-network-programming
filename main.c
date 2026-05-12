#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <gtk/gtk.h>
#include "protocol.h"
#include "theme.h"
#include "ui_login.h"
#include "ui_chat.h" 
#include "ui_sidebar.h" 
#include "ui_input.h" 
#include "network.h"

/* 
    the compilation command -------> gcc main.c theme.c ui_chat.c ui_sidebar.c ui_input.c ui_login.c network.c \
    -o client \
    $(pkg-config --cflags --libs gtk+-3.0)     
*/

// ─────────────────────────────────────────────────────────────────────────────
// main.c
// The glue file. main() does only these things:
//   1. Init GTK + CSS
//   2. Run the login dialog
//   3. Connect to server
//   4. Build the main window by assembling widgets from each ui_*.c
//   5. Run the select loop (process GTK events + check socket)
// All logic lives in the other files — main.c just calls them in order.
// ─────────────────────────────────────────────────────────────────────────────

// ── Global state owned by main ────────────────────────────────────────────────
static int  sock    = -1;  // TCP socket fd; -1 = disconnected
static int  running =  1;  // select loop continues while this is 1
static char my_username[MAX_NAME]; // set once by ui_login_run()

// ── on_quit ───────────────────────────────────────────────────────────────────
// Connected to the window "destroy" signal.
// Sets running = 0 so the select loop exits cleanly.
static void on_quit(GtkWidget *win, gpointer data)
{
    running = 0;
}

// ── build_main_window ─────────────────────────────────────────────────────────
// Creates the main window and assembles the layout from sub-widgets.
// Each ui_*.c file builds its own section; this function arranges them.
// Extracted from main() to keep main() short and readable.
static GtkWidget *build_main_window(void)
{
    // Window title: "mark  |  Assiut Messenger"
    char title[110];
    snprintf(title, sizeof(title), "%s  |  Assiut Messenger", my_username);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_default_size(GTK_WINDOW(win), 820, 560);
    g_signal_connect(win, "destroy", G_CALLBACK(on_quit), NULL);

    // ── Chat panel (left side) ────────────────────────────────────────────────
    // ui_chat_create() builds the GtkListBox + scroll and returns a GtkBox
    GtkWidget *chat_box = ui_chat_create();

    // ── Input bar (bottom of the left panel) ─────────────────────────────────
    // ui_input_create() builds the entry + buttons and returns a GtkBox
    // It needs: the window (for file dialog parent), socket pointer, username
    GtkWidget *input_bar = ui_input_create(win, &sock, my_username);

    // Pack chat_box + input_bar into a vertical column
    GtkWidget *left_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(left_col), chat_box,   TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(left_col), input_bar,  FALSE, FALSE, 0);
    // TRUE,TRUE   → chat_box stretches to fill all remaining space
    // FALSE,FALSE → input_bar stays at its natural height

    // ── Sidebar (right side) ──────────────────────────────────────────────────
    // ui_sidebar_create() needs the entry widget so double-click on a user
    // can write "@name: " into it
    GtkWidget *entry    = ui_input_get_entry();
    GtkWidget *sidebar  = ui_sidebar_create(entry);

    // ── GtkPaned: horizontal split with a draggable divider ───────────────────
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), left_col, TRUE,  TRUE);
    // TRUE,TRUE  → left side resizes when window resizes
    gtk_paned_pack2(GTK_PANED(paned), sidebar,  FALSE, FALSE);
    // FALSE,FALSE → sidebar keeps its fixed width
    gtk_paned_set_position(GTK_PANED(paned), 600);
    // 600px from the left → sidebar starts at x=600

    gtk_container_add(GTK_CONTAINER(win), paned);
    gtk_widget_show_all(win);

    return win;
}

// ── run_select_loop ───────────────────────────────────────────────────────────
// The heart of the client.
// Alternates between:
//   - processing pending GTK events (redraws, button clicks, etc.)
//   - checking the socket for new data or write-readiness (for file sending)
// This is why the client doesn't need a separate thread for networking.
static void run_select_loop(void)
{
    // These are declared outside the loop to avoid re-declaration overhead
    fd_set readfds, writefds;
    struct timeval tv;

    // Access file-sending state from ui_input.c
    extern int  file_fd;
    extern int  is_sending;
    extern char current_sending_filename[128];

    while (running)
    {
        // ── Step 1: drain all pending GTK events ─────────────────────────────
        // gtk_events_pending() returns TRUE if there are events queued
        // gtk_main_iteration() processes one event (button click, redraw, etc.)
        // We loop until the queue is empty before blocking on select()
        while (gtk_events_pending())
            gtk_main_iteration();

        if (sock == -1) {
            // Not connected — just sleep briefly and check GTK events again
            usleep(10000); // 10ms
            continue;
        }

        // ── Step 2: set up fd_sets for select() ──────────────────────────────
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(sock, &readfds);  // always watch for incoming data
        if (is_sending)
            FD_SET(sock, &writefds); // watch for write-readiness during file send

        // Timeout: 10ms — short enough that GTK events feel responsive
        // Without a timeout, select() would block forever when no data arrives,
        // freezing the GTK interface
        tv.tv_sec  = 0;
        tv.tv_usec = 10000; // 10,000 microseconds = 10ms

        int ready = select(sock + 1, &readfds, &writefds, NULL, &tv);
        // sock + 1 = number of fds to scan (0 through sock)
        // ready    = how many fds are ready (0 = timeout, -1 = error)

        if (ready <= 0) continue; // timeout or error — go back to GTK events

        // ── Step 3: incoming data? ────────────────────────────────────────────
        if (FD_ISSET(sock, &readfds))
            handle_incoming_data(&sock);
        // handle_incoming_data sets sock = -1 on disconnect

        // ── Step 4: file sending ──────────────────────────────────────────────
        // is_sending is set to 1 by on_file_clicked() in ui_input.c
        // We send one chunk per iteration so GTK stays responsive
        if (is_sending && FD_ISSET(sock, &writefds))
        {
            char f_buf[MAX_BUF];
            int n = read(file_fd, f_buf, MAX_BUF);
            // read() returns: bytes read, 0 at end-of-file, -1 on error

            if (n > 0) {
                // Send this chunk as a TYPE_FILE packet
                PacketHeader h;
                h.type         = TYPE_FILE;
                h.payload_size = n;
                strncpy(h.sender_name, my_username, MAX_NAME - 1);
                strncpy(h.filename, current_sending_filename, 127);
                send(sock, &h, sizeof(h), 0);
                send(sock, f_buf, n, 0);
            } else {
                // n == 0 → reached end of file → upload complete
                is_sending = 0;
                close(file_fd);
                file_fd = -1;

                // Broadcast a "file sent" chat message to the group
                char done_msg[256];
                snprintf(done_msg, sizeof(done_msg),
                         "✅ Sent file: %s", current_sending_filename);

                PacketHeader h_done;
                h_done.type         = TYPE_CHAT;
                h_done.payload_size = strlen(done_msg);
                strncpy(h_done.sender_name, my_username, MAX_NAME - 1);
                memset(h_done.filename, 0, sizeof(h_done.filename));
                send(sock, &h_done, sizeof(h_done), 0);
                send(sock, done_msg, h_done.payload_size, 0);

                append_system_msg("[System] File upload finished.");
            }
        }
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
// Entry point. Short and readable — all logic is delegated.
int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);
    // gtk_init must be first — initialises GTK, connects to the display server

    signal(SIGPIPE, SIG_IGN);
    // Ignore SIGPIPE: without this, writing to a closed socket would
    // kill the process with a signal instead of returning -1 from send()

    // 1. Apply global CSS (defined in theme.c)
    theme_apply_css();

    // 2. Show login dialog — blocks until user clicks Join or Cancel
    if (!ui_login_run(my_username, MAX_NAME))
        return 0; // user cancelled

    // 3. Connect to the server
    sock = network_connect(my_username);
    if (sock < 0) {
        g_printerr("Could not connect to server at 127.0.0.1:%d\n", PORT);
        return 1;
    }

    // 4. Build and show the main window
    build_main_window();

    // 5. Show welcome messages in the chat area
    append_system_msg("──────────────────────────────────────────");
    char welcome[128];
    snprintf(welcome, sizeof(welcome),
             "👋  Welcome, %s! You are connected.", my_username);
    append_system_msg(welcome);
    append_system_msg("Double-click a name to send a private message.");
    append_system_msg("──────────────────────────────────────────");

    // 6. Run the select loop — blocks here until the window is closed
    run_select_loop();

    // 7. Cleanup
    if (sock != -1)
        close(sock);

    return 0;
}
