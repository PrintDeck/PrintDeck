#pragma once
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include "printdeck/core/device_state.hpp"

namespace printdeck::core {
inline constexpr std::uint64_t kGcodeFileLimit = 16ULL*1024*1024*1024;
inline constexpr std::uint32_t kGcodeChunkLimit = 32768;
inline bool gcode_fdm(PrinterProtocol p) {
  return p != PrinterProtocol::uniformation_sdcp && p != PrinterProtocol::tinymaker;
}
inline bool gcode_uint(std::string_view text, std::uint64_t& value) {
  if(text.empty() || text.size()>20) return false;
  const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
  return result.ec==std::errc{} && result.ptr==text.data()+text.size();
}
inline bool gcode_plain(std::string_view file) {
  if(file.size()<7) return false;
  auto suffix=file.substr(file.size()-6);
  constexpr std::string_view ext=".gcode";
  for(unsigned i=0;i<6;++i) if((suffix[i]>='A'&&suffix[i]<='Z'?suffix[i]+32:suffix[i])!=ext[i])return false;
  return true;
}
inline std::optional<std::string> gcode_encoded_path(std::string_view raw) {
  if(raw.empty() || raw.size()>512) return {};
  if(raw.front()=='/')raw.remove_prefix(1);
  if(raw.empty())return {};
  std::string out;
  constexpr char hex[]="0123456789ABCDEF";
  std::size_t at=0;
  while(at<raw.size()){
    const auto end=raw.find('/',at);const auto part=raw.substr(at,end==raw.npos?raw.size()-at:end-at);
    if(part.empty()||part=="."||part=="..")return {};
    for(unsigned char c:part){
      if(c<32||c==127||c=='\\'||c==':'||c=='%'||c=='?'||c=='#')return {};
      if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')out+=char(c);
      else{out+='%';out+=hex[c>>4];out+=hex[c&15];}
    }
    if(end==raw.npos)break;
    out+='/';at=end+1;if(at==raw.size())return {};
  }
  return out;
}
// Paths come from the selected adapter's job, never a browser URL or filename.
inline std::optional<std::string> gcode_http_path(PrinterProtocol protocol,std::string_view file) {
  if(protocol==PrinterProtocol::octoprint){if(!file.starts_with("local:"))return {};file.remove_prefix(6);}
  if(!gcode_plain(file))return {};
  const auto path=gcode_encoded_path(file);if(!path)return {};
  switch(protocol){
    case PrinterProtocol::moonraker:return "/server/files/gcodes/"+*path;
    case PrinterProtocol::octoprint:return "/downloads/files/local/"+*path;
    // PrusaLink file.path includes the storage root, e.g. /usb/example.gcode.
    case PrinterProtocol::prusalink:return "/"+*path;
    case PrinterProtocol::elegoo_sdcp:return "/downloadFile/"+*path;
    case PrinterProtocol::elegoo_cc2:return "/download/udisk/"+*path;
    default:return {};
  }
}
// Download references are accepted only on the configured origin and known file routes.
inline std::optional<std::string> gcode_prusa_download(std::string_view reference, std::string_view origin) {
  if (reference.starts_with(origin) && reference.size() > origin.size() && reference[origin.size()] == '/')
    reference.remove_prefix(origin.size());
  if (reference.size() > 512 || reference.find_first_of("?#\\\r\n") != reference.npos) return {};
  std::string decoded;
  auto nibble=[](char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;};
  for(std::size_t i=0;i<reference.size();++i){
    unsigned char c=reference[i];
    if(c=='%'){
      if(i+2>=reference.size()||nibble(reference[i+1])<0||nibble(reference[i+2])<0)return {};
      c=nibble(reference[i+1])*16+nibble(reference[i+2]);i+=2;
      if(c=='/'||c=='%')return {};
    }
    decoded+=char(c);
  }
  auto file=std::string_view(decoded);
  if(file.starts_with("/api/files/local/")||file.starts_with("/api/files/usb/")){
    if(!file.ends_with("/raw"))return {};
    file.remove_suffix(4);
  }else if(!file.starts_with("/usb/"))return {};
  if(!gcode_plain(file))return {};
  auto encoded=gcode_encoded_path(decoded);
  return encoded?std::optional<std::string>("/"+*encoded):std::nullopt;
}
inline std::optional<std::string> gcode_bambu_path(std::string_view file, std::string_view hint) {
  // A plate_N.gcode inside a 3MF is not an independently downloadable original file.
  if(!hint.empty())file=hint;
  if(!gcode_plain(file)||file.starts_with("Metadata/")||file.starts_with("/Metadata/")||!gcode_encoded_path(file))return {};
  return file.starts_with('/')?std::string(file):"/"+std::string(file);
}
inline std::optional<std::string> gcode_query_token(std::string_view value){
  if(value.empty()||value.size()>128)return {};
  std::string out;constexpr char hex[]="0123456789ABCDEF";
  for(unsigned char c:value){
    if(c<32||c==127)return {};
    if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=char(c);
    else{out+='%';out+=hex[c>>4];out+=hex[c&15];}
  }
  return out;
}
inline std::optional<unsigned> gcode_pasv_port(std::string_view reply) {
  const auto start=reply.find('('),end=reply.find(')');
  if(start==reply.npos||end==reply.npos||end<=start)return {};
  auto fields=reply.substr(start+1,end-start-1);unsigned port=0;
  for(unsigned i=0;i<6;++i){
    const auto comma=fields.find(',');std::uint64_t n=0;
    if((i<5&&comma==fields.npos)||(i==5&&comma!=fields.npos)||!gcode_uint(fields.substr(0,comma),n)||n>255)return {};
    if(i>=4)port=port*256+static_cast<unsigned>(n);
    if(i<5)fields.remove_prefix(comma+1);
  }
  return port>=1024?std::optional<unsigned>(port):std::nullopt;
}
inline bool gcode_range(std::string_view text,std::uint64_t offset,std::uint32_t length,std::uint64_t& size){
  if(!length||length>kGcodeChunkLimit||offset>kGcodeFileLimit-length||!text.starts_with("bytes "))return false;
  text.remove_prefix(6);const auto dash=text.find('-'),slash=text.find('/');
  if(dash==text.npos||slash==text.npos||dash>=slash)return false;
  std::uint64_t first=0,last=0,total=0;
  if(!gcode_uint(text.substr(0,dash),first)||!gcode_uint(text.substr(dash+1,slash-dash-1),last)||!gcode_uint(text.substr(slash+1),total))return false;
  if(first!=offset||last!=offset+length-1||total<=last||total>kGcodeFileLimit)return false;
  size=total;return true;
}
} // namespace printdeck::core
