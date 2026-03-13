#include "common.h"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

char *ao_strchrnul(const char *s, int c)
{
    unsigned char target = (unsigned char)c;

    while (*s != '\0' && (unsigned char)*s != target) {
        ++s;
    }

    return (char *)s;
}

void ao_randname(char *template_buf)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    struct timespec ts;
    unsigned long value;
    long tid;
    int i;

    clock_gettime(CLOCK_REALTIME, &ts);
    tid = syscall(SYS_gettid);
    value = (unsigned long)ts.tv_sec ^ (unsigned long)ts.tv_nsec ^ ((unsigned long)tid * 65537UL);

    for (i = 0; i < 6; ++i) {
        template_buf[i] = alphabet[value % (sizeof(alphabet) - 1)];
        value = (value >> 5) ^ (value << 11) ^ (unsigned long)(i * 97 + 13);
    }
}
