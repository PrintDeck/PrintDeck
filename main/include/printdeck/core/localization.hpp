#pragma once

#include <array>
#include <string_view>

namespace printdeck::core {

struct Language {
  std::string_view code;
  std::string_view native_name;
};

inline constexpr std::array<Language, 6> kLanguages{{
    {"en", "English"},
    {"pl", "Polski"},
    {"es", "Español"},
    {"fr", "Français"},
    {"de", "Deutsch"},
    {"zh-CN", "简体中文"},
}};

bool supported_language(std::string_view code);
std::string_view normalize_language(std::string_view browser_language);
const char* localized_text(std::string_view language, std::string_view english);

}  // namespace printdeck::core
