// CLOCK15: the guest clock's value at boot.
//
// Dolphin boots every title (GC and Wii) with the time base already at "now"
// (SystemTimers::Init):
//   CoreTiming::SetFakeTBStartValue((cpu_core_clock / TIMER_RATIO) *
//                                   CEXIIPL::GetEmulatedTime(CEXIIPL::GC_EPOCH));
// GetEmulatedTime() is the host's LOCAL wall-clock time in seconds since 1970
// (Common::Timer::GetLocalTimeSinceJan1970, or the custom RTC value when
// MAIN_CUSTOM_RTC_ENABLE is set) minus GC_EPOCH = 0x386D4380 (2000-01-01 00:00:00).
// The SDK's OSGetTime() is the raw time base and OSTicksToCalendarTime() counts
// from 2000-01-01 with OS_TIMER_CLOCK = bus clock (0x800000F8) / 4, so the
// guest's calendar is the host's local date and time. Skyward Sword stores
// OSGetTime() as SaveFile.savedTime (FileManager::setFileTimes) and shows it in
// the file select; with a time base starting at 0 every save made in the port
// said 01/01/2000 and saved_time was not chronological across sessions.
//
// NWII_RTC (Dolphin's custom RTC) pins the clock for reproducible runs:
//   unset / "host"  host local time (what Dolphin does by default)
//   "0"             time base starts at 0 = 2000-01-01 00:00:00 (the pre-CLOCK15 clock;
//                   probe.py pins this by default so earlier runs stay comparable)
//   "<N>"           N seconds since 1970, read as local wall-clock time
//                   (e.g. 1790016010 = 2026-09-21 18:40:10); before 2000 -> as "0"
// Anything else falls back to host time (reported in the [Clock] log line).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace nwii::runtime::guest_clock {

// 2000-01-01 00:00:00 in seconds since 1970 (Dolphin CEXIIPL::GC_EPOCH).
constexpr int64_t kGcEpoch = 0x386D4380;
// Latest boot time accepted: the RTC is a u32 count of seconds since the epoch.
constexpr int64_t kMaxLocalUnix = kGcEpoch + (int64_t)0xFFFFFFFFu;

// Host local wall-clock time as seconds since 1970 (UTC + zone offset incl. DST),
// like Dolphin's Common::Timer::GetLocalTimeSinceJan1970.
inline int64_t host_local_unix_seconds(std::time_t utc = std::time(nullptr)) {
  std::tm lt{};
#if defined(_WIN32)
  if (localtime_s(&lt, &utc) != 0) return (int64_t)utc;
  return (int64_t)_mkgmtime(&lt);
#else
  if (!localtime_r(&utc, &lt)) return (int64_t)utc;
  return (int64_t)timegm(&lt);
#endif
}

struct Choice {
  int64_t local_unix = 0;      // local wall-clock seconds since 1970 at boot; <= kGcEpoch -> epoch
  const char *source = "host"; // for the log line
};

// Picks the boot time from NWII_RTC (nullptr = unset) and the host's local time.
inline Choice choose(const char *env, int64_t host_local) {
  Choice c;
  c.local_unix = host_local;
  if (!env || !*env || std::strcmp(env, "host") == 0) return c;
  int64_t v = 0;
  size_t n = 0;
  for (const char *p = env; *p; ++p, ++n) {
    if (*p < '0' || *p > '9' || n >= 12) {
      c.source = "host (NWII_RTC not a number of seconds; ignored)";
      return c;
    }
    v = v * 10 + (*p - '0');
  }
  if (v <= kGcEpoch) {
    c.local_unix = kGcEpoch;
    c.source = v == 0 ? "NWII_RTC=0: time base starts at 0 (pre-CLOCK15 clock)"
                      : "NWII_RTC before 2000 -> 2000-01-01 00:00:00";
    return c;
  }
  c.local_unix = v > kMaxLocalUnix ? kMaxLocalUnix : v;
  c.source = "NWII_RTC";
  return c;
}

// Whole seconds since 2000-01-01 00:00:00 local time (never negative).
inline uint64_t seconds_since_2000(int64_t local_unix) {
  if (local_unix <= kGcEpoch) return 0;
  return (uint64_t)((local_unix > kMaxLocalUnix ? kMaxLocalUnix : local_unix) - kGcEpoch);
}

// Time base value at boot: timer clock (bus / 4 = core / 12; 60.75 MHz on Wii,
// 40.5 MHz on GC) times the seconds since 2000-01-01. At most 2^32 s * 60.75e6
// = 2.6e17 < 2^63: OSTime (s64) and inst_for_tb() (x12) stay in range.
inline uint64_t boot_timebase(uint64_t tb_freq, int64_t local_unix) {
  return tb_freq * seconds_since_2000(local_unix);
}

// "YYYY-MM-DD hh:mm:ss" of a local wall-clock time (for the [Clock] log line).
inline std::string format(int64_t local_unix) {
  const std::time_t t = (std::time_t)local_unix;
  std::tm g{};
#if defined(_WIN32)
  if (gmtime_s(&g, &t) != 0) return "?";
#else
  if (!gmtime_r(&t, &g)) return "?";
#endif
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                g.tm_hour, g.tm_min, g.tm_sec);
  return buf;
}

} // namespace nwii::runtime::guest_clock
