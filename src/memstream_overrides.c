#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct ao_memstream_cookie {
    char **bufp;
    size_t *sizep;
    size_t pos;
    char *buf;
    size_t len;
    size_t space;
} ao_memstream_cookie;

typedef struct ao_wmemstream_cookie {
    wchar_t **bufp;
    size_t *sizep;
    size_t pos;
    wchar_t *buf;
    size_t len;
    size_t space;
    mbstate_t state;
} ao_wmemstream_cookie;

static int ao_memstream_grow(ao_memstream_cookie *cookie, size_t len)
{
    size_t required;
    size_t doubled;
    size_t new_space;
    char *new_buf;

    if (cookie->pos > SIZE_MAX - len - 1U) {
        errno = ENOMEM;
        return -1;
    }

    required = cookie->pos + len + 1U;
    if (required <= cookie->space) {
        return 0;
    }

    if (cookie->space > (SIZE_MAX - 1U) / 2U) {
        doubled = SIZE_MAX;
    } else {
        doubled = 2U * cookie->space + 1U;
    }

    new_space = doubled | required;
    if (new_space < required) {
        new_space = required;
    }

    new_buf = realloc(cookie->buf, new_space);
    if (!new_buf) {
        return -1;
    }

    memset(new_buf + cookie->space, 0, new_space - cookie->space);
    cookie->buf = new_buf;
    cookie->space = new_space;
    *cookie->bufp = new_buf;
    return 0;
}

static ssize_t ao_memstream_write(void *context, const char *buf, size_t len)
{
    ao_memstream_cookie *cookie = context;

    if (ao_memstream_grow(cookie, len) != 0) {
        return 0;
    }

    if (len != 0) {
        memcpy(cookie->buf + cookie->pos, buf, len);
    }

    cookie->pos += len;
    if (cookie->pos >= cookie->len) {
        cookie->len = cookie->pos;
    }
    *cookie->sizep = cookie->pos;

    return (ssize_t)len;
}

static int ao_memstream_seek(void *context, off_t *offset, int whence)
{
    ao_memstream_cookie *cookie = context;
    off_t base;

    if (whence < 0 || whence > 2) {
        errno = EINVAL;
        return -1;
    }

    base = (off_t)((size_t[3]) {0U, cookie->pos, cookie->len}[whence]);
    if (*offset < -base || *offset > (off_t)SSIZE_MAX - base) {
        errno = EINVAL;
        return -1;
    }

    *offset = base + *offset;
    cookie->pos = (size_t)*offset;
    return 0;
}

static int ao_memstream_close(void *context)
{
    ao_memstream_cookie *cookie = context;

    free(cookie);
    return 0;
}

static int ao_wmemstream_grow(ao_wmemstream_cookie *cookie, size_t len)
{
    const size_t max_chars = (size_t)SSIZE_MAX / sizeof(wchar_t);
    size_t required;
    size_t doubled;
    size_t new_space;
    wchar_t *new_buf;

    if (cookie->pos > max_chars - len - 1U) {
        errno = ENOMEM;
        return -1;
    }

    required = cookie->pos + len + 1U;
    if (required <= cookie->space) {
        return 0;
    }

    if (cookie->space > (SIZE_MAX - 1U) / 2U) {
        doubled = SIZE_MAX;
    } else {
        doubled = 2U * cookie->space + 1U;
    }

    new_space = doubled | required;
    if (new_space < required) {
        new_space = required;
    }
    if (new_space > max_chars) {
        errno = ENOMEM;
        return -1;
    }

    new_buf = realloc(cookie->buf, new_space * sizeof(*new_buf));
    if (!new_buf) {
        return -1;
    }

    memset(new_buf + cookie->space, 0, (new_space - cookie->space) * sizeof(*new_buf));
    cookie->buf = new_buf;
    cookie->space = new_space;
    *cookie->bufp = new_buf;
    return 0;
}

static ssize_t ao_wmemstream_write(void *context, const char *buf, size_t len)
{
    ao_wmemstream_cookie *cookie = context;
    const char *src = buf;
    size_t converted;

    if (ao_wmemstream_grow(cookie, len) != 0) {
        return 0;
    }

    converted = mbsnrtowcs(cookie->buf + cookie->pos, &src, len, cookie->space - cookie->pos, &cookie->state);
    if (converted == (size_t)-1) {
        return 0;
    }

    cookie->pos += converted;
    if (cookie->pos >= cookie->len) {
        cookie->len = cookie->pos;
    }
    *cookie->sizep = cookie->pos;

    return (ssize_t)len;
}

static int ao_wmemstream_seek(void *context, off_t *offset, int whence)
{
    ao_wmemstream_cookie *cookie = context;
    off_t base;
    off_t limit = (off_t)((size_t)SSIZE_MAX / sizeof(wchar_t));

    if (whence < 0 || whence > 2) {
        errno = EINVAL;
        return -1;
    }

    base = (off_t)((size_t[3]) {0U, cookie->pos, cookie->len}[whence]);
    if (*offset < -base || *offset > limit - base) {
        errno = EINVAL;
        return -1;
    }

    *offset = base + *offset;
    cookie->pos = (size_t)*offset;
    memset(&cookie->state, 0, sizeof(cookie->state));
    return 0;
}

static int ao_wmemstream_close(void *context)
{
    ao_wmemstream_cookie *cookie = context;

    free(cookie);
    return 0;
}

FILE *open_memstream(char **bufp, size_t *sizep)
{
    static const cookie_io_functions_t io_funcs = {
        .read = NULL,
        .write = ao_memstream_write,
        .seek = ao_memstream_seek,
        .close = ao_memstream_close,
    };
    ao_memstream_cookie *cookie;
    char *buf;
    FILE *stream;

    cookie = malloc(sizeof(*cookie));
    if (!cookie) {
        return NULL;
    }

    buf = malloc(sizeof(*buf));
    if (!buf) {
        free(cookie);
        return NULL;
    }

    cookie->bufp = bufp;
    cookie->sizep = sizep;
    cookie->pos = 0;
    cookie->buf = buf;
    cookie->len = 0;
    cookie->space = 0;

    *sizep = 0;
    *bufp = buf;
    *buf = '\0';

    stream = fopencookie(cookie, "w", io_funcs);
    if (!stream) {
        free(buf);
        free(cookie);
        return NULL;
    }

    return stream;
}

FILE *open_wmemstream(wchar_t **bufp, size_t *sizep)
{
    static const cookie_io_functions_t io_funcs = {
        .read = NULL,
        .write = ao_wmemstream_write,
        .seek = ao_wmemstream_seek,
        .close = ao_wmemstream_close,
    };
    ao_wmemstream_cookie *cookie;
    wchar_t *buf;
    FILE *stream;

    cookie = malloc(sizeof(*cookie));
    if (!cookie) {
        return NULL;
    }

    buf = malloc(sizeof(*buf));
    if (!buf) {
        free(cookie);
        return NULL;
    }

    cookie->bufp = bufp;
    cookie->sizep = sizep;
    cookie->pos = 0;
    cookie->buf = buf;
    cookie->len = 0;
    cookie->space = 0;
    memset(&cookie->state, 0, sizeof(cookie->state));

    *sizep = 0;
    *bufp = buf;
    *buf = L'\0';

    stream = fopencookie(cookie, "w", io_funcs);
    if (!stream) {
        free(buf);
        free(cookie);
        return NULL;
    }

    (void)fwide(stream, 1);
    return stream;
}
