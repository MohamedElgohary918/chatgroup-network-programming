#include <arpa/inet.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "chat_database.h"

#define PORT 8080
#define MAX_CLIENTS 100
#define FILES_DIR "files"
#define RECV_TIMEOUT_SEC 5

typedef struct {
    int socket;
    char username[MAX_NAME];
    int receiving_file;
    char upload_filename[MAX_FILENAME];
    char temp_filename[512];
    FILE *temp_file;
    long received_bytes;
} ChatClient;

static ChatClient *clients[MAX_CLIENTS] = {0};
static GtkTextBuffer *log_buffer = NULL;
static GtkListStore *user_list_store = NULL;
static int server_running = 1;
static sqlite3 *db = NULL;

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

static int send_all(int sock, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent_total = 0;

    while (sent_total < len) {
        ssize_t n = send(sock, p + sent_total, len - sent_total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent_total += (size_t)n;
    }
    return 0;
}

static int recv_all(int sock, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t total = 0;

    while (total < len) {
        ssize_t n = recv(sock, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int send_packet(int sock, const PacketHeader *h, const void *payload) {
    PacketHeader net_h;
    header_to_network(&net_h, h);

    if (send_all(sock, &net_h, sizeof(net_h)) < 0) return -1;
    if (h->payload_size > 0 && payload) {
        if (send_all(sock, payload, h->payload_size) < 0) return -1;
    }
    return 0;
}

static int recv_packet(int sock, PacketHeader *h, char *payload, size_t payload_cap) {
    if (recv_all(sock, h, sizeof(*h)) < 0) return -1;
    header_from_network(h);

    if (h->payload_size > payload_cap) return -2;

    if (h->payload_size > 0) {
        if (recv_all(sock, payload, h->payload_size) < 0) return -1;
    }
    if (payload_cap > h->payload_size) payload[h->payload_size] = '\0';
    return 0;
}

static void set_socket_timeout(int sock) {
    struct timeval timeout;
    timeout.tv_sec = RECV_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

static void log_message(const char *msg) {
    if (!log_buffer) {
        printf("%s\n", msg);
        return;
    }
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(log_buffer, &iter);
    gtk_text_buffer_insert(log_buffer, &iter, msg, -1);
    gtk_text_buffer_insert(log_buffer, &iter, "\n", -1);
}

static int valid_username(const char *name) {
    if (!name || name[0] == '\0') return 0;
    if (strlen(name) >= MAX_NAME) return 0;
    for (size_t i = 0; name[i]; i++) {
        if (name[i] == ',' || name[i] == ':' || name[i] == '|' ||
            name[i] == '\n' || name[i] == '\r' || name[i] == '/') {
            return 0;
        }
    }
    return 1;
}

static int valid_filename(const char *name) {
    if (!name || name[0] == '\0') return 0;
    if (strlen(name) >= MAX_FILENAME) return 0;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return 0;
    return 1;
}

static int username_exists(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && strcmp(clients[i]->username, name) == 0) return 1;
    }
    return 0;
}

static int find_client_by_name(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && strcmp(clients[i]->username, name) == 0) return i;
    }
    return -1;
}

static void update_user_list_ui(void) {
    gtk_list_store_clear(user_list_store);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            GtkTreeIter iter;
            gtk_list_store_append(user_list_store, &iter);
            gtk_list_store_set(user_list_store, &iter, 0, clients[i]->username, -1);
        }
    }
}

static void send_system_message(int sock, uint32_t type, const char *msg) {
    PacketHeader h = {0};
    h.type = type;
    h.payload_size = (uint32_t)strlen(msg);
    snprintf(h.sender_name, sizeof(h.sender_name), "%s", "SYSTEM");
    send_packet(sock, &h, msg);
}

static int send_packet_to_client(int idx, const PacketHeader *h, const void *payload) {
    if (!clients[idx]) return -1;
    return send_packet(clients[idx]->socket, h, payload);
}

static void broadcast_packet(int sender_idx, PacketHeader *h, const void *payload) {
    if (sender_idx >= 0 && clients[sender_idx]) {
        snprintf(h->sender_name, sizeof(h->sender_name), "%s", clients[sender_idx]->username);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && i != sender_idx) {
            if (send_packet_to_client(i, h, payload) < 0) {
                /* Disconnect later through receive path; avoid modifying array during broadcast. */
            }
        }
    }
}

static void broadcast_user_list(void) {
    char list_str[8192] = "";
    size_t used = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            int written = snprintf(list_str + used, sizeof(list_str) - used,
                                   "%s,", clients[i]->username);
            if (written < 0 || (size_t)written >= sizeof(list_str) - used) break;
            used += (size_t)written;
        }
    }

    PacketHeader h = {0};
    h.type = TYPE_LIST;
    h.payload_size = (uint32_t)strlen(list_str);
    snprintf(h.sender_name, sizeof(h.sender_name), "%s", "SYSTEM");
    broadcast_packet(-1, &h, list_str);
    update_user_list_ui();
}

static void disconnect_client(int idx) {
    if (!clients[idx]) return;

    char exit_msg[160];
    snprintf(exit_msg, sizeof(exit_msg), "🏃 %s left", clients[idx]->username);
    log_message(exit_msg);

    if (clients[idx]->temp_file) {
        fclose(clients[idx]->temp_file);
        remove(clients[idx]->temp_filename);
    }

    close(clients[idx]->socket);
    free(clients[idx]);
    clients[idx] = NULL;
    broadcast_user_list();
}

static void send_chat_history(int client_socket) {
    char history[16384] = {0};
    if (get_public_messages(db, "general", 50, history, sizeof(history)) && strlen(history) > 0) {
        PacketHeader h = {0};
        h.type = TYPE_SYSTEM;
        h.payload_size = (uint32_t)strlen(history);
        snprintf(h.sender_name, sizeof(h.sender_name), "%s", "SYSTEM");
        send_packet(client_socket, &h, history);
    }

    send_system_message(client_socket, TYPE_SYSTEM, "--- Chat history loaded ---");
}

static void send_file_list(int client_socket) {
    FileInfo *files = NULL;
    int count = 0;
    char list_str[4096] = "Available files:\n";
    size_t used = strlen(list_str);

    if (get_all_files(db, &files, &count) && count > 0) {
        for (int i = 0; i < count; i++) {
            int written = snprintf(list_str + used, sizeof(list_str) - used,
                                   "  %s (%ld bytes) - uploaded by %s\n",
                                   files[i].filename, files[i].size, files[i].sender);
            if (written < 0 || (size_t)written >= sizeof(list_str) - used) break;
            used += (size_t)written;
        }
    } else {
        snprintf(list_str, sizeof(list_str), "Available files:\n  No files uploaded yet.\n");
    }

    free_file_list(files, count);

    PacketHeader h = {0};
    h.type = TYPE_FILE_INFO;
    h.payload_size = (uint32_t)strlen(list_str);
    snprintf(h.sender_name, sizeof(h.sender_name), "%s", "SYSTEM");
    send_packet(client_socket, &h, list_str);
}

static void handle_file_request(int client_socket, const char *filename) {
    if (!valid_filename(filename)) {
        send_system_message(client_socket, TYPE_ERROR, "Invalid filename.");
        return;
    }

    char *filepath = get_file_path(db, filename);
    if (!filepath) {
        send_system_message(client_socket, TYPE_ERROR, "File not found on server.");
        return;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        free(filepath);
        send_system_message(client_socket, TYPE_ERROR, "Cannot open requested file on server.");
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
    snprintf(start.filename, sizeof(start.filename), "%s", filename);
    snprintf(start.sender_name, sizeof(start.sender_name), "%s", "SERVER");
    send_packet(client_socket, &start, size_text);

    char buffer[MAX_BUF];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        PacketHeader chunk = {0};
        chunk.type = TYPE_FILE_CHUNK;
        chunk.payload_size = (uint32_t)bytes;
        snprintf(chunk.filename, sizeof(chunk.filename), "%s", filename);
        snprintf(chunk.sender_name, sizeof(chunk.sender_name), "%s", "SERVER");
        if (send_packet(client_socket, &chunk, buffer) < 0) break;
    }

    PacketHeader end = {0};
    end.type = TYPE_FILE_END;
    end.payload_size = 0;
    snprintf(end.filename, sizeof(end.filename), "%s", filename);
    snprintf(end.sender_name, sizeof(end.sender_name), "%s", "SERVER");
    send_packet(client_socket, &end, NULL);

    fclose(f);
    free(filepath);
}

static void handle_private_message(int idx, PacketHeader *h, char *payload) {
    ChatClient *client = clients[idx];
    if (!client) return;

    if (h->receiver_name[0] == '\0') {
        send_system_message(client->socket, TYPE_ERROR, "Private message missing receiver.");
        return;
    }

    int target_idx = find_client_by_name(h->receiver_name);
    if (target_idx < 0) {
        send_system_message(client->socket, TYPE_ERROR, "Private message failed: user not found.");
        return;
    }

    save_message(db, "general", client->username, payload, 1, h->receiver_name);

    PacketHeader out = {0};
    out.type = TYPE_PRIVATE;
    out.payload_size = h->payload_size;
    snprintf(out.sender_name, sizeof(out.sender_name), "%s", client->username);
    snprintf(out.receiver_name, sizeof(out.receiver_name), "%s", h->receiver_name);
    send_packet_to_client(target_idx, &out, payload);
}

static void handle_file_start(int idx, PacketHeader *h) {
    ChatClient *client = clients[idx];
    if (!client) return;

    if (!valid_filename(h->filename)) {
        send_system_message(client->socket, TYPE_ERROR, "Invalid upload filename.");
        return;
    }

    mkdir(FILES_DIR, 0777);

    if (client->temp_file) {
        fclose(client->temp_file);
        remove(client->temp_filename);
        client->temp_file = NULL;
    }

    snprintf(client->upload_filename, sizeof(client->upload_filename), "%s", h->filename);
    snprintf(client->temp_filename, sizeof(client->temp_filename), "%s/temp_%s_%ld.tmp",
             FILES_DIR, client->username, (long)time(NULL));

    client->temp_file = fopen(client->temp_filename, "wb");
    if (!client->temp_file) {
        send_system_message(client->socket, TYPE_ERROR, "Server failed to create upload file.");
        return;
    }

    client->receiving_file = 1;
    client->received_bytes = 0;
    send_system_message(client->socket, TYPE_SYSTEM, "Server is receiving file...");
}

static void handle_file_chunk(int idx, PacketHeader *h, const char *payload) {
    ChatClient *client = clients[idx];
    if (!client || !client->receiving_file || !client->temp_file) {
        if (client) send_system_message(client->socket, TYPE_ERROR, "File chunk received without FILE_START.");
        return;
    }

    size_t written = fwrite(payload, 1, h->payload_size, client->temp_file);
    client->received_bytes += (long)written;
    if (written != h->payload_size) {
        send_system_message(client->socket, TYPE_ERROR, "Server failed while writing file chunk.");
    }
}

static void handle_file_end(int idx) {
    ChatClient *client = clients[idx];
    if (!client || !client->receiving_file || !client->temp_file) return;

    fclose(client->temp_file);
    client->temp_file = NULL;
    client->receiving_file = 0;

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", FILES_DIR, client->upload_filename);
    if (rename(client->temp_filename, final_path) != 0) {
        send_system_message(client->socket, TYPE_ERROR, "Server failed to finalize uploaded file.");
        return;
    }

    struct stat st;
    long file_size = 0;
    if (stat(final_path, &st) == 0) file_size = (long)st.st_size;
    save_file_metadata(db, client->upload_filename, client->username, file_size, "general");

    char notice[MAX_BUF];
    snprintf(notice, sizeof(notice), "📁 New file uploaded: %s. Use /get %s to download.",
             client->upload_filename, client->upload_filename);

    PacketHeader h = {0};
    h.type = TYPE_SYSTEM;
    h.payload_size = (uint32_t)strlen(notice);
    snprintf(h.sender_name, sizeof(h.sender_name), "%s", "SYSTEM");
    broadcast_packet(-1, &h, notice);

    log_message(notice);
}

static void handle_client_message(int idx) {
    ChatClient *client = clients[idx];
    if (!client) return;

    PacketHeader h;
    char payload[MAX_BUF + 1] = {0};
    int rc = recv_packet(client->socket, &h, payload, MAX_BUF);
    if (rc < 0) {
        if (rc == -2) send_system_message(client->socket, TYPE_ERROR, "Invalid packet size.");
        disconnect_client(idx);
        return;
    }

    snprintf(h.sender_name, sizeof(h.sender_name), "%s", client->username);

    switch (h.type) {
        case TYPE_CHAT: {
            save_message(db, "general", client->username, payload, 0, NULL);
            char log_buf[MAX_BUF + 80];
            snprintf(log_buf, sizeof(log_buf), "%s: %s", client->username, payload);
            log_message(log_buf);
            broadcast_packet(idx, &h, payload);
            break;
        }
        case TYPE_PRIVATE:
            handle_private_message(idx, &h, payload);
            break;
        case TYPE_FILE_INFO:
            send_file_list(client->socket);
            break;
        case TYPE_FILE_REQ:
            handle_file_request(client->socket, payload);
            break;
        case TYPE_FILE_START:
            handle_file_start(idx, &h);
            break;
        case TYPE_FILE_CHUNK:
            handle_file_chunk(idx, &h, payload);
            break;
        case TYPE_FILE_END:
            handle_file_end(idx);
            break;
        default:
            send_system_message(client->socket, TYPE_ERROR, "Unknown packet type.");
            break;
    }
}

static void on_admin_send(GtkWidget *btn, gpointer entry) {
    (void)btn;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!text || strlen(text) == 0) return;

    PacketHeader h = {0};
    h.type = TYPE_ADMIN;
    h.payload_size = (uint32_t)strlen(text);
    snprintf(h.sender_name, sizeof(h.sender_name), "%s", "ADMIN");
    log_message(text);
    broadcast_packet(-1, &h, text);
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

static void on_kick_user(GtkWidget *btn, gpointer list_view) {
    (void)btn;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(list_view));
    GtkTreeIter iter;
    gchar *username = NULL;

    if (gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(user_list_store), &iter, 0, &username, -1);
        int idx = find_client_by_name(username);
        if (idx >= 0) {
            send_system_message(clients[idx]->socket, TYPE_SYSTEM, "⚠️ You were kicked from the server.");
            disconnect_client(idx);
        }
        g_free(username);
    }
}

static void on_close_server(GtkWidget *btn, gpointer data) {
    (void)btn;
    (void)data;
    server_running = 0;
}

static int add_client(int c_sock, const char *username) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i]) {
            clients[i] = calloc(1, sizeof(ChatClient));
            if (!clients[i]) return -1;
            clients[i]->socket = c_sock;
            snprintf(clients[i]->username, sizeof(clients[i]->username), "%s", username);
            return i;
        }
    }
    return -1;
}

static void accept_new_client(int s_sock) {
    int c_sock = accept(s_sock, NULL, NULL);
    if (c_sock < 0) return;
    set_socket_timeout(c_sock);

    PacketHeader h;
    char username[MAX_BUF + 1] = {0};
    if (recv_packet(c_sock, &h, username, MAX_BUF) < 0 || h.type != TYPE_LOGIN) {
        send_system_message(c_sock, TYPE_ERROR, "Invalid login packet.");
        close(c_sock);
        return;
    }

    if (!valid_username(username)) {
        send_system_message(c_sock, TYPE_ERROR, "Invalid username.");
        close(c_sock);
        return;
    }

    if (username_exists(username)) {
        send_system_message(c_sock, TYPE_ERROR, "Username already exists.");
        close(c_sock);
        return;
    }

    int idx = add_client(c_sock, username);
    if (idx < 0) {
        send_system_message(c_sock, TYPE_ERROR, "Server is full.");
        close(c_sock);
        return;
    }

    char join_msg[160];
    snprintf(join_msg, sizeof(join_msg), "🚀 %s joined", username);
    log_message(join_msg);

    send_chat_history(c_sock);
    send_file_list(c_sock);
    broadcast_user_list();
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);
    mkdir(FILES_DIR, 0777);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Assiut Chat Server Pro");
    gtk_window_set_default_size(GTK_WINDOW(win), 650, 450);
    g_signal_connect(win, "destroy", G_CALLBACK(on_close_server), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *user_list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(user_list_view),
        gtk_tree_view_column_new_with_attributes("Users", gtk_cell_renderer_text_new(), "text", 0, NULL));
    GtkWidget *user_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(user_scroll), user_list_view);
    gtk_widget_set_size_request(user_scroll, 150, -1);

    log_buffer = gtk_text_buffer_new(NULL);
    GtkWidget *text_view = gtk_text_view_new_with_buffer(log_buffer);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    GtkWidget *log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(log_scroll), text_view);

    GtkWidget *admin_entry = gtk_entry_new();
    GtkWidget *admin_btn = gtk_button_new_with_label("📢 Broadcast Message");
    GtkWidget *kick_btn = gtk_button_new_with_label("❌ Kick User");
    GtkWidget *close_btn = gtk_button_new_with_label("⛔ Shutdown Server");
    g_signal_connect(admin_btn, "clicked", G_CALLBACK(on_admin_send), admin_entry);
    g_signal_connect(kick_btn, "clicked", G_CALLBACK(on_kick_user), user_list_view);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_server), NULL);

    gtk_box_pack_start(GTK_BOX(hbox), user_scroll, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(vbox), log_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), admin_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), admin_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), kick_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 5);
    gtk_container_add(GTK_CONTAINER(win), hbox);
    gtk_widget_show_all(win);

    if (!init_database(&db)) {
        log_message("❌ Failed to initialize database.");
        return 1;
    }
    log_message("✅ Database ready");

    int s_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_sock < 0) {
        log_message("❌ socket() failed");
        return 1;
    }

    int opt = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        log_message("❌ Bind failed");
        close(s_sock);
        return 1;
    }

    if (listen(s_sock, 10) < 0) {
        perror("listen");
        log_message("❌ Listen failed");
        close(s_sock);
        return 1;
    }

    log_message("✅ Server started on port 8080...");

    while (server_running) {
        while (gtk_events_pending()) gtk_main_iteration();

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(s_sock, &readfds);
        int maxfd = s_sock;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i]) {
                FD_SET(clients[i]->socket, &readfds);
                if (clients[i]->socket > maxfd) maxfd = clients[i]->socket;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (activity == 0) continue;

        if (FD_ISSET(s_sock, &readfds)) accept_new_client(s_sock);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] && FD_ISSET(clients[i]->socket, &readfds)) {
                handle_client_message(i);
            }
        }
    }

    PacketHeader shut = {0};
    const char *shutdown_msg = "Server is shutting down.";
    shut.type = TYPE_SHUTDOWN;
    shut.payload_size = (uint32_t)strlen(shutdown_msg);
    snprintf(shut.sender_name, sizeof(shut.sender_name), "%s", "SYSTEM");
    broadcast_packet(-1, &shut, shutdown_msg);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) disconnect_client(i);
    }
    close(s_sock);
    close_database(db);
    return 0;
}
