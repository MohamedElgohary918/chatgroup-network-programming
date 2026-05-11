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
#include "protocol.h"
#include "chat_database.h"

#define PORT 8080
#define MAX_CLIENTS 100
#define FILES_DIR "files"

typedef struct {
    int socket;
    char username[50];
    FILE *temp_file;
    char temp_filename[300];
    int receiving_file;
} chat_client;

chat_client *clients[MAX_CLIENTS] = {NULL};
sqlite3 *db = NULL;
GtkTextBuffer *log_buffer;
GtkListStore *user_list_store;
int server_running = 1;

// Helper: Ensure we get full data (Critical for files)
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

// Fixed: Now correctly attaches sender_name to the header before sending
void broadcast_packet(int sender_idx, PacketHeader *h, char *payload) {
    // If a regular user is sending, ensure their name is in the header
    if (sender_idx != -1) {
        strncpy(h->sender_name, clients[sender_idx]->username, sizeof(h->sender_name) - 1);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && i != sender_idx) {
            send(clients[i]->socket, h, sizeof(PacketHeader), 0);
            send(clients[i]->socket, payload, h->payload_size, 0);
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

void send_chat_history(int client_socket) {
    char history[16384] = {0};
    if(get_public_messages(db, "general", 50, history, sizeof(history))) {
        PacketHeader h = {TYPE_CHAT, strlen(history)};
        strcpy(h.sender_name, "HISTORY");
        send(client_socket, &h, sizeof(h), 0);
        send(client_socket, history, h.payload_size, 0);
    }
}

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
    strncpy(end.filename, filename, 255);
    send(client_socket, &end, sizeof(end), 0);
}


void disconnect_client(int index) {
    if (clients[index]) {
        if(clients[index]->temp_file) {
            fclose(clients[index]->temp_file);
            remove(clients[index]->temp_filename);
        }
        char exit_msg[120];
        snprintf(exit_msg, sizeof(exit_msg), "🏃 [%s] left", clients[index]->username);
        log_message(exit_msg);
        close(clients[index]->socket);
        free(clients[index]);
        clients[index] = NULL;
        broadcast_user_list();
    }
}

void handle_client_message(int index) {
    PacketHeader h;
    if (recv_all(clients[index]->socket, (char*)&h, sizeof(h)) < 0) {
        disconnect_client(index);
        return;
    }

    char payload[MAX_BUF];
    if (recv_all(clients[index]->socket, payload, h.payload_size) < 0) return;
    payload[h.payload_size] = '\0';

    // --- FEATURE: PRIVATE MESSAGE ---
    if (payload[0] == '@') {
        // Copy payload before strtok shreds it so we can save the FULL message to DB
        char full_msg_copy[MAX_BUF];
        strcpy(full_msg_copy, payload);

        char *target_name = strtok(payload + 1, ":");
        char *msg_content = strtok(NULL, "");

        if (target_name && msg_content) {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] && strcmp(clients[i]->username, target_name) == 0) {
                    h.type = TYPE_CHAT;
                    strncpy(h.sender_name, clients[index]->username, sizeof(h.sender_name)-1);

                    char private_buf[MAX_BUF];
                    snprintf(private_buf, sizeof(private_buf), "[Private]: %s", msg_content);
                    h.payload_size = strlen(private_buf);

                    send(clients[i]->socket, &h, sizeof(h), 0);
                    send(clients[i]->socket, private_buf, h.payload_size, 0);

                    // FIXED: Use target_name instead of "target"
                    save_message(db, "general", clients[index]->username, full_msg_copy, 1, target_name);
                    return;
                }
            }
        }
        return; // Exit if user not found
    }

    if (h.type == TYPE_FILE_INFO) {
    // 1. Log it on the server console so you know it's working
    log_message("🔍 Client requested file list");

    // 2. Call the function that actually queries the database
    // Note: We send it ONLY to the person who asked (index)
    send_file_list(clients[index]->socket);

    // 3. CRITICAL: Return here so the request isn't broadcasted to others
    return;
}
    if (h.type == TYPE_FILE_REQ) {
        handle_file_request(clients[index]->socket, payload);
        return;
    }

    if (h.type == TYPE_FILE) {
        if (!clients[index]->receiving_file) {
            snprintf(clients[index]->temp_filename, sizeof(clients[index]->temp_filename), "files/%s", h.filename);
            clients[index]->temp_file = fopen(clients[index]->temp_filename, "wb");
            if (clients[index]->temp_file == NULL) {
                log_message("❌ Error: Could not open file for writing.");
                return;
            }
            clients[index]->receiving_file = 1;
        }
        if (clients[index]->temp_file != NULL) {
            fwrite(payload, 1, h.payload_size, clients[index]->temp_file);
        }
        // Fall through to broadcast so others get chunks
    }
    else if(h.type == TYPE_FILE_END) {
        if (clients[index]->receiving_file) {
            fclose(clients[index]->temp_file);
            clients[index]->receiving_file = 0;
            clients[index]->temp_file = NULL;
            struct stat st;
            stat(clients[index]->temp_filename, &st);
            save_file_metadata(db, h.filename, clients[index]->username, st.st_size, "general");
        }
        broadcast_packet(index, &h, payload);
        return;
    }
    else if (h.type == TYPE_CHAT) {
        save_message(db, "general", clients[index]->username, payload, 0, NULL);
        char log_buf[MAX_BUF + 60];
        snprintf(log_buf, sizeof(log_buf), "%s: %s", clients[index]->username, payload);
        log_message(log_buf);
        // Fall through to broadcast
    }

    broadcast_packet(index, &h, payload);
}

// --- GTK Callbacks ---
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

void on_close_server(GtkWidget *btn, gpointer data) { server_running = 0; }

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    mkdir("files", 0777);
    signal(SIGPIPE, SIG_IGN);

    // [GUI Setup remains the same as your Code 2...]
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Assiut Chat Server Pro");
    gtk_window_set_default_size(GTK_WINDOW(win), 650, 450);
    g_signal_connect(win, "destroy", G_CALLBACK(on_close_server), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *user_list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(user_list_view), gtk_tree_view_column_new_with_attributes("Users", gtk_cell_renderer_text_new(), "text", 0, NULL));

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

    if(!init_database(&db)) { return 1;}

    // [Socket Setup remains the same...]
    int s_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("Bind failed"); exit(1); }
    listen(s_sock, 10);
    log_message("✅ Server started ...");

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
        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (activity > 0) {
            if (FD_ISSET(s_sock, &readfds)) {
                int c_sock = accept(s_sock, NULL, NULL);
                if (c_sock >= 0) {
                    char temp_name[50];
                    int n = recv(c_sock, temp_name, sizeof(temp_name) - 1, 0);
                    if (n > 0) {
                        char enter_msg[100];
                        temp_name[n] = '\0';
                        temp_name[strcspn(temp_name, "\r\n")] = '\0';
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (!clients[i]) {
                                clients[i] = malloc(sizeof(chat_client));
                                memset(clients[i], 0, sizeof(chat_client)); // CRITICAL: Sets all pointers (like temp_file) to NULL
                                clients[i]->socket = c_sock;
                                strncpy(clients[i]->username, temp_name, 49);
                                snprintf(enter_msg, sizeof(enter_msg), "🚀 [%s] joined", temp_name);
                                log_message(enter_msg);
                                send_chat_history(c_sock);
                                broadcast_user_list();
                                break;
                            }
                        }
                    }
                }
            }
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] && FD_ISSET(clients[i]->socket, &readfds)) {
                    handle_client_message(i);
                }
            }
        }
    }
    for(int i=0; i<MAX_CLIENTS; i++) if(clients[i]) close(clients[i]->socket);
    close(s_sock);
    close_database(db);
    return 0;
}
