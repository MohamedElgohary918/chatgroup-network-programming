#ifndef CHAT_DB_H
#define CHAT_DB_H

#include <sqlite3.h>
#include <stddef.h>

// Initialize database (creates tables)
int init_database(sqlite3 **db);

// Save text message (public or private)
int save_message(sqlite3 *db, const char *room, const char *sender,const char *message, int is_private, const char *target);

// Get last N public messages (for chat history)
int get_public_messages(sqlite3 *db, const char *room, int limit,char *out, size_t out_size);

// Get last N private messages for a user
int get_private_messages(sqlite3 *db, const char *username, int limit,char *out, size_t out_size);

// ----- File metadata functions -----
typedef struct {
    char filename[256];
    char sender[50];
    long size;
    char timestamp[64];
} FileInfo;

// Save file metadata after successful upload
int save_file_metadata(sqlite3 *db, const char *filename, const char *sender,long file_size, const char *room);

// Get list of all available files (for download)
int get_all_files(sqlite3 *db, FileInfo **list, int *count);

// Get file path from filename (returns dynamically allocated string, caller must free)
char *get_file_path(sqlite3 *db, const char *filename);

// Free list of FileInfo
void free_file_list(FileInfo *list, int count);

// Close database
void close_database(sqlite3 *db);

#endif
