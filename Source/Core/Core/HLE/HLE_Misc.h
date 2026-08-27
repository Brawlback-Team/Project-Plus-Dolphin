// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class CPUThreadGuard;
}

namespace HLE_Misc
{
void UnimplementedFunction(const Core::CPUThreadGuard& guard);
void HBReload(const Core::CPUThreadGuard& guard);
void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard);
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetUnconditionalFrame(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetFrameEnd(const Core::CPUThreadGuard& guard);
void BrawlbackGekkoNetLoopEnd(const Core::CPUThreadGuard& guard);
void BrawlbackDVDCancelSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackCancelTaskSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackFileIOMutexSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackDVDReadPrioSleepHook(const Core::CPUThreadGuard& guard);
void BrawlbackSkipResimRenderHook(const Core::CPUThreadGuard& guard);
}  // namespace HLE_Misc
