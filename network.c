#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "network.h"
#include "protocol.h"
#include "ui_chat.h"
#include "ui_sidebar.h"

// ─────────────────────────────────────────────────────────────────────────────
// network.c
// Handles everything related to the TCP socket:
//   - connecting to the server
//   - reliably reading N bytes (recv_all)
//   - reading and dispatching incoming packets (handle_incoming_data)
// No GTK layout code here — it calls ui_chat and ui_sidebar functions
// to display data, but never creates or arranges widgets itself.
// ─────────────────────────────────────────────────────────────────────────────

// ── network_connect ───────────────────────────────────────────────────────────
// Creates a TCP socket, connects to the server, sends the username.
// Returns the fd on success, -1 on any failure.
int network_connect(const char *username)
{
    // Create a TCP IPv4 socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    // Fill in server address: 127.0.0.1 : PORT
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    // inet_pton converts the string "127.0.0.1" to binary form

    // Attempt the TCP 3-way handshake
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }

    // Server expects the username as the very first thing on the socket
    send(sock, username, strlen(username), 0);

    return sock; // caller stores this fd and uses it for all future I/O
}

// ── recv_all ──────────────────────────────────────────────────────────────────
// TCP may split one logical message into multiple recv() calls.
// This function loops until all `len` bytes have arrived.
// Returns 0 on success, -1 if the connection dropped mid-receive.
int recv_all(int s, char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = recv(s, buf + total, len - total, 0);
        // buf + total = pointer to where the next chunk should be stored
        // len - total = how many bytes we still need
        if (n <= 0) return -1; // 0 = graceful close, -1 = error
        total += n;
    }
    return 0;
}

// ── handle_incoming_data ──────────────────────────────────────────────────────
// Called by the select loop in main.c whenever the socket has data.
// Reads one PacketHeader, then reads `payload_size` bytes of payload,
// then dispatches based on packet type.
// sock is a pointer so we can set *sock = -1 on disconnect,
// which signals main.c to stop watching this fd.
void handle_incoming_data(int *sock)
{
    PacketHeader h;

    // Step 1: read the fixed-size header
    if (recv_all(*sock, (char *)&h, sizeof(h)) < 0) {
        append_system_msg("⚠️  Connection to server lost.");
        close(*sock);
        *sock = -1; // tell the select loop we are disconnected
        return;
    }

    // Step 2: read the variable-size payload
    char payload[MAX_BUF + 1];
    if (h.payload_size > 0) {
        if (recv_all(*sock, payload, h.payload_size) < 0) {
            append_system_msg("⚠️  Incomplete packet received.");
            return;
        }
        payload[h.payload_size] = '\0'; // null-terminate for string use
    } else {
        payload[0] = '\0';
    }

    // Step 3: dispatch based on type
    switch (h.type)
    {
        case TYPE_LIST:
            // payload = "mark,mina,fo2sh" — rebuild the sidebar user list
            ui_sidebar_update_users(payload);
            break;

        case TYPE_CHAT:
            // Normal group message — show on LEFT with sender's color
            append_bubble(h.sender_name, payload, 0, 0, 0);
            break;

        case TYPE_PRIVATE:
            // Private message — show with purple bubble and lock icon
            append_bubble(h.sender_name, payload, 0, 1, 0);
            break;

        case TYPE_FILE:
            // File chunk — write to disk, prefix filename with "received_"
            {
                char out_path[256];
                snprintf(out_path, sizeof(out_path), "received_%s", h.filename);
                int f = open(out_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
                // O_CREAT  = create if doesn't exist
                // O_APPEND = each write goes to the end (for chunked files)
                // 0644     = rw-r--r-- permissions
                if (f >= 0) {
                    write(f, payload, h.payload_size);
                    close(f);
                }
            }
            break;

        default:
            break;
    }
}
