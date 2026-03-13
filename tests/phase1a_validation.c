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

typedef struct mock_api {
    ao_mock_reset_fn reset;
    ao_mock_capture_fn begin_capture;
    ao_mock_capture_fn end_capture;
    ao_mock_get_stats_fn get_stats;
    ao_mock_is_tracked_fn is_tracked;
} mock_api;

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

static get_current_dir_name_fn_t lookup_get_current_dir_name(void)
{
    get_current_dir_name_fn_t fn = NULL;
    void *symbol = lookup_symbol("get_current_dir_name");

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

static void test_strdup(mock_api *api)
{
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    char *dup;

    api->begin_capture();
    dup = strdup("allocator validation");
    api->end_capture();
    after_alloc = snapshot(api);

    check(dup != NULL, "strdup returned null");
    check(strcmp(dup, "allocator validation") == 0, "strdup content mismatch");
    check(api->is_tracked(dup), "strdup result was not tracked by mock allocator");
    check(alloc_call_count(&after_alloc) > alloc_call_count(&before), "strdup did not trigger any allocation calls");
    check(after_alloc.successful_allocations > before.successful_allocations, "strdup allocation was not recorded");
    check(after_alloc.live_blocks == before.live_blocks + 1, "strdup live block count mismatch after allocation");

    api->begin_capture();
    free(dup);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + 1, "strdup free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "strdup live block count mismatch after free");
}

static void test_strndup(mock_api *api)
{
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    char *dup;

    api->begin_capture();
    dup = strndup("allocator validation", 9);
    api->end_capture();
    after_alloc = snapshot(api);

    check(dup != NULL, "strndup returned null");
    check(strcmp(dup, "allocator") == 0, "strndup content mismatch");
    check(api->is_tracked(dup), "strndup result was not tracked by mock allocator");
    check(after_alloc.live_blocks == before.live_blocks + 1, "strndup live block count mismatch after allocation");

    api->begin_capture();
    free(dup);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + 1, "strndup free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "strndup live block count mismatch after free");
}

static void test_wcsdup(mock_api *api)
{
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    wchar_t sample[] = L"wide validation";
    wchar_t *dup;

    api->begin_capture();
    dup = wcsdup(sample);
    api->end_capture();
    after_alloc = snapshot(api);

    check(dup != NULL, "wcsdup returned null");
    check(wcscmp(dup, sample) == 0, "wcsdup content mismatch");
    check(api->is_tracked(dup), "wcsdup result was not tracked by mock allocator");
    check(after_alloc.live_blocks == before.live_blocks + 1, "wcsdup live block count mismatch after allocation");

    api->begin_capture();
    free(dup);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + 1, "wcsdup free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "wcsdup live block count mismatch after free");
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

static void test_asprintf(mock_api *api)
{
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    char *msg = NULL;

    api->begin_capture();
    check(asprintf(&msg, "%s %d", "phase", 1) == 7, "asprintf length mismatch");
    api->end_capture();
    after_alloc = snapshot(api);

    check(msg != NULL, "asprintf returned null string");
    check(strcmp(msg, "phase 1") == 0, "asprintf content mismatch");
    check(api->is_tracked(msg), "asprintf result was not tracked by mock allocator");
    check(after_alloc.live_blocks == before.live_blocks + 1, "asprintf live block count mismatch after allocation");

    api->begin_capture();
    free(msg);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + 1, "asprintf free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "asprintf live block count mismatch after free");
}

static void test_vasprintf(mock_api *api)
{
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    char *msg = NULL;

    api->begin_capture();
    check(call_vasprintf(&msg, "%s-%s", "override", "ok") == 11, "vasprintf length mismatch");
    api->end_capture();
    after_alloc = snapshot(api);

    check(msg != NULL, "vasprintf returned null string");
    check(strcmp(msg, "override-ok") == 0, "vasprintf content mismatch");
    check(api->is_tracked(msg), "vasprintf result was not tracked by mock allocator");
    check(after_alloc.live_blocks == before.live_blocks + 1, "vasprintf live block count mismatch after allocation");

    api->begin_capture();
    free(msg);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + 1, "vasprintf free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "vasprintf live block count mismatch after free");
}

static void test_getcwd_allocates_only_when_needed(mock_api *api)
{
    char buffer[PATH_MAX];
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_stack_call;
    ao_mock_stats after_alloc_call;
    ao_mock_stats after_free;
    char *dynamic_result;

    api->begin_capture();
    check(getcwd(buffer, sizeof(buffer)) == buffer, "getcwd with caller buffer failed");
    api->end_capture();
    after_stack_call = snapshot(api);

    check(alloc_call_count(&after_stack_call) == alloc_call_count(&before), "getcwd with caller buffer unexpectedly allocated");

    api->begin_capture();
    dynamic_result = getcwd(NULL, 0);
    api->end_capture();
    after_alloc_call = snapshot(api);

    check(dynamic_result != NULL, "getcwd(NULL, 0) returned null");
    check(strcmp(dynamic_result, buffer) == 0, "getcwd(NULL, 0) content mismatch");
    check(api->is_tracked(dynamic_result), "getcwd(NULL, 0) result was not tracked by mock allocator");
    check(after_alloc_call.live_blocks == before.live_blocks + 1, "getcwd(NULL, 0) live block count mismatch after allocation");

    api->begin_capture();
    free(dynamic_result);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc_call.free_calls + 1, "getcwd(NULL, 0) free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "getcwd(NULL, 0) live block count mismatch after free");
}

static void test_get_current_dir_name(mock_api *api)
{
    char cwd_buffer[PATH_MAX];
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    char *cwd;
    get_current_dir_name_fn_t get_current_dir_name_fn = lookup_get_current_dir_name();

    check(getcwd(cwd_buffer, sizeof(cwd_buffer)) == cwd_buffer, "baseline getcwd failed");

    api->begin_capture();
    cwd = get_current_dir_name_fn();
    api->end_capture();
    after_alloc = snapshot(api);

    check(cwd != NULL, "get_current_dir_name returned null");
    check(strcmp(cwd, cwd_buffer) == 0, "get_current_dir_name content mismatch");
    check(api->is_tracked(cwd), "get_current_dir_name result was not tracked by mock allocator");
    check(after_alloc.live_blocks == before.live_blocks + 1, "get_current_dir_name live block count mismatch after allocation");

    api->begin_capture();
    free(cwd);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + 1, "get_current_dir_name free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "get_current_dir_name live block count mismatch after free");
}

static void test_realpath(mock_api *api)
{
    char expected[PATH_MAX];
    char resolved_buffer[PATH_MAX];
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_buffer_call;
    ao_mock_stats after_alloc_call;
    ao_mock_stats after_free;
    char *resolved;

    check(getcwd(expected, sizeof(expected)) == expected, "baseline getcwd failed");

    api->begin_capture();
    check(realpath(".", resolved_buffer) == resolved_buffer, "realpath with caller buffer failed");
    api->end_capture();
    after_buffer_call = snapshot(api);

    check(strcmp(resolved_buffer, expected) == 0, "realpath caller buffer content mismatch");
    check(alloc_call_count(&after_buffer_call) == alloc_call_count(&before), "realpath with caller buffer unexpectedly allocated");

    api->begin_capture();
    resolved = realpath(".", NULL);
    api->end_capture();
    after_alloc_call = snapshot(api);

    check(resolved != NULL, "realpath(NULL) returned null");
    check(strcmp(resolved, expected) == 0, "realpath(NULL) content mismatch");
    check(api->is_tracked(resolved), "realpath(NULL) result was not tracked by mock allocator");
    check(after_alloc_call.live_blocks == before.live_blocks + 1, "realpath(NULL) live block count mismatch after allocation");

    api->begin_capture();
    free(resolved);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc_call.free_calls + 1, "realpath(NULL) free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "realpath(NULL) live block count mismatch after free");
}

static void test_tempnam(mock_api *api)
{
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    tempnam_fn_t tempnam_fn = lookup_tempnam();
    char *path;

    api->begin_capture();
    path = tempnam_fn(".", "ovr");
    api->end_capture();
    after_alloc = snapshot(api);

    check(path != NULL, "tempnam returned null");
    check(strncmp(path, "./ovr_", 6) == 0, "tempnam prefix mismatch");
    check(api->is_tracked(path), "tempnam result was not tracked by mock allocator");
    check(after_alloc.live_blocks == before.live_blocks + 1, "tempnam live block count mismatch after allocation");

    api->begin_capture();
    free(path);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + 1, "tempnam free was not recorded");
    check(after_free.live_blocks == before.live_blocks, "tempnam live block count mismatch after free");
}

static int dirent_name_cmp(const struct dirent **lhs, const struct dirent **rhs)
{
    return strcmp((*lhs)->d_name, (*rhs)->d_name);
}

static int dirent64_name_cmp(const struct dirent64 **lhs, const struct dirent64 **rhs)
{
    return strcmp((*lhs)->d_name, (*rhs)->d_name);
}

static void create_empty_file(const char *path)
{
    FILE *file = fopen(path, "w");

    check(file != NULL, "failed to create test file");
    fclose(file);
}

static void test_scandir(mock_api *api)
{
    char template[PATH_MAX];
    char file_a[PATH_MAX];
    char file_b[PATH_MAX];
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    struct dirent **entries = NULL;
    int count;
    int i;

    ao_test_make_tmp_template(template, sizeof(template), "alloc-override-validate");
    check(mkdtemp(template) != NULL, "mkdtemp failed");
    ao_test_join_path(file_a, sizeof(file_a), template, "a.txt");
    ao_test_join_path(file_b, sizeof(file_b), template, "b.txt");
    create_empty_file(file_a);
    create_empty_file(file_b);

    api->begin_capture();
    count = scandir(template, &entries, NULL, dirent_name_cmp);
    api->end_capture();
    after_alloc = snapshot(api);

    check(count >= 4, "scandir returned too few entries");
    check(entries != NULL, "scandir returned null entries");
    check(api->is_tracked(entries), "scandir vector was not tracked by mock allocator");
    for (i = 0; i < count; ++i) {
        check(api->is_tracked(entries[i]), "scandir entry was not tracked by mock allocator");
    }
    check(after_alloc.live_blocks >= before.live_blocks + (uint64_t)count + 1, "scandir live block count mismatch after allocation");

    api->begin_capture();
    for (i = 0; i < count; ++i) {
        free(entries[i]);
    }
    free(entries);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + (uint64_t)count + 1, "scandir cleanup free count mismatch");
    check(after_free.live_blocks == before.live_blocks, "scandir live block count mismatch after cleanup");

    unlink(file_a);
    unlink(file_b);
    rmdir(template);
}

static void test_scandir64(mock_api *api)
{
    char template[PATH_MAX];
    char file_a[PATH_MAX];
    char file_b[PATH_MAX];
    const ao_mock_stats before = snapshot(api);
    ao_mock_stats after_alloc;
    ao_mock_stats after_free;
    struct dirent64 **entries = NULL;
    int count;
    int i;

    ao_test_make_tmp_template(template, sizeof(template), "alloc-override-validate64");
    check(mkdtemp(template) != NULL, "mkdtemp failed");
    ao_test_join_path(file_a, sizeof(file_a), template, "a.txt");
    ao_test_join_path(file_b, sizeof(file_b), template, "b.txt");
    create_empty_file(file_a);
    create_empty_file(file_b);

    api->begin_capture();
    count = scandir64(template, &entries, NULL, dirent64_name_cmp);
    api->end_capture();
    after_alloc = snapshot(api);

    check(count >= 4, "scandir64 returned too few entries");
    check(entries != NULL, "scandir64 returned null entries");
    check(api->is_tracked(entries), "scandir64 vector was not tracked by mock allocator");
    for (i = 0; i < count; ++i) {
        check(api->is_tracked(entries[i]), "scandir64 entry was not tracked by mock allocator");
    }
    check(after_alloc.live_blocks >= before.live_blocks + (uint64_t)count + 1, "scandir64 live block count mismatch after allocation");

    api->begin_capture();
    for (i = 0; i < count; ++i) {
        free(entries[i]);
    }
    free(entries);
    api->end_capture();
    after_free = snapshot(api);

    check(after_free.free_calls == after_alloc.free_calls + (uint64_t)count + 1, "scandir64 cleanup free count mismatch");
    check(after_free.live_blocks == before.live_blocks, "scandir64 live block count mismatch after cleanup");

    unlink(file_a);
    unlink(file_b);
    rmdir(template);
}

int main(int argc, char **argv)
{
    mock_api api;

    check(argc == 2, "expected mock allocator basename argument");

    api = load_mock_api();
    check_mock_preload(argv[1]);
    api.reset();

    test_strdup(&api);
    test_strndup(&api);
    test_wcsdup(&api);
    test_asprintf(&api);
    test_vasprintf(&api);
    test_getcwd_allocates_only_when_needed(&api);
    test_get_current_dir_name(&api);
    test_realpath(&api);
    test_tempnam(&api);
    test_scandir(&api);
    test_scandir64(&api);

    return 0;
}
