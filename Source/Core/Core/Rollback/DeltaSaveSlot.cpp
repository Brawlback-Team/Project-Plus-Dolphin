// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include "Core/Rollback/DeltaSaveSlot.h"

#include <bitset>

#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include <Common/Assert.h>
#include "Perf.h"

namespace Rollback
{

static void RestoreRegionDelta(const RegionDelta& delta, uint8_t* region_base)
{
  for (uint32_t i = 0; i < delta.page_count; ++i)
  {
    std::memcpy(region_base + static_cast<size_t>(delta.page_indices[i]) * PAGE_SIZE,
                delta.page_data.data() + static_cast<size_t>(i) * PAGE_SIZE, PAGE_SIZE);
  }
}
void DeltaSaveSlot::Init(uint8_t* mem1_ptr, size_t mem1_size,
                         uint8_t* mem2_ptr, size_t mem2_size,
                         uint8_t* l1_cache_ptr, size_t l1_cache_size)
{
  m_mem1_ptr = mem1_ptr;
  m_mem1_size = mem1_size;
  m_mem2_ptr = mem2_ptr;
  m_mem2_size = mem2_size;
  m_l1_cache_ptr = l1_cache_ptr;
  m_l1_cache_size = l1_cache_size;

  m_mem1_page_count = static_cast<uint32_t>(mem1_size / PAGE_SIZE);
  m_mem2_page_count = static_cast<uint32_t>(mem2_size / PAGE_SIZE);

  m_l1_cache_snapshot.reset(l1_cache_size);
  m_save_buffer.reset(0x100000);  // Estimate: 1MB for non-RAM state
}

void DeltaSaveSlot::Reset()
{
  m_has_state = false;
  m_mem1_delta.Reset();
  m_mem2_delta.Reset();
  m_l1_cache_snapshot.reset();
}

void DeltaSaveSlot::Save(Core::System& system)
{
  // Snapshot L1 cache if available
  if (m_l1_cache_ptr && m_l1_cache_size > 0)
    std::memcpy(m_l1_cache_snapshot.data(), m_l1_cache_ptr, m_l1_cache_size);

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  State::SaveToBuffer(system, m_save_buffer);
  mgr.EndDoState();

  m_has_state = true;
}

bool DeltaSaveSlot::RestoreNonDeltaState(Core::System& system)
{
  ASSERT(m_has_state);

  if (m_l1_cache_ptr && m_l1_cache_size > 0 && m_l1_cache_snapshot.data())
    std::memcpy(m_l1_cache_ptr, m_l1_cache_snapshot.data(), m_l1_cache_size);

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  const bool restored = State::LoadFromBufferForRollback(system, m_save_buffer);
  mgr.EndDoState();

  return restored;
}

EvictedDelta DeltaSaveSlot::ExtractDeltas()
{
  ROLLBACK_ZONE();
  EvictedDelta out;
  out.mem1 = std::move(m_mem1_delta);
  out.mem2 = std::move(m_mem2_delta);
  m_has_state = false;
  return out;
}

void DeltaSaveSlot::MarkTouchedGlobalPages(std::bitset<JITDirtyBitmap::ENTRY_COUNT>& touched) const
{
  ROLLBACK_ZONE();
  for (uint32_t i = 0; i < m_mem1_delta.page_count; ++i)
    touched.set(m_mem1_delta.page_indices[i]);
  for (uint32_t i = 0; i < m_mem2_delta.page_count; ++i)
    touched.set(MEM2_FIRST_PAGE + m_mem2_delta.page_indices[i]);
}

void DeltaSaveSlot::ApplyDeltaReverse() const
{
  ROLLBACK_ZONE();

  ASSERT(m_has_state);
  ASSERT(m_mem1_ptr);

  RestoreRegionDelta(m_mem1_delta, m_mem1_ptr);
  if (m_mem2_ptr && m_mem2_page_count > 0)
    RestoreRegionDelta(m_mem2_delta, m_mem2_ptr);
}


}  // namespace Rollback

#endif
