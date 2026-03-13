#ifndef MOCK_ALLOCATOR_TEST_API_H
#define MOCK_ALLOCATOR_TEST_API_H

#include <stdint.h>

typedef struct ao_mock_stats {
    uint64_t malloc_calls;
    uint64_t calloc_calls;
    uint64_t realloc_calls;
    uint64_t free_calls;
    uint64_t successful_allocations;
    uint64_t successful_allocation_bytes;
    uint64_t failed_allocations;
    uint64_t bypass_realloc_calls;
    uint64_t bypass_free_calls;
    uint64_t live_blocks;
    uint64_t live_bytes;
} ao_mock_stats;

void ao_mock_reset(void);
void ao_mock_begin_capture(void);
void ao_mock_end_capture(void);
void ao_mock_get_stats(ao_mock_stats *out);
int ao_mock_is_tracked_allocation(const void *ptr);

#endif
