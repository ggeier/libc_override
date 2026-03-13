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
    test_path_family();
    test_scandir_family();

    return 0;
}
