// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include "Core/Rollback/FullMemcpySaveSlot.h"

#include <cstring>

#include "Core/HW/Memmap.h"
#include "Core/Rollback/Perf.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/State.h"
#include "Core/System.h"

namespace Rollback
{

void FullMemcpySaveSlot::Init(size_t mem1_size, size_t mem2_size, size_t l1_cache_size)
{
  m_mem1_buffer.reset(mem1_size);
  m_mem2_buffer.reset(mem2_size);
  m_l1_cache_buffer.reset(l1_cache_size);
  m_has_state = false;
}

void FullMemcpySaveSlot::Reset()
{
  m_has_state = false;
}

void FullMemcpySaveSlot::Save(Core::System& system)
{
  ROLLBACK_ZONE();

  auto& memory = system.GetMemory();

  std::memcpy(m_mem1_buffer.data(), memory.GetSpanForAddress(0).data(), m_mem1_buffer.size());

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  State::SaveToBuffer(system, m_save_buffer);
  mgr.EndDoState();

  m_has_state = true;
}

void FullMemcpySaveSlot::Load(Core::System& system)
{
  ROLLBACK_ZONE();

  if (!m_has_state)
    return;

  auto& memory = system.GetMemory();
  std::memcpy(memory.GetSpanForAddress(0).data(), m_mem1_buffer.data(), m_mem1_buffer.size());

  auto& mgr = RollbackManager::Get();
  mgr.BeginDoState();
  State::LoadFromBuffer(system, m_save_buffer);
  mgr.EndDoState();
}

}  // namespace Rollback

#endif
