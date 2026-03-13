#include <stdlib.h>
#include <string.h>
#include <wchar.h>

char *strdup(const char *s)
{
    size_t len = strlen(s);
    char *dup = malloc(len + 1);

    if (!dup) {
        return NULL;
    }

    return memcpy(dup, s, len + 1);
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *dup = malloc(len + 1);

    if (!dup) {
        return NULL;
    }

    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

wchar_t *wcsdup(const wchar_t *s)
{
    size_t len = wcslen(s);
    wchar_t *dup = malloc((len + 1) * sizeof(*dup));

    if (!dup) {
        return NULL;
    }

    return wmemcpy(dup, s, len + 1);
}
