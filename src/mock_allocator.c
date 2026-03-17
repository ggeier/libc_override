#define _GNU_SOURCE 1

#include "../include/allocator_bridge.h"
#include "../include/mock_allocator_test_api.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AO_REGISTRY_CAPACITY 65536u
#define AO_REGISTRY_EMPTY ((uintptr_t)0)
#define AO_REGISTRY_TOMBSTONE ((uintptr_t)1)
#define AO_BOOTSTRAP_HEAP_SIZE (64u * 1024u)

typedef void *(*ao_malloc_fn)(size_t);
typedef void *(*ao_calloc_fn)(size_t, size_t);
typedef void *(*ao_realloc_fn)(void *, size_t);
typedef void (*ao_free_fn)(void *);

typedef struct ao_registry_entry {
    uintptr_t key;
    size_t requested_size;
    uint64_t allocation_id;
} ao_registry_entry;

typedef struct ao_atomic_stats {
    atomic_ullong malloc_calls;
    atomic_ullong calloc_calls;
    atomic_ullong realloc_calls;
    atomic_ullong free_calls;
    atomic_ullong successful_allocations;
    atomic_ullong successful_allocation_bytes;
    atomic_ullong failed_allocations;
    atomic_ullong bypass_realloc_calls;
    atomic_ullong bypass_free_calls;
    atomic_ullong live_blocks;
    atomic_ullong live_bytes;
} ao_atomic_stats;

typedef struct ao_bootstrap_header {
    size_t requested_size;
    max_align_t align;
} ao_bootstrap_header;

static pthread_once_t g_resolve_once = PTHREAD_ONCE_INIT;
static _Thread_local int g_in_resolver = 0;

static ao_malloc_fn g_real_malloc = NULL;
static ao_calloc_fn g_real_calloc = NULL;
static ao_realloc_fn g_real_realloc = NULL;
static ao_free_fn g_real_free = NULL;

static ao_registry_entry g_registry[AO_REGISTRY_CAPACITY];
static atomic_flag g_registry_lock = ATOMIC_FLAG_INIT;
static ao_atomic_stats g_stats;
static atomic_int g_capture_depth = 0;
static atomic_ullong g_next_allocation_id = 1;
static unsigned char g_bootstrap_heap[AO_BOOTSTRAP_HEAP_SIZE];
static atomic_size_t g_bootstrap_offset = 0;

static void ao_write_literal(const char *message)
{
    size_t len = 0;

    while (message[len] != '\0') {
        ++len;
    }

    while (len > 0) {
        ssize_t written = write(STDERR_FILENO, message, len);
        if (written <= 0) {
            break;
        }
        message += written;
        len -= (size_t)written;
    }
}

static void ao_fatal(const char *message)
{
    ao_write_literal(message);
    _exit(127);
}

static size_t ao_align_up(size_t value, size_t alignment)
{
    size_t remainder = value % alignment;

    if (remainder == 0) {
        return value;
    }

    return value + (alignment - remainder);
}

static void *ao_bootstrap_alloc(size_t size)
{
    size_t total = ao_align_up(sizeof(ao_bootstrap_header) + size, sizeof(max_align_t));
    size_t current;
    size_t next;
    ao_bootstrap_header *header;

    do {
        current = atomic_load_explicit(&g_bootstrap_offset, memory_order_relaxed);
        if (current > AO_BOOTSTRAP_HEAP_SIZE || total > AO_BOOTSTRAP_HEAP_SIZE - current) {
            errno = ENOMEM;
            return NULL;
        }
        next = current + total;
    } while (!atomic_compare_exchange_weak_explicit(
        &g_bootstrap_offset,
        &current,
        next,
        memory_order_relaxed,
        memory_order_relaxed));

    header = (ao_bootstrap_header *)(void *)(g_bootstrap_heap + current);
    header->requested_size = size;
    return (void *)(header + 1);
}

static int ao_bootstrap_contains(const void *ptr)
{
    const unsigned char *byte_ptr = (const unsigned char *)ptr;

    return byte_ptr >= g_bootstrap_heap && byte_ptr < g_bootstrap_heap + AO_BOOTSTRAP_HEAP_SIZE;
}

static void *ao_bootstrap_realloc(void *ptr, size_t size)
{
    void *new_ptr;
    ao_bootstrap_header *header;
    size_t copy_size;

    if (!ptr) {
        return ao_bootstrap_alloc(size);
    }

    if (!ao_bootstrap_contains(ptr)) {
        errno = ENOMEM;
        return NULL;
    }

    header = ((ao_bootstrap_header *)ptr) - 1;
    new_ptr = ao_bootstrap_alloc(size);
    if (!new_ptr) {
        return NULL;
    }

    copy_size = header->requested_size < size ? header->requested_size : size;
    if (copy_size != 0) {
        memcpy(new_ptr, ptr, copy_size);
    }

    return new_ptr;
}

static void ao_registry_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_registry_lock, memory_order_acquire)) {
    }
}

static void ao_registry_unlock(void)
{
    atomic_flag_clear_explicit(&g_registry_lock, memory_order_release);
}

static size_t ao_registry_index_for(uintptr_t key)
{
    return (size_t)((key >> 4u) & (AO_REGISTRY_CAPACITY - 1u));
}

static int ao_registry_insert_locked(void *ptr, size_t requested_size, uint64_t allocation_id)
{
    uintptr_t key = (uintptr_t)ptr;
    size_t index = ao_registry_index_for(key);
    size_t first_tombstone = AO_REGISTRY_CAPACITY;
    size_t i;

    for (i = 0; i < AO_REGISTRY_CAPACITY; ++i) {
        ao_registry_entry *entry = &g_registry[(index + i) & (AO_REGISTRY_CAPACITY - 1u)];

        if (entry->key == key) {
            entry->requested_size = requested_size;
            entry->allocation_id = allocation_id;
            return 1;
        }

        if (entry->key == AO_REGISTRY_TOMBSTONE && first_tombstone == AO_REGISTRY_CAPACITY) {
            first_tombstone = (index + i) & (AO_REGISTRY_CAPACITY - 1u);
            continue;
        }

        if (entry->key == AO_REGISTRY_EMPTY) {
            size_t target = first_tombstone == AO_REGISTRY_CAPACITY
                ? ((index + i) & (AO_REGISTRY_CAPACITY - 1u))
                : first_tombstone;
            g_registry[target].key = key;
            g_registry[target].requested_size = requested_size;
            g_registry[target].allocation_id = allocation_id;
            return 1;
        }
    }

    if (first_tombstone != AO_REGISTRY_CAPACITY) {
        g_registry[first_tombstone].key = key;
        g_registry[first_tombstone].requested_size = requested_size;
        g_registry[first_tombstone].allocation_id = allocation_id;
        return 1;
    }

    return 0;
}

static int ao_registry_lookup_locked(const void *ptr, size_t *requested_size, uint64_t *allocation_id)
{
    uintptr_t key = (uintptr_t)ptr;
    size_t index = ao_registry_index_for(key);
    size_t i;

    for (i = 0; i < AO_REGISTRY_CAPACITY; ++i) {
        ao_registry_entry *entry = &g_registry[(index + i) & (AO_REGISTRY_CAPACITY - 1u)];

        if (entry->key == AO_REGISTRY_EMPTY) {
            return 0;
        }

        if (entry->key == key) {
            if (requested_size) {
                *requested_size = entry->requested_size;
            }
            if (allocation_id) {
                *allocation_id = entry->allocation_id;
            }
            return 1;
        }
    }

    return 0;
}

static int ao_registry_remove_locked(const void *ptr, size_t *requested_size, uint64_t *allocation_id)
{
    uintptr_t key = (uintptr_t)ptr;
    size_t index = ao_registry_index_for(key);
    size_t i;

    for (i = 0; i < AO_REGISTRY_CAPACITY; ++i) {
        ao_registry_entry *entry = &g_registry[(index + i) & (AO_REGISTRY_CAPACITY - 1u)];

        if (entry->key == AO_REGISTRY_EMPTY) {
            return 0;
        }

        if (entry->key == key) {
            if (requested_size) {
                *requested_size = entry->requested_size;
            }
            if (allocation_id) {
                *allocation_id = entry->allocation_id;
            }
            entry->key = AO_REGISTRY_TOMBSTONE;
            entry->requested_size = 0;
            entry->allocation_id = 0;
            return 1;
        }
    }

    return 0;
}

static int ao_registry_contains(const void *ptr)
{
    int found;

    ao_registry_lock();
    found = ao_registry_lookup_locked(ptr, NULL, NULL);
    ao_registry_unlock();

    return found;
}

static void ao_resolve_real_functions(void)
{
    void *symbol;

    g_in_resolver = 1;

    symbol = dlsym(RTLD_NEXT, "malloc");
    memcpy(&g_real_malloc, &symbol, sizeof(g_real_malloc));

    symbol = dlsym(RTLD_NEXT, "calloc");
    memcpy(&g_real_calloc, &symbol, sizeof(g_real_calloc));

    symbol = dlsym(RTLD_NEXT, "realloc");
    memcpy(&g_real_realloc, &symbol, sizeof(g_real_realloc));

    symbol = dlsym(RTLD_NEXT, "free");
    memcpy(&g_real_free, &symbol, sizeof(g_real_free));

    g_in_resolver = 0;

    if (!g_real_malloc || !g_real_calloc || !g_real_realloc || !g_real_free) {
        ao_fatal("mock_allocator: failed to resolve real allocation symbols\n");
    }
}

static void ao_ensure_real_functions(void)
{
    pthread_once(&g_resolve_once, ao_resolve_real_functions);
}

static int ao_capture_enabled(void)
{
    return atomic_load_explicit(&g_capture_depth, memory_order_relaxed) > 0;
}

static void ao_record_failed_allocation(void)
{
    if (ao_capture_enabled()) {
        atomic_fetch_add_explicit(&g_stats.failed_allocations, 1, memory_order_relaxed);
    }
}

static void ao_record_successful_allocation(int kind, size_t requested_size)
{
    if (!ao_capture_enabled()) {
        return;
    }

    switch (kind) {
    case 0:
        atomic_fetch_add_explicit(&g_stats.malloc_calls, 1, memory_order_relaxed);
        break;
    case 1:
        atomic_fetch_add_explicit(&g_stats.calloc_calls, 1, memory_order_relaxed);
        break;
    case 2:
        atomic_fetch_add_explicit(&g_stats.realloc_calls, 1, memory_order_relaxed);
        break;
    default:
        break;
    }

    atomic_fetch_add_explicit(&g_stats.successful_allocations, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stats.successful_allocation_bytes, (unsigned long long)requested_size, memory_order_relaxed);
}

static void ao_record_free(int bypass)
{
    if (!ao_capture_enabled()) {
        return;
    }

    atomic_fetch_add_explicit(&g_stats.free_calls, 1, memory_order_relaxed);
    if (bypass) {
        atomic_fetch_add_explicit(&g_stats.bypass_free_calls, 1, memory_order_relaxed);
    }
}

static void ao_record_realloc(int bypass)
{
    if (!ao_capture_enabled()) {
        return;
    }

    atomic_fetch_add_explicit(&g_stats.realloc_calls, 1, memory_order_relaxed);
    if (bypass) {
        atomic_fetch_add_explicit(&g_stats.bypass_realloc_calls, 1, memory_order_relaxed);
    }
}

static void ao_live_add(size_t requested_size)
{
    atomic_fetch_add_explicit(&g_stats.live_blocks, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_stats.live_bytes, (unsigned long long)requested_size, memory_order_relaxed);
}

static void ao_live_update(size_t old_size, size_t new_size)
{
    if (new_size > old_size) {
        atomic_fetch_add_explicit(&g_stats.live_bytes, (unsigned long long)(new_size - old_size), memory_order_relaxed);
    } else if (old_size > new_size) {
        atomic_fetch_sub_explicit(&g_stats.live_bytes, (unsigned long long)(old_size - new_size), memory_order_relaxed);
    }
}

static void ao_live_remove(size_t requested_size)
{
    atomic_fetch_sub_explicit(&g_stats.live_blocks, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&g_stats.live_bytes, (unsigned long long)requested_size, memory_order_relaxed);
}

static uint64_t ao_next_allocation_id(void)
{
    return atomic_fetch_add_explicit(&g_next_allocation_id, 1, memory_order_relaxed);
}

static void ao_track_allocation_or_die(void *ptr, size_t requested_size)
{
    int inserted;

    ao_registry_lock();
    inserted = ao_registry_insert_locked(ptr, requested_size, ao_next_allocation_id());
    ao_registry_unlock();

    if (!inserted) {
        ao_fatal("mock_allocator: allocation registry exhausted\n");
    }

    ao_live_add(requested_size);
}

void ao_mock_reset(void)
{
    atomic_store_explicit(&g_stats.malloc_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.calloc_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.realloc_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.free_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.successful_allocations, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.successful_allocation_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.failed_allocations, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.bypass_realloc_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.bypass_free_calls, 0, memory_order_relaxed);
}

void ao_mock_begin_capture(void)
{
    atomic_fetch_add_explicit(&g_capture_depth, 1, memory_order_relaxed);
}

void ao_mock_end_capture(void)
{
    int current = atomic_load_explicit(&g_capture_depth, memory_order_relaxed);

    if (current > 0) {
        atomic_fetch_sub_explicit(&g_capture_depth, 1, memory_order_relaxed);
    }
}

void ao_mock_get_stats(ao_mock_stats *out)
{
    if (!out) {
        return;
    }

    out->malloc_calls = atomic_load_explicit(&g_stats.malloc_calls, memory_order_relaxed);
    out->calloc_calls = atomic_load_explicit(&g_stats.calloc_calls, memory_order_relaxed);
    out->realloc_calls = atomic_load_explicit(&g_stats.realloc_calls, memory_order_relaxed);
    out->free_calls = atomic_load_explicit(&g_stats.free_calls, memory_order_relaxed);
    out->successful_allocations = atomic_load_explicit(&g_stats.successful_allocations, memory_order_relaxed);
    out->successful_allocation_bytes = atomic_load_explicit(&g_stats.successful_allocation_bytes, memory_order_relaxed);
    out->failed_allocations = atomic_load_explicit(&g_stats.failed_allocations, memory_order_relaxed);
    out->bypass_realloc_calls = atomic_load_explicit(&g_stats.bypass_realloc_calls, memory_order_relaxed);
    out->bypass_free_calls = atomic_load_explicit(&g_stats.bypass_free_calls, memory_order_relaxed);
    out->live_blocks = atomic_load_explicit(&g_stats.live_blocks, memory_order_relaxed);
    out->live_bytes = atomic_load_explicit(&g_stats.live_bytes, memory_order_relaxed);
}

int ao_mock_is_tracked_allocation(const void *ptr)
{
    if (!ptr) {
        return 0;
    }

    return ao_registry_contains(ptr);
}

int ao_alloc_bridge_owns(const void *ptr)
{
    return ao_mock_is_tracked_allocation(ptr);
}

void ao_alloc_bridge_libc_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    ao_ensure_real_functions();
    g_real_free(ptr);
}

void *ao_alloc_bridge_libc_realloc(void *ptr, size_t size)
{
    ao_ensure_real_functions();
    return g_real_realloc(ptr, size);
}

void *malloc(size_t size)
{
    void *ptr;

    if (g_in_resolver) {
        return ao_bootstrap_alloc(size);
    }

    ao_ensure_real_functions();
    ptr = g_real_malloc(size);
    if (!ptr) {
        ao_record_failed_allocation();
        return NULL;
    }

    ao_track_allocation_or_die(ptr, size);
    ao_record_successful_allocation(0, size);
    return ptr;
}

void *calloc(size_t nmemb, size_t size)
{
    void *ptr;
    size_t requested_size;

    if (g_in_resolver) {
        requested_size = nmemb * size;
        ptr = ao_bootstrap_alloc(requested_size);
        if (ptr) {
            memset(ptr, 0, requested_size);
        }
        return ptr;
    }

    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        errno = ENOMEM;
        ao_record_failed_allocation();
        return NULL;
    }

    requested_size = nmemb * size;
    ao_ensure_real_functions();
    ptr = g_real_calloc(nmemb, size);
    if (!ptr) {
        ao_record_failed_allocation();
        return NULL;
    }

    ao_track_allocation_or_die(ptr, requested_size);
    ao_record_successful_allocation(1, requested_size);
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    void *new_ptr;
    size_t old_size = 0;
    uint64_t allocation_id = 0;
    int tracked = 0;

    if (g_in_resolver) {
        return ao_bootstrap_realloc(ptr, size);
    }

    if (ao_bootstrap_contains(ptr)) {
        return ao_bootstrap_realloc(ptr, size);
    }

    ao_ensure_real_functions();

    if (!ptr) {
        new_ptr = g_real_realloc(NULL, size);
        if (!new_ptr) {
            ao_record_failed_allocation();
            return NULL;
        }

        ao_track_allocation_or_die(new_ptr, size);
        ao_record_successful_allocation(2, size);
        return new_ptr;
    }

    ao_registry_lock();
    tracked = ao_registry_lookup_locked(ptr, &old_size, &allocation_id);
    ao_registry_unlock();

    if (!tracked) {
        ao_record_realloc(1);
        new_ptr = g_real_realloc(ptr, size);
        if (!new_ptr && size != 0) {
            ao_record_failed_allocation();
        }
        return new_ptr;
    }

    new_ptr = g_real_realloc(ptr, size);
    if (!new_ptr && size != 0) {
        ao_record_realloc(0);
        ao_record_failed_allocation();
        return NULL;
    }

    ao_registry_lock();
    (void)ao_registry_remove_locked(ptr, NULL, NULL);
    if (new_ptr) {
        if (!ao_registry_insert_locked(new_ptr, size, allocation_id)) {
            ao_registry_unlock();
            ao_fatal("mock_allocator: allocation registry exhausted during realloc\n");
        }
    }
    ao_registry_unlock();

    ao_record_realloc(0);

    if (new_ptr) {
        ao_live_update(old_size, size);
    } else {
        ao_live_remove(old_size);
    }

    return new_ptr;
}

void free(void *ptr)
{
    size_t requested_size = 0;
    int tracked = 0;

    if (!ptr) {
        return;
    }

    if (ao_bootstrap_contains(ptr)) {
        return;
    }

    if (g_in_resolver) {
        return;
    }

    ao_ensure_real_functions();

    ao_registry_lock();
    tracked = ao_registry_remove_locked(ptr, &requested_size, NULL);
    ao_registry_unlock();

    ao_record_free(!tracked);

    if (tracked) {
        ao_live_remove(requested_size);
    }

    g_real_free(ptr);
}
