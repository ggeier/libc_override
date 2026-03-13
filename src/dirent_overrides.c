#define _GNU_SOURCE 1
#define _LARGEFILE64_SOURCE 1

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int scandir(const char *path, struct dirent ***res,
    int (*sel)(const struct dirent *),
    int (*cmp)(const struct dirent **, const struct dirent **))
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    struct dirent **names = NULL;
    struct dirent **tmp;
    size_t count = 0;
    size_t capacity = 0;
    int old_errno = errno;

    if (!dir) {
        return -1;
    }

    while ((errno = 0), (entry = readdir(dir)) != NULL) {
        if (sel && !sel(entry)) {
            continue;
        }

        if (count >= capacity) {
            capacity = 2 * capacity + 1;
            if (capacity > SIZE_MAX / sizeof(*names)) {
                break;
            }
            tmp = realloc(names, capacity * sizeof(*names));
            if (!tmp) {
                break;
            }
            names = tmp;
        }

        names[count] = malloc((size_t)entry->d_reclen);
        if (!names[count]) {
            break;
        }

        memcpy(names[count], entry, (size_t)entry->d_reclen);
        ++count;
    }

    closedir(dir);

    if (errno) {
        while (count > 0) {
            free(names[--count]);
        }
        free(names);
        return -1;
    }

    errno = old_errno;

    if (cmp) {
        qsort(names, count, sizeof(*names), (int (*)(const void *, const void *))cmp);
    }

    *res = names;
    return (int)count;
}

#ifndef scandir64
int scandir64(const char *path, struct dirent64 ***res,
    int (*sel)(const struct dirent64 *),
    int (*cmp)(const struct dirent64 **, const struct dirent64 **))
{
    DIR *dir = opendir(path);
    struct dirent64 *entry;
    struct dirent64 **names = NULL;
    struct dirent64 **tmp;
    size_t count = 0;
    size_t capacity = 0;
    int old_errno = errno;

    if (!dir) {
        return -1;
    }

    while ((errno = 0), (entry = readdir64(dir)) != NULL) {
        if (sel && !sel(entry)) {
            continue;
        }

        if (count >= capacity) {
            capacity = 2 * capacity + 1;
            if (capacity > SIZE_MAX / sizeof(*names)) {
                break;
            }
            tmp = realloc(names, capacity * sizeof(*names));
            if (!tmp) {
                break;
            }
            names = tmp;
        }

        names[count] = malloc((size_t)entry->d_reclen);
        if (!names[count]) {
            break;
        }

        memcpy(names[count], entry, (size_t)entry->d_reclen);
        ++count;
    }

    closedir(dir);

    if (errno) {
        while (count > 0) {
            free(names[--count]);
        }
        free(names);
        return -1;
    }

    errno = old_errno;

    if (cmp) {
        qsort(names, count, sizeof(*names), (int (*)(const void *, const void *))cmp);
    }

    *res = names;
    return (int)count;
}
#endif
