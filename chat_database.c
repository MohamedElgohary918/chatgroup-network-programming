#include "chat_database.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int execute_sql(sqlite3 *db, const char *sql) {
    char *err = 0;
    int rc = sqlite3_exec(db, sql, 0, 0, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err);
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

int init_database(sqlite3 **db) {
    if (sqlite3_open("chat.db", db) != SQLITE_OK) {
        fprintf(stderr, "Can't open database\n");
        return 0;
    }

    const char *sql_messages =
        "CREATE TABLE IF NOT EXISTS messages ("
        "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "   room TEXT NOT NULL,"
        "   sender TEXT NOT NULL,"
        "   message TEXT,"
        "   is_private INTEGER DEFAULT 0,"
        "   target TEXT,"
        "   timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";

    const char *sql_files =
        "CREATE TABLE IF NOT EXISTS files ("
        "   id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "   filename TEXT UNIQUE NOT NULL,"
        "   filepath TEXT NOT NULL,"
        "   filesize INTEGER NOT NULL,"
        "   sender TEXT NOT NULL,"
        "   room TEXT DEFAULT 'general',"
        "   timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";

    if (!execute_sql(*db, sql_messages)) return 0;
    if (!execute_sql(*db, sql_files)) return 0;

    // Create indexes for speed
    execute_sql(*db, "CREATE INDEX IF NOT EXISTS idx_messages_time ON messages(timestamp DESC);");
    execute_sql(*db, "CREATE INDEX IF NOT EXISTS idx_files_name ON files(filename);");

    return 1;
}

int save_message(sqlite3 *db, const char *room, const char *sender,
                 const char *message, int is_private, const char *target) {
    char sql[1024];
    if (is_private && target) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO messages (room, sender, message, is_private, target) "
                 "VALUES ('%s', '%s', '%s', 1, '%s');",
                 room, sender, message, target);
    } else {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO messages (room, sender, message) "
                 "VALUES ('%s', '%s', '%s');",
                 room, sender, message);
    }
    return execute_sql(db, sql);
}

int get_public_messages(sqlite3 *db, const char *room, int limit,
                        char *out, size_t out_size) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT sender, message, timestamp FROM messages "
             "WHERE room = '%s' AND is_private = 0 "
             "ORDER BY timestamp DESC LIMIT %d;",
             room, limit);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) return 0;

    out[0] = '\0';
    size_t used = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && used < out_size - 200) {
        const char *sender = (const char*)sqlite3_column_text(stmt, 0);
        const char *msg    = (const char*)sqlite3_column_text(stmt, 1);
        const char *ts     = (const char*)sqlite3_column_text(stmt, 2);
        used += snprintf(out + used, out_size - used,
                         "[%s] %s: %s\n", ts, sender, msg);
    }
    sqlite3_finalize(stmt);
    return 1;
}

int get_private_messages(sqlite3 *db, const char *username, int limit,
                         char *out, size_t out_size) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT sender, message, timestamp FROM messages "
             "WHERE is_private = 1 AND target = '%s' "
             "ORDER BY timestamp DESC LIMIT %d;",
             username, limit);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) return 0;

    out[0] = '\0';
    size_t used = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && used < out_size - 200) {
        const char *sender = (const char*)sqlite3_column_text(stmt, 0);
        const char *msg    = (const char*)sqlite3_column_text(stmt, 1);
        const char *ts     = (const char*)sqlite3_column_text(stmt, 2);
        used += snprintf(out + used, out_size - used,
                         "[Private from %s at %s] %s\n", sender, ts, msg);
    }
    sqlite3_finalize(stmt);
    return 1;
}

// ----- File metadata functions -----

int save_file_metadata(sqlite3 *db, const char *filename, const char *sender,
                       long file_size, const char *room) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO files (filename, filepath, filesize, sender, room) "
             "VALUES ('%s', 'files/%s', %ld, '%s', '%s');",
             filename, filename, file_size, sender, room);
    return execute_sql(db, sql);
}

int get_all_files(sqlite3 *db, FileInfo **list, int *count) {
    const char *sql = "SELECT filename, sender, filesize, timestamp FROM files ORDER BY timestamp DESC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) return 0;

    *list = NULL;
    *count = 0;
    int capacity = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= capacity) {
            capacity = capacity ? capacity * 2 : 10;
            FileInfo *new_list = realloc(*list, capacity * sizeof(FileInfo));
            if (!new_list) {
                sqlite3_finalize(stmt);
                free(*list);
                *list = NULL;
                return 0;
            }
            *list = new_list;
        }
        FileInfo *fi = &(*list)[*count];
        strncpy(fi->filename, (const char*)sqlite3_column_text(stmt, 0), 255);
        strncpy(fi->sender,   (const char*)sqlite3_column_text(stmt, 1), 49);
        fi->size = sqlite3_column_int64(stmt, 2);
        strncpy(fi->timestamp, (const char*)sqlite3_column_text(stmt, 3), 63);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    return 1;
}

char *get_file_path(sqlite3 *db, const char *filename) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT filepath FROM files WHERE filename = '%s';", filename);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) != SQLITE_OK) return NULL;
    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        path = strdup((const char*)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return path;
}

void free_file_list(FileInfo *list, int count) {
    (void)count;
    if (list) free(list);
}

void close_database(sqlite3 *db) {
    if (db) sqlite3_close(db);
}