// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VideoState.h"

#include "Common/ChunkFile.h"
#include "Core/System.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/Widescreen.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"
#include "VideoCommon/VideoBackendBase.h"

#include <atomic>
#include "Common/Logging/Log.h"

static std::atomic<bool> s_skip_render{false};
static bool s_rollback_snapshot = false;
static bool s_snapshot_redisplays = false;

void VideoCommon_SetRollbackSnapshot(bool snapshot)
{
  s_rollback_snapshot = snapshot;
}

bool VideoCommon_IsRollbackSnapshot()
{
  return s_rollback_snapshot;
}

void VideoCommon_SetSnapshotRedisplays(bool redisplays)
{
  s_snapshot_redisplays = redisplays;
}

bool VideoCommon_LoadRedisplays()
{
  return !s_rollback_snapshot || s_snapshot_redisplays;
}

void VideoCommon_SetSkipRender(bool skip)
{
  if (skip && Core::System::GetInstance().IsDualCoreMode())
  {
    WARN_LOG_FMT(VIDEO, "Skip render refused: dual core");
    skip = false;
  }
  s_skip_render.store(skip, std::memory_order_relaxed);
}

bool VideoCommon_IsSkippingRender()
{
  return s_skip_render.load(std::memory_order_relaxed);
}

void VideoCommon_DoState(PointerWrap& p)
{
  bool software = false;
  p.Do(software);

  if (p.IsReadMode() && software == true)
  {
    // change mode to abort load of incompatible save state.
    p.SetVerifyMode();
  }

  // Orca: on a load, draw any buffered vertices first, while the old registers are still in
  // place. Flushed after the restore, they would be drawn with the snapshot's matrices and TEV
  // state and show up as garbage triangles. See ORCA.md, "Rollback and snapshots".
  if (p.IsReadMode())
    g_vertex_manager->Flush();

  // BP Memory
  p.Do(bpmem);
  p.DoMarker("BP Memory");

  // CP Memory
  // We don't save g_preprocess_cp_state separately because the GPU should be
  // synced around state save/load.
  p.Do(g_main_cp_state);
  p.DoMarker("CP Memory");
  if (p.IsReadMode())
    CopyPreprocessCPStateFromMain();

  // XF Memory
  p.Do(xfmem);
  p.DoMarker("XF Memory");

  // Texture decoder
  p.DoArray(s_tex_mem);
  p.DoMarker("texMem");

  // TMEM
  TMEM::DoState(p);
  p.DoMarker("TMEM");

  // FIFO
  auto& system = Core::System::GetInstance();
  system.GetFifo().DoState(p);
  p.DoMarker("Fifo");

  auto& command_processor = system.GetCommandProcessor();
  command_processor.DoState(p);
  p.DoMarker("CommandProcessor");

  system.GetPixelEngine().DoState(p);
  p.DoMarker("PixelEngine");

  // the old way of replaying current bpmem as writes to push side effects to pixel shader manager
  // doesn't really work.
  system.GetPixelShaderManager().DoState(p);
  p.DoMarker("PixelShaderManager");

  system.GetVertexShaderManager().DoState(p);
  p.DoMarker("VertexShaderManager");

  system.GetGeometryShaderManager().DoState(p);
  p.DoMarker("GeometryShaderManager");

  // Orca: always saved, even for rollback: it holds emulated GX state (z slope, cached
  // normal/tangent/binormal). Its flush on load is a no-op after the flush above.
  g_vertex_manager->DoState(p);
  p.DoMarker("VertexManager");

  if (!s_rollback_snapshot)
    g_framebuffer_manager->DoState(p);
  p.DoMarker("FramebufferManager");

  if (!s_rollback_snapshot)
    g_texture_cache->DoState(p);
  p.DoMarker("TextureCache");

  g_presenter->DoState(p);
  g_frame_dumper->DoState(p);
  p.DoMarker("Presenter");

  if (!s_rollback_snapshot)
    g_bounding_box->DoState(p);
  p.DoMarker("Bounding Box");

  g_widescreen->DoState(p);
  p.DoMarker("Widescreen");

  system.GetXFStateManager().DoState(p);
  p.DoMarker("XFStateManager");

  // Refresh state.
  if (p.IsReadMode())
  {
    // Inform backend of new state from registers.
    BPReload();
    VertexLoaderManager::MarkAllDirty();
  }

  // Orca: a full save submits GPU work through its EFB readback, but a rollback snapshot skips
  // that. Submit encoded work here (no wait): some backends (Metal) otherwise submit only on
  // present, and re-run frames present nothing.
  if (s_rollback_snapshot && !p.IsMeasureMode())
    g_gfx->Flush();
}
