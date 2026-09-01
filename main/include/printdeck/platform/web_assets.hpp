#pragma once

#include <string_view>

namespace printdeck::platform {

std::string_view web_config_page();
std::string_view web_localizations_script();
std::string_view world_map_svg();
std::string_view reactions_script();
std::string_view reaction_set_preview(std::string_view id);

}  // namespace printdeck::platform
