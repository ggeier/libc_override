#define _GNU_SOURCE 1
#define _LARGEFILE64_SOURCE 1

#include "../include/mock_allocator_test_api.h"
#include "test_support.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

typedef void (*ao_mock_reset_fn)(void);
typedef void (*ao_mock_capture_fn)(void);
typedef void (*ao_mock_get_stats_fn)(ao_mock_stats *);
typedef int (*ao_mock_is_tracked_fn)(const void *);
typedef char *(*tempnam_fn_t)(const char *, const char *);
typedef char *(*get_current_dir_name_fn_t)(void);

typedef enum run_mode {
    RUN_MODE_DIAGNOSTIC,
    RUN_MODE_STRICT_NEGATIVE,
} run_mode;

typedef struct mock_api {
    ao_mock_reset_fn reset;
    ao_mock_capture_fn begin_capture;
    ao_mock_capture_fn end_capture;
    ao_mock_get_stats_fn get_stats;
    ao_mock_is_tracked_fn is_tracked;
} mock_api;

typedef struct case_result {
    const char *name;
    int supported;
    uint64_t create_alloc_calls;
    uint64_t cleanup_free_calls;
    uint64_t cleanup_bypass_free_calls;
    uint64_t created_objects;
    uint64_t tracked_objects;
} case_result;

static void fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void check(bool condition, const char *message)
{
    if (!condition) {
        fail(message);
    }
}

static uint64_t alloc_call_count(const ao_mock_stats *stats)
{
    return stats->malloc_calls + stats->calloc_calls + stats->realloc_calls;
}

static void *lookup_symbol(const char *name)
{
    void *symbol = dlsym(RTLD_DEFAULT, name);

    check(symbol != NULL, "required test symbol not found");
    return symbol;
}

static tempnam_fn_t lookup_tempnam(void)
{
    tempnam_fn_t fn = NULL;
    void *symbol = lookup_symbol("tempnam");

    memcpy(&fn, &symbol, sizeof(fn));
    return fn;
}

static get_current_dir_name_fn_t lookup_get_current_dir_name_optional(void)
{
    get_current_dir_name_fn_t fn = NULL;
    void *symbol = dlsym(RTLD_DEFAULT, "get_current_dir_name");

    memcpy(&fn, &symbol, sizeof(fn));
    return fn;
}

static mock_api load_mock_api(void)
{
    mock_api api = {0};
    void *symbol;

    symbol = lookup_symbol("ao_mock_reset");
    memcpy(&api.reset, &symbol, sizeof(api.reset));

    symbol = lookup_symbol("ao_mock_begin_capture");
    memcpy(&api.begin_capture, &symbol, sizeof(api.begin_capture));

    symbol = lookup_symbol("ao_mock_end_capture");
    memcpy(&api.end_capture, &symbol, sizeof(api.end_capture));

    symbol = lookup_symbol("ao_mock_get_stats");
    memcpy(&api.get_stats, &symbol, sizeof(api.get_stats));

    symbol = lookup_symbol("ao_mock_is_tracked_allocation");
    memcpy(&api.is_tracked, &symbol, sizeof(api.is_tracked));

    return api;
}

static void check_mock_preload(const char *library_name)
{
    Dl_info info;
    void *symbol = lookup_symbol("malloc");

    check(dladdr(symbol, &info) != 0, "dladdr(malloc) failed");
    check(info.dli_fname != NULL, "dladdr(malloc) returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "malloc did not resolve to mock allocator");

    symbol = lookup_symbol("ao_mock_reset");
    check(dladdr(symbol, &info) != 0, "dladdr(ao_mock_reset) failed");
    check(info.dli_fname != NULL, "dladdr(ao_mock_reset) returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "mock allocator test API did not resolve to mock allocator");
}

static ao_mock_stats snapshot(mock_api *api)
{
    ao_mock_stats stats;
    api->get_stats(&stats);
    return stats;
}

static void fill_result(case_result *result, const char *name, const ao_mock_stats *before, const ao_mock_stats *after_alloc, const ao_mock_stats *after_free, uint64_t created_objects, uint64_t tracked_objects)
{
    result->name = name;
    result->supported = 1;
    result->create_alloc_calls = alloc_call_count(after_alloc) - alloc_call_count(before);
    result->cleanup_free_calls = after_free->free_calls - after_alloc->free_calls;
    result->cleanup_bypass_free_calls = after_free->bypass_free_calls - after_alloc->bypass_free_calls;
    result->created_objects = created_objects;
    result->tracked_objects = tracked_objects;
}

static void write_result(FILE *out, const case_result *result)
{
    fprintf(out,
        "case=%s supported=%llu create_alloc_calls=%llu cleanup_free_calls=%llu cleanup_bypass_free_calls=%llu created_objects=%llu tracked_objects=%llu\n",
        result->name,
        result->supported ? 1ULL : 0ULL,
        (unsigned long long)result->create_alloc_calls,
        (unsigned long long)result->cleanup_free_calls,
        (unsigned long long)result->cleanup_bypass_free_calls,
        (unsigned long long)result->created_objects,
        (unsigned long long)result->tracked_objects);
}

static void validate_strict_negative(const case_result *result)
{
    if (!result->supported) {
        return;
    }

    if (strcmp(result->name, "open_memstream") == 0 || strcmp(result->name, "open_wmemstream") == 0) {
        return;
    }

    if (result->created_objects == 0) {
        check(result->tracked_objects == 0, "non-allocating case unexpectedly reported tracked objects");
        return;
    }

    check(result->tracked_objects == 0, "mock-only strict negative expected untracked returned objects");
    check(result->cleanup_free_calls == result->created_objects, "mock-only strict negative expected one free per returned object");
    check(result->cleanup_bypass_free_calls == result->created_objects, "mock-only strict negative expected bypass frees for every returned object");
}

static void create_empty_file(const char *path)
{
    FILE *file = fopen(path, "w");

    check(file != NULL, "failed to create test file");
    fclose(file);
}

static FILE *open_line_reader_stream(const char *contents, char *buffer, size_t buffer_size)
{
    FILE *file = tmpfile();
    size_t length = strlen(contents);

    check(file != NULL, "tmpfile failed");
    check(setvbuf(file, buffer, _IOFBF, buffer_size) == 0, "setvbuf failed");
    check(fwrite(contents, 1, length, file) == length, "failed to write line reader input");
    check(fflush(file) == 0, "fflush failed");
    rewind(file);
    return file;
}

static int call_vasprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vasprintf(out, fmt, ap);
    va_end(ap);

    return ret;
}

static int dirent_name_cmp(const struct dirent **lhs, const struct dirent **rhs)
{
    return strcmp((*lhs)->d_name, (*rhs)->d_name);
}

static int dirent64_name_cmp(const struct dirent64 **lhs, const struct dirent64 **rhs)
{
    return strcmp((*lhs)->d_name, (*rhs)->d_name);
}

static case_result run_strdup_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char *dup;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    dup = strdup("allocator validation");
    api->end_capture();
    after_alloc = snapshot(api);

    check(dup != NULL, "strdup returned null");
    check(strcmp(dup, "allocator validation") == 0, "strdup content mismatch");
    tracked = api->is_tracked(dup) ? 1u : 0u;

    api->begin_capture();
    free(dup);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "strdup", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_strndup_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char *dup;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    dup = strndup("allocator validation", 9);
    api->end_capture();
    after_alloc = snapshot(api);

    check(dup != NULL, "strndup returned null");
    check(strcmp(dup, "allocator") == 0, "strndup content mismatch");
    tracked = api->is_tracked(dup) ? 1u : 0u;

    api->begin_capture();
    free(dup);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "strndup", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_wcsdup_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    wchar_t sample[] = L"wide validation";
    wchar_t *dup;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    dup = wcsdup(sample);
    api->end_capture();
    after_alloc = snapshot(api);

    check(dup != NULL, "wcsdup returned null");
    check(wcscmp(dup, sample) == 0, "wcsdup content mismatch");
    tracked = api->is_tracked(dup) ? 1u : 0u;

    api->begin_capture();
    free(dup);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "wcsdup", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_asprintf_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char *msg = NULL;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    check(asprintf(&msg, "%s %d", "phase", 1) == 7, "asprintf length mismatch");
    api->end_capture();
    after_alloc = snapshot(api);

    check(msg != NULL, "asprintf returned null string");
    check(strcmp(msg, "phase 1") == 0, "asprintf content mismatch");
    tracked = api->is_tracked(msg) ? 1u : 0u;

    api->begin_capture();
    free(msg);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "asprintf", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_vasprintf_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char *msg = NULL;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    check(call_vasprintf(&msg, "%s-%s", "override", "ok") == 11, "vasprintf length mismatch");
    api->end_capture();
    after_alloc = snapshot(api);

    check(msg != NULL, "vasprintf returned null string");
    check(strcmp(msg, "override-ok") == 0, "vasprintf content mismatch");
    tracked = api->is_tracked(msg) ? 1u : 0u;

    api->begin_capture();
    free(msg);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "vasprintf", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_getdelim_case(mock_api *api)
{
    static const char input[] = "hello,world";
    static const char expected[] = "hello,";
    char file_buffer[64];
    FILE *file = open_line_reader_stream(input, file_buffer, sizeof(file_buffer));
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    length = getdelim(&line, &capacity, ',', file);
    api->end_capture();
    after_alloc = snapshot(api);

    check(length == (ssize_t)strlen(expected), "getdelim length mismatch");
    check(line != NULL, "getdelim returned null");
    check(strcmp(line, expected) == 0, "getdelim content mismatch");
    tracked = api->is_tracked(line) ? 1u : 0u;

    api->begin_capture();
    free(line);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "getdelim", &before, &after_alloc, &after_free, 1, tracked);
    fclose(file);
    return result;
}

static case_result run_getline_case(mock_api *api)
{
    static const char expected[] = "allocator growth line\n";
    char file_buffer[128];
    FILE *file = open_line_reader_stream(expected, file_buffer, sizeof(file_buffer));
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    length = getline(&line, &capacity, file);
    api->end_capture();
    after_alloc = snapshot(api);

    check(length == (ssize_t)strlen(expected), "getline length mismatch");
    check(line != NULL, "getline returned null");
    check(strcmp(line, expected) == 0, "getline content mismatch");
    tracked = api->is_tracked(line) ? 1u : 0u;

    api->begin_capture();
    free(line);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "getline", &before, &after_alloc, &after_free, 1, tracked);
    fclose(file);
    return result;
}

static case_result run_open_memstream_case(mock_api *api)
{
    static const char payload[] = "allocator override buffer";
    ao_mock_stats before;
    ao_mock_stats after_open;
    ao_mock_stats after_close;
    ao_mock_stats after_free;
    case_result result;
    FILE *stream;
    char *buffer = NULL;
    size_t size = 0;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    stream = open_memstream(&buffer, &size);
    check(stream != NULL, "open_memstream returned null");
    check(fwrite(payload, 1, strlen(payload), stream) == strlen(payload), "open_memstream fwrite failed");
    check(fflush(stream) == 0, "open_memstream fflush failed");
    api->end_capture();
    after_open = snapshot(api);

    check(buffer != NULL, "open_memstream did not publish buffer");
    check(strcmp(buffer, payload) == 0, "open_memstream content mismatch");
    tracked = api->is_tracked(buffer) ? 1u : 0u;

    api->begin_capture();
    check(fclose(stream) == 0, "open_memstream fclose failed");
    api->end_capture();
    after_close = snapshot(api);

    api->begin_capture();
    free(buffer);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "open_memstream", &before, &after_open, &after_free, 1, tracked);
    result.cleanup_free_calls = after_free.free_calls - after_close.free_calls;
    result.cleanup_bypass_free_calls = after_free.bypass_free_calls - after_close.bypass_free_calls;
    return result;
}

static case_result run_open_wmemstream_case(mock_api *api)
{
    static const wchar_t payload[] = L"allocator wide buffer";
    ao_mock_stats before;
    ao_mock_stats after_open;
    ao_mock_stats after_close;
    ao_mock_stats after_free;
    case_result result;
    FILE *stream;
    wchar_t *buffer = NULL;
    size_t size = 0;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    stream = open_wmemstream(&buffer, &size);
    check(stream != NULL, "open_wmemstream returned null");
    check(fwide(stream, 0) > 0, "open_wmemstream is not wide-oriented");
    check(fputws(payload, stream) >= 0, "open_wmemstream fputws failed");
    check(fflush(stream) == 0, "open_wmemstream fflush failed");
    api->end_capture();
    after_open = snapshot(api);

    check(buffer != NULL, "open_wmemstream did not publish buffer");
    check(wcscmp(buffer, payload) == 0, "open_wmemstream content mismatch");
    tracked = api->is_tracked(buffer) ? 1u : 0u;

    api->begin_capture();
    check(fclose(stream) == 0, "open_wmemstream fclose failed");
    api->end_capture();
    after_close = snapshot(api);

    api->begin_capture();
    free(buffer);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "open_wmemstream", &before, &after_open, &after_free, 1, tracked);
    result.cleanup_free_calls = after_free.free_calls - after_close.free_calls;
    result.cleanup_bypass_free_calls = after_free.bypass_free_calls - after_close.bypass_free_calls;
    return result;
}

static case_result run_getcwd_buffer_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char buffer[PATH_MAX];

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    check(getcwd(buffer, sizeof(buffer)) == buffer, "getcwd with caller buffer failed");
    api->end_capture();
    after_alloc = snapshot(api);

    after_free = after_alloc;
    fill_result(&result, "getcwd_buffer", &before, &after_alloc, &after_free, 0, 0);
    return result;
}

static case_result run_getcwd_alloc_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char expected[PATH_MAX];
    char *dynamic_result;
    uint64_t tracked;

    check(getcwd(expected, sizeof(expected)) == expected, "baseline getcwd failed");

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    dynamic_result = getcwd(NULL, 0);
    api->end_capture();
    after_alloc = snapshot(api);

    check(dynamic_result != NULL, "getcwd(NULL, 0) returned null");
    check(strcmp(dynamic_result, expected) == 0, "getcwd(NULL, 0) content mismatch");
    tracked = api->is_tracked(dynamic_result) ? 1u : 0u;

    api->begin_capture();
    free(dynamic_result);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "getcwd_alloc", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_get_current_dir_name_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char expected[PATH_MAX];
    char *cwd;
    uint64_t tracked;
    get_current_dir_name_fn_t get_current_dir_name_fn = lookup_get_current_dir_name_optional();

    if (!get_current_dir_name_fn) {
        memset(&result, 0, sizeof(result));
        result.name = "get_current_dir_name";
        return result;
    }

    check(getcwd(expected, sizeof(expected)) == expected, "baseline getcwd failed");

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    cwd = get_current_dir_name_fn();
    api->end_capture();
    after_alloc = snapshot(api);

    check(cwd != NULL, "get_current_dir_name returned null");
    check(strcmp(cwd, expected) == 0, "get_current_dir_name content mismatch");
    tracked = api->is_tracked(cwd) ? 1u : 0u;

    api->begin_capture();
    free(cwd);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "get_current_dir_name", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_realpath_buffer_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char expected[PATH_MAX];
    char resolved[PATH_MAX];

    check(getcwd(expected, sizeof(expected)) == expected, "baseline getcwd failed");

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    check(realpath(".", resolved) == resolved, "realpath with caller buffer failed");
    api->end_capture();
    after_alloc = snapshot(api);

    check(strcmp(resolved, expected) == 0, "realpath caller buffer content mismatch");
    after_free = after_alloc;
    fill_result(&result, "realpath_buffer", &before, &after_alloc, &after_free, 0, 0);
    return result;
}

static case_result run_realpath_alloc_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char expected[PATH_MAX];
    char *resolved;
    uint64_t tracked;

    check(getcwd(expected, sizeof(expected)) == expected, "baseline getcwd failed");

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    resolved = realpath(".", NULL);
    api->end_capture();
    after_alloc = snapshot(api);

    check(resolved != NULL, "realpath(NULL) returned null");
    check(strcmp(resolved, expected) == 0, "realpath(NULL) content mismatch");
    tracked = api->is_tracked(resolved) ? 1u : 0u;

    api->begin_capture();
    free(resolved);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "realpath_alloc", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_tempnam_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    tempnam_fn_t tempnam_fn = lookup_tempnam();
    char *path;
    uint64_t tracked;

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    path = tempnam_fn(".", "ovr");
    api->end_capture();
    after_alloc = snapshot(api);

    check(path != NULL, "tempnam returned null");
    check(strncmp(path, "./ovr", 5) == 0, "tempnam prefix mismatch");
    tracked = api->is_tracked(path) ? 1u : 0u;

    api->begin_capture();
    free(path);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "tempnam", &before, &after_alloc, &after_free, 1, tracked);
    return result;
}

static case_result run_scandir_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char template[PATH_MAX];
    char file_a[PATH_MAX];
    char file_b[PATH_MAX];
    struct dirent **entries = NULL;
    int count;
    int i;
    uint64_t tracked = 0;

    ao_test_make_tmp_template(template, sizeof(template), "alloc-override-mock-only");
    check(mkdtemp(template) != NULL, "mkdtemp failed");
    ao_test_join_path(file_a, sizeof(file_a), template, "a.txt");
    ao_test_join_path(file_b, sizeof(file_b), template, "b.txt");
    create_empty_file(file_a);
    create_empty_file(file_b);

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    count = scandir(template, &entries, NULL, dirent_name_cmp);
    api->end_capture();
    after_alloc = snapshot(api);

    check(count >= 4, "scandir returned too few entries");
    check(entries != NULL, "scandir returned null entries");
    tracked += api->is_tracked(entries) ? 1u : 0u;
    for (i = 0; i < count; ++i) {
        tracked += api->is_tracked(entries[i]) ? 1u : 0u;
    }

    api->begin_capture();
    for (i = 0; i < count; ++i) {
        free(entries[i]);
    }
    free(entries);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "scandir", &before, &after_alloc, &after_free, (uint64_t)count + 1u, tracked);

    unlink(file_a);
    unlink(file_b);
    rmdir(template);
    return result;
}

static case_result run_scandir64_case(mock_api *api)
{
    ao_mock_stats before;
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    case_result result;
    char template[PATH_MAX];
    char file_a[PATH_MAX];
    char file_b[PATH_MAX];
    struct dirent64 **entries = NULL;
    int count;
    int i;
    uint64_t tracked = 0;

    ao_test_make_tmp_template(template, sizeof(template), "alloc-override-mock-only64");
    check(mkdtemp(template) != NULL, "mkdtemp failed");
    ao_test_join_path(file_a, sizeof(file_a), template, "a.txt");
    ao_test_join_path(file_b, sizeof(file_b), template, "b.txt");
    create_empty_file(file_a);
    create_empty_file(file_b);

    api->reset();
    before = snapshot(api);
    api->begin_capture();
    count = scandir64(template, &entries, NULL, dirent64_name_cmp);
    api->end_capture();
    after_alloc = snapshot(api);

    check(count >= 4, "scandir64 returned too few entries");
    check(entries != NULL, "scandir64 returned null entries");
    tracked += api->is_tracked(entries) ? 1u : 0u;
    for (i = 0; i < count; ++i) {
        tracked += api->is_tracked(entries[i]) ? 1u : 0u;
    }

    api->begin_capture();
    for (i = 0; i < count; ++i) {
        free(entries[i]);
    }
    free(entries);
    api->end_capture();
    after_free = snapshot(api);

    fill_result(&result, "scandir64", &before, &after_alloc, &after_free, (uint64_t)count + 1u, tracked);

    unlink(file_a);
    unlink(file_b);
    rmdir(template);
    return result;
}

int main(int argc, char **argv)
{
    mock_api api;
    run_mode mode = RUN_MODE_DIAGNOSTIC;
    FILE *report;
    case_result results[15];
    size_t i;

    check(argc == 4, "usage: phase1a_mock_only <mock_allocator_basename> <report_path> <diagnostic|strict_negative>");

    if (strcmp(argv[3], "strict_negative") == 0) {
        mode = RUN_MODE_STRICT_NEGATIVE;
    } else {
        check(strcmp(argv[3], "diagnostic") == 0, "unknown run mode");
    }

    api = load_mock_api();
    check_mock_preload(argv[1]);

    results[0] = run_strdup_case(&api);
    results[1] = run_strndup_case(&api);
    results[2] = run_wcsdup_case(&api);
    results[3] = run_asprintf_case(&api);
    results[4] = run_vasprintf_case(&api);
    results[5] = run_getdelim_case(&api);
    results[6] = run_getline_case(&api);
    results[7] = run_open_memstream_case(&api);
    results[8] = run_open_wmemstream_case(&api);
    results[9] = run_getcwd_buffer_case(&api);
    results[10] = run_getcwd_alloc_case(&api);
    results[11] = run_get_current_dir_name_case(&api);
    results[12] = run_realpath_buffer_case(&api);
    results[13] = run_realpath_alloc_case(&api);
    results[14] = run_tempnam_case(&api);

    report = fopen(argv[2], "w");
    check(report != NULL, "failed to open diagnostic report file");

    for (i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        write_result(report, &results[i]);
        if (mode == RUN_MODE_STRICT_NEGATIVE) {
            validate_strict_negative(&results[i]);
        }
    }

    results[0] = run_scandir_case(&api);
    results[1] = run_scandir64_case(&api);

    for (i = 0; i < 2; ++i) {
        write_result(report, &results[i]);
        if (mode == RUN_MODE_STRICT_NEGATIVE) {
            validate_strict_negative(&results[i]);
        }
    }

    fclose(report);
    return 0;
}
