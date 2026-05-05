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

#define PORT 5050
#define MAX_CLIENTS 100

typedef struct
{
    int socket;
    char username[50];
} chat_client;

chat_client *clients[MAX_CLIENTS] = {NULL};
GtkTextBuffer *log_buffer;
GtkListStore *user_list_store;
int server_running = 1;

void update_user_list_ui()
{
    gtk_list_store_clear(user_list_store);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i])
        {
            GtkTreeIter iter;
            gtk_list_store_append(user_list_store, &iter);
            gtk_list_store_set(user_list_store, &iter, 0, clients[i]->username, -1);
        }
    }
}

void log_message(const char *msg)
{
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(log_buffer, &iter);
    gtk_text_buffer_insert(log_buffer, &iter, msg, -1);
    gtk_text_buffer_insert(log_buffer, &iter, "\n", -1);
}

void broadcast_user_list()
{
    char list_msg[2048] = "LIST:";
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i])
        {
            strncat(list_msg, clients[i]->username, sizeof(list_msg) - strlen(list_msg) - 2);
            strcat(list_msg, ",");
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i])
        {
            send(clients[i]->socket, list_msg, strlen(list_msg), 0);
        }
    }
    update_user_list_ui();
}

void disconnect_client(int index)
{
    if (clients[index])
    {
        char exit_msg[120];
        snprintf(exit_msg, sizeof(exit_msg), "🏃 [%s] left", clients[index]->username);
        log_message(exit_msg);

        close(clients[index]->socket);
        free(clients[index]);
        clients[index] = NULL;
        broadcast_user_list();
    }
}

void handle_client_message(int index)
{
    char buffer[1024];
    int bytes = recv(clients[index]->socket, buffer, sizeof(buffer) - 1, 0);

    if (bytes <= 0)
    {
        disconnect_client(index);
        return;
    }

    buffer[bytes] = '\0';
    buffer[strcspn(buffer, "\r\n")] = '\0';

    if (buffer[0] == '@')
    {
        char *target_name = strtok(buffer + 1, ":");
        char *msg_content = strtok(NULL, "");
        if (target_name && msg_content)
        {
            for (int i = 0; i < MAX_CLIENTS; i++)
            {
                if (clients[i] && strcmp(clients[i]->username, target_name) == 0)
                {
                    char private_buf[1200];
                    snprintf(private_buf, sizeof(private_buf), "[Private from %s]: %s", clients[index]->username, msg_content);
                    send(clients[i]->socket, private_buf, strlen(private_buf), 0);
                    break;
                }
            }
        }
    }
    else
    {
        char final_msg[1200];
        snprintf(final_msg, sizeof(final_msg), "%s: %s", clients[index]->username, buffer);
        log_message(final_msg);
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i] && i != index)
            {
                send(clients[i]->socket, final_msg, strlen(final_msg), 0);
            }
        }
    }
}

// GTK Callbacks
void on_admin_send(GtkWidget *btn, gpointer entry)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (strlen(text) == 0) return;
    char admin_msg[1300];
    snprintf(admin_msg, sizeof(admin_msg), "📢 [ADMIN]: %s", text);
    log_message(admin_msg);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i]) send(clients[i]->socket, admin_msg, strlen(admin_msg), 0);
    }
    gtk_entry_set_text(GTK_ENTRY(entry), "");
}

void on_kick_user(GtkWidget *btn, gpointer list_view)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(list_view));
    GtkTreeIter iter;
    gchar *username;
    if (gtk_tree_selection_get_selected(sel, NULL, &iter))
    {
        gtk_tree_model_get(GTK_TREE_MODEL(user_list_store), &iter, 0, &username, -1);
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i] && strcmp(clients[i]->username, username) == 0)
            {
                const char *kick_msg = "⚠️ You have been kicked from the server";
                send(clients[i]->socket, kick_msg, strlen(kick_msg), 0);
                disconnect_client(i);
                break;
            }
        }
        g_free(username);
    }
}

void on_close_server(GtkWidget *btn, gpointer data)
{
    server_running = 0;
}

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    // --- GUI SETUP ---
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Assiut Chat Server");
    gtk_window_set_default_size(GTK_WINDOW(win), 650, 450);
    g_signal_connect(win, "destroy", G_CALLBACK(on_close_server), NULL);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);

    user_list_store = gtk_list_store_new(1, G_TYPE_STRING);
    GtkWidget *user_list_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_list_store));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Users", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(user_list_view), col);

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

    // --- SOCKET SETUP ---
    int s_sock = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("Bind failed");
        exit(1);
    }
    listen(s_sock, 10);
    log_message("✅ Server started ...");

    // --- MAIN HYBRID LOOP ---
    fd_set readfds;
    struct timeval tv;

    while (server_running)
    {
        // 1. Process GTK Events (Keep UI alive)
        while (gtk_events_pending()) gtk_main_iteration();

        // 2. Setup Select
        FD_ZERO(&readfds);
        FD_SET(s_sock, &readfds);
        int maxfd = s_sock;

        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i])
            {
                FD_SET(clients[i]->socket, &readfds);
                if (clients[i]->socket > maxfd) maxfd = clients[i]->socket;
            }
        }

        tv.tv_sec = 0;
        tv.tv_usec = 10000; // 10ms wait

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (activity > 0)
        {
            // New connection
            if (FD_ISSET(s_sock, &readfds))
            {
                int c_sock = accept(s_sock, NULL, NULL);
                if (c_sock >= 0)
                {
                    char temp_name[50];
                    int n = recv(c_sock, temp_name, sizeof(temp_name) - 1, 0);
                    if (n > 0)
                    {
                        temp_name[n] = '\0';
                        temp_name[strcspn(temp_name, "\r\n")] = '\0';

                        int added = 0;
                        for (int i = 0; i < MAX_CLIENTS; i++)
                        {
                            if (!clients[i])
                            {
                                clients[i] = malloc(sizeof(chat_client));
                                clients[i]->socket = c_sock;
                                strncpy(clients[i]->username, temp_name, 49);
                                added = 1;
                                break;
                            }
                        }
                        if (added)
                        {
                            char enter_msg[100];
                            snprintf(enter_msg, sizeof(enter_msg), "🚀 [%s] joined", temp_name);
                            log_message(enter_msg);
                            broadcast_user_list();
                        }
                        else
                        {
                            send(c_sock, "Server Full", 11, 0);
                            close(c_sock);
                        }
                    }
                }
            }

            // Existing clients
            for (int i = 0; i < MAX_CLIENTS; i++)
            {
                if (clients[i] && FD_ISSET(clients[i]->socket, &readfds))
                {
                    handle_client_message(i);
                }
            }
        }
    }

    // Cleanup
    for(int i=0; i<MAX_CLIENTS; i++) if(clients[i]) close(clients[i]->socket);
    close(s_sock);
    return 0;
}
