#define _GNU_SOURCE 1

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#undef fscanf
#undef scanf
#undef sscanf
#undef vfscanf
#undef vscanf
#undef vsscanf
#undef fwscanf
#undef wscanf
#undef swscanf
#undef vfwscanf
#undef vwscanf
#undef vswscanf

typedef int (*ao_vfscanf_fn)(FILE *restrict, const char *restrict, va_list);
typedef int (*ao_vsscanf_fn)(const char *restrict, const char *restrict, va_list);
typedef int (*ao_vfwscanf_fn)(FILE *restrict, const wchar_t *restrict, va_list);
typedef int (*ao_vswscanf_fn)(const wchar_t *restrict, const wchar_t *restrict, va_list);

typedef int (*ao_bridge_owns_fn)(const void *);
typedef void (*ao_bridge_libc_free_fn)(void *);
typedef void *(*ao_bridge_libc_realloc_fn)(void *, size_t);

enum {
    AO_SIZE_DEF = 0,
    AO_SIZE_L = 1,
};

typedef enum ao_rehome_mode {
    AO_REHOME_CHAR_STRING,
    AO_REHOME_WIDE_STRING,
    AO_REHOME_CHAR_COUNT,
    AO_REHOME_WIDE_COUNT,
    AO_REHOME_UNSUPPORTED_WIDE_CHAR_COUNT,
} ao_rehome_mode;

typedef struct ao_scanf_record {
    void *slot;
    unsigned int ordinal;
    ao_rehome_mode mode;
    size_t width;
} ao_scanf_record;

typedef struct ao_scanf_plan {
    ao_scanf_record *records;
    size_t count;
    size_t capacity;
} ao_scanf_plan;

static pthread_once_t g_ao_scanf_once = PTHREAD_ONCE_INIT;

static ao_vfscanf_fn g_real_vfscanf = NULL;
static ao_vfscanf_fn g_real_isoc99_vfscanf = NULL;
static ao_vsscanf_fn g_real_vsscanf = NULL;
static ao_vsscanf_fn g_real_isoc99_vsscanf = NULL;
static ao_vfwscanf_fn g_real_vfwscanf = NULL;
static ao_vfwscanf_fn g_real_isoc99_vfwscanf = NULL;
static ao_vswscanf_fn g_real_vswscanf = NULL;
static ao_vswscanf_fn g_real_isoc99_vswscanf = NULL;

static ao_bridge_owns_fn g_bridge_owns = NULL;
static ao_bridge_libc_free_fn g_bridge_libc_free = NULL;
static ao_bridge_libc_realloc_fn g_bridge_libc_realloc = NULL;
static int g_bridge_ready = 0;

static void ao_resolve_function(void *target, const char *name)
{
    void *symbol = dlsym(RTLD_NEXT, name);

    memcpy(target, &symbol, sizeof(symbol));
}

static void ao_resolve_default_symbol(void *target, const char *name)
{
    void *symbol = dlsym(RTLD_DEFAULT, name);

    memcpy(target, &symbol, sizeof(symbol));
}

static void ao_resolve_scanf_symbols(void)
{
    ao_resolve_function(&g_real_vfscanf, "vfscanf");
    ao_resolve_function(&g_real_isoc99_vfscanf, "__isoc99_vfscanf");
    ao_resolve_function(&g_real_vsscanf, "vsscanf");
    ao_resolve_function(&g_real_isoc99_vsscanf, "__isoc99_vsscanf");
    ao_resolve_function(&g_real_vfwscanf, "vfwscanf");
    ao_resolve_function(&g_real_isoc99_vfwscanf, "__isoc99_vfwscanf");
    ao_resolve_function(&g_real_vswscanf, "vswscanf");
    ao_resolve_function(&g_real_isoc99_vswscanf, "__isoc99_vswscanf");

    if (!g_real_isoc99_vfscanf) {
        g_real_isoc99_vfscanf = g_real_vfscanf;
    }
    if (!g_real_isoc99_vsscanf) {
        g_real_isoc99_vsscanf = g_real_vsscanf;
    }
    if (!g_real_isoc99_vfwscanf) {
        g_real_isoc99_vfwscanf = g_real_vfwscanf;
    }
    if (!g_real_isoc99_vswscanf) {
        g_real_isoc99_vswscanf = g_real_vswscanf;
    }

    ao_resolve_default_symbol(&g_bridge_owns, "ao_alloc_bridge_owns");
    ao_resolve_default_symbol(&g_bridge_libc_free, "ao_alloc_bridge_libc_free");
    ao_resolve_default_symbol(&g_bridge_libc_realloc, "ao_alloc_bridge_libc_realloc");
    g_bridge_ready = g_bridge_owns && g_bridge_libc_free && g_bridge_libc_realloc;
}

static void ao_ensure_scanf_symbols(void)
{
    pthread_once(&g_ao_scanf_once, ao_resolve_scanf_symbols);
}

static void ao_scanf_plan_reset(ao_scanf_plan *plan)
{
    free(plan->records);
    plan->records = NULL;
    plan->count = 0;
    plan->capacity = 0;
}

static int ao_scanf_plan_push(ao_scanf_plan *plan, ao_scanf_record record)
{
    ao_scanf_record *records;
    size_t new_capacity;

    if (plan->count == plan->capacity) {
        new_capacity = plan->capacity == 0 ? 8U : plan->capacity * 2U;
        records = realloc(plan->records, new_capacity * sizeof(*records));
        if (!records) {
            return -1;
        }
        plan->records = records;
        plan->capacity = new_capacity;
    }

    plan->records[plan->count++] = record;
    return 0;
}

static int ao_is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

static int ao_is_digit_wchar(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

static size_t ao_parse_width_char(const char **cursor)
{
    size_t width = 0;
    const char *p = *cursor;

    while (ao_is_digit_char(*p)) {
        if (width > (SIZE_MAX - 9U) / 10U) {
            width = SIZE_MAX;
        } else {
            width = width * 10U + (size_t)(*p - '0');
        }
        ++p;
    }

    *cursor = p;
    return width;
}

static size_t ao_parse_width_wchar(const wchar_t **cursor)
{
    size_t width = 0;
    const wchar_t *p = *cursor;

    while (ao_is_digit_wchar(*p)) {
        if (width > (SIZE_MAX - 9U) / 10U) {
            width = SIZE_MAX;
        } else {
            width = width * 10U + (size_t)(*p - L'0');
        }
        ++p;
    }

    *cursor = p;
    return width;
}

static int ao_parse_position_char(const char **cursor, unsigned int *position)
{
    const char *p = *cursor;
    unsigned int value = 0;

    if (!ao_is_digit_char(*p)) {
        return 0;
    }

    while (ao_is_digit_char(*p)) {
        value = value * 10U + (unsigned int)(*p - '0');
        ++p;
    }

    if (*p != '$') {
        return 0;
    }

    *position = value;
    *cursor = p + 1;
    return value != 0;
}

static int ao_parse_position_wchar(const wchar_t **cursor, unsigned int *position)
{
    const wchar_t *p = *cursor;
    unsigned int value = 0;

    if (!ao_is_digit_wchar(*p)) {
        return 0;
    }

    while (ao_is_digit_wchar(*p)) {
        value = value * 10U + (unsigned int)(*p - L'0');
        ++p;
    }

    if (*p != L'$') {
        return 0;
    }

    *position = value;
    *cursor = p + 1;
    return value != 0;
}

static void *ao_arg_n(va_list ap, unsigned int n)
{
    va_list ap_copy;
    void *result = NULL;
    unsigned int i;

    va_copy(ap_copy, ap);
    for (i = 1; i < n; ++i) {
        (void)va_arg(ap_copy, void *);
    }
    result = va_arg(ap_copy, void *);
    va_end(ap_copy);
    return result;
}

static void ao_skip_scanset_char(const char **cursor)
{
    const char *p = *cursor;

    if (*p == '^') {
        ++p;
    }
    if (*p == ']') {
        ++p;
    }
    while (*p && *p != ']') {
        ++p;
    }
    if (*p == ']') {
        ++p;
    }

    *cursor = p;
}

static void ao_skip_scanset_wchar(const wchar_t **cursor)
{
    const wchar_t *p = *cursor;

    if (*p == L'^') {
        ++p;
    }
    if (*p == L']') {
        ++p;
    }
    while (*p && *p != L']') {
        ++p;
    }
    if (*p == L']') {
        ++p;
    }

    *cursor = p;
}

static int ao_record_narrow_m_conversion(
    ao_scanf_plan *plan,
    void *dest,
    unsigned int ordinal,
    int size,
    int spec,
    size_t width)
{
    ao_scanf_record record;

    if (!dest) {
        return 0;
    }

    record.slot = dest;
    record.ordinal = ordinal;
    record.width = width;

    if (spec == 'c') {
        record.mode = (size == AO_SIZE_L) ? AO_REHOME_WIDE_COUNT : AO_REHOME_CHAR_COUNT;
    } else {
        record.mode = (size == AO_SIZE_L) ? AO_REHOME_WIDE_STRING : AO_REHOME_CHAR_STRING;
    }

    return ao_scanf_plan_push(plan, record);
}

static int ao_record_wide_m_conversion(
    ao_scanf_plan *plan,
    void *dest,
    unsigned int ordinal,
    int size,
    int spec,
    size_t width)
{
    ao_scanf_record record;

    if (!dest) {
        return 0;
    }

    record.slot = dest;
    record.ordinal = ordinal;
    record.width = width;

    if (spec == 'c') {
        record.mode = (size == AO_SIZE_L) ? AO_REHOME_WIDE_COUNT : AO_REHOME_UNSUPPORTED_WIDE_CHAR_COUNT;
    } else {
        record.mode = (size == AO_SIZE_L) ? AO_REHOME_WIDE_STRING : AO_REHOME_CHAR_STRING;
    }

    return ao_scanf_plan_push(plan, record);
}

static int ao_parse_scanf_format(const char *format, va_list ap, ao_scanf_plan *plan)
{
    const char *p = format;
    unsigned int ordinal = 0;

    while (*p) {
        int suppressed = 0;
        int alloc = 0;
        int size = AO_SIZE_DEF;
        int spec;
        size_t width = 0;
        void *dest = NULL;
        unsigned int position = 0;

        if (*p != '%') {
            ++p;
            continue;
        }

        ++p;
        if (*p == '%') {
            ++p;
            continue;
        }

        if (*p == '*') {
            suppressed = 1;
            ++p;
        } else if (ao_parse_position_char(&p, &position)) {
            dest = ao_arg_n(ap, position);
        } else {
            dest = va_arg(ap, void *);
        }

        if (ao_is_digit_char(*p)) {
            width = ao_parse_width_char(&p);
        }

        if (*p == 'm') {
            alloc = 1;
            ++p;
        }

        if (*p == 'l') {
            ++p;
            if (*p != 'l') {
                size = AO_SIZE_L;
            }
            if (*p == 'l') {
                ++p;
            }
        } else if (*p == 'h' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'L') {
            if ((*p == 'h' && p[1] == 'h')) {
                ++p;
            }
            ++p;
        }

        if (*p == '\0') {
            break;
        }

        spec = (unsigned char)*p++;
        if ((spec & 0x2f) == 3) {
            spec |= 32;
            size = AO_SIZE_L;
        }

        if (alloc && !suppressed && (spec == 's' || spec == 'c' || spec == '[')) {
            size_t effective_width = (spec == 'c' && width == 0) ? 1U : width;

            if (ao_record_narrow_m_conversion(plan, dest, ordinal + 1U, size, spec, effective_width) != 0) {
                return -1;
            }
        }

        if (spec == '[') {
            ao_skip_scanset_char(&p);
        }

        if (!suppressed && spec != 'n') {
            ++ordinal;
        }
    }

    return 0;
}

static int ao_parse_wscanf_format(const wchar_t *format, va_list ap, ao_scanf_plan *plan)
{
    const wchar_t *p = format;
    unsigned int ordinal = 0;

    while (*p) {
        int suppressed = 0;
        int alloc = 0;
        int size = AO_SIZE_DEF;
        wchar_t spec;
        size_t width = 0;
        void *dest = NULL;
        unsigned int position = 0;

        if (*p != L'%') {
            ++p;
            continue;
        }

        ++p;
        if (*p == L'%') {
            ++p;
            continue;
        }

        if (*p == L'*') {
            suppressed = 1;
            ++p;
        } else if (ao_parse_position_wchar(&p, &position)) {
            dest = ao_arg_n(ap, position);
        } else {
            dest = va_arg(ap, void *);
        }

        if (ao_is_digit_wchar(*p)) {
            width = ao_parse_width_wchar(&p);
        }

        if (*p == L'm') {
            alloc = 1;
            ++p;
        }

        if (*p == L'l') {
            ++p;
            if (*p != L'l') {
                size = AO_SIZE_L;
            }
            if (*p == L'l') {
                ++p;
            }
        } else if (*p == L'h' || *p == L'j' || *p == L'z' || *p == L't' || *p == L'L') {
            if ((*p == L'h' && p[1] == L'h')) {
                ++p;
            }
            ++p;
        }

        if (*p == L'\0') {
            break;
        }

        spec = *p++;
        if ((spec & 0x2f) == 3) {
            spec |= 32;
            size = AO_SIZE_L;
        }

        if (alloc && !suppressed && (spec == L's' || spec == L'c' || spec == L'[')) {
            size_t effective_width = (spec == L'c' && width == 0) ? 1U : width;

            if (ao_record_wide_m_conversion(plan, dest, ordinal + 1U, size, (int)spec, effective_width) != 0) {
                return -1;
            }
        }

        if (spec == L'[') {
            ao_skip_scanset_wchar(&p);
        }

        if (!suppressed && spec != L'n') {
            ++ordinal;
        }
    }

    return 0;
}

static void *ao_load_pointer_slot(const void *slot)
{
    void *value = NULL;

    memcpy(&value, slot, sizeof(value));
    return value;
}

static void ao_store_pointer_slot(void *slot, void *value)
{
    memcpy(slot, &value, sizeof(value));
}

static void *ao_clone_char_string(const char *source)
{
    size_t len = strlen(source) + 1U;
    char *copy = malloc(len);

    if (!copy) {
        return NULL;
    }

    memcpy(copy, source, len);
    return copy;
}

static void *ao_clone_wide_string(const wchar_t *source)
{
    size_t len = (wcslen(source) + 1U) * sizeof(*source);
    wchar_t *copy = malloc(len);

    if (!copy) {
        return NULL;
    }

    memcpy(copy, source, len);
    return copy;
}

static void *ao_clone_bytes(const void *source, size_t len)
{
    void *copy = malloc(len);

    if (!copy) {
        return NULL;
    }

    if (len != 0) {
        memcpy(copy, source, len);
    }
    return copy;
}

static void ao_rehome_scanf_record(const ao_scanf_record *record)
{
    void *original = ao_load_pointer_slot(record->slot);
    void *replacement = NULL;

    if (!original || !g_bridge_ready || g_bridge_owns(original)) {
        return;
    }

    switch (record->mode) {
    case AO_REHOME_CHAR_STRING:
        /* Embedded NUL bytes truncate here because the hybrid path only sees the post-scan pointer. */
        replacement = ao_clone_char_string(original);
        break;
    case AO_REHOME_WIDE_STRING:
        /* Embedded wide NUL values truncate here because the hybrid path only sees the post-scan pointer. */
        replacement = ao_clone_wide_string(original);
        break;
    case AO_REHOME_CHAR_COUNT:
        replacement = ao_clone_bytes(original, record->width);
        break;
    case AO_REHOME_WIDE_COUNT:
        replacement = ao_clone_bytes(original, record->width * sizeof(wchar_t));
        break;
    case AO_REHOME_UNSUPPORTED_WIDE_CHAR_COUNT:
        /* MEMORY LEAK: wide-input %mc byte lengths are not recoverable safely after libc scanning. */
        return;
    }

    if (!replacement) {
        /* MEMORY LEAK: keeping the libc-owned %m result in place preserves the committed scanf result. */
        return;
    }

    ao_store_pointer_slot(record->slot, replacement);
    g_bridge_libc_free(original);
}

static void ao_rehome_scanf_plan(const ao_scanf_plan *plan, int matches)
{
    size_t i;

    for (i = 0; i < plan->count; ++i) {
        if ((int)plan->records[i].ordinal > matches) {
            break;
        }
        ao_rehome_scanf_record(&plan->records[i]);
    }
}

static int ao_wrap_vfscanf_common(ao_vfscanf_fn real_fn, FILE *stream, const char *format, va_list ap)
{
    ao_scanf_plan plan = {0};
    va_list parse_ap;
    va_list call_ap;
    int ret;
    int parse_ok = 0;

    ao_ensure_scanf_symbols();
    if (!real_fn) {
        errno = ENOSYS;
        return EOF;
    }

    va_copy(parse_ap, ap);
    parse_ok = (ao_parse_scanf_format(format, parse_ap, &plan) == 0);
    va_end(parse_ap);

    va_copy(call_ap, ap);
    ret = real_fn(stream, format, call_ap);
    va_end(call_ap);

    if (parse_ok && g_bridge_ready && ret > 0) {
        ao_rehome_scanf_plan(&plan, ret);
    }

    ao_scanf_plan_reset(&plan);
    return ret;
}

static int ao_wrap_vsscanf_common(ao_vsscanf_fn real_fn, const char *input, const char *format, va_list ap)
{
    ao_scanf_plan plan = {0};
    va_list parse_ap;
    va_list call_ap;
    int ret;
    int parse_ok = 0;

    ao_ensure_scanf_symbols();
    if (!real_fn) {
        errno = ENOSYS;
        return EOF;
    }

    va_copy(parse_ap, ap);
    parse_ok = (ao_parse_scanf_format(format, parse_ap, &plan) == 0);
    va_end(parse_ap);

    va_copy(call_ap, ap);
    ret = real_fn(input, format, call_ap);
    va_end(call_ap);

    if (parse_ok && g_bridge_ready && ret > 0) {
        ao_rehome_scanf_plan(&plan, ret);
    }

    ao_scanf_plan_reset(&plan);
    return ret;
}

static int ao_wrap_vfwscanf_common(ao_vfwscanf_fn real_fn, FILE *stream, const wchar_t *format, va_list ap)
{
    ao_scanf_plan plan = {0};
    va_list parse_ap;
    va_list call_ap;
    int ret;
    int parse_ok = 0;

    ao_ensure_scanf_symbols();
    if (!real_fn) {
        errno = ENOSYS;
        return EOF;
    }

    va_copy(parse_ap, ap);
    parse_ok = (ao_parse_wscanf_format(format, parse_ap, &plan) == 0);
    va_end(parse_ap);

    va_copy(call_ap, ap);
    ret = real_fn(stream, format, call_ap);
    va_end(call_ap);

    if (parse_ok && g_bridge_ready && ret > 0) {
        ao_rehome_scanf_plan(&plan, ret);
    }

    ao_scanf_plan_reset(&plan);
    return ret;
}

static int ao_wrap_vswscanf_common(ao_vswscanf_fn real_fn, const wchar_t *input, const wchar_t *format, va_list ap)
{
    ao_scanf_plan plan = {0};
    va_list parse_ap;
    va_list call_ap;
    int ret;
    int parse_ok = 0;

    ao_ensure_scanf_symbols();
    if (!real_fn) {
        errno = ENOSYS;
        return EOF;
    }

    va_copy(parse_ap, ap);
    parse_ok = (ao_parse_wscanf_format(format, parse_ap, &plan) == 0);
    va_end(parse_ap);

    va_copy(call_ap, ap);
    ret = real_fn(input, format, call_ap);
    va_end(call_ap);

    if (parse_ok && g_bridge_ready && ret > 0) {
        ao_rehome_scanf_plan(&plan, ret);
    }

    ao_scanf_plan_reset(&plan);
    return ret;
}

int ao_plain_vfscanf(FILE *restrict stream, const char *restrict format, va_list ap) __asm__("vfscanf");
int ao_plain_vfscanf(FILE *restrict stream, const char *restrict format, va_list ap)
{
    return ao_wrap_vfscanf_common(g_real_vfscanf, stream, format, ap);
}

int __isoc99_vfscanf(FILE *restrict stream, const char *restrict format, va_list ap)
{
    return ao_wrap_vfscanf_common(g_real_isoc99_vfscanf, stream, format, ap);
}

int ao_plain_vscanf(const char *restrict format, va_list ap) __asm__("vscanf");
int ao_plain_vscanf(const char *restrict format, va_list ap)
{
    return ao_wrap_vfscanf_common(g_real_vfscanf, stdin, format, ap);
}

int __isoc99_vscanf(const char *restrict format, va_list ap)
{
    return ao_wrap_vfscanf_common(g_real_isoc99_vfscanf, stdin, format, ap);
}

int ao_plain_fscanf(FILE *restrict stream, const char *restrict format, ...) __asm__("fscanf");
int ao_plain_fscanf(FILE *restrict stream, const char *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfscanf_common(g_real_vfscanf, stream, format, ap);
    va_end(ap);
    return ret;
}

int __isoc99_fscanf(FILE *restrict stream, const char *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfscanf_common(g_real_isoc99_vfscanf, stream, format, ap);
    va_end(ap);
    return ret;
}

int ao_plain_scanf(const char *restrict format, ...) __asm__("scanf");
int ao_plain_scanf(const char *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfscanf_common(g_real_vfscanf, stdin, format, ap);
    va_end(ap);
    return ret;
}

int __isoc99_scanf(const char *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfscanf_common(g_real_isoc99_vfscanf, stdin, format, ap);
    va_end(ap);
    return ret;
}

int ao_plain_vsscanf(const char *restrict input, const char *restrict format, va_list ap) __asm__("vsscanf");
int ao_plain_vsscanf(const char *restrict input, const char *restrict format, va_list ap)
{
    return ao_wrap_vsscanf_common(g_real_vsscanf, input, format, ap);
}

int __isoc99_vsscanf(const char *restrict input, const char *restrict format, va_list ap)
{
    return ao_wrap_vsscanf_common(g_real_isoc99_vsscanf, input, format, ap);
}

int ao_plain_sscanf(const char *restrict input, const char *restrict format, ...) __asm__("sscanf");
int ao_plain_sscanf(const char *restrict input, const char *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vsscanf_common(g_real_vsscanf, input, format, ap);
    va_end(ap);
    return ret;
}

int __isoc99_sscanf(const char *restrict input, const char *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vsscanf_common(g_real_isoc99_vsscanf, input, format, ap);
    va_end(ap);
    return ret;
}

int ao_plain_vfwscanf(FILE *restrict stream, const wchar_t *restrict format, va_list ap) __asm__("vfwscanf");
int ao_plain_vfwscanf(FILE *restrict stream, const wchar_t *restrict format, va_list ap)
{
    return ao_wrap_vfwscanf_common(g_real_vfwscanf, stream, format, ap);
}

int __isoc99_vfwscanf(FILE *restrict stream, const wchar_t *restrict format, va_list ap)
{
    return ao_wrap_vfwscanf_common(g_real_isoc99_vfwscanf, stream, format, ap);
}

int ao_plain_vwscanf(const wchar_t *restrict format, va_list ap) __asm__("vwscanf");
int ao_plain_vwscanf(const wchar_t *restrict format, va_list ap)
{
    return ao_wrap_vfwscanf_common(g_real_vfwscanf, stdin, format, ap);
}

int __isoc99_vwscanf(const wchar_t *restrict format, va_list ap)
{
    return ao_wrap_vfwscanf_common(g_real_isoc99_vfwscanf, stdin, format, ap);
}

int ao_plain_fwscanf(FILE *restrict stream, const wchar_t *restrict format, ...) __asm__("fwscanf");
int ao_plain_fwscanf(FILE *restrict stream, const wchar_t *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfwscanf_common(g_real_vfwscanf, stream, format, ap);
    va_end(ap);
    return ret;
}

int __isoc99_fwscanf(FILE *restrict stream, const wchar_t *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfwscanf_common(g_real_isoc99_vfwscanf, stream, format, ap);
    va_end(ap);
    return ret;
}

int ao_plain_wscanf(const wchar_t *restrict format, ...) __asm__("wscanf");
int ao_plain_wscanf(const wchar_t *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfwscanf_common(g_real_vfwscanf, stdin, format, ap);
    va_end(ap);
    return ret;
}

int __isoc99_wscanf(const wchar_t *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vfwscanf_common(g_real_isoc99_vfwscanf, stdin, format, ap);
    va_end(ap);
    return ret;
}

int ao_plain_vswscanf(const wchar_t *restrict input, const wchar_t *restrict format, va_list ap) __asm__("vswscanf");
int ao_plain_vswscanf(const wchar_t *restrict input, const wchar_t *restrict format, va_list ap)
{
    return ao_wrap_vswscanf_common(g_real_vswscanf, input, format, ap);
}

int __isoc99_vswscanf(const wchar_t *restrict input, const wchar_t *restrict format, va_list ap)
{
    return ao_wrap_vswscanf_common(g_real_isoc99_vswscanf, input, format, ap);
}

int ao_plain_swscanf(const wchar_t *restrict input, const wchar_t *restrict format, ...) __asm__("swscanf");
int ao_plain_swscanf(const wchar_t *restrict input, const wchar_t *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vswscanf_common(g_real_vswscanf, input, format, ap);
    va_end(ap);
    return ret;
}

int __isoc99_swscanf(const wchar_t *restrict input, const wchar_t *restrict format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = ao_wrap_vswscanf_common(g_real_isoc99_vswscanf, input, format, ap);
    va_end(ap);
    return ret;
}
