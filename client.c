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
#include "protocol.h"

#define PORT 5050

int sock;
GtkTextBuffer *chat_buffer;
GtkListStore *user_list_store;
GtkWidget *entry_msg;
char my_username[50];
int running = 1;

// File Upload State
int file_fd = -1;
int is_sending = 0;

void append_chat(const char *msg) {
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(chat_buffer, &iter);
    gtk_text_buffer_insert(chat_buffer, &iter, msg, -1);
    gtk_text_buffer_insert(chat_buffer, &iter, "\n", -1);
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
        append_chat("⚠️ Connection lost.");
        sock = -1; return;
    }

    char payload[MAX_BUF + 1];
    recv_all(sock, payload, h.payload_size);
    payload[h.payload_size] = '\0';

    if (h.type == TYPE_LIST) {
        gtk_list_store_clear(user_list_store);
        char *name = strtok(payload, ",");
        while (name) {
            GtkTreeIter iter;
            gtk_list_store_append(user_list_store, &iter);
            gtk_list_store_set(user_list_store, &iter, 0, name, -1);
            name = strtok(NULL, ",");
        }
    } else if (h.type == TYPE_CHAT) {
        append_chat(payload);
    } else if (h.type == TYPE_FILE) {
        char out_file[128];
        snprintf(out_file, 128, "received_%s", h.filename);
        int f = open(out_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
        write(f, payload, h.payload_size);
        close(f);
    }
}

// GUI Callbacks
void on_send_clicked(GtkWidget *btn, gpointer data) {
    const char *msg = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (strlen(msg) == 0 || sock == -1) return;

    PacketHeader h = {TYPE_CHAT, strlen(msg)};
    strcpy(h.sender_name, my_username);
    send(sock, &h, sizeof(h), 0);
    send(sock, msg, h.payload_size, 0);

    char display_msg[1200];
    snprintf(display_msg, sizeof(display_msg), "[Me]: %s", msg);
    append_chat(display_msg);
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

void on_file_clicked(GtkWidget *btn, gpointer win) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Select File", GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        file_fd = open(filename, O_RDONLY);
        if (file_fd >= 0) {
            is_sending = 1;
            append_chat("[System] Starting file upload...");
        }
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void on_quit(GtkWidget *win, gpointer data) { running = 0; }

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    /* --- 1.Login --- */
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK, "Enter Username:");
    GtkWidget *name_entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), name_entry);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        strcpy(my_username, gtk_entry_get_text(GTK_ENTRY(name_entry)));
    } else return 0;
    gtk_widget_destroy(dialog);

    /* --- 2. Network Connect --- */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Connect failed");
        return 1;
    }
    send(sock, my_username, strlen(my_username), 0);

    /* --- 3. UI Setup --- */
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 700, 450);
    g_signal_connect(win, "destroy", G_CALLBACK(on_quit), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    chat_buffer = gtk_text_buffer_new(NULL);
    GtkWidget *chat_view = gtk_text_view_new_with_buffer(chat_buffer);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), chat_view);

    entry_msg = gtk_entry_new();
    GtkWidget *send_btn = gtk_button_new_with_label("Send");
    g_signal_connect(send_btn, "clicked", G_CALLBACK(on_send_clicked), NULL);
    GtkWidget *file_btn = gtk_button_new_with_label("📁 File");
    g_signal_connect(file_btn, "clicked", G_CALLBACK(on_file_clicked), win);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_pack_start(GTK_BOX(btn_box), entry_msg, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), file_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_box), send_btn, FALSE, FALSE, 0);

    user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), gtk_tree_view_column_new_with_attributes("Users", gtk_cell_renderer_text_new(), "text", 0, NULL));

    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btn_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 5);
    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(list_scroll), tree);
    gtk_widget_set_size_request(list_scroll, 150, -1);
    gtk_box_pack_start(GTK_BOX(hbox), list_scroll, FALSE, FALSE, 5);
    gtk_container_add(GTK_CONTAINER(win), hbox);
    gtk_widget_show_all(win);

    /* --- 4. Hybrid Select Loop --- */
    fd_set readfds, writefds;
    struct timeval tv;
    while (running) {
        // Handle UI events (clicks, typing, drawing)
        while (gtk_events_pending()) gtk_main_iteration();

        if (sock != -1) {
            FD_ZERO(&readfds);
            FD_ZERO(&writefds);
            FD_SET(sock, &readfds);
            if (is_sending) FD_SET(sock, &writefds);

            tv.tv_sec = 0;
            tv.tv_usec = 10000; // Wait 10ms for data

            if (select(sock + 1, &readfds, &writefds, NULL, &tv) > 0) {

                if (FD_ISSET(sock, &readfds)) handle_incoming_data();

                if (is_sending && FD_ISSET(sock, &writefds)) {
                    char f_buf[MAX_BUF];
                    int n = read(file_fd, f_buf, MAX_BUF);
                    if (n > 0) {
                        PacketHeader h = {TYPE_FILE, n};
                        send(sock, &h, sizeof(h), 0);
                        send(sock, f_buf, n, 0);
                    } else {
                        is_sending = 0;
                        close(file_fd);
                        append_chat("[System] File upload complete.");
                    }
                }
            }
        }
        else
        {
            // If socket is dead, just keep UI alive so user can close it
            usleep(10000);
        }
    }
    if (sock != -1) close(sock);
    return 0;
}
