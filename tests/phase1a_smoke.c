#define _GNU_SOURCE 1
#define _LARGEFILE64_SOURCE 1

#include "test_support.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>

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

static int dirent_name_cmp(const struct dirent **lhs, const struct dirent **rhs)
{
    return strcmp((*lhs)->d_name, (*rhs)->d_name);
}

static int dirent64_name_cmp(const struct dirent64 **lhs, const struct dirent64 **rhs)
{
    return strcmp((*lhs)->d_name, (*rhs)->d_name);
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

typedef char *(*tempnam_fn_t)(const char *, const char *);
typedef char *(*get_current_dir_name_fn_t)(void);
typedef ssize_t (*getdelim_fn_t)(char **restrict, size_t *restrict, int, FILE *restrict);
typedef int (*sscanf_fn_t)(const char *restrict, const char *restrict, ...);
typedef int (*swscanf_fn_t)(const wchar_t *restrict, const wchar_t *restrict, ...);
typedef int (*vsscanf_fn_t)(const char *restrict, const char *restrict, va_list);
typedef int (*vswscanf_fn_t)(const wchar_t *restrict, const wchar_t *restrict, va_list);
typedef void *(*sched_cpualloc_fn_t)(size_t);

static tempnam_fn_t lookup_tempnam(void)
{
    tempnam_fn_t fn = NULL;
    void *sym = dlsym(RTLD_DEFAULT, "tempnam");

    memcpy(&fn, &sym, sizeof(fn));
    return fn;
}

static get_current_dir_name_fn_t lookup_get_current_dir_name(void)
{
    get_current_dir_name_fn_t fn = NULL;
    void *sym = dlsym(RTLD_DEFAULT, "get_current_dir_name");

    memcpy(&fn, &sym, sizeof(fn));
    return fn;
}

static getdelim_fn_t lookup_internal_getdelim(void)
{
    getdelim_fn_t fn = NULL;
    void *sym = dlsym(RTLD_DEFAULT, "__getdelim");

    memcpy(&fn, &sym, sizeof(fn));
    return fn;
}

static sched_cpualloc_fn_t lookup_sched_cpualloc(void)
{
    sched_cpualloc_fn_t fn = NULL;
    void *sym = dlsym(RTLD_DEFAULT, "__sched_cpualloc");

    memcpy(&fn, &sym, sizeof(fn));
    return fn;
}

static FILE *open_input_stream(const char *contents)
{
    FILE *file = tmpfile();
    size_t length = strlen(contents);

    check(file != NULL, "tmpfile failed");
    check(fwrite(contents, 1, length, file) == length, "failed to write input stream");
    check(fflush(file) == 0, "fflush failed");
    rewind(file);
    return file;
}

static int call_named_vsscanf(vsscanf_fn_t fn, const char *input, const char *format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = fn(input, format, ap);
    va_end(ap);
    return ret;
}

static int call_named_vswscanf(vswscanf_fn_t fn, const wchar_t *input, const wchar_t *format, ...)
{
    va_list ap;
    int ret;

    va_start(ap, format);
    ret = fn(input, format, ap);
    va_end(ap);
    return ret;
}

static void check_preload(const char *library_name)
{
    Dl_info info;
    void *sym;

    sym = dlsym(RTLD_DEFAULT, "strdup");
    check(sym != NULL, "dlsym(strdup) failed");
    check(dladdr(sym, &info) != 0, "dladdr(strdup) failed");
    check(info.dli_fname != NULL, "dladdr returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "strdup did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "scandir");
    check(sym != NULL, "dlsym(scandir) failed");
    check(dladdr(sym, &info) != 0, "dladdr(scandir) failed");
    check(info.dli_fname != NULL, "dladdr for scandir returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "scandir did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "tempnam");
    check(sym != NULL, "dlsym(tempnam) failed");
    check(dladdr(sym, &info) != 0, "dladdr(tempnam) failed");
    check(info.dli_fname != NULL, "dladdr for tempnam returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "tempnam did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "getdelim");
    check(sym != NULL, "dlsym(getdelim) failed");
    check(dladdr(sym, &info) != 0, "dladdr(getdelim) failed");
    check(info.dli_fname != NULL, "dladdr for getdelim returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "getdelim did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "getline");
    check(sym != NULL, "dlsym(getline) failed");
    check(dladdr(sym, &info) != 0, "dladdr(getline) failed");
    check(info.dli_fname != NULL, "dladdr for getline returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "getline did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "__getdelim");
    check(sym != NULL, "dlsym(__getdelim) failed");
    check(dladdr(sym, &info) != 0, "dladdr(__getdelim) failed");
    check(info.dli_fname != NULL, "dladdr for __getdelim returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "__getdelim did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "open_memstream");
    check(sym != NULL, "dlsym(open_memstream) failed");
    check(dladdr(sym, &info) != 0, "dladdr(open_memstream) failed");
    check(info.dli_fname != NULL, "dladdr for open_memstream returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "open_memstream did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "open_wmemstream");
    check(sym != NULL, "dlsym(open_wmemstream) failed");
    check(dladdr(sym, &info) != 0, "dladdr(open_wmemstream) failed");
    check(info.dli_fname != NULL, "dladdr for open_wmemstream returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "open_wmemstream did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "sscanf");
    check(sym != NULL, "dlsym(sscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(sscanf) failed");
    check(info.dli_fname != NULL, "dladdr for sscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "sscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "vsscanf");
    check(sym != NULL, "dlsym(vsscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(vsscanf) failed");
    check(info.dli_fname != NULL, "dladdr for vsscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "vsscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "swscanf");
    check(sym != NULL, "dlsym(swscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(swscanf) failed");
    check(info.dli_fname != NULL, "dladdr for swscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "swscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "vswscanf");
    check(sym != NULL, "dlsym(vswscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(vswscanf) failed");
    check(info.dli_fname != NULL, "dladdr for vswscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "vswscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "__isoc99_sscanf");
    check(sym != NULL, "dlsym(__isoc99_sscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(__isoc99_sscanf) failed");
    check(info.dli_fname != NULL, "dladdr for __isoc99_sscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "__isoc99_sscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "__isoc99_vsscanf");
    check(sym != NULL, "dlsym(__isoc99_vsscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(__isoc99_vsscanf) failed");
    check(info.dli_fname != NULL, "dladdr for __isoc99_vsscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "__isoc99_vsscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "__isoc99_swscanf");
    check(sym != NULL, "dlsym(__isoc99_swscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(__isoc99_swscanf) failed");
    check(info.dli_fname != NULL, "dladdr for __isoc99_swscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "__isoc99_swscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "__isoc99_vswscanf");
    check(sym != NULL, "dlsym(__isoc99_vswscanf) failed");
    check(dladdr(sym, &info) != 0, "dladdr(__isoc99_vswscanf) failed");
    check(info.dli_fname != NULL, "dladdr for __isoc99_vswscanf returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "__isoc99_vswscanf did not resolve to preload library");

    sym = dlsym(RTLD_DEFAULT, "__sched_cpualloc");
    check(sym != NULL, "dlsym(__sched_cpualloc) failed");
    check(dladdr(sym, &info) != 0, "dladdr(__sched_cpualloc) failed");
    check(info.dli_fname != NULL, "dladdr for __sched_cpualloc returned null path");
    check(strstr(info.dli_fname, library_name) != NULL, "__sched_cpualloc did not resolve to preload library");
}

static void test_strdup_family(void)
{
    char *dup = strdup("allocator override");
    char *ndup = strndup("allocator override", 9);
    wchar_t sample[] = L"wide override";
    wchar_t *wdup = wcsdup(sample);

    check(dup && strcmp(dup, "allocator override") == 0, "strdup failed");
    check(ndup && strcmp(ndup, "allocator") == 0, "strndup failed");
    check(wdup && wcscmp(wdup, sample) == 0, "wcsdup failed");

    free(dup);
    free(ndup);
    free(wdup);
}

static void test_printf_family(void)
{
    char *msg = NULL;
    char *vmsg = NULL;

    check(asprintf(&msg, "%s %d", "phase", 1) == 7, "asprintf length mismatch");
    check(msg && strcmp(msg, "phase 1") == 0, "asprintf content mismatch");

    check(call_vasprintf(&vmsg, "%s-%s", "override", "ok") == 11, "vasprintf length mismatch");
    check(vmsg && strcmp(vmsg, "override-ok") == 0, "vasprintf content mismatch");

    free(msg);
    free(vmsg);
}

static void test_line_reader_family(void)
{
    static const char getdelim_input[] = "alpha,beta";
    static const char getdelim_expected[] = "alpha,";
    static const char getline_expected[] = "growth line\n";
    getdelim_fn_t internal_getdelim = lookup_internal_getdelim();
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;

    check(internal_getdelim != NULL, "failed to resolve __getdelim");

    file = open_input_stream(getdelim_input);
    length = getdelim(&line, &capacity, ',', file);
    check(length == (ssize_t)strlen(getdelim_expected), "getdelim length mismatch");
    check(line != NULL && strcmp(line, getdelim_expected) == 0, "getdelim content mismatch");
    fclose(file);
    free(line);

    file = open_input_stream(getline_expected);
    line = malloc(2);
    capacity = 2;
    check(line != NULL, "initial getline buffer allocation failed");
    length = internal_getdelim(&line, &capacity, '\n', file);
    check(length == (ssize_t)strlen(getline_expected), "__getdelim length mismatch");
    check(strcmp(line, getline_expected) == 0, "__getdelim content mismatch");
    fclose(file);
    free(line);

    file = open_input_stream(getline_expected);
    line = malloc(2);
    capacity = 2;
    check(line != NULL, "initial getline buffer allocation failed");
    length = getline(&line, &capacity, file);
    check(length == (ssize_t)strlen(getline_expected), "getline length mismatch");
    check(strcmp(line, getline_expected) == 0, "getline content mismatch");
    fclose(file);
    free(line);
}

static void test_open_memstream(void)
{
    FILE *stream;
    char *buffer = NULL;
    size_t size = 0;
    int ret;

    stream = open_memstream(&buffer, &size);
    check(stream != NULL, "open_memstream failed");
    check(putc('a', stream) == 'a', "open_memstream putc a failed");
    check(putc('b', stream) == 'b', "open_memstream putc b failed");
    check(putc('c', stream) == 'c', "open_memstream putc c failed");
    check(fflush(stream) == 0, "open_memstream fflush failed");
    check(size == 3, "open_memstream size after abc mismatch");
    check(buffer != NULL && strcmp(buffer, "abc") == 0, "open_memstream abc content mismatch");
    check(fclose(stream) == 0, "open_memstream fclose failed");
    free(buffer);

    buffer = NULL;
    size = 0;
    stream = open_memstream(&buffer, &size);
    check(stream != NULL, "open_memstream second open failed");
    check(fseek(stream, 1, SEEK_CUR) == 0, "open_memstream forward seek failed");
    check(putc('q', stream) == 'q', "open_memstream putc q failed");
    check(fflush(stream) == 0, "open_memstream second fflush failed");
    check(buffer != NULL, "open_memstream did not publish buffer");
    check(memcmp(buffer, "\0q", 3) == 0, "open_memstream sparse write mismatch");

    errno = 0;
    ret = fseek(stream, -3, SEEK_CUR);
    check(ret == -1, "open_memstream invalid seek unexpectedly succeeded");
    check(errno == EINVAL, "open_memstream invalid seek errno mismatch");
    check(ftell(stream) == 2, "open_memstream ftell after invalid seek mismatch");

    check(fseek(stream, -2, SEEK_CUR) == 0, "open_memstream rewind seek failed");
    check(putc('e', stream) == 'e', "open_memstream overwrite failed");
    check(fflush(stream) == 0, "open_memstream overwrite fflush failed");
    check(strcmp(buffer, "eq") == 0, "open_memstream overwrite content mismatch");
    check(fclose(stream) == 0, "open_memstream second fclose failed");
    free(buffer);
}

static void test_open_wmemstream(void)
{
    static const wchar_t abc_expected[] = L"abc";
    static const wchar_t eq_expected[] = L"eq";
    static const wchar_t sparse_expected[] = {L'\0', L'q', L'\0'};
    FILE *stream;
    wchar_t *buffer = NULL;
    size_t size = 0;
    int ret;

    stream = open_wmemstream(&buffer, &size);
    check(stream != NULL, "open_wmemstream failed");
    check(fwide(stream, 0) > 0, "open_wmemstream is not wide-oriented");
    check(fputwc(L'a', stream) == L'a', "open_wmemstream putwc a failed");
    check(fputws(L"bc", stream) >= 0, "open_wmemstream fputws failed");
    check(fflush(stream) == 0, "open_wmemstream fflush failed");
    check(size == 3, "open_wmemstream size after abc mismatch");
    check(buffer != NULL && wcscmp(buffer, abc_expected) == 0, "open_wmemstream abc content mismatch");
    check(fclose(stream) == 0, "open_wmemstream fclose failed");
    free(buffer);

    buffer = NULL;
    size = 0;
    stream = open_wmemstream(&buffer, &size);
    check(stream != NULL, "open_wmemstream second open failed");
    check(fseek(stream, 1, SEEK_CUR) == 0, "open_wmemstream forward seek failed");
    check(fputwc(L'q', stream) == L'q', "open_wmemstream putwc q failed");
    check(fflush(stream) == 0, "open_wmemstream second fflush failed");
    check(buffer != NULL, "open_wmemstream did not publish buffer");
    check(wmemcmp(buffer, sparse_expected, 3) == 0, "open_wmemstream sparse write mismatch");

    errno = 0;
    ret = fseek(stream, -3, SEEK_CUR);
    check(ret == -1, "open_wmemstream invalid seek unexpectedly succeeded");
    check(errno == EINVAL, "open_wmemstream invalid seek errno mismatch");
    check(ftell(stream) == 2, "open_wmemstream ftell after invalid seek mismatch");

    check(fseek(stream, -2, SEEK_CUR) == 0, "open_wmemstream rewind seek failed");
    check(fputwc(L'e', stream) == L'e', "open_wmemstream overwrite failed");
    check(fflush(stream) == 0, "open_wmemstream overwrite fflush failed");
    check(wcscmp(buffer, eq_expected) == 0, "open_wmemstream overwrite content mismatch");
    check(fclose(stream) == 0, "open_wmemstream second fclose failed");
    free(buffer);
}

static void test_scanf_m_family(void)
{
    char *string_result = NULL;
    char *set_result = NULL;
    char *char_result = NULL;
    char *wide_input_string = NULL;
    wchar_t *wide_result = NULL;
    wchar_t *wide_vresult = NULL;

    check(call_named_vsscanf(vsscanf, "hello", "%ms", &string_result) == 1, "sscanf %ms failed");
    check(string_result != NULL && strcmp(string_result, "hello") == 0, "sscanf %ms content mismatch");
    free(string_result);

    check(call_named_vsscanf(vsscanf, "abc123", "%m[a-z]", &set_result) == 1, "vsscanf %m[] failed");
    check(set_result != NULL && strcmp(set_result, "abc") == 0, "vsscanf %m[] content mismatch");
    free(set_result);

    check(call_named_vsscanf(vsscanf, "XYZ", "%3mc", &char_result) == 1, "vsscanf %mc failed");
    check(char_result != NULL && memcmp(char_result, "XYZ", 3) == 0, "vsscanf %mc content mismatch");
    free(char_result);

    check(call_named_vsscanf(vsscanf, "wide", "%mls", &wide_result) == 1, "vsscanf %mls failed");
    check(wide_result != NULL && wcscmp(wide_result, L"wide") == 0, "vsscanf %mls content mismatch");
    free(wide_result);

    check(call_named_vswscanf(vswscanf, L"hello", L"%ms", &wide_input_string) == 1, "swscanf %ms failed");
    check(wide_input_string != NULL && strcmp(wide_input_string, "hello") == 0, "swscanf %ms content mismatch");
    free(wide_input_string);

    check(call_named_vswscanf(vswscanf, L"wide", L"%mls", &wide_result) == 1, "vswscanf %mls failed");
    check(wide_result != NULL && wcscmp(wide_result, L"wide") == 0, "vswscanf %mls content mismatch");
    free(wide_result);

    check(call_named_vswscanf(vswscanf, L"ascii", L"%ms", &wide_input_string) == 1, "vswscanf %ms failed");
    check(wide_input_string != NULL && strcmp(wide_input_string, "ascii") == 0, "vswscanf %ms content mismatch");
    free(wide_input_string);

    check(call_named_vswscanf(vswscanf, L"hybrid", L"%mls", &wide_vresult) == 1, "vswscanf %mls second case failed");
    check(wide_vresult != NULL && wcscmp(wide_vresult, L"hybrid") == 0, "vswscanf %mls second content mismatch");
    free(wide_vresult);
}

static void test_sched_cpualloc(void)
{
    sched_cpualloc_fn_t sched_cpualloc_fn = lookup_sched_cpualloc();
    void *set;

    check(sched_cpualloc_fn != NULL, "failed to resolve __sched_cpualloc");
    set = sched_cpualloc_fn(130);
    check(set != NULL, "__sched_cpualloc returned null");
    free(set);
}

static void test_path_family(void)
{
    char cwd_buf[PATH_MAX];
    char real_buf[PATH_MAX];
    char *cwd_alloc;
    char *cwd_current;
    char *resolved;
    char *temp;
    tempnam_fn_t tempnam_fn = lookup_tempnam();
    get_current_dir_name_fn_t get_current_dir_name_fn = lookup_get_current_dir_name();

    check(getcwd(cwd_buf, sizeof(cwd_buf)) == cwd_buf, "getcwd with caller buffer failed");

    cwd_alloc = getcwd(NULL, 0);
    check(cwd_alloc && strcmp(cwd_alloc, cwd_buf) == 0, "getcwd(NULL, 0) failed");

    check(get_current_dir_name_fn != NULL, "failed to resolve get_current_dir_name");
    cwd_current = get_current_dir_name_fn();
    check(cwd_current && strcmp(cwd_current, cwd_buf) == 0, "get_current_dir_name failed");

    resolved = realpath(".", NULL);
    check(resolved && strcmp(resolved, cwd_buf) == 0, "realpath(NULL) failed");

    check(realpath(".", real_buf) == real_buf, "realpath with caller buffer failed");
    check(strcmp(real_buf, cwd_buf) == 0, "realpath caller buffer mismatch");

    check(tempnam_fn != NULL, "failed to resolve tempnam");
    temp = tempnam_fn(".", "ovr");
    check(temp != NULL, "tempnam failed");
    check(strncmp(temp, "./ovr_", 6) == 0, "tempnam prefix mismatch");

    free(cwd_alloc);
    free(cwd_current);
    free(resolved);
    free(temp);
}

static void test_scandir_family(void)
{
    char template[PATH_MAX];
    char file_a[PATH_MAX];
    char file_b[PATH_MAX];
    struct dirent **entries = NULL;
    struct dirent64 **entries64 = NULL;
    int count;
    int count64;
    int i;

    ao_test_make_tmp_template(template, sizeof(template), "alloc-override");
    check(mkdtemp(template) != NULL, "mkdtemp failed");

    ao_test_join_path(file_a, sizeof(file_a), template, "a.txt");
    ao_test_join_path(file_b, sizeof(file_b), template, "b.txt");

    {
        FILE *fp = fopen(file_a, "w");
        check(fp != NULL, "fopen for a.txt failed");
        fclose(fp);
    }
    {
        FILE *fp = fopen(file_b, "w");
        check(fp != NULL, "fopen for b.txt failed");
        fclose(fp);
    }

    count = scandir(template, &entries, NULL, dirent_name_cmp);
    check(count >= 4, "scandir returned too few entries");
    for (i = 0; i < count; ++i) {
        free(entries[i]);
    }
    free(entries);

    count64 = scandir64(template, &entries64, NULL, dirent64_name_cmp);
    check(count64 >= 4, "scandir64 returned too few entries");
    for (i = 0; i < count64; ++i) {
        free(entries64[i]);
    }
    free(entries64);

    unlink(file_a);
    unlink(file_b);
    rmdir(template);
}

int main(int argc, char **argv)
{
    check(argc == 2, "expected preload library basename argument");

    check_preload(argv[1]);
    test_strdup_family();
    test_printf_family();
    test_line_reader_family();
    test_open_memstream();
    test_open_wmemstream();
    test_scanf_m_family();
    test_sched_cpualloc();
    test_path_family();
    test_scandir_family();

    return 0;
}
