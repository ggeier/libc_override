#define _GNU_SOURCE 1

#include "common.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAXTRIES 100

#ifndef SYMLOOP_MAX
#define SYMLOOP_MAX 40
#endif

static size_t ao_slash_len(const char *s)
{
    const char *start = s;

    while (*s == '/') {
        ++s;
    }

    return (size_t)(s - start);
}

char *getcwd(char *buf, size_t size)
{
    char stack_buf[PATH_MAX];
    long ret;

    if (!buf) {
        buf = stack_buf;
        size = sizeof(stack_buf);
    } else if (size == 0) {
        errno = EINVAL;
        return NULL;
    }

    ret = syscall(SYS_getcwd, buf, size);
    if (ret < 0) {
        return NULL;
    }

    if (ret == 0 || buf[0] != '/') {
        errno = ENOENT;
        return NULL;
    }

    if (buf == stack_buf) {
        return strdup(buf);
    }

    return buf;
}

char *get_current_dir_name(void)
{
    struct stat pwd_stat;
    struct stat dot_stat;
    char *pwd = getenv("PWD");

    if (pwd && *pwd && !stat(pwd, &pwd_stat) && !stat(".", &dot_stat) &&
        pwd_stat.st_dev == dot_stat.st_dev && pwd_stat.st_ino == dot_stat.st_ino) {
        return strdup(pwd);
    }

    return getcwd(NULL, 0);
}

char *realpath(const char *restrict filename, char *restrict resolved)
{
    char stack[PATH_MAX + 1];
    char output[PATH_MAX];
    size_t p;
    size_t q;
    size_t len;
    size_t component_len;
    size_t loop_count = 0;
    size_t pending_up = 0;
    int check_dir = 0;

    if (!filename) {
        errno = EINVAL;
        return NULL;
    }

    len = strnlen(filename, sizeof(stack));
    if (len == 0) {
        errno = ENOENT;
        return NULL;
    }
    if (len >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    p = sizeof(stack) - len - 1;
    q = 0;
    memcpy(stack + p, filename, len + 1);

restart:
    for (;; p += ao_slash_len(stack + p)) {
        char *end;
        int up = 0;
        ssize_t link_len;
        size_t component_size;

        if (stack[p] == '/') {
            check_dir = 0;
            pending_up = 0;
            q = 0;
            output[q++] = '/';
            ++p;
            if (stack[p] == '/' && stack[p + 1] != '/') {
                output[q++] = '/';
            }
            continue;
        }

        end = ao_strchrnul(stack + p, '/');
        component_len = (size_t)(end - (stack + p));
        component_size = component_len;

        if (component_len == 0 && !check_dir) {
            break;
        }

        if (component_len == 1 && stack[p] == '.') {
            p += component_len;
            continue;
        }

        if (q && output[q - 1] != '/') {
            if (p == 0) {
                errno = ENAMETOOLONG;
                return NULL;
            }
            stack[--p] = '/';
            ++component_len;
        }
        if (q + component_len >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }

        memcpy(output + q, stack + p, component_len);
        output[q + component_len] = '\0';
        p += component_len;

        if (component_size == 2 && stack[p - 2] == '.' && stack[p - 1] == '.') {
            up = 1;
            if (q <= 3 * pending_up) {
                ++pending_up;
                q += component_len;
                continue;
            }
            if (!check_dir) {
                goto skip_readlink;
            }
        }

        link_len = readlink(output, stack, p);
        if (link_len == (ssize_t)p) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        if (link_len == 0) {
            errno = ENOENT;
            return NULL;
        }
        if (link_len < 0) {
            if (errno != EINVAL) {
                return NULL;
            }

skip_readlink:
            check_dir = 0;
            if (up) {
                while (q && output[q - 1] != '/') {
                    --q;
                }
                if (q > 1 && (q > 2 || output[0] != '/')) {
                    --q;
                }
                continue;
            }
            if (component_size != 0) {
                q += component_len;
            }
            check_dir = stack[p] != '\0';
            continue;
        }

        if (++loop_count == SYMLOOP_MAX) {
            errno = ELOOP;
            return NULL;
        }

        if (stack[link_len - 1] == '/') {
            while (stack[p] == '/') {
                ++p;
            }
        }

        p -= (size_t)link_len;
        memmove(stack + p, stack, (size_t)link_len);
        goto restart;
    }

    output[q] = '\0';

    if (output[0] != '/') {
        size_t cwd_len;
        size_t skip = 0;

        if (!getcwd(stack, sizeof(stack))) {
            return NULL;
        }

        cwd_len = strlen(stack);
        while (pending_up--) {
            while (cwd_len > 1 && stack[cwd_len - 1] != '/') {
                --cwd_len;
            }
            if (cwd_len > 1) {
                --cwd_len;
            }
            skip += 2;
            if (skip < q) {
                ++skip;
            }
        }
        if (q - skip && stack[cwd_len - 1] != '/') {
            stack[cwd_len++] = '/';
        }
        if (cwd_len + (q - skip) + 1 >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }

        memmove(output + cwd_len, output + skip, q - skip + 1);
        memcpy(output, stack, cwd_len);
        q = cwd_len + q - skip;
    }

    if (resolved) {
        return memcpy(resolved, output, q + 1);
    }

    return strdup(output);
}

char *tempnam(const char *dir, const char *pfx)
{
    char path[PATH_MAX];
    size_t dir_len;
    size_t pfx_len;
    size_t total_len;
    int attempt;

    if (!dir) {
        dir = P_tmpdir;
    }
    if (!pfx) {
        pfx = "temp";
    }

    dir_len = strlen(dir);
    pfx_len = strlen(pfx);
    total_len = dir_len + 1 + pfx_len + 1 + 6;

    if (total_len >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    memcpy(path, dir, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1, pfx, pfx_len);
    path[dir_len + 1 + pfx_len] = '_';
    path[total_len] = '\0';

    for (attempt = 0; attempt < MAXTRIES; ++attempt) {
        char dummy[1];
        ao_randname(path + total_len - 6);
        if (readlink(path, dummy, sizeof(dummy)) < 0 && errno == ENOENT) {
            return strdup(path);
        }
    }

    return NULL;
}
