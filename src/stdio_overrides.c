#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int vasprintf(char **s, const char *fmt, va_list ap)
{
    va_list ap_copy;
    int len;

    va_copy(ap_copy, ap);
    len = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);

    if (len < 0) {
        return -1;
    }

    *s = malloc((size_t)len + 1U);
    if (!*s) {
        return -1;
    }

    return vsnprintf(*s, (size_t)len + 1U, fmt, ap);
}

int asprintf(char **s, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vasprintf(s, fmt, ap);
    va_end(ap);

    return ret;
}
