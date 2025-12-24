#pragma once

#include <iostream>
#include <cstring>
#include <chrono>
#include <ctime>
#include "Common/CommonTypes.h"
#include "boost/icl/interval_set.hpp"
#include "brawlback-common/BrawlbackConstants.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) ( sizeof((arr))/sizeof((arr)[0]) )
#endif

struct Arena
{
  unsigned char* backing_mem = 0;
  size_t backing_mem_size = 0;
  size_t offset = 0;
  size_t prev_offset = 0;
  const char* name = "UNNAMED_ARENA";
};

namespace IncrementalRB
{
  struct Savestate
  {
    // sorted (ascending) list of changed pages
    std::vector<uintptr_t> changedPages = {};
    // page-sized blocks of memory that contains data after this frame wrote to the pages
    std::vector<uintptr_t> afterCopies = {};
    Arena arena = {};
    u32 frame = 0;
    bool valid = false;
  };

  struct SavestateInfo
  {
    Savestate savestates[MAX_SAVESTATES] = {};
  };
}

template <typename T> 
inline T Clamp(T& value, const T& low, const T& high) 
{
    return value < low ? low : (value > high ? high : value); 
}

template <typename T>
inline T PercentOf(T x, u32 percentOutOf100)
{
    percentOutOf100 = Clamp(percentOutOf100, 0u, 100u);
    return (x * percentOutOf100) / 100;
}

#define KILOBYTES_BYTES(kb) (kb*1024)
#define MEGABYTES_BYTES(mb) (mb*KILOBYTES_BYTES(1024))
#define GIGABYTES_BYTES(gb) (gb*MEGABYTES_BYTES(1024))

#define IS_ALIGNED(ptr, alignment) (((u64)ptr % alignment) == 0)

inline u32 HashBytes(char* data, u32 size)
{
    // FNV-1 hash -> doing the *PRIME, and THEN the XOR.
    // FNV-1a hash -> doing the XOR, then the *PRIME
    // https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
    constexpr u32 FNV_offset_basis = 0x811c9dc5;
    constexpr u32 FNV_prime = 0x01000193;
    u32 hash = FNV_offset_basis;
    for (u32 i = 0; i < size; i++)
    {
        u8 byte_of_data = data[i];
        hash = hash ^ byte_of_data;
        hash = hash * FNV_prime;
    }
    return hash;
}
