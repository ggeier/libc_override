#ifndef ALLOC_OVERRIDE_ALLOCATOR_BRIDGE_H
#define ALLOC_OVERRIDE_ALLOCATOR_BRIDGE_H

#include <stddef.h>

int ao_alloc_bridge_owns(const void *ptr);
void ao_alloc_bridge_libc_free(void *ptr);
void *ao_alloc_bridge_libc_realloc(void *ptr, size_t size);

#endif
