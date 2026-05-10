#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define MAX_BUF 4096
#define MAX_NAME 50
#define MAX_FILENAME 128

#define TYPE_LOGIN       0
#define TYPE_CHAT        1
#define TYPE_PRIVATE     2
#define TYPE_LIST        3
#define TYPE_FILE_START  4
#define TYPE_FILE_CHUNK  5
#define TYPE_FILE_END    6
#define TYPE_FILE_INFO   7
#define TYPE_FILE_REQ    8
#define TYPE_SYSTEM      9
#define TYPE_ADMIN       10
#define TYPE_ERROR       11
#define TYPE_SHUTDOWN    12

typedef struct {
    uint32_t type;
    uint32_t payload_size;
    char filename[MAX_FILENAME];
    char sender_name[MAX_NAME];
    char receiver_name[MAX_NAME];
} PacketHeader;

#endif
