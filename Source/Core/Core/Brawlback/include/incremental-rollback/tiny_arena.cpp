//#include "pch.h"
#include "tiny_arena.h"
#include "assert.h"
#include <Common/Logging/Log.h>

#define MAX_ARENA_NAME_LEN 30

Arena arena_init(void* backing_buffer, size_t arena_size) 
{
    Arena a;
    a.backing_mem = (unsigned char*)backing_buffer;
    a.backing_mem_size = arena_size;
    a.offset = 0;
    a.prev_offset = 0;
    return a;
}

Arena arena_init(void* backing_buffer, size_t arena_size, const char* name_owning) 
{
    Arena a = arena_init(backing_buffer, arena_size);
    a.name = name_owning;
    return a;
}

const char* arena_get_name(Arena* arena) 
{
    return arena->name;
}

void* arena_alloc(Arena* arena, size_t alloc_size) 
{
    size_t& offset = arena->offset;
    bool is_out_of_mem = offset + alloc_size > arena->backing_mem_size;
    if (is_out_of_mem) 
    {
        INFO_LOG_FMT(BRAWLBACK, "Out of memory in arena {}\n", arena_get_name(arena));
        // maybe we automatically resize here?
        return nullptr;
    }
    // TODO: enforce alignment    
    void* new_alloc = arena->backing_mem + offset;
    arena->prev_offset = offset;
    offset += alloc_size;
    return new_alloc;
}

void* arena_resize(Arena* arena, void* old_mem, size_t old_size, size_t new_size) {
    // resize memory block if it's the most recent alloc.
    // otherwise, resizing just means reallocating and copying old mem to new spot
    uintptr_t old_mem_addr = (uintptr_t)old_mem;
    uintptr_t backing_mem_addr = (uintptr_t)arena->backing_mem;
    bool is_old_mem_in_range = old_mem_addr >= backing_mem_addr && old_mem_addr < backing_mem_addr + arena->offset;
    if (is_old_mem_in_range) 
    {
        bool is_most_recent_alloc = old_mem_addr == backing_mem_addr + arena->prev_offset;
        if (is_most_recent_alloc) 
        {
            arena->offset = arena->prev_offset + new_size;
            return old_mem;
        }
        else 
        {
            void* new_mem = arena_alloc(arena, new_size);
            size_t copy_size = old_size < new_size ? old_size : new_size; // smaller of the two
            memmove(new_mem, old_mem, copy_size);
            return new_mem;
        }
    }
    else 
    {
        return nullptr;
    }
}

void arena_pop_latest(Arena* arena, void* data)
{
    if (arena->offset == arena->prev_offset)
    {
        INFO_LOG_FMT(BRAWLBACK, "Attempted to pop on arena without a valid most recent allocation\n");
        return;
    }
    if (data == nullptr || arena->backing_mem + arena->offset == data)
    {
        arena->offset = arena->prev_offset;
    }
}

void arena_clear(Arena* arena) 
{
    arena->offset = 0;
    arena->prev_offset = 0;
}

