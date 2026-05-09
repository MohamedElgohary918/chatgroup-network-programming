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

// File transfer state
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

void send_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        append_chat("❌ Cannot open file.");
        return;
    }
    char *base = basename((char*)filepath);
    append_chat("[System] Uploading file...");

    char buffer[MAX_BUF];
    size_t bytes;
    while ((bytes = fread(buffer, 1, MAX_BUF, f)) > 0) {
        PacketHeader h = {TYPE_FILE, bytes};
        strncpy(h.filename, base, 127);
        send(sock, &h, sizeof(h), 0);
        send(sock, buffer, bytes, 0);
        usleep(1000);
    }
    fclose(f);

    PacketHeader end = {TYPE_FILE_END, 0};
    strncpy(end.filename, base, 127);
    send(sock, &end, sizeof(end), 0);
    append_chat("[System] File upload finished.");
}

void handle_incoming_data() {
    PacketHeader h;
    if (recv_all(sock, (char*)&h, sizeof(h)) < 0) {
        append_chat("⚠️ Connection to server lost.");
        close(sock);
        sock = -1;
        return;
    }

    char payload[MAX_BUF+1] = {0};
    if (h.payload_size > 0) {
        recv_all(sock, payload, h.payload_size);
        payload[h.payload_size] = '\0';
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
        char display_buf[MAX_BUF+100];
        snprintf(display_buf, sizeof(display_buf), "%s: %s", h.sender_name, payload);
        append_chat(display_buf);
    }
    else if (h.type == TYPE_FILE) {
        char out_path[256];
        snprintf(out_path, sizeof(out_path), "received_%s", h.filename);
        int f = open(out_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (f >= 0) {
            write(f, payload, h.payload_size);
            close(f);
        }
    }
    else if (h.type == TYPE_FILE_END) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[System] File download complete: %s", h.filename);
        append_chat(msg);
    }
    else if (h.type == TYPE_FILE_INFO) {
        append_chat(payload);
    }
}

void on_send_clicked(GtkWidget *btn, gpointer data) {
    const char *msg = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (strlen(msg) == 0 || sock == -1) return;

    // Handle commands
    if (strncmp(msg, "/get ", 5) == 0) {
        const char *filename = msg + 5;
        PacketHeader h = {TYPE_FILE_REQ, strlen(filename)};
        send(sock, &h, sizeof(h), 0);
        send(sock, filename, strlen(filename), 0);
        append_chat("[System] Requesting file...");
        gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
        return;
    }
    if (strcmp(msg, "/files") == 0) {
        PacketHeader h = {TYPE_FILE_INFO, 0};
        send(sock, &h, sizeof(h), 0);
        gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
        return;
    }

    // Normal message (public or private)
    PacketHeader h = {TYPE_CHAT, strlen(msg)};
    strncpy(h.sender_name, my_username, sizeof(h.sender_name)-1);
    if (msg[0] == '@' && strchr(msg, ':')) {
        h.type = TYPE_PRIVATE;
    }
    send(sock, &h, sizeof(h), 0);
    send(sock, msg, h.payload_size, 0);

    char display_buf[MAX_BUF+60];
    snprintf(display_buf, sizeof(display_buf), "[Me]: %s", msg);
    append_chat(display_buf);
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

void on_file_clicked(GtkWidget *btn, gpointer win) {
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Select File to Send", GTK_WINDOW(win),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Send", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        send_file(filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void on_quit(GtkWidget *win, gpointer data) {
    running = 0;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    // Login dialog
    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK, "Welcome");
    GtkWidget *entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entry);
    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        strncpy(my_username, gtk_entry_get_text(GTK_ENTRY(entry)), 49);
    } else {
        gtk_widget_destroy(dialog);
        return 0;
    }
    gtk_widget_destroy(dialog);

    // Connect to server
    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Connection failed");
        return 1;
    }
    send(sock, my_username, strlen(my_username), 0);

    // Build main window
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Assiut Messenger");
    gtk_window_set_default_size(GTK_WINDOW(win), 750, 500);
    g_signal_connect(win, "destroy", G_CALLBACK(on_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    chat_buffer = gtk_text_buffer_new(NULL);
    GtkWidget *chat_view = gtk_text_view_new_with_buffer(chat_buffer);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(chat_view), FALSE);
    g_object_set_data(G_OBJECT(chat_buffer), "view", chat_view);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), chat_view);

    user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *user_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(user_tree),
        gtk_tree_view_column_new_with_attributes("Active Users", gtk_cell_renderer_text_new(), "text", 0, NULL));
    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(list_scroll), user_tree);
    gtk_widget_set_size_request(list_scroll, 180, -1);

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

    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btn_box, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 5);
    gtk_box_pack_start(GTK_BOX(hbox), list_scroll, FALSE, FALSE, 5);
    gtk_container_add(GTK_CONTAINER(win), hbox);
    gtk_widget_show_all(win);

    // Main loop with select()
    fd_set readfds;
    struct timeval tv;
    while (running && sock != -1) {
        while (gtk_events_pending()) gtk_main_iteration();
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        if (select(sock+1, &readfds, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(sock, &readfds)) {
                handle_incoming_data();
            }
        }
    }

    if (sock != -1) close(sock);
    return 0;
}
