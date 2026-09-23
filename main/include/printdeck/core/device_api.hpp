#pragma once
#include <cmath>
#include <cstring>
#include <utility>
#include <memory>
#include <string>
#include <string_view>
#include "cJSON.h"
#include "printdeck/core/settings.hpp"
#include "printdeck/core/timezone.hpp"
#include "printdeck/core/theme.hpp"

namespace printdeck::core {
constexpr std::size_t kDeviceCommandMaximumBytes = 8192;
using DeviceJson = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
inline DeviceJson device_json(std::string_view text) {
  if (text.empty() || text.size()>kDeviceCommandMaximumBytes || text.find('\0')!=text.npos || text.find("\\u0000")!=text.npos) return {nullptr,cJSON_Delete};
  unsigned depth=0; bool quoted=false,escaped=false;
  for(char c:text){if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}else if(c=='"')quoted=true;else if(c=='{'||c=='['){if(++depth>4)return {nullptr,cJSON_Delete};}else if(c=='}'||c==']'){if(!depth)return {nullptr,cJSON_Delete};--depth;}}
  if(quoted||depth)return {nullptr,cJSON_Delete};
  const std::string terminated(text);
  return {cJSON_ParseWithOpts(terminated.c_str(),nullptr,true),cJSON_Delete};
}
inline bool device_unique_object(const cJSON* object) {
  if(!cJSON_IsObject(object))return false;
  for(auto* item=object->child;item;item=item->next){if(!item->string)return false;for(auto* other=item->next;other;other=other->next)if(other->string&&std::strcmp(item->string,other->string)==0)return false;}
  return true;
}
inline bool device_integer(const cJSON* item,double minimum,double maximum){return cJSON_IsNumber(item)&&std::isfinite(item->valuedouble)&&item->valuedouble>=minimum&&item->valuedouble<=maximum&&std::floor(item->valuedouble)==item->valuedouble;}
inline std::string device_json_text(const cJSON* value){char* raw=cJSON_PrintUnformatted(value);if(!raw)return {};std::string text(raw);cJSON_free(raw);return text;}

// Export the resolved palette, so remote clients need no copy of firmware presets.
inline std::string device_appearance_json(const DeviceSettings& settings) {
  const auto colors=resolved_theme(settings.theme,settings.custom_theme);
  const auto style=resolved_theme_style(settings.theme,colors);
  DeviceJson root(cJSON_CreateObject(),cJSON_Delete);if(!root)return "{}";
  const auto add=[&](const char* name,std::uint32_t value){cJSON_AddNumberToObject(root.get(),name,value&0xffffffU);};
  add("accent",style.accent);add("on_accent",style.on_accent);
  add("printing",colors.printing);add("done",colors.done);add("error",colors.error);
  add("paused",colors.paused);add("preparing",colors.preparing);add("idle",colors.idle);
  return device_json_text(root.get());
}

// The allowlist deliberately excludes credentials, pairing, printer-control
// consent, networking and physical recovery settings.
inline std::string device_settings_json(const DeviceSettings& s, bool audio_available = true, bool power_button = true, bool cloud = false) {
  DeviceJson root(cJSON_CreateObject(),cJSON_Delete);if(!root)return {};
  cJSON_AddNumberToObject(root.get(),"brightness",s.brightness_percent);
  cJSON_AddNumberToObject(root.get(),"audio_volume",audio_available ? s.audio_volume_percent : 0);
  cJSON_AddNumberToObject(root.get(),"audio_muted_events",s.audio_muted_events);
  cJSON_AddNumberToObject(root.get(),"printer_list_poll_s",s.inactive_printer_poll_interval_s);
  cJSON_AddNumberToObject(root.get(),"dim_brightness",s.display_power.dim_brightness_percent);
  cJSON_AddNumberToObject(root.get(),"saver_animation",s.display_power.screen_saver_animation);
  cJSON_AddNumberToObject(root.get(),"shutdown_s",s.display_power.shutdown_timeout_s);
  cJSON_AddNumberToObject(root.get(),"dim_audio",s.display_power.dim_audio_percent);
  cJSON_AddNumberToObject(root.get(),"off_audio",s.display_power.off_audio_percent);
  cJSON_AddNumberToObject(root.get(),"start_idle",s.display_power.start_timeout_idle_s);
  cJSON_AddNumberToObject(root.get(),"start_active",s.display_power.start_timeout_active_s);
  cJSON_AddNumberToObject(root.get(),"dim_for_idle",s.display_power.dim_duration_idle_s);
  cJSON_AddNumberToObject(root.get(),"dim_for_active",s.display_power.dim_duration_active_s);
  cJSON_AddNumberToObject(root.get(),"saver_for_idle",s.display_power.saver_duration_idle_s);
  cJSON_AddNumberToObject(root.get(),"saver_for_active",s.display_power.saver_duration_active_s);
  cJSON_AddBoolToObject(root.get(),"printer_animations_enabled",s.printer_animations_enabled);
  cJSON_AddBoolToObject(root.get(),"reaction_progress_bar_enabled",s.reaction_progress_bar_enabled);
  cJSON_AddBoolToObject(root.get(),"reaction_progress_percent_enabled",s.reaction_progress_percent_enabled);
  cJSON_AddBoolToObject(root.get(),"audio_enabled",audio_available && s.audio_enabled);
  cJSON_AddBoolToObject(root.get(),"usb_power_save",s.display_power.usb_power_save_enabled);
  cJSON_AddBoolToObject(root.get(),"wake_on_orientation_change",s.display_power.wake_on_orientation_change);
  cJSON_AddBoolToObject(root.get(),"wake_on_touch",!power_button || s.display_power.wake_on_touch);
  if(!cloud)cJSON_AddStringToObject(root.get(),"device_name",s.device_name.c_str());
  cJSON_AddStringToObject(root.get(),"theme",s.theme.c_str());
  cJSON_AddStringToObject(root.get(),"timezone",s.timezone.c_str());
  cJSON_AddStringToObject(root.get(),"language",s.language.c_str());
  cJSON_AddStringToObject(root.get(),"rotation",s.rotation.c_str());
  cJSON_AddStringToObject(root.get(),"audio_preset",s.audio_preset.c_str());
  cJSON_AddStringToObject(root.get(),"printer_view",s.printer_view.c_str());
  auto* colors=cJSON_AddObjectToObject(root.get(),"custom_theme");
  cJSON_AddNumberToObject(colors,"printing",s.custom_theme.printing);
  cJSON_AddNumberToObject(colors,"done",s.custom_theme.done);
  cJSON_AddNumberToObject(colors,"error",s.custom_theme.error);
  cJSON_AddNumberToObject(colors,"idle",s.custom_theme.idle);
  cJSON_AddNumberToObject(colors,"preparing",s.custom_theme.preparing);
  cJSON_AddNumberToObject(colors,"paused",s.custom_theme.paused);
  cJSON_AddNumberToObject(colors,"filament",s.custom_theme.filament);
  cJSON_AddNumberToObject(colors,"setup",s.custom_theme.setup);
  cJSON_AddNumberToObject(colors,"offline",s.custom_theme.offline);
  cJSON_AddNumberToObject(colors,"unknown",s.custom_theme.unknown);
  cJSON_AddNumberToObject(colors,"background",s.custom_theme.background);
  cJSON_AddNumberToObject(colors,"preview",s.custom_theme.preview_background);
  return device_json_text(root.get());
}
inline bool apply_device_settings_patch(const cJSON* patch,DeviceSettings& destination,bool audio_available,bool power_button) {
  if(!device_unique_object(patch)||!patch->child)return false;
  DeviceSettings candidate=destination;
  for(auto* value=patch->child;value;value=value->next){const std::string_view key(value->string);
    if(key=="brightness"){if(!device_integer(value,5,100))return false;candidate.brightness_percent=static_cast<decltype(candidate.brightness_percent)>(value->valuedouble);continue;}
    if(key=="audio_volume"){if(!device_integer(value,0,100))return false;candidate.audio_volume_percent=static_cast<decltype(candidate.audio_volume_percent)>(value->valuedouble);continue;}
    if(key=="audio_muted_events"){if(!device_integer(value,0,16383))return false;candidate.audio_muted_events=static_cast<decltype(candidate.audio_muted_events)>(value->valuedouble);continue;}
    if(key=="printer_list_poll_s"){if(!device_integer(value,0,300))return false;candidate.inactive_printer_poll_interval_s=static_cast<decltype(candidate.inactive_printer_poll_interval_s)>(value->valuedouble);continue;}
    if(key=="dim_brightness"){if(!device_integer(value,0,100))return false;candidate.display_power.dim_brightness_percent=static_cast<decltype(candidate.display_power.dim_brightness_percent)>(value->valuedouble);continue;}
    if(key=="saver_animation"){if(!device_integer(value,0,4))return false;candidate.display_power.screen_saver_animation=static_cast<decltype(candidate.display_power.screen_saver_animation)>(value->valuedouble);continue;}
    if(key=="shutdown_s"){if(!device_integer(value,0,604800))return false;candidate.display_power.shutdown_timeout_s=static_cast<decltype(candidate.display_power.shutdown_timeout_s)>(value->valuedouble);continue;}
    if(key=="dim_audio"){if(!device_integer(value,0,100))return false;candidate.display_power.dim_audio_percent=static_cast<decltype(candidate.display_power.dim_audio_percent)>(value->valuedouble);continue;}
    if(key=="off_audio"){if(!device_integer(value,0,100))return false;candidate.display_power.off_audio_percent=static_cast<decltype(candidate.display_power.off_audio_percent)>(value->valuedouble);continue;}
    if(key=="start_idle"){if(!device_integer(value,0,3610))return false;candidate.display_power.start_timeout_idle_s=static_cast<decltype(candidate.display_power.start_timeout_idle_s)>(value->valuedouble);continue;}
    if(key=="start_active"){if(!device_integer(value,0,3610))return false;candidate.display_power.start_timeout_active_s=static_cast<decltype(candidate.display_power.start_timeout_active_s)>(value->valuedouble);continue;}
    if(key=="dim_for_idle"){if(!device_integer(value,0,86401))return false;candidate.display_power.dim_duration_idle_s=static_cast<decltype(candidate.display_power.dim_duration_idle_s)>(value->valuedouble);continue;}
    if(key=="dim_for_active"){if(!device_integer(value,0,86401))return false;candidate.display_power.dim_duration_active_s=static_cast<decltype(candidate.display_power.dim_duration_active_s)>(value->valuedouble);continue;}
    if(key=="saver_for_idle"){if(!device_integer(value,0,86401))return false;candidate.display_power.saver_duration_idle_s=static_cast<decltype(candidate.display_power.saver_duration_idle_s)>(value->valuedouble);continue;}
    if(key=="saver_for_active"){if(!device_integer(value,0,86401))return false;candidate.display_power.saver_duration_active_s=static_cast<decltype(candidate.display_power.saver_duration_active_s)>(value->valuedouble);continue;}
    if(key=="printer_animations_enabled"){if(!cJSON_IsBool(value))return false;candidate.printer_animations_enabled=cJSON_IsTrue(value);continue;}
    if(key=="reaction_progress_bar_enabled"){if(!cJSON_IsBool(value))return false;candidate.reaction_progress_bar_enabled=cJSON_IsTrue(value);continue;}
    if(key=="reaction_progress_percent_enabled"){if(!cJSON_IsBool(value))return false;candidate.reaction_progress_percent_enabled=cJSON_IsTrue(value);continue;}
    if(key=="audio_enabled"){if(!cJSON_IsBool(value))return false;candidate.audio_enabled=cJSON_IsTrue(value);continue;}
    if(key=="usb_power_save"){if(!cJSON_IsBool(value))return false;candidate.display_power.usb_power_save_enabled=cJSON_IsTrue(value);continue;}
    if(key=="wake_on_orientation_change"){if(!cJSON_IsBool(value))return false;candidate.display_power.wake_on_orientation_change=cJSON_IsTrue(value);continue;}
    if(key=="wake_on_touch"){if(!cJSON_IsBool(value))return false;candidate.display_power.wake_on_touch=cJSON_IsTrue(value);continue;}
    if(key=="device_name"){if(!cJSON_IsString(value)||std::strlen(value->valuestring)>64)return false;candidate.device_name=value->valuestring;continue;}
    if(key=="theme"){if(!cJSON_IsString(value)||std::strlen(value->valuestring)>64)return false;candidate.theme=value->valuestring;continue;}
    if(key=="timezone"){if(!cJSON_IsString(value)||std::strlen(value->valuestring)>64)return false;candidate.timezone=value->valuestring;continue;}
    if(key=="language"){if(!cJSON_IsString(value)||std::strlen(value->valuestring)>64)return false;candidate.language=value->valuestring;continue;}
    if(key=="rotation"){if(!cJSON_IsString(value)||std::strlen(value->valuestring)>64)return false;candidate.rotation=value->valuestring;continue;}
    if(key=="audio_preset"){if(!cJSON_IsString(value)||std::strlen(value->valuestring)>64)return false;candidate.audio_preset=value->valuestring;continue;}
    if(key=="printer_view"){if(!cJSON_IsString(value)||std::strlen(value->valuestring)>64)return false;candidate.printer_view=value->valuestring;continue;}
    if(key=="custom_theme"){if(!device_unique_object(value)||!value->child)return false;for(auto* color=value->child;color;color=color->next){if(!device_integer(color,0,0xffffff))return false;const std::string_view name(color->string);
      if(name=="printing"){candidate.custom_theme.printing=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="done"){candidate.custom_theme.done=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="error"){candidate.custom_theme.error=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="idle"){candidate.custom_theme.idle=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="preparing"){candidate.custom_theme.preparing=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="paused"){candidate.custom_theme.paused=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="filament"){candidate.custom_theme.filament=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="setup"){candidate.custom_theme.setup=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="offline"){candidate.custom_theme.offline=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="unknown"){candidate.custom_theme.unknown=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="background"){candidate.custom_theme.background=static_cast<std::uint32_t>(color->valuedouble);continue;}
      if(name=="preview"){candidate.custom_theme.preview_background=static_cast<std::uint32_t>(color->valuedouble);continue;}
      return false;}continue;}
    return false;
  }
  if(!supported_timezone(candidate.timezone)||!validate(candidate).empty())return false;
  if(!audio_available&&((cJSON_GetObjectItemCaseSensitive(patch,"audio_enabled")&&candidate.audio_enabled)||(cJSON_GetObjectItemCaseSensitive(patch,"audio_volume")&&candidate.audio_volume_percent)))return false;
  if(!power_button&&cJSON_GetObjectItemCaseSensitive(patch,"wake_on_touch")&&!candidate.display_power.wake_on_touch)return false;
  destination=std::move(candidate);return true;
}
// Cloud appearance commands intentionally expose only these three settings pages.
inline bool appearance_settings_patch(const cJSON* patch) {
  if (!device_unique_object(patch) || !patch->child) return false;
  constexpr std::string_view allowed[] = {"brightness", "rotation", "theme", "custom_theme",
      "audio_enabled", "audio_volume", "audio_muted_events", "audio_preset", "start_idle", "start_active",
      "dim_for_idle", "dim_for_active", "saver_for_idle", "saver_for_active", "dim_brightness",
      "saver_animation", "shutdown_s", "dim_audio", "off_audio", "usb_power_save",
      "wake_on_orientation_change", "wake_on_touch"};
  for (auto* item = patch->child; item; item = item->next) {
    bool found = false;
    for (const auto key : allowed) if (key == item->string) found = true;
    if (!found) return false;
  }
  return true;
}
inline bool reaction_settings_patch(const cJSON* patch) {
  if (!device_unique_object(patch) || !patch->child) return false;
  for (auto* item = patch->child; item; item = item->next) {
    const std::string_view key(item->string);
    if ((key != "printer_animations_enabled" && key != "reaction_progress_bar_enabled" &&
         key != "reaction_progress_percent_enabled") || !cJSON_IsBool(item)) return false;
  }
  return true;
}
struct DeviceCommand {
  std::string action;
  DeviceJson parameters{nullptr,cJSON_Delete};
};
inline bool parse_device_command(std::string_view payload,DeviceCommand& command){
  auto root=device_json(payload);if(!device_unique_object(root.get())||cJSON_GetArraySize(root.get())!=3)return false;
  auto* version=cJSON_GetObjectItemCaseSensitive(root.get(),"schema_version");auto* action=cJSON_GetObjectItemCaseSensitive(root.get(),"action");auto* parameters=cJSON_GetObjectItemCaseSensitive(root.get(),"parameters");
  if(!device_integer(version,1,1)||!cJSON_IsString(action)||std::strlen(action->valuestring)>40||!device_unique_object(parameters))return false;
  command.action=action->valuestring;command.parameters.reset(cJSON_DetachItemFromObjectCaseSensitive(root.get(),"parameters"));return true;
}
inline bool firmware_request_id(std::string_view id) {
  if (id.size()!=36) return false;
  for (std::size_t i=0;i<id.size();++i) {
    if (i==8||i==13||i==18||i==23) { if(id[i]!='-') return false; }
    else if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f'))) return false;
  }
  return true;
}
inline bool firmware_command(const DeviceCommand& command) {
  const auto* p=command.parameters.get();
  const auto text=[&](const char* key)->std::string_view {
    const auto* item=cJSON_GetObjectItemCaseSensitive(p,key);
    return cJSON_IsString(item)?std::string_view(item->valuestring):std::string_view{};
  };
  if(!firmware_request_id(text("request_id")))return false;
  if(command.action=="firmware.check")return cJSON_GetArraySize(p)==1;
  if(command.action!="firmware.install"||cJSON_GetArraySize(p)!=3||!firmware_request_id(text("check_id")))return false;
  const auto version=text("version");
  return !version.empty()&&version.size()<=32&&version.find_first_not_of("0123456789.")==version.npos;
}
// Cloud device commands expose individually validated settings operations.
inline bool is_device_name_command(std::string_view payload) {
  DeviceCommand command;
  if (!parse_device_command(payload, command) || command.action != "device.name.set" ||
      cJSON_GetArraySize(command.parameters.get()) != 1) return false;
  const auto* name = cJSON_GetObjectItemCaseSensitive(command.parameters.get(), "name");
  return cJSON_IsString(name) && name->valuestring[0] != '\0' && valid_device_name(name->valuestring);
}
inline bool is_device_timezone_command(std::string_view payload) {
  DeviceCommand command;
  if (!parse_device_command(payload, command) || command.action != "device.timezone.set" ||
      cJSON_GetArraySize(command.parameters.get()) != 1) return false;
  const auto* zone = cJSON_GetObjectItemCaseSensitive(command.parameters.get(), "timezone");
  return cJSON_IsString(zone) && std::strlen(zone->valuestring) <= 64 && supported_timezone(zone->valuestring);
}
inline bool is_device_voice_command(std::string_view payload) {
  DeviceCommand command;
  return parse_device_command(payload, command) && command.action == "device.voice.set" &&
      cJSON_GetArraySize(command.parameters.get()) == 1 &&
      cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(command.parameters.get(), "enabled"));
}
inline bool is_device_unified_api_command(std::string_view payload) {
  DeviceCommand command;
  return parse_device_command(payload, command) && command.action == "device.unified_api.set" &&
      cJSON_GetArraySize(command.parameters.get()) == 1 &&
      cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(command.parameters.get(), "enabled"));
}
// Partial MQTT updates preserve fields changed locally since the last report.
inline bool apply_device_mqtt_patch(const cJSON* patch, MqttSettings& destination) {
  if (!device_unique_object(patch) || !patch->child) return false;
  auto candidate = destination;
  for (auto* item = patch->child; item; item = item->next) {
    const std::string_view key(item->string);
    if (key == "enabled" || key == "tls" || key == "discovery" || key == "clear_password" || key == "clear_ca") {
      if (!cJSON_IsBool(item)) return false;
      const bool value = cJSON_IsTrue(item);
      if (key == "enabled") candidate.enabled = value;
      else if (key == "tls") candidate.tls = value;
      else if (key == "discovery") candidate.discovery = value;
      else if (key == "clear_password" && value) candidate.password.clear();
      else if (key == "clear_ca" && value) candidate.ca_certificate.clear();
    } else if (key == "port") {
      if (!device_integer(item, 1, 65535)) return false;
      candidate.port = item->valueint;
    } else if (key == "host" || key == "username" || key == "password" || key == "ca_certificate") {
      if (!cJSON_IsString(item)) return false;
      if (key == "host") candidate.host = item->valuestring;
      else if (key == "username") candidate.username = item->valuestring;
      else if (key == "password") candidate.password = item->valuestring;
      else candidate.ca_certificate = item->valuestring;
    } else return false;
  }
  if (!valid_mqtt_settings(candidate)) return false;
  destination = std::move(candidate);
  return true;
}
struct DeviceCommandResult {
  int status=400;
  std::string body=R"({"error":"This action could not be understood. Refresh the page and try again."})";
};
} // namespace printdeck::core
