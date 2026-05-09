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
#include <sys/stat.h>
#include <fcntl.h>
#include "protocol.h"
#include "chat_db.h"
#include <stddef.h>


#define PORT 8080
#define MAX_CLIENTS 100
#define FILES_DIR "files"

typedef struct {
    int socket;
    char username[50];
    // For partial file reception
    int receiving_file;
    char temp_filename[256];
    FILE *temp_file;
    long expected_size;   // not used, but can be extended
} chat_client;

chat_client *clients[MAX_CLIENTS] = {NULL};
GtkTextBuffer *log_buffer;
GtkListStore *user_list_store;
int server_running = 1;
sqlite3 *db = NULL;

int recv_all(int s, char *buf, int len) {
    int total = 0;
    while (total < len) {
        int n = recv(s, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

void log_message(const char *msg) {
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(log_buffer, &iter);
    gtk_text_buffer_insert(log_buffer, &iter, msg, -1);
    gtk_text_buffer_insert(log_buffer, &iter, "\n", -1);
}

void update_user_list_ui() {
    gtk_list_store_clear(user_list_store);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            GtkTreeIter iter;
            gtk_list_store_append(user_list_store, &iter);
            gtk_list_store_set(user_list_store, &iter, 0, clients[i]->username, -1);
        }
    }
}

void broadcast_packet(int sender_idx, PacketHeader *h, char *payload) {
    if (sender_idx != -1 && clients[sender_idx]) {
        strncpy(h->sender_name, clients[sender_idx]->username, sizeof(h->sender_name)-1);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && i != sender_idx) {
            send(clients[i]->socket, h, sizeof(PacketHeader), 0);
            if (h->payload_size > 0) {
                send(clients[i]->socket, payload, h->payload_size, 0);
            }
        }
    }
}

void broadcast_user_list() {
    char list_str[2048] = "";
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            strcat(list_str, clients[i]->username);
            strcat(list_str, ",");
        }
    }
    PacketHeader h = {TYPE_LIST, strlen(list_str)};
    broadcast_packet(-1, &h, list_str);
    update_user_list_ui();
}

void disconnect_client(int idx) {
    if (clients[idx]) {
        char exit_msg[120];
        snprintf(exit_msg, sizeof(exit_msg), "🏃 [%s] left", clients[idx]->username);
        log_message(exit_msg);
        // Clean up partial file if any
        if (clients[idx]->temp_file) {
            fclose(clients[idx]->temp_file);
            remove(clients[idx]->temp_filename);
        }
        close(clients[idx]->socket);
        free(clients[idx]);
        clients[idx] = NULL;
        broadcast_user_list();
    }
}

void send_chat_history(int client_socket) {
    char history[16384] = {0};
    if (get_public_messages(db, "general", 50, history, sizeof(history))) {
        send(client_socket, history, strlen(history), 0);
    }
    send(client_socket, "\n--- Chat history loaded ---\n", 28, 0);
}

// Send list of available files to a client
void send_file_list(int client_socket) {
    FileInfo *files = NULL;
    int count = 0;
    if (!get_all_files(db, &files, &count)) return;

    char list_str[4096] = "Available files:\n";
    for (int i = 0; i < count; i++) {
        char line[512];
        snprintf(line, sizeof(line), "  %s  (%ld bytes) - uploaded by %s\n",
                 files[i].filename, files[i].size, files[i].sender);
        strncat(list_str, line, sizeof(list_str) - strlen(list_str) - 1);
    }
    free_file_list(files, count);

    PacketHeader h = {TYPE_FILE_INFO, strlen(list_str)};
    send(client_socket, &h, sizeof(h), 0);
    send(client_socket, list_str, h.payload_size, 0);
}

// Handle file request from client
void handle_file_request(int client_socket, const char *filename) {
    char *filepath = get_file_path(db, filename);
    if (!filepath) {
        char *err = "File not found on server.\n";
        PacketHeader h = {TYPE_CHAT, strlen(err)};
        strncpy(h.sender_name, "SYSTEM", sizeof(h.sender_name)-1);
        send(client_socket, &h, sizeof(h), 0);
        send(client_socket, err, strlen(err), 0);
        return;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        free(filepath);
        return;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Send file in chunks
    char buffer[MAX_BUF];
    size_t bytes;
    while ((bytes = fread(buffer, 1, MAX_BUF, f)) > 0) {
        PacketHeader h = {TYPE_FILE, bytes};
        strncpy(h.filename, filename, 127);
        send(client_socket, &h, sizeof(h), 0);
        send(client_socket, buffer, bytes, 0);
        usleep(1000);  // small delay to avoid flooding
    }
    fclose(f);
    free(filepath);

    // Send end marker
    PacketHeader end = {TYPE_FILE_END, 0};
    strncpy(end.filename, filename, 127);
    send(client_socket, &end, sizeof(end), 0);
}

void handle_client_message(int idx) {
    chat_client *client = clients[idx];
    if (!client) return;

    PacketHeader h;
    if (recv_all(client->socket, (char*)&h, sizeof(h)) < 0) {
        disconnect_client(idx);
        return;
    }

    char payload[MAX_BUF] = {0};
    if (h.payload_size > 0) {
        if (recv_all(client->socket, payload, h.payload_size) < 0) return;
        payload[h.payload_size] = '\0';
    }

    // --- FILE REQUEST (download) ---
    if (h.type == TYPE_FILE_REQ) {
        handle_file_request(client->socket, payload);
        return;
    }

    // --- FILE UPLOAD (chunk) ---
    if (h.type == TYPE_FILE) {
        // Ensure files directory exists
        mkdir(FILES_DIR, 0777);

        if (!client->receiving_file) {
            // First chunk: open a temp file
            snprintf(client->temp_filename, sizeof(client->temp_filename),
                     "%s/temp_%s_%d", FILES_DIR, client->username, rand());
            client->temp_file = fopen(client->temp_filename, "wb");
            if (!client->temp_file) {
                log_message("Failed to create temp file");
                return;
            }
            client->receiving_file = 1;
        }
        // Write chunk
        fwrite(payload, 1, h.payload_size, client->temp_file);
        // Broadcast to others (original behavior)
        broadcast_packet(idx, &h, payload);
        return;
    }

    // --- FILE END (transfer complete) ---
    if (h.type == TYPE_FILE_END) {
        if (client->receiving_file && client->temp_file) {
            fclose(client->temp_file);
            // Move to final name
            char final_path[512];
            snprintf(final_path, sizeof(final_path), "%s/%s", FILES_DIR, h.filename);
            rename(client->temp_filename, final_path);
            // Get file size
            struct stat st;
            stat(final_path, &st);
            // Save metadata
            save_file_metadata(db, h.filename, client->username, st.st_size, "general");
            char saved_msg[256];
            snprintf(saved_msg, sizeof(saved_msg), "File saved: %s", h.filename);
            log_message(saved_msg);
            // Notify everyone that a new file is available (optional)
            char notice[MAX_BUF];
            snprintf(notice, sizeof(notice), "📁 New file uploaded: %s", h.filename);
            PacketHeader notice_h = {TYPE_CHAT, strlen(notice)};
            strncpy(notice_h.sender_name, "SYSTEM", sizeof(notice_h.sender_name)-1);
            broadcast_packet(-1, &notice_h, notice);
        }
        client->receiving_file = 0;
        client->temp_file = NULL;
        return;
    }

    // --- PRIVATE MESSAGE ---
    if (h.type == TYPE_PRIVATE || payload[0] == '@') {
        char *target_name = strtok(payload + (payload[0]=='@'?1:0), ":");
        char *msg_content = strtok(NULL, "");
        if (target_name && msg_content) {
            save_message(db, "general", client->username, msg_content, 1, target_name);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] && strcmp(clients[i]->username, target_name) == 0) {
                    char private_buf[MAX_BUF];
                    snprintf(private_buf, sizeof(private_buf), "[Private] %s: %s", client->username, msg_content);
                    h.type = TYPE_CHAT;
                    h.payload_size = strlen(private_buf);
                    strncpy(h.sender_name, client->username, sizeof(h.sender_name)-1);
                    send(clients[i]->socket, &h, sizeof(h), 0);
                    send(clients[i]->socket, private_buf, h.payload_size, 0);
                    break;
                }
            }
        }
        return;
    }

    // --- REGULAR PUBLIC CHAT ---
    if (h.type == TYPE_CHAT) {
        save_message(db, "general", client->username, payload, 0, NULL);
        char log_buf[MAX_BUF + 60];
        snprintf(log_buf, sizeof(log_buf), "%s: %s", client->username, payload);
        log_message(log_buf);
        broadcast_packet(idx, &h, payload);
    }
    else if (h.type == TYPE_LIST) {
        // ignore, handled elsewhere
    }
}

// GTK callbacks (same as before)
void on_admin_send(GtkWidget *btn, gpointer entry) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (strlen(text) == 0) return;
    PacketHeader h = {TYPE_CHAT, strlen(text)};
    strncpy(h.sender_name, "📢 [ADMIN]:", sizeof(h.sender_name)-1);
    log_message(text);
    broadcast_packet(-1, &h, (char*)text);
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

void on_kick_user(GtkWidget *btn, gpointer list_view) {
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(list_view));
    GtkTreeIter iter;
    gchar *username;
    if (gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(user_list_store), &iter, 0, &username, -1);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] && strcmp(clients[i]->username, username) == 0) {
                PacketHeader h = {TYPE_CHAT, 35};
                strncpy(h.sender_name, "SYSTEM", sizeof(h.sender_name)-1);
                char *msg = "⚠️ You were kicked";
                send(clients[i]->socket, &h, sizeof(h), 0);
                send(clients[i]->socket, msg, strlen(msg), 0);
                disconnect_client(i);
                break;
            }
        }
        g_free(username);
    }
}

void on_close_server(GtkWidget *btn, gpointer data) {
    server_running = 0;
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);
    srand(time(NULL));

    // Ensure files directory exists
    mkdir(FILES_DIR, 0777);

    if (!init_database(&db)) {
        printf("Failed to init database\n");
        return 1;
    }
    log_message("✅ Database ready");

    // GTK UI (same as original)
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Assiut Chat Server Pro");
    gtk_window_set_default_size(GTK_WINDOW(win), 650, 450);
    g_signal_connect(win, "destroy", G_CALLBACK(on_close_server), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *user_list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(user_list_view),
                                gtk_tree_view_column_new_with_attributes("Users",
                                gtk_cell_renderer_text_new(), "text", 0, NULL));
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
    g_signal_connect(admin_btn, "clicked", G_CALLBACK(on_admin_send), admin_entry);
    GtkWidget *kick_btn = gtk_button_new_with_label("❌ Kick User");
    g_signal_connect(kick_btn, "clicked", G_CALLBACK(on_kick_user), user_list_view);
    GtkWidget *close_btn = gtk_button_new_with_label("⛔ Shutdown Server");
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

    // Socket setup
    int s_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_message("❌ Bind failed");
        return 1;
    }
    listen(s_sock, 10);
    log_message("✅ Server started on port 8080...");

    fd_set readfds;
    struct timeval tv;

    while (server_running) {
        while (gtk_events_pending()) gtk_main_iteration();

        FD_ZERO(&readfds);
        FD_SET(s_sock, &readfds);
        int maxfd = s_sock;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i]) {
                FD_SET(clients[i]->socket, &readfds);
                if (clients[i]->socket > maxfd) maxfd = clients[i]->socket;
            }
        }

        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        if (select(maxfd + 1, &readfds, NULL, NULL, &tv) > 0) {
            // New connection
            if (FD_ISSET(s_sock, &readfds)) {
                int c_sock = accept(s_sock, NULL, NULL);
                if (c_sock >= 0) {
                    char username[50];
                    int n = recv(c_sock, username, sizeof(username)-1, 0);
                    if (n > 0) {
                        username[n] = '\0';
                        username[strcspn(username, "\r\n")] = '\0';
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (!clients[i]) {
                                clients[i] = malloc(sizeof(chat_client));
                                memset(clients[i], 0, sizeof(chat_client));
                                clients[i]->socket = c_sock;
                                strncpy(clients[i]->username, username, 49);
                                char join_msg[100];
                                snprintf(join_msg, sizeof(join_msg), "🚀 %s joined", username);
                                log_message(join_msg);
                                send_chat_history(c_sock);
                                send_file_list(c_sock);  // send available files list
                                broadcast_user_list();
                                break;
                            }
                        }
                    } else close(c_sock);
                }
            }
            // Existing clients
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] && FD_ISSET(clients[i]->socket, &readfds)) {
                    handle_client_message(i);
                }
            }
        }
    }

    // Cleanup
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) disconnect_client(i);
    }
    close(s_sock);
    close_database(db);
    return 0;
}
