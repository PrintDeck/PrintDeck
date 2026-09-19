#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "cJSON.h"

namespace printdeck::core {
namespace event_detail {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
inline Json own(cJSON* value) { return Json(value, cJSON_Delete); }
inline bool put(cJSON* object, const char* key, cJSON* value) {
  if (!value) return false;
  if (cJSON_AddItemToObject(object, key, value)) return true;
  cJSON_Delete(value); return false;
}
inline bool difference(const cJSON* old, const cJSON* next, Json& patch, int depth = 0) {
  if (depth > 12) return false;
  if (cJSON_Compare(old, next, true)) return true;
  if (!cJSON_IsObject(old) || !cJSON_IsObject(next)) {
    patch = own(next ? cJSON_Duplicate(next, true) : cJSON_CreateNull());
    return !!patch;
  }
  patch = own(cJSON_CreateObject()); if (!patch) return false;
  for (const cJSON* item = next->child; item; item = item->next) {
    auto child = own(nullptr);
    if (!difference(cJSON_GetObjectItemCaseSensitive(old, item->string), item, child, depth + 1)) return false;
    if (child && !put(patch.get(), item->string, child.release())) return false;
  }
  for (const cJSON* item = old->child; item; item = item->next)
    if (!cJSON_GetObjectItemCaseSensitive(next, item->string) &&
        !put(patch.get(), item->string, cJSON_CreateNull())) return false;
  return true;
}
}

struct PrinterEventDelta {
  bool snapshot = false;
  std::string json;  // Empty means no changed fields.
};

// Only sanitized /api/printers output enters this bounded serializer.
// A changed profile list/order starts a new complete baseline.
inline std::optional<PrinterEventDelta> printer_event_delta(std::string_view previous,
                                                           std::string_view current) {
  if (previous.size() > 32768 || current.size() > 32768) return std::nullopt;
  if (previous == current) return PrinterEventDelta{};
  auto old = event_detail::own(cJSON_ParseWithLength(previous.data(), previous.size()));
  auto next = event_detail::own(cJSON_ParseWithLength(current.data(), current.size()));
  const auto* rows = cJSON_GetObjectItemCaseSensitive(next.get(), "printers");
  const auto* before = cJSON_GetObjectItemCaseSensitive(old.get(), "printers");
  if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) > 10) return std::nullopt;
  if (!cJSON_IsArray(before) || cJSON_GetArraySize(before) != cJSON_GetArraySize(rows))
    return PrinterEventDelta{true, std::string(current)};
  auto changes = event_detail::own(cJSON_CreateArray()); if (!changes) return std::nullopt;
  for (int i = 0; i < cJSON_GetArraySize(rows); ++i) {
    const auto* row = cJSON_GetArrayItem(rows, i);
    const auto* prior = cJSON_GetArrayItem(before, i);
    const auto* id = cJSON_GetObjectItemCaseSensitive(row, "id");
    if (!cJSON_IsNumber(id) || !cJSON_Compare(id, cJSON_GetObjectItemCaseSensitive(prior, "id"), true))
      return PrinterEventDelta{true, std::string(current)};
    auto patch = event_detail::own(nullptr);
    if (!event_detail::difference(prior, row, patch)) return std::nullopt;
    if (!patch) continue;
    // Each job update carries fresh anchors for browser-local timers. Omitted
    // large details/arrays remain cached; null explicitly clears old job data.
    auto* job_patch = cJSON_GetObjectItemCaseSensitive(patch.get(), "job");
    const auto* job = cJSON_GetObjectItemCaseSensitive(row, "job");
    if (cJSON_IsObject(job_patch) && cJSON_IsObject(job)) {
      for (const auto* key : {"elapsed_seconds", "remaining_seconds", "exposure_remaining_ms",
                              "exposure_valid_for_ms", "preview"}) {
        const auto* value = cJSON_GetObjectItemCaseSensitive(job, key);
        if (!value) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(job_patch, key);
        if (!event_detail::put(job_patch, key, cJSON_Duplicate(value, true))) return std::nullopt;
      }
    }
    auto entry = event_detail::own(cJSON_CreateObject());
    if (!entry || !event_detail::put(entry.get(), "id", cJSON_Duplicate(id, true)) ||
        !event_detail::put(entry.get(), "patch", patch.release())) return std::nullopt;
    if (!cJSON_AddItemToArray(changes.get(), entry.get())) return std::nullopt;
    entry.release();
  }
  if (!changes->child) return PrinterEventDelta{};
  char* bytes = cJSON_PrintUnformatted(changes.get()); if (!bytes) return std::nullopt;
  std::string result(bytes); cJSON_free(bytes);
  if (result.size() > 32768) return std::nullopt;
  return PrinterEventDelta{false, std::move(result)};
}
}  // namespace printdeck::core
