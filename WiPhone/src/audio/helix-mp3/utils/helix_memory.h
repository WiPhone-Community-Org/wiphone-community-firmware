/*
 * helix_memory.h — the allocator helix's buffers.c calls.
 *
 * Vendored declaration, implemented in helix_memory.c. Helix routes EVERY allocation it
 * makes through these two functions (buffers.c: "All memory allocation for the codec is
 * done in this file"), which is the hook that makes an MP3 decoder possible on this
 * phone at all — see helix_memory.c for why it must not use the ordinary heap.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void* helix_malloc(int size);
void  helix_free(void *ptr);

#ifdef __cplusplus
}
#endif
