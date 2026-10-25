/* Redirect only the Qt guard's read-only proc measurements to test fixtures. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *fopen(const char *path, const char *mode) {
    static FILE *(*original)(const char *, const char *);
    if (!original) original = dlsym(RTLD_NEXT, "fopen");
    const char *root = getenv("ZERO_TEST_GUARD_PROC");
    const char *name = NULL;
    if (root && strcmp(mode, "r") == 0) {
        if (strcmp(path, "/proc/meminfo") == 0) name = "meminfo";
        if (strcmp(path, "/proc/pressure/memory") == 0) name = "memory";
        if (strcmp(path, "/proc/pressure/io") == 0) name = "io";
    }
    char replacement[4096];
    if (name) {
        int length = snprintf(replacement, sizeof replacement, "%s/%s", root, name);
        if (length < 0 || length >= (int)sizeof replacement) abort();
        path = replacement;
    }
    return original(path, mode);
}
