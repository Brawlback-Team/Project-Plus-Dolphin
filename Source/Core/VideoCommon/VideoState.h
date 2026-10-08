// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class PointerWrap;

void VideoCommon_DoState(PointerWrap& p);

// Orca: skip host rendering on throwaway re-run frames. The GX command stream is still parsed, so
// registers, TMEM, PE tokens, interrupts and emulated GPU timing match a rendered frame; only
// vertex loading, draws, copies, clears and presentation are skipped. Texture-only copies still
// write their placeholder, so RAM matches too. Refused in dual core.
void VideoCommon_SetSkipRender(bool skip);
bool VideoCommon_IsSkippingRender();

// Orca: set around a rollback snapshot's DoState (the same for measure, save and load). Host GPU
// state is then skipped: no EFB readback, texture cache or bounding box. None of it reaches RAM in
// a session, and skipping it saves a GPU sync per snapshot. After a load, EFB-copy effects may show
// the newest frame's copy for one frame.
void VideoCommon_SetRollbackSnapshot(bool snapshot);
bool VideoCommon_IsRollbackSnapshot();

// Orca: whether loading a state shows its last frame again. Savestates and drop-in keyframes do,
// so something is on screen until the next frame. Rollback snapshots don't: the re-run frames end
// with the corrected frame, and showing the stale one first would flicker backward for a frame.
void VideoCommon_SetSnapshotRedisplays(bool redisplays);
bool VideoCommon_LoadRedisplays();
