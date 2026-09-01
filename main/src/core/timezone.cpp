#include "printdeck/core/timezone.hpp"

#include <utility>

namespace printdeck::core {
namespace {
using Mapping = std::pair<std::string_view, const char*>;

// POSIX rules are kept locally so clocks remain correct without an online time-zone service.
// The list mirrors the choices exposed by Web Config and covers every inhabited UTC offset.
constexpr Mapping kMappings[] = {
    {"UTC", "UTC0"},
    {"Etc/UTC", "UTC0"},
    {"Atlantic/Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"Atlantic/Cape_Verde", "<-01>1"},
    {"Atlantic/Reykjavik", "GMT0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "<+03>-3"},
    {"Europe/Moscow", "MSK-3"},
    {"Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa/Johannesburg", "SAST-2"},
    {"Africa/Nairobi", "EAT-3"},
    {"Africa/Lagos", "WAT-1"},
    {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu", "HST10"},
    {"America/Mexico_City", "CST6"},
    {"America/Bogota", "<-05>5"},
    {"America/Lima", "<-05>5"},
    {"America/Caracas", "<-04>4"},
    {"America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"America/Sao_Paulo", "<-03>3"},
    {"America/Argentina/Buenos_Aires", "<-03>3"},
    {"Asia/Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia/Dubai", "<+04>-4"},
    {"Asia/Karachi", "PKT-5"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dhaka", "<+06>-6"},
    {"Asia/Bangkok", "<+07>-7"},
    {"Asia/Jakarta", "WIB-7"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Singapore", "<+08>-8"},
    {"Asia/Manila", "PST-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Australia/Perth", "AWST-8"},
    {"Australia/Darwin", "ACST-9:30"},
    {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia/Brisbane", "AEST-10"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia/Hobart", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific/Fiji", "<+12>-12"},
};
}  // namespace

const char* posix_timezone(std::string_view zone) {
  for (const auto& [iana, posix] : kMappings) {
    if (zone == iana) return posix;
  }
  return "UTC0";
}

bool supported_timezone(std::string_view zone) {
  for (const auto& [iana, unused] : kMappings) {
    static_cast<void>(unused);
    if (zone == iana) return true;
  }
  return false;
}

CalendarDateFormat calendar_date_format(std::string_view zone) {
  // Month-first is customary in the United States and the Philippines.
  constexpr std::string_view kMonthFirstZones[] = {
      "America/New_York", "America/Chicago", "America/Denver",
      "America/Phoenix", "America/Los_Angeles", "America/Anchorage",
      "Pacific/Honolulu", "Asia/Manila",
  };
  for (const std::string_view candidate : kMonthFirstZones) {
    if (zone == candidate) return CalendarDateFormat::month_day_year;
  }

  // Canada commonly uses the unambiguous ISO order, while China, Hong Kong,
  // Japan and Korea conventionally put the year first.
  constexpr std::string_view kYearFirstZones[] = {
      "UTC", "Etc/UTC", "America/St_Johns", "America/Halifax",
      "Asia/Shanghai", "Asia/Hong_Kong", "Asia/Tokyo", "Asia/Seoul",
  };
  for (const std::string_view candidate : kYearFirstZones) {
    if (zone == candidate) return CalendarDateFormat::year_month_day;
  }
  return CalendarDateFormat::day_month_year;
}

}  // namespace printdeck::core
