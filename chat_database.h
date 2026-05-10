#ifndef CHAT_DATABASE_H
#define CHAT_DATABASE_H

#include <sqlite3.h>
#include <stddef.h>

typedef struct {
    char filename[256];
    char sender[50];
    long size;
    char timestamp[64];
} FileInfo;

int init_database(sqlite3 **db);
int save_message(sqlite3 *db, const char *room, const char *sender,
                 const char *message, int is_private, const char *target);
int get_public_messages(sqlite3 *db, const char *room, int limit,
                        char *out, size_t out_size);
int get_private_messages(sqlite3 *db, const char *username, int limit,
                         char *out, size_t out_size);
int save_file_metadata(sqlite3 *db, const char *filename, const char *sender,
                       long file_size, const char *room);
int get_all_files(sqlite3 *db, FileInfo **list, int *count);
char *get_file_path(sqlite3 *db, const char *filename);
void free_file_list(FileInfo *list, int count);
void close_database(sqlite3 *db);

#endif
