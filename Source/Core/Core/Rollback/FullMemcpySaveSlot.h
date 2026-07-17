// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _WIN32

#include "Common/Buffer.h"
#include "Core/Rollback/IRollbackSaveSlot.h"

namespace Rollback
{

class FullMemcpySaveSlot final : public IRollbackSaveSlot
{
public:
  void Init(size_t mem1_size, size_t mem2_size, size_t l1_cache_size);

  bool HasState() const override { return m_has_state; }
  void Reset() override;
  void Save(Core::System& system) override;
  void Load(Core::System& system) override;

private:
  Common::UniqueBuffer<uint8_t> m_mem1_buffer;
  Common::UniqueBuffer<uint8_t> m_mem2_buffer;
  Common::UniqueBuffer<uint8_t> m_l1_cache_buffer;
  Common::UniqueBuffer<uint8_t> m_save_buffer;

  bool m_has_state = false;
};

}  // namespace Rollback

#endif  // _WIN32
