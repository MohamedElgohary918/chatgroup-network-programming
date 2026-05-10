#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <libgen.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"

#define PORT 8080
#define RECV_TIMEOUT_SEC 5

static int sock = -1;
static GtkTextBuffer *chat_buffer = NULL;
static GtkListStore *user_list_store = NULL;
static GtkWidget *entry_msg = NULL;
static char my_username[MAX_NAME] = {0};
static int running = 1;

static int recv_file_fd = -1;
static char recv_file_path[256] = {0};

static void header_to_network(PacketHeader *dst, const PacketHeader *src) {
    *dst = *src;
    dst->type = htonl(src->type);
    dst->payload_size = htonl(src->payload_size);
}

static void header_from_network(PacketHeader *h) {
    h->type = ntohl(h->type);
    h->payload_size = ntohl(h->payload_size);
    h->filename[MAX_FILENAME - 1] = '\0';
    h->sender_name[MAX_NAME - 1] = '\0';
    h->receiver_name[MAX_NAME - 1] = '\0';
}

static int send_all(int s, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(s, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int recv_all(int s, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = recv(s, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int send_packet(int s, const PacketHeader *h, const void *payload) {
    PacketHeader net_h;
    header_to_network(&net_h, h);
    if (send_all(s, &net_h, sizeof(net_h)) < 0) return -1;
    if (h->payload_size > 0 && payload) {
        if (send_all(s, payload, h->payload_size) < 0) return -1;
    }
    return 0;
}

static int recv_packet(int s, PacketHeader *h, char *payload, size_t payload_cap) {
    if (recv_all(s, h, sizeof(*h)) < 0) return -1;
    header_from_network(h);
    if (h->payload_size > payload_cap) return -2;
    if (h->payload_size > 0) {
        if (recv_all(s, payload, h->payload_size) < 0) return -1;
    }
    if (payload_cap > h->payload_size) payload[h->payload_size] = '\0';
    return 0;
}

static void set_socket_timeout(int s) {
    struct timeval timeout;
    timeout.tv_sec = RECV_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static void append_chat(const char *msg) {
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(chat_buffer, &iter);
    gtk_text_buffer_insert(chat_buffer, &iter, msg, -1);
    gtk_text_buffer_insert(chat_buffer, &iter, "\n", -1);

    GtkWidget *view = GTK_WIDGET(g_object_get_data(G_OBJECT(chat_buffer), "view"));
    if (view) {
        GtkTextMark *mark = gtk_text_buffer_get_insert(chat_buffer);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(view), mark);
    }
}

static int valid_filename(const char *name) {
    if (!name || name[0] == '\0') return 0;
    if (strlen(name) >= MAX_FILENAME) return 0;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return 0;
    return 1;
}

static void send_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        append_chat("❌ Cannot open file.");
        return;
    }

    char path_copy[512];
    snprintf(path_copy, sizeof(path_copy), "%s", filepath);
    char *base = basename(path_copy);

    if (!valid_filename(base)) {
        fclose(f);
        append_chat("❌ Invalid filename.");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char size_text[64];
    snprintf(size_text, sizeof(size_text), "%ld", size);

    PacketHeader start = {0};
    start.type = TYPE_FILE_START;
    start.payload_size = (uint32_t)strlen(size_text);
    snprintf(start.filename, sizeof(start.filename), "%s", base);
    snprintf(start.sender_name, sizeof(start.sender_name), "%s", my_username);

    if (send_packet(sock, &start, size_text) < 0) {
        fclose(f);
        append_chat("❌ Failed to start file upload.");
        return;
    }

    append_chat("[System] Uploading file...");

    char buffer[MAX_BUF];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        PacketHeader chunk = {0};
        chunk.type = TYPE_FILE_CHUNK;
        chunk.payload_size = (uint32_t)bytes;
        snprintf(chunk.filename, sizeof(chunk.filename), "%s", base);
        snprintf(chunk.sender_name, sizeof(chunk.sender_name), "%s", my_username);
        if (send_packet(sock, &chunk, buffer) < 0) {
            append_chat("❌ File upload failed during sending.");
            fclose(f);
            return;
        }
        while (gtk_events_pending()) gtk_main_iteration();
    }

    fclose(f);

    PacketHeader end = {0};
    end.type = TYPE_FILE_END;
    end.payload_size = 0;
    snprintf(end.filename, sizeof(end.filename), "%s", base);
    snprintf(end.sender_name, sizeof(end.sender_name), "%s", my_username);
    send_packet(sock, &end, NULL);
    append_chat("[System] File upload finished.");
}

static void update_user_list(char *payload) {
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

static void handle_incoming_data(void) {
    PacketHeader h;
    char payload[MAX_BUF + 1] = {0};
    int rc = recv_packet(sock, &h, payload, MAX_BUF);
    if (rc < 0) {
        append_chat(rc == -2 ? "⚠️ Invalid packet size from server." : "⚠️ Connection to server lost.");
        close(sock);
        sock = -1;
        running = 0;
        return;
    }

    switch (h.type) {
        case TYPE_LIST:
            update_user_list(payload);
            break;
        case TYPE_CHAT: {
            char display[MAX_BUF + 100];
            snprintf(display, sizeof(display), "%s: %s", h.sender_name, payload);
            append_chat(display);
            break;
        }
        case TYPE_PRIVATE: {
            char display[MAX_BUF + 100];
            snprintf(display, sizeof(display), "[Private from %s]: %s", h.sender_name, payload);
            append_chat(display);
            break;
        }
        case TYPE_ADMIN: {
            char display[MAX_BUF + 100];
            snprintf(display, sizeof(display), "📢 [ADMIN]: %s", payload);
            append_chat(display);
            break;
        }
        case TYPE_SYSTEM:
        case TYPE_FILE_INFO:
        case TYPE_ERROR:
            append_chat(payload);
            break;
        case TYPE_FILE_START: {
            if (!valid_filename(h.filename)) {
                append_chat("❌ Invalid incoming filename.");
                break;
            }
            if (recv_file_fd >= 0) close(recv_file_fd);
            snprintf(recv_file_path, sizeof(recv_file_path), "received_%s", h.filename);
            recv_file_fd = open(recv_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (recv_file_fd < 0) append_chat("❌ Cannot create received file.");
            else append_chat("[System] Download started...");
            break;
        }
        case TYPE_FILE_CHUNK:
            if (recv_file_fd >= 0) {
                if (write(recv_file_fd, payload, h.payload_size) < 0) {
                    append_chat("❌ Failed writing received file.");
                }
            }
            break;
        case TYPE_FILE_END:
            if (recv_file_fd >= 0) {
                close(recv_file_fd);
                recv_file_fd = -1;
                char msg[300];
                snprintf(msg, sizeof(msg), "[System] File download complete: %s", recv_file_path);
                append_chat(msg);
            }
            break;
        case TYPE_SHUTDOWN:
            append_chat("[System] Server is shutting down.");
            close(sock);
            sock = -1;
            running = 0;
            break;
        default:
            append_chat("[System] Unknown packet received.");
            break;
    }
}

static void on_send_clicked(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    const char *msg = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (!msg || strlen(msg) == 0 || sock == -1) return;

    if (strncmp(msg, "/get ", 5) == 0) {
        const char *filename = msg + 5;
        PacketHeader h = {0};
        h.type = TYPE_FILE_REQ;
        h.payload_size = (uint32_t)strlen(filename);
        snprintf(h.sender_name, sizeof(h.sender_name), "%s", my_username);
        send_packet(sock, &h, filename);
        append_chat("[System] Requesting file...");
        gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
        return;
    }

    if (strcmp(msg, "/files") == 0) {
        PacketHeader h = {0};
        h.type = TYPE_FILE_INFO;
        h.payload_size = 0;
        snprintf(h.sender_name, sizeof(h.sender_name), "%s", my_username);
        send_packet(sock, &h, NULL);
        gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
        return;
    }

    PacketHeader h = {0};
    const char *payload = msg;
    char private_payload[MAX_BUF + 1] = {0};

    if (msg[0] == '@') {
        const char *colon = strchr(msg, ':');
        if (colon && colon > msg + 1) {
            size_t target_len = (size_t)(colon - (msg + 1));
            if (target_len >= sizeof(h.receiver_name)) target_len = sizeof(h.receiver_name) - 1;
            memcpy(h.receiver_name, msg + 1, target_len);
            h.receiver_name[target_len] = '\0';
            snprintf(private_payload, sizeof(private_payload), "%s", colon + 1);
            payload = private_payload;
            h.type = TYPE_PRIVATE;
        }
    }

    if (h.type != TYPE_PRIVATE) h.type = TYPE_CHAT;
    h.payload_size = (uint32_t)strlen(payload);
    snprintf(h.sender_name, sizeof(h.sender_name), "%s", my_username);

    if (send_packet(sock, &h, payload) < 0) {
        append_chat("❌ Failed to send message.");
        return;
    }

    char display[MAX_BUF + 100];
    if (h.type == TYPE_PRIVATE) {
        snprintf(display, sizeof(display), "[Me -> %s]: %s", h.receiver_name, payload);
    } else {
        snprintf(display, sizeof(display), "[Me]: %s", payload);
    }
    append_chat(display);
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

static void on_file_clicked(GtkWidget *btn, gpointer win) {
    (void)btn;
    GtkWidget *dialog = gtk_file_chooser_dialog_new(
        "Select File to Send", GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Send", GTK_RESPONSE_ACCEPT, NULL);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        send_file(filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static void on_user_activated(GtkTreeView *tree_view, GtkTreePath *path,
                              GtkTreeViewColumn *column, gpointer data) {
    (void)column;
    (void)data;
    GtkTreeIter iter;
    gchar *username = NULL;
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_model_get(model, &iter, 0, &username, -1);
        if (username && strcmp(username, my_username) != 0) {
            char prefix[MAX_NAME + 4];
            snprintf(prefix, sizeof(prefix), "@%s:", username);
            gtk_entry_set_text(GTK_ENTRY(entry_msg), prefix);
            gtk_widget_grab_focus(entry_msg);
            gtk_editable_set_position(GTK_EDITABLE(entry_msg), -1);
        }
        g_free(username);
    }
}

static void on_quit(GtkWidget *win, gpointer data) {
    (void)win;
    (void)data;
    running = 0;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Login", NULL, GTK_DIALOG_MODAL,
                                                     "OK", GTK_RESPONSE_OK,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new("Enter username:");
    GtkWidget *entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 5);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        snprintf(my_username, sizeof(my_username), "%s", gtk_entry_get_text(GTK_ENTRY(entry)));
    } else {
        gtk_widget_destroy(dialog);
        return 0;
    }
    gtk_widget_destroy(dialog);

    if (strlen(my_username) == 0) snprintf(my_username, sizeof(my_username), "%s", "Guest");

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    set_socket_timeout(sock);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    PacketHeader login = {0};
    login.type = TYPE_LOGIN;
    login.payload_size = (uint32_t)strlen(my_username);
    snprintf(login.sender_name, sizeof(login.sender_name), "%s", my_username);
    if (send_packet(sock, &login, my_username) < 0) {
        perror("send login");
        close(sock);
        return 1;
    }

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
    g_signal_connect(user_tree, "row-activated", G_CALLBACK(on_user_activated), NULL);
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

    while (running && sock != -1) {
        while (gtk_events_pending()) gtk_main_iteration();

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;

        int activity = select(sock + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (activity > 0 && FD_ISSET(sock, &readfds)) {
            handle_incoming_data();
        }
    }

    if (recv_file_fd >= 0) close(recv_file_fd);
    if (sock != -1) close(sock);
    return 0;
}
