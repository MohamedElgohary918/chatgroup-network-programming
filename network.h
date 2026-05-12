#ifndef NETWORK_H
#define NETWORK_H

// ─────────────────────────────────────────────────────────────────────────────
// network.h
// All socket logic: connect, recv_all, and handling incoming packets.
// This file knows about the protocol and calls ui_chat / ui_sidebar
// to display received data, but knows nothing about GTK layout.
// ─────────────────────────────────────────────────────────────────────────────

// Connects to the server at 127.0.0.1:PORT.
// Sends `username` immediately after connecting (server expects this).
// Returns the connected socket fd on success, -1 on failure.
int network_connect(const char *username);

// Reads exactly `len` bytes from socket `s` into `buf`.
// Loops internally because TCP may deliver data in pieces.
// Returns 0 on success, -1 if the connection was closed or errored.
int recv_all(int s, char *buf, int len);

// Reads one complete packet (header + payload) from `sock`.
// Dispatches to the correct UI function based on packet type:
//   TYPE_CHAT    → append_bubble()
//   TYPE_PRIVATE → append_bubble()
//   TYPE_LIST    → ui_sidebar_update_users()
//   TYPE_FILE    → write to disk
// `sock` is passed by pointer so this function can set it to -1
// on disconnect (main.c checks for -1 in the select loop).
void handle_incoming_data(int *sock);

#endif // NETWORK_H
