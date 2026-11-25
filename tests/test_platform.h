#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>

static inline void *test_allocate(size_t size, int executable) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
}

static inline void test_release(void *memory, size_t size) {
    (void)size;
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
}

static inline int test_make_executable(void *memory, size_t size) {
    DWORD old;
    return VirtualProtect(memory, size, PAGE_EXECUTE_READ, &old) != 0;
}

static inline void test_setenv(const char *name, const char *value) { _putenv_s(name, value); }

/* A new empty folder of this process in the temporary directory. */
static inline void test_temp_folder(char *out, size_t size, const char *name) {
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    snprintf(out, size, "%s%s_%lu", temp, name, GetCurrentProcessId());
    CreateDirectoryA(out, NULL);
}

static inline void test_remove_file(const char *path) { DeleteFileA(path); }
static inline void test_remove_folder(const char *path) { RemoveDirectoryA(path); }
static inline int test_dup(int fd) { return _dup(fd); }
static inline int test_dup2(int from, int to) { return _dup2(from, to); }
static inline void test_close(int fd) { _close(fd); }
static inline int test_fileno(FILE *file) { return _fileno(file); }
#else
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static inline void *test_allocate(size_t size, int executable) {
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
}

static inline void test_release(void *memory, size_t size) {
    if (memory) munmap(memory, size);
}

static inline int test_make_executable(void *memory, size_t size) { return !mprotect(memory, size, PROT_READ | PROT_EXEC); }

static inline void test_setenv(const char *name, const char *value) {
    if (*value) setenv(name, value, 1);
    else unsetenv(name);
}

/* A new empty folder of this process in the temporary directory. */
static inline void test_temp_folder(char *out, size_t size, const char *name) {
    const char *temp = getenv("TMPDIR");
    snprintf(out, size, "%s/%s_%ld", temp && *temp ? temp : "/tmp", name, (long)getpid());
    mkdir(out, 0700);
}

static inline void test_remove_file(const char *path) { unlink(path); }
static inline void test_remove_folder(const char *path) { rmdir(path); }
static inline int test_dup(int fd) { return dup(fd); }
static inline int test_dup2(int from, int to) { return dup2(from, to); }
static inline void test_close(int fd) { close(fd); }
static inline int test_fileno(FILE *file) { return fileno(file); }
#endif

#ifdef _WIN32
static inline void test_remove_tree(const char *path) {
    char pattern[1100], child[1400];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            snprintf(child, sizeof(child), "%s\\%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) test_remove_tree(child);
            else DeleteFileA(child);
        } while (FindNextFileA(find, &found));
        FindClose(find);
    }
    RemoveDirectoryA(path);
}
#else
static inline void test_remove_tree(const char *path) {
    char child[1400];
    DIR *folder = opendir(path);
    for (struct dirent *entry; folder && (entry = readdir(folder));) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        struct stat info;
        if (!lstat(child, &info) && S_ISDIR(info.st_mode)) test_remove_tree(child);
        else unlink(child);
    }
    if (folder) closedir(folder);
    rmdir(path);
}
#endif
