#ifndef ALLOC_OVERRIDE_TEST_SUPPORT_H
#define ALLOC_OVERRIDE_TEST_SUPPORT_H

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *ao_test_tmpdir(void)
{
    const char *value = getenv("ALLOC_OVERRIDE_TEST_TMPDIR");

    if (value && value[0] != '\0') {
        return value;
    }

    return "/tmp";
}

static void ao_test_make_tmp_template(char *buffer, size_t buffer_size, const char *stem)
{
    int written = snprintf(buffer, buffer_size, "%s/%s-XXXXXX", ao_test_tmpdir(), stem);

    if (written < 0 || (size_t)written >= buffer_size) {
        fprintf(stderr, "temporary path template too long\n");
        exit(1);
    }
}

static void ao_test_join_path(char *buffer, size_t buffer_size, const char *dir, const char *leaf)
{
    int written = snprintf(buffer, buffer_size, "%s/%s", dir, leaf);

    if (written < 0 || (size_t)written >= buffer_size) {
        fprintf(stderr, "joined path too long\n");
        exit(1);
    }
}

char *get_current_dir_name(void);

#endif
