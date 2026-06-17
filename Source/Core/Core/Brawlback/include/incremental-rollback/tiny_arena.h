#pragma once

#include "util.h"

// ARENAS

#define arena_alloc_type(arena, type, num) ((type*)arena_alloc(arena, sizeof(type) * num))

Arena arena_init(void* backing_buffer, size_t arena_size);
Arena arena_init(void* backing_buffer, size_t arena_size, const char* name); // name should be owning. arena takes a cpy of the ptr
void* arena_alloc(Arena* arena, size_t alloc_size);
void* arena_resize(Arena* arena, void* old_mem, size_t old_size, size_t new_size);
// pops the latest allocation off. Forcing user to pass that allocation to ensure we're popping the right thing
void arena_pop_latest(Arena* arena, void* data);
void arena_clear(Arena* arena);
const char* arena_get_name(Arena* arena);
inline size_t get_free_space(Arena* arena) { return arena->backing_mem_size - arena->offset; }

template <typename T>
T* arena_alloc_and_init(Arena* arena, u32 numElements = 1)
{
    T* alloc = arena_alloc_type(arena, T, numElements);
    new(alloc) T(); // because some c++ types need their ctors called
    return alloc;
}