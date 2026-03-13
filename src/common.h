#ifndef ALLOC_OVERRIDE_COMMON_H
#define ALLOC_OVERRIDE_COMMON_H

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

char *ao_strchrnul(const char *s, int c);
void ao_randname(char *template_buf);

#endif
