#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <fcntl.h>
#include <libgen.h>
#include "protocol.h"

#define PORT 8080

int sock;
GtkTextBuffer *chat_buffer;
GtkListStore *user_list_store;
GtkWidget *entry_msg;
char my_username[50];
int running = 1;

int file_fd = -1;
int is_sending = 0;
char current_sending_filename[128];

void append_chat(const char *msg) {
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(chat_buffer, &iter);
    gtk_text_buffer_insert(chat_buffer, &iter, msg, -1);
    gtk_text_buffer_insert(chat_buffer, &iter, "\n", -1);
    GtkTextMark *mark = gtk_text_buffer_get_insert(chat_buffer);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(chat_buffer), "view")), mark);
}

int recv_all(int s, char *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(s, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

void handle_incoming_data() {
    PacketHeader h;
    if (recv_all(sock, (char*)&h, sizeof(h)) < 0) {
        append_chat("⚠️ Connection to server lost.");
        close(sock);
        sock = -1;
        return;
    }


    char payload[MAX_BUF + 1];
    if (h.payload_size > 0) {
        recv_all(sock, payload, h.payload_size);
        payload[h.payload_size] = '\0';
    }

    if (h.type == TYPE_FILE_INFO) {
        // New type from teammate: shows the list of files on server
        append_chat("--- Server Files ---");
        append_chat(payload);
    }

    if (h.type == TYPE_LIST) {
        gtk_list_store_clear(user_list_store);
        char *name = strtok(payload, ",");
        while (name) {
            if (strlen(name) > 0) {
                GtkTreeIter iter;
                gtk_list_store_append(user_list_store, &iter);
                gtk_list_store_set(user_list_store, &iter, 0, name, -1);
            }
            name = strtok(NULL, ",");
        }
    }
    else if (h.type == TYPE_CHAT) {
        char display_buf[MAX_BUF + 100];
        snprintf(display_buf, sizeof(display_buf), "%s: %s", h.sender_name, payload);
        append_chat(display_buf);
    }
    else if (h.type == TYPE_FILE) {
        char out_path[300];
        snprintf(out_path, sizeof(out_path), "received_%s", h.filename);

        int f = open(out_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (f >= 0) {
            write(f, payload, h.payload_size);
            close(f);
            // Log message removed from here to prevent "chunk spam"
        }
    }
    else if (h.type == TYPE_FILE_END) {
        char msg[300];
        snprintf(msg, sizeof(msg), "[System] File download complete: %s", h.filename);
        append_chat(msg);
    }
}

void on_send_clicked(GtkWidget *btn, gpointer data) {
    const char *msg = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (strlen(msg) == 0 || sock == -1) return;

    if (strncmp(msg, "/get ", 5) == 0) {
        const char *fname = msg + 5;
        PacketHeader h = {TYPE_FILE_REQ, strlen(fname)};
        send(sock, &h, sizeof(h), 0);
        send(sock, fname, h.payload_size, 0);
        gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
        return;
    }

    if (strcmp(msg, "/files") == 0) {
        PacketHeader h = {TYPE_FILE_INFO, 0};
        send(sock, &h, sizeof(h), 0);
        gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
        return;
    }

    PacketHeader h = {TYPE_CHAT, strlen(msg)};
    strncpy(h.sender_name, my_username, sizeof(h.sender_name)-1);

    if (msg[0] == '@' && strchr(msg, ':')) {
        h.type = TYPE_PRIVATE;
    }

    if (send(sock, &h, sizeof(h), 0) < 0 || send(sock, msg, h.payload_size, 0) < 0) {
        append_chat("❌ Failed to send message.");
        return;
    }

    char display_msg[MAX_BUF + 60];
    snprintf(display_msg, sizeof(display_msg), "[Me]: %s", msg);
    append_chat(display_msg);
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

void on_file_clicked(GtkWidget *btn, gpointer win) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Select File to Send", GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Send", GTK_RESPONSE_ACCEPT, NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        file_fd = open(filename, O_RDONLY);
        if (file_fd >= 0) {
            is_sending = 1;
            char *base = basename(filename);
            strncpy(current_sending_filename, base, 127);
            append_chat("[System] Starting file upload...");
        } else {
            append_chat("❌ Could not open file.");
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void on_quit(GtkWidget *win, gpointer data) { running = 0; }

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    /* --- Login and Connect (Unchanged) --- */
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK, "Welcome to Assiut Chat");
    GtkWidget *name_entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), name_entry);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        strncpy(my_username, gtk_entry_get_text(GTK_ENTRY(name_entry)), 49);
    } else return 0;
    gtk_widget_destroy(dialog);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    send(sock, my_username, strlen(my_username), 0);

    /* --- UI Setup (Unchanged) --- */
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Assiut Messenger");
    gtk_window_set_default_size(GTK_WINDOW(win), 750, 500);
    g_signal_connect(win, "destroy", G_CALLBACK(on_quit), NULL);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    chat_buffer = gtk_text_buffer_new(NULL);
    GtkWidget *chat_view = gtk_text_view_new_with_buffer(chat_buffer);
    g_object_set_data(G_OBJECT(chat_buffer), "view", chat_view);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), chat_view);
    entry_msg = gtk_entry_new();
    g_signal_connect(entry_msg, "activate", G_CALLBACK(on_send_clicked), NULL);
    GtkWidget *send_btn = gtk_button_new_with_label("Send");
    g_signal_connect(send_btn, "clicked", G_CALLBACK(on_send_clicked), NULL);
    GtkWidget *file_btn = gtk_button_new_with_label("📁 File");
    g_signal_connect(file_btn, "clicked", G_CALLBACK(on_file_clicked), win);
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(btn_box), entry_msg, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), file_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), send_btn, FALSE, FALSE, 0);
    user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), gtk_tree_view_column_new_with_attributes("Active Users", gtk_cell_renderer_text_new(), "text", 0, NULL));
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btn_box, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 5);
    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(list_scroll), tree);
    gtk_widget_set_size_request(list_scroll, 180, -1);
    gtk_box_pack_start(GTK_BOX(hbox), list_scroll, FALSE, FALSE, 5);
    gtk_container_add(GTK_CONTAINER(win), hbox);
    gtk_widget_show_all(win);

    /* --- Hybrid Select Loop --- */
    fd_set readfds, writefds;
    struct timeval tv;
    while (running) {
        while (gtk_events_pending()) gtk_main_iteration();

        if (sock != -1) {
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            FD_SET(sock, &readfds);
            if (is_sending) FD_SET(sock, &writefds);

            tv.tv_sec = 0;
            tv.tv_usec = 10000;

            if (select(sock + 1, &readfds, &writefds, NULL, &tv) > 0) {
                if (FD_ISSET(sock, &readfds)) handle_incoming_data();

                if (is_sending && FD_ISSET(sock, &writefds)) {
                    char f_buf[MAX_BUF];
                    int n = read(file_fd, f_buf, MAX_BUF);
                    if (n > 0) {
                        PacketHeader h = {TYPE_FILE, n};
                        strncpy(h.sender_name, my_username, 49);
                        strncpy(h.filename, current_sending_filename, 255);
                        send(sock, &h, sizeof(h), 0);
                        send(sock, f_buf, n, 0);
                    } else {
                        // 1. Stop the sending state
                        is_sending = 0;
                        close(file_fd);

                        // 2. CRITICAL: Send the TYPE_FILE_END packet so the server saves to DB
                        PacketHeader h_end = {TYPE_FILE_END, 0};
                        strncpy(h_end.filename, current_sending_filename, 127);
                        send(sock, &h_end, sizeof(h_end), 0);
                        // (Note: payload_size is 0, so no second send() is needed here)

                        // 3. Create a "File finished" message for the chat UI
                        char completion_msg[256];
                        snprintf(completion_msg, sizeof(completion_msg), "✅ Sent file: %s", current_sending_filename);

                        // 4. Send the chat notification so other people see it in text
                        PacketHeader h_chat = {TYPE_CHAT, strlen(completion_msg)};
                        strncpy(h_chat.sender_name, my_username, 49);
                        send(sock, &h_chat, sizeof(h_chat), 0);
                        send(sock, completion_msg, h_chat.payload_size, 0);

                        append_chat("[System] File upload finished.");
                    }

                }
            }
        } else {
            usleep(10000);
        }
    }
    if (sock != -1) close(sock);
    return 0;
}
