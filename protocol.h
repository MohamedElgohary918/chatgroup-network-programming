#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_BUF 4096
#define TYPE_CHAT 1
#define TYPE_FILE 2
#define TYPE_LIST 3 

typedef struct {
    int type;
    int payload_size;
    char filename[64];
    char sender_name[50];
} PacketHeader;

#endif
