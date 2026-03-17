#define _GNU_SOURCE 1

#include <sched.h>
#include <stddef.h>
#include <stdlib.h>

static size_t ao_cpu_alloc_size(size_t count)
{
    return sizeof(unsigned long)
        * (count / (8U * sizeof(unsigned long))
            + (count % (8U * sizeof(unsigned long)) + 8U * sizeof(unsigned long) - 1U)
                / (8U * sizeof(unsigned long)));
}

cpu_set_t *__sched_cpualloc(size_t count)
{
    return (cpu_set_t *)malloc(ao_cpu_alloc_size(count));
}
