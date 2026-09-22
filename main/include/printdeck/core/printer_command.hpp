#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include "cJSON.h"

namespace printdeck::core {

// Set an explicit state rather than toggle: retrying a delivered command is safe.
// Queueing/delivery belongs to the transport; this bounded envelope and the
// execution gate are shared by local callers and the cloud worker.
struct PrinterCommand {
  std::uint32_t printer_id = 0;
  bool light_on = false;
  bool select = false;
};

inline bool parse_printer_command(std::string_view payload, PrinterCommand& command) {
  if (payload.empty() || payload.size() > 256 || payload.find('\0') != std::string_view::npos || payload.find("\\u0000") != std::string_view::npos) return false;
  // Reject deeply nested input before cJSON recurses on the HTTP task stack.
  // The supported envelope has only root + parameters objects.
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (char c : payload) {
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else if (c == '"') quoted = true;
    else if (c == '{' || c == '[') { if (++depth > 2) return false; }
    else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
  }
  if (quoted || depth) return false;
  const std::string terminated(payload);
  cJSON* root = cJSON_ParseWithOpts(terminated.c_str(), nullptr, true);
  if (!root) return false;
  const auto* version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
  const auto* id = cJSON_GetObjectItemCaseSensitive(root, "printer_id");
  const auto* action = cJSON_GetObjectItemCaseSensitive(root, "action");
  const auto* parameters = cJSON_GetObjectItemCaseSensitive(root, "parameters");
  const auto* on = cJSON_GetObjectItemCaseSensitive(parameters, "on");
  const bool selection = cJSON_IsString(action) && std::string_view(action->valuestring) == "printer.select";
  const bool light = cJSON_IsString(action) && std::string_view(action->valuestring) == "light.set";
  const bool valid = cJSON_IsObject(root) && cJSON_GetArraySize(root) == 4 &&
      cJSON_IsNumber(version) && version->valuedouble == 1 &&
      cJSON_IsNumber(id) && id->valuedouble >= 1 &&
      id->valuedouble <= std::numeric_limits<std::uint32_t>::max() &&
      std::floor(id->valuedouble) == id->valuedouble &&
      cJSON_IsObject(parameters) && ((selection && cJSON_GetArraySize(parameters) == 0) ||
      (light && cJSON_GetArraySize(parameters) == 1 && cJSON_IsBool(on)));
  if (valid) command = {static_cast<std::uint32_t>(id->valuedouble), cJSON_IsTrue(on) != 0, selection};
  cJSON_Delete(root);
  return valid;
}
}  // namespace printdeck::core
