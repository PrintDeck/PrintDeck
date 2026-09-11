#pragma once

#include <cstdio>
#include <ctime>
#include <limits>
#include <string>

#include "printdeck/core/job_state.hpp"
#include "printdeck/core/timezone.hpp"

namespace printdeck::core {

inline std::string print_duration(std::uint32_t seconds) {
  char text[24]{};
  std::snprintf(text, sizeof(text), "%u:%02u:%02u",
                static_cast<unsigned>(seconds / 3600U),
                static_cast<unsigned>((seconds / 60U) % 60U),
                static_cast<unsigned>(seconds % 60U));
  return text;
}

inline std::string print_duration_units(std::uint32_t seconds, bool include_zero_hours = true) {
  char text[32]{};
  if (!include_zero_hours && seconds < 3600U) {
    std::snprintf(text, sizeof(text), "%um %02us",
                  static_cast<unsigned>(seconds / 60U),
                  static_cast<unsigned>(seconds % 60U));
    return text;
  }
  std::snprintf(text, sizeof(text), "%uh %02um %02us",
                static_cast<unsigned>(seconds / 3600U),
                static_cast<unsigned>((seconds / 60U) % 60U),
                static_cast<unsigned>(seconds % 60U));
  return text;
}

struct PrintTimeDisplay {
  enum class Kind { unavailable, end_at, elapsed };
  Kind kind = Kind::unavailable;
  std::string value;
  std::string date;

  const char* caption_key(const JobState& job) const {
    if (kind == Kind::end_at) return "End at";
    if (kind == Kind::elapsed) {
      return job.phase == JobPhase::printing || job.phase == JobPhase::preparing ||
             job.phase == JobPhase::paused ? "Elapsed" : "Print time";
    }
    return "";
  }
};

// TZ is configured by Runtime. Add the duration to UTC first so crossing a
// daylight-saving boundary does not turn elapsed hours into wall-clock hours.
inline PrintTimeDisplay print_time_display(const JobState& job, std::time_t now,
                                           CalendarDateFormat date_format,
                                           std::uint64_t view_elapsed_ms = 0) {
  PrintTimeDisplay result;
  const bool running = job.phase == JobPhase::printing || job.phase == JobPhase::preparing;
  // Rotate the secondary field on monotonic screen time. Printer counters and
  // clock synchronization must not reset the interval or change its cadence.
  const bool show_elapsed = job.elapsed_known && (view_elapsed_ms / 10'000U) % 2U != 0;
  if (!show_elapsed && running && job.remaining_known && job.remaining_seconds > 0 &&
      now > 1'700'000'000 &&
      static_cast<std::uint64_t>(job.remaining_seconds) <=
          static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max() - now)) {
    const std::time_t end = now + static_cast<std::time_t>(job.remaining_seconds);
    std::tm local_now{}, local_end{};
    if (localtime_r(&now, &local_now) && localtime_r(&end, &local_end)) {
      char text[32]{};
      std::snprintf(text, sizeof(text), "%d:%02d %s",
          local_end.tm_hour % 12 == 0 ? 12 : local_end.tm_hour % 12,
          local_end.tm_min, local_end.tm_hour < 12 ? "AM" : "PM");
      result.kind = PrintTimeDisplay::Kind::end_at;
      result.value = text;
      if (local_end.tm_year != local_now.tm_year || local_end.tm_yday != local_now.tm_yday) {
        if (date_format == CalendarDateFormat::month_day_year)
          std::snprintf(text, sizeof(text), "%02d/%02d", local_end.tm_mon + 1, local_end.tm_mday);
        else if (date_format == CalendarDateFormat::year_month_day)
          std::snprintf(text, sizeof(text), "%02d-%02d", local_end.tm_mon + 1, local_end.tm_mday);
        else std::snprintf(text, sizeof(text), "%02d.%02d", local_end.tm_mday, local_end.tm_mon + 1);
        result.date = text;
      }
      return result;
    }
  }
  if (job.elapsed_known && job.phase != JobPhase::idle) {
    result.kind = PrintTimeDisplay::Kind::elapsed;
    result.value = print_duration(job.elapsed_seconds);
  }
  return result;
}

}  // namespace printdeck::core
