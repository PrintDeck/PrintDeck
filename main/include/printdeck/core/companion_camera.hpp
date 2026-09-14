#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
namespace printdeck::core {
inline constexpr std::size_t kMaximumCompanionCameras = 8;
struct CompanionCamera {
  std::string id;
  std::string name;
  std::string host;
  std::vector<std::uint32_t> printers;
  bool assigned(std::uint32_t printer) const {
    return std::find(printers.begin(), printers.end(), printer) != printers.end();
  }
};
inline bool valid_companion_camera(const CompanionCamera& camera) {
  if (camera.id.size() != 36 || camera.name.empty() || camera.name.size() > 64 ||
      camera.host.empty() || camera.host.size() > 253 || camera.printers.size() > 10) return false;
  for (std::size_t i=0; i<camera.id.size(); ++i) {
    const char c=camera.id[i];
    if (i==8 || i==13 || i==18 || i==23) { if (c!='-') return false; }
    else if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) return false;
  }
  for (unsigned char c:camera.name) if (c<32 || c==127) return false;
  for (char c:camera.host)
    if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='.' || c=='-')) return false;
  for (std::size_t i=0;i<camera.printers.size();++i)
    if (!camera.printers[i] || std::find(camera.printers.begin(),camera.printers.begin()+i,camera.printers[i])!=camera.printers.begin()+i) return false;
  return true;
}
inline bool assign_companion_camera(std::vector<CompanionCamera>& cameras,
                                   CompanionCamera camera, std::uint32_t printer) {
  if (!valid_companion_camera(camera) || !printer) return false;
  auto found=std::find_if(cameras.begin(),cameras.end(),[&](const auto& c){return c.id==camera.id;});
  if (found==cameras.end()) {
    if (cameras.size()>=kMaximumCompanionCameras) return false;
    camera.printers.clear(); camera.printers.push_back(printer); cameras.push_back(std::move(camera));
  } else {
    if (!found->assigned(printer)) {
      if (found->printers.size()>=10) return false;
      found->printers.push_back(printer);
    }
    found->name=camera.name; found->host=camera.host;
  }
  return true;
}
inline bool unassign_companion_camera(std::vector<CompanionCamera>& cameras,
                                      const std::string& id, std::uint32_t printer) {
  auto camera=std::find_if(cameras.begin(),cameras.end(),[&](const auto& c){return c.id==id;});
  if(camera==cameras.end() || !printer)return false;
  camera->printers.erase(std::remove(camera->printers.begin(),camera->printers.end(),printer),camera->printers.end());
  return true;
}
inline bool forget_companion_camera(std::vector<CompanionCamera>& cameras,const std::string& id) {
  const auto count=cameras.size();
  cameras.erase(std::remove_if(cameras.begin(),cameras.end(),[&](const auto& c){return c.id==id;}),cameras.end());
  return cameras.size()!=count;
}
} // namespace printdeck::core
