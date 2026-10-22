#ifndef MYDB_SQLITE_DRIVER_H
#define MYDB_SQLITE_DRIVER_H

#include <sqlite3.h>
#include "sql_engine.h"

typedef struct {
    sqlite3* db;
    int implicit_open; /* the open transaction was started by the driver */
} SQLiteImpl;

void sqlite_driver_init(DBDriver* driver);

#endif
