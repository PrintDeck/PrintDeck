#pragma once

#include <string_view>

namespace printdeck::core {

enum class CalendarDateFormat {
  day_month_year,
  month_day_year,
  year_month_day,
};

// Converts browser/IANA zone identifiers into the POSIX form used by newlib.
// Unknown zones deliberately fall back to UTC instead of silently applying a
// host-specific rule.
const char* posix_timezone(std::string_view iana_zone);
bool supported_timezone(std::string_view iana_zone);
// Uses the region represented by the configured IANA time zone.  This is a
// pragmatic locale hint for an offline device which has no independent
// country/region setting.
CalendarDateFormat calendar_date_format(std::string_view iana_zone);

}  // namespace printdeck::core
