// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include "Core/Rollback/Perf.h"

#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI) && defined(ROLLBACK_PROFILE_SUPERLUMINAL)

namespace Rollback
{
PerformanceAPI_Functions g_perf_api = {};
}

#endif

namespace Rollback
{
void PerfInit()
{
#if defined(HAVE_SUPERLUMINAL_PERFORMANCEAPI) && defined(ROLLBACK_PROFILE_SUPERLUMINAL)
  if (PerformanceAPI_LoadFrom("PerformanceAPI.dll", &Rollback::g_perf_api))
  {
    if (g_perf_api.Initialize)
      g_perf_api.Initialize();
  }
#endif
}
}  // namespace Rollback

#endif
