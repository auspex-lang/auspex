#ifndef OS_H
#define OS_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

char* os_read_file(const char* path);
int os_file_exists(const char* path);
int os_write_file(const char* path, const char* contents, size_t len);
int os_is_dir(const char* path);
int os_mkdir(const char* path);
/* Removes a file or an empty directory, as POSIX remove() does; 0 on
   success, -1 on failure. */
int os_remove(const char* path);
int os_list_dir(const char* path, char*** out_names, int* out_count);

int os_open(const char* path);
int os_close(int fd);
int os_read(int fd, void* buf, size_t count, off_t offset);
int os_write(int fd, const void* buf, size_t count, off_t offset);
int os_ftruncate(int fd, off_t length);
/* Flushes a file's data to stable storage (fsync, or _commit on Windows);
   0 on success, -1 on failure. */
int os_fsync(int fd);

/* Reads a line of any length into *line (grown as needed, like POSIX
   getline): returns its length including the newline, or -1 at end of
   file or on error. */
long os_getline(char** line, size_t* capacity, FILE* f);

/* Absolute, canonical form of `path` (malloc'd), or NULL. */
char* os_realpath(const char* path);
int os_path_is_absolute(const char* path);

/* Shared libraries: dlopen/dlsym on POSIX, LoadLibrary/GetProcAddress on
   Windows. os_dl_error describes the last failure. */
void* os_dl_open(const char* path);
void* os_dl_sym(void* handle, const char* name);
void os_dl_close(void* handle);
const char* os_dl_error(void);

#endif
