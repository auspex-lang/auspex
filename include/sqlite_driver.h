#ifndef MYDB_SQLITE_DRIVER_H
#define MYDB_SQLITE_DRIVER_H

#include <sqlite3.h>
#include "sql_engine.h"

/* Prepared statements are cached by their SQL text, so a statement run in a
   loop is prepared once (#108). Idle entries are always reset, so they hold
   no locks; an entry whose statement backs an open result is checked out
   and is not handed to anyone else. */
#define SQLITE_STMT_CACHE_SIZE 64

typedef struct {
    char* sql;            /* the SQL text as passed in, before normalising */
    unsigned long hash;
    sqlite3_stmt* stmt;
    unsigned long last_used;
    int checked_out;
} SQLiteCachedStmt;

typedef struct {
    sqlite3* db;
    int implicit_open; /* the open transaction was started by the driver */
    SQLiteCachedStmt cache[SQLITE_STMT_CACHE_SIZE];
    int cache_count;
    unsigned long cache_clock;
} SQLiteImpl;

void sqlite_driver_init(DBDriver* driver);

#endif
