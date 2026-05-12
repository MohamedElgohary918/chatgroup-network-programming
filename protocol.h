#ifndef PROTOCOL_H
#define PROTOCOL_H

// ─────────────────────────────────────────────────────────────────────────────
// protocol.h
// Shared between client and server.
// Defines every message type and the packet header struct.
// Every other file includes this — it is the contract between both sides.
// ─────────────────────────────────────────────────────────────────────────────

#define MAX_BUF  8192
#define MAX_NAME 50
#define PORT     8080

// Message types — stored in PacketHeader.type
#define TYPE_CHAT    1   // normal group message
#define TYPE_FILE    2   // file chunk
#define TYPE_LIST    3   // server → client: comma-separated online usernames
#define TYPE_PRIVATE 4   // private message to one user

// Every packet starts with this fixed-size header.
// After the header, `payload_size` bytes of payload follow.
typedef struct
{
    int  type;                // one of TYPE_* above
    int  payload_size;        // how many bytes come after this header
    char filename[128];       // used only for TYPE_FILE
    char sender_name[MAX_NAME]; // who sent it
} PacketHeader;

#endif // PROTOCOL_H
