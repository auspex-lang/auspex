#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "os.h"

/* Files are opened in binary mode: on Windows the default text mode would
   rewrite the newline bytes in database pages. POSIX has no such mode. */
#ifndef O_BINARY
#define O_BINARY 0
#endif

int os_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int os_write_file(const char* path, const char* contents, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) return 0;

    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, contents + total, len - total);
        if (n < 0) {
            close(fd);
            return 0;
        }
        total += (size_t)n;
    }
    close(fd);
    return 1;
}

char* os_read_file(const char* path) {
    int fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return NULL;

    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        close(fd);
        return NULL;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return NULL;
    }

    char* buffer = malloc((size_t)size + 1);
    if (buffer == NULL) {
        close(fd);
        return NULL;
    }

    ssize_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, buffer + total, (size_t)(size - total));
        if (n <= 0) {
            free(buffer);
            close(fd);
            return NULL;
        }
        total += n;
    }
    buffer[size] = '\0';
    close(fd);
    return buffer;
}

int os_is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

int os_mkdir(const char* path) {
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

int os_remove(const char* path) {
#ifdef _WIN32
    /* The Windows C runtime's remove() deletes files only. */
    if (os_is_dir(path)) return _rmdir(path) == 0 ? 0 : -1;
#endif
    return remove(path) == 0 ? 0 : -1;
}

int os_list_dir(const char* path, char*** out_names, int* out_count) {
    if (out_names == NULL || out_count == NULL) return 0;
    *out_names = NULL;
    *out_count = 0;
    DIR* dir = opendir(path);
    if (dir == NULL) return 0;

    int capacity = 8;
    int count = 0;
    char** names = malloc(sizeof(char*) * (size_t)capacity);
    if (names == NULL) {
        closedir(dir);
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (count >= capacity) {
            capacity *= 2;
            char** new_names = realloc(names, sizeof(char*) * (size_t)capacity);
            if (new_names == NULL) {
                for (int i = 0; i < count; i++) free(names[i]);
                free(names);
                closedir(dir);
                return 0;
            }
            names = new_names;
        }
        names[count] = strdup(entry->d_name);
        if (names[count] == NULL) {
            for (int i = 0; i < count; i++) free(names[i]);
            free(names);
            closedir(dir);
            return 0;
        }
        count++;
    }
    closedir(dir);
    *out_names = names;
    *out_count = count;
    return 1;
}

int os_open(const char* path) {
    return open(path, O_RDWR | O_CREAT | O_BINARY, 0644);
}

int os_close(int fd) {
    return close(fd);
}

#ifdef _WIN32
/* No pread/pwrite: seek, then read or write. Each file descriptor is used
   by one pager, so the seek cannot race another positioned access. */
int os_read(int fd, void* buf, size_t count, off_t offset) {
    if (_lseeki64(fd, (long long)offset, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned int)count);
}

int os_write(int fd, const void* buf, size_t count, off_t offset) {
    if (_lseeki64(fd, (long long)offset, SEEK_SET) < 0) return -1;
    return _write(fd, buf, (unsigned int)count);
}
#else
int os_read(int fd, void* buf, size_t count, off_t offset) {
    return (int)pread(fd, buf, count, offset);
}

int os_write(int fd, const void* buf, size_t count, off_t offset) {
    return (int)pwrite(fd, buf, count, offset);
}
#endif

int os_ftruncate(int fd, off_t length) {
    return ftruncate(fd, length);
}

int os_fsync(int fd) {
    if (fd < 0) return -1;
#ifdef _WIN32
    return _commit(fd) == 0 ? 0 : -1;
#else
    return fsync(fd) == 0 ? 0 : -1;
#endif
}

int os_fsync_dir(const char* path) {
    if (path == NULL) return -1;
#ifdef _WIN32
    /* NTFS makes directory entries durable on its own; there is no handle
       to flush through the CRT. */
    return 0;
#else
    const char* slash = strrchr(path, '/');
    char dir[4096];
    if (slash == NULL) {
        snprintf(dir, sizeof(dir), ".");
    } else if (slash == path) {
        snprintf(dir, sizeof(dir), "/");
    } else {
        size_t len = (size_t)(slash - path);
        if (len >= sizeof(dir)) return -1;
        memcpy(dir, path, len);
        dir[len] = '\0';
    }
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd) == 0 ? 0 : -1;
    close(fd);
    return rc;
#endif
}

long os_getline(char** line, size_t* capacity, FILE* f) {
#ifdef _WIN32
    if (*line == NULL || *capacity == 0) {
        *capacity = 128;
        *line = malloc(*capacity);
        if (*line == NULL) return -1;
    }
    size_t len = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (len + 2 > *capacity) {
            size_t grown = *capacity * 2;
            char* bigger = realloc(*line, grown);
            if (bigger == NULL) return -1;
            *line = bigger;
            *capacity = grown;
        }
        (*line)[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0) return -1;
    (*line)[len] = '\0';
    return (long)len;
#else
    return (long)getline(line, capacity, f);
#endif
}

char* os_realpath(const char* path) {
#ifdef _WIN32
    char* full = _fullpath(NULL, path, 0);
    if (full == NULL || !os_file_exists(full)) {
        free(full);
        return NULL;
    }
    return full;
#else
    return realpath(path, NULL);
#endif
}

int os_path_is_absolute(const char* path) {
    if (path == NULL || path[0] == '\0') return 0;
#ifdef _WIN32
    /* C:\x, C:/x, \\server\share and a rooted \x or /x */
    if (path[0] == '/' || path[0] == '\\') return 1;
    return ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
           path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
    return path[0] == '/';
#endif
}

/* The last shared-library failure, for os_dl_error. */
static char dl_error_message[256];

#ifdef _WIN32
void* os_dl_open(const char* path) {
    HMODULE module = LoadLibraryA(path);
    if (module == NULL) {
        DWORD code = GetLastError();
        int n = snprintf(dl_error_message, sizeof(dl_error_message), "%s: ", path);
        if (n < 0 || (size_t)n >= sizeof(dl_error_message)) n = 0;
        if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                           code, 0, dl_error_message + n,
                           (DWORD)(sizeof(dl_error_message) - (size_t)n), NULL) == 0) {
            snprintf(dl_error_message + n, sizeof(dl_error_message) - (size_t)n,
                     "error %lu", (unsigned long)code);
        }
        /* FormatMessage ends its text with CR LF. */
        size_t len = strlen(dl_error_message);
        while (len > 0 && (dl_error_message[len - 1] == '\n' || dl_error_message[len - 1] == '\r')) {
            dl_error_message[--len] = '\0';
        }
        return NULL;
    }
    return (void*)module;
}

void* os_dl_sym(void* handle, const char* name) {
    FARPROC proc = GetProcAddress((HMODULE)handle, name);
    if (proc == NULL) {
        snprintf(dl_error_message, sizeof(dl_error_message), "undefined symbol: %s", name);
        return NULL;
    }
    /* A function pointer: copied out rather than cast, as in natives.c. */
    void* addr = NULL;
    memcpy(&addr, &proc, sizeof(addr));
    return addr;
}

void os_dl_close(void* handle) {
    if (handle != NULL) FreeLibrary((HMODULE)handle);
}
#else
void* os_dl_open(const char* path) {
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        const char* message = dlerror();
        snprintf(dl_error_message, sizeof(dl_error_message), "%s",
                 message != NULL ? message : "dlopen failed");
    }
    return handle;
}

void* os_dl_sym(void* handle, const char* name) {
    dlerror();
    void* addr = dlsym(handle, name);
    const char* message = dlerror();
    if (message != NULL) {
        snprintf(dl_error_message, sizeof(dl_error_message), "%s", message);
        return NULL;
    }
    if (addr == NULL) {
        snprintf(dl_error_message, sizeof(dl_error_message), "symbol %s is NULL", name);
    }
    return addr;
}

void os_dl_close(void* handle) {
    if (handle != NULL) dlclose(handle);
}
#endif

const char* os_dl_error(void) {
    return dl_error_message[0] != '\0' ? dl_error_message : "unknown error";
}
