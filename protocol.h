#ifndef PROTOCOL_H
#define PROTOCOL_H

#define MAX_BUF 8192
#define MAX_NAME 50

#define TYPE_CHAT 1
#define TYPE_FILE 2
#define TYPE_LIST 3 
#define TYPE_PRIVATE 4
#define TYPE_FILE_REQ 5
#define TYPE_FILE_INFO 6
#define TYPE_FILE_END 7

typedef struct {
    int type;
    int payload_size;
    char filename[256];
    char sender_name[MAX_NAME];
} PacketHeader;

#endif
