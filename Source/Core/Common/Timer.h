// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "Common/CommonTypes.h"

namespace Common
{
class Timer
{
public:
  Timer();

  void Start();
  void Stop();
  void Update();

  // The time difference is always returned in milliseconds, regardless of alternative internal
  // representation
  u64 GetTimeDifference();
  void AddTimeDifference();

  bool IsRunning() const { return m_Running; }

  static void IncreaseResolution();
  static void RestoreResolution();
  static u64 GetTimeSinceJan1970();
  static u64 GetLocalTimeSinceJan1970();
  // Returns a timestamp with decimals for precise time comparisons
  static double GetDoubleTime();

  static std::string GetTimeFormatted();
  // Formats a timestamp from GetDoubleTime() into a date and time string
  static std::string GetDateTimeFormatted(double time);
  std::string GetTimeElapsedFormatted() const;
  u64 GetTimeElapsed();

  static u32 GetTimeMs();
  static u64 GetTimeUs();

  // Arbitrarily chosen value (38 years) that is subtracted in GetDoubleTime()
  // to increase sub-second precision of the resulting double timestamp
  static constexpr int DOUBLE_TIME_OFFSET = (38 * 365 * 24 * 60 * 60);

private:
  u64 m_LastTime;
  u64 m_StartTime;
  bool m_Running;
};

}  // Namespace Common
