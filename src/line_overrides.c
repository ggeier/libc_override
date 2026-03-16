#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int ao_grow_line_buffer(char **buffer, size_t *capacity, size_t length)
{
    size_t min_capacity;
    size_t new_capacity;
    char *new_buffer;

    if (length > SIZE_MAX - 2U) {
        errno = ENOMEM;
        return -1;
    }

    min_capacity = length + 2U;
    new_capacity = min_capacity;
    if (new_capacity < SIZE_MAX / 4U) {
        new_capacity += new_capacity / 2U;
    }

    new_buffer = realloc(*buffer, new_capacity);
    if (!new_buffer) {
        new_capacity = min_capacity;
        new_buffer = realloc(*buffer, new_capacity);
        if (!new_buffer) {
            errno = ENOMEM;
            return -1;
        }
    }

    *buffer = new_buffer;
    *capacity = new_capacity;
    return 0;
}

ssize_t getdelim(char **restrict s, size_t *restrict n, int delim, FILE *restrict f)
{
    size_t length = 0;
    int c;

    if (!s || !n) {
        errno = EINVAL;
        return -1;
    }

    if (!*s) {
        *n = 0;
    }

    flockfile(f);

    for (;;) {
        if (length + 1U >= *n) {
            if (ao_grow_line_buffer(s, n, length) != 0) {
                funlockfile(f);
                return -1;
            }
        }

        c = getc_unlocked(f);
        if (c == EOF) {
            if (length == 0 || ferror(f)) {
                funlockfile(f);
                return -1;
            }
            break;
        }

        (*s)[length++] = (char)c;
        if (c == delim) {
            break;
        }
    }

    (*s)[length] = '\0';
    funlockfile(f);
    return (ssize_t)length;
}

ssize_t __getdelim(char **restrict s, size_t *restrict n, int delim, FILE *restrict f)
{
    return getdelim(s, n, delim, f);
}

ssize_t getline(char **restrict s, size_t *restrict n, FILE *restrict f)
{
    return getdelim(s, n, '\n', f);
}
