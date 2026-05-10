#include "chat_database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILES_DIR "files"

static int execute_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err ? err : "unknown");
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

int init_database(sqlite3 **db) {
    if (sqlite3_open("chat.db", db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(*db));
        return 0;
    }

    const char *sql_messages =
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "room TEXT NOT NULL,"
        "sender TEXT NOT NULL,"
        "message TEXT NOT NULL,"
        "is_private INTEGER DEFAULT 0,"
        "target TEXT,"
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";

    const char *sql_files =
        "CREATE TABLE IF NOT EXISTS files ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "filename TEXT UNIQUE NOT NULL,"
        "filepath TEXT NOT NULL,"
        "filesize INTEGER NOT NULL,"
        "sender TEXT NOT NULL,"
        "room TEXT DEFAULT 'general',"
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";

    if (!execute_sql(*db, sql_messages)) return 0;
    if (!execute_sql(*db, sql_files)) return 0;
    execute_sql(*db, "CREATE INDEX IF NOT EXISTS idx_messages_time ON messages(timestamp DESC);");
    execute_sql(*db, "CREATE INDEX IF NOT EXISTS idx_files_name ON files(filename);");
    return 1;
}

int save_message(sqlite3 *db, const char *room, const char *sender,
                 const char *message, int is_private, const char *target) {
    const char *sql =
        "INSERT INTO messages (room, sender, message, is_private, target) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, room ? room : "general", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, sender ? sender : "unknown", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, message ? message : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, is_private ? 1 : 0);
    if (target) sqlite3_bind_text(stmt, 5, target, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 5);

    int ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

int get_public_messages(sqlite3 *db, const char *room, int limit,
                        char *out, size_t out_size) {
    const char *sql =
        "SELECT sender, message, timestamp FROM messages "
        "WHERE room = ? AND is_private = 0 "
        "ORDER BY timestamp DESC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, room ? room : "general", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    out[0] = '\0';
    size_t used = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && used < out_size - 1) {
        const char *sender = (const char *)sqlite3_column_text(stmt, 0);
        const char *msg = (const char *)sqlite3_column_text(stmt, 1);
        const char *ts = (const char *)sqlite3_column_text(stmt, 2);
        int written = snprintf(out + used, out_size - used,
                               "[%s] %s: %s\n",
                               ts ? ts : "", sender ? sender : "", msg ? msg : "");
        if (written < 0 || (size_t)written >= out_size - used) break;
        used += (size_t)written;
    }

    sqlite3_finalize(stmt);
    return 1;
}

int get_private_messages(sqlite3 *db, const char *username, int limit,
                         char *out, size_t out_size) {
    const char *sql =
        "SELECT sender, message, timestamp FROM messages "
        "WHERE is_private = 1 AND target = ? "
        "ORDER BY timestamp DESC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, username ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    out[0] = '\0';
    size_t used = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && used < out_size - 1) {
        const char *sender = (const char *)sqlite3_column_text(stmt, 0);
        const char *msg = (const char *)sqlite3_column_text(stmt, 1);
        const char *ts = (const char *)sqlite3_column_text(stmt, 2);
        int written = snprintf(out + used, out_size - used,
                               "[Private from %s at %s] %s\n",
                               sender ? sender : "", ts ? ts : "", msg ? msg : "");
        if (written < 0 || (size_t)written >= out_size - used) break;
        used += (size_t)written;
    }

    sqlite3_finalize(stmt);
    return 1;
}

int save_file_metadata(sqlite3 *db, const char *filename, const char *sender,
                       long file_size, const char *room) {
    const char *sql =
        "INSERT OR REPLACE INTO files (filename, filepath, filesize, sender, room) "
        "VALUES (?, ?, ?, ?, ?);";

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", FILES_DIR, filename ? filename : "unknown");

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, filename ? filename : "unknown", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, filepath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)file_size);
    sqlite3_bind_text(stmt, 4, sender ? sender : "unknown", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, room ? room : "general", -1, SQLITE_TRANSIENT);

    int ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

int get_all_files(sqlite3 *db, FileInfo **list, int *count) {
    const char *sql = "SELECT filename, sender, filesize, timestamp FROM files ORDER BY timestamp DESC;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;

    *list = NULL;
    *count = 0;
    int capacity = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= capacity) {
            capacity = capacity ? capacity * 2 : 8;
            FileInfo *tmp = realloc(*list, (size_t)capacity * sizeof(FileInfo));
            if (!tmp) {
                free(*list);
                *list = NULL;
                *count = 0;
                sqlite3_finalize(stmt);
                return 0;
            }
            *list = tmp;
        }

        FileInfo *fi = &(*list)[*count];
        memset(fi, 0, sizeof(*fi));
        snprintf(fi->filename, sizeof(fi->filename), "%s",
                 (const char *)sqlite3_column_text(stmt, 0));
        snprintf(fi->sender, sizeof(fi->sender), "%s",
                 (const char *)sqlite3_column_text(stmt, 1));
        fi->size = (long)sqlite3_column_int64(stmt, 2);
        snprintf(fi->timestamp, sizeof(fi->timestamp), "%s",
                 (const char *)sqlite3_column_text(stmt, 3));
        (*count)++;
    }

    sqlite3_finalize(stmt);
    return 1;
}

char *get_file_path(sqlite3 *db, const char *filename) {
    const char *sql = "SELECT filepath FROM files WHERE filename = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, filename ? filename : "", -1, SQLITE_TRANSIENT);

    char *path = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        if (p) path = strdup(p);
    }
    sqlite3_finalize(stmt);
    return path;
}

void free_file_list(FileInfo *list, int count) {
    (void)count;
    free(list);
}

void close_database(sqlite3 *db) {
    if (db) sqlite3_close(db);
}
