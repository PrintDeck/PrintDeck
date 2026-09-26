#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "zlib.h"

namespace printdeck::platform {

struct BambuJobMetadata { std::string title, profile; };

namespace bambu_metadata {
inline std::uint64_t number(std::string_view bytes, std::size_t at, unsigned length) {
  if (at > bytes.size() || length > bytes.size() - at) return 0;
  std::uint64_t result = 0;
  for (unsigned i = 0; i < length; ++i)
    result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at+i])) << (i*8);
  return result;
}

inline std::string xml_text(std::string_view text) {
  if (text.size() > 4096) return {};
  std::string result;
  for (std::size_t i = 0; i < text.size(); ++i) {
    unsigned char c = text[i];
    if (c == '<' || c == 0 || (c < 32 && c != '\t' && c != '\n' && c != '\r')) return {};
    if (c != '&') { result += c < 32 ? ' ' : static_cast<char>(c); continue; }
    const auto end = text.find(';', i+1);
    if (end == text.npos || end-i > 12) return {};
    const auto entity = text.substr(i+1, end-i-1);
    std::uint32_t cp = 0;
    if (entity == "amp") cp = '&';
    else if (entity == "lt") cp = '<';
    else if (entity == "gt") cp = '>';
    else if (entity == "quot") cp = '"';
    else if (entity == "apos") cp = '\'';
    else if (entity.starts_with("#")) {
      const bool hex = entity.size() > 1 && entity[1] == 'x';
      const auto digits = entity.substr(hex ? 2 : 1);
      if (digits.empty()) return {};
      for (char digit : digits) {
        const unsigned value = digit >= '0' && digit <= '9' ? digit-'0' :
            digit >= 'a' && digit <= 'f' ? digit-'a'+10 : digit >= 'A' && digit <= 'F' ? digit-'A'+10 : 99;
        if (value >= (hex ? 16U : 10U) || cp > 0x10ffffU / (hex ? 16U : 10U)) return {};
        cp = cp * (hex ? 16 : 10) + value;
      }
    } else return {};
    if (cp < 32 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return {};
    if (cp < 0x80) result += static_cast<char>(cp);
    else {
      if (cp >= 0x10000) result += static_cast<char>(0xf0 | (cp >> 18));
      if (cp >= 0x800) result += static_cast<char>((cp >= 0x10000 ? 0x80 : 0xe0) | ((cp >> 12) & 0x3f));
      result += static_cast<char>((cp >= 0x800 ? 0x80 : 0xc0) | ((cp >> 6) & 0x3f));
      result += static_cast<char>(0x80 | (cp & 0x3f));
    }
    i = end;
  }
  return result;
}

inline BambuJobMetadata parse(std::string_view xml) {
  BambuJobMetadata result;
  // Only root metadata, before geometry; no DTD, entities or external resources.
  if (xml.find("<!DOCTYPE") != xml.npos || xml.find("<!ENTITY") != xml.npos) return result;
  xml = xml.substr(0, xml.find("<resources"));
  std::size_t at = 0;
  while ((at = xml.find("<metadata", at)) != xml.npos) {
    const auto end = xml.find('>', at), close = xml.find("</metadata>", at);
    if (end == xml.npos || close == xml.npos || close < end) break;
    const auto tag = xml.substr(at, end-at);
    const auto attr = tag.find("name=");
    if (attr != tag.npos && attr+6 < tag.size() && (tag[attr+5] == '"' || tag[attr+5] == '\'')) {
      const auto quote = tag.find(tag[attr+5], attr+6);
      if (quote != tag.npos) {
        const auto name = tag.substr(attr+6, quote-attr-6);
        if (name == "Title" || name == "ProfileTitle") {
          auto value = xml_text(xml.substr(end+1, close-end-1));
          if (name == "Title") result.title = std::move(value);
          else result.profile = std::move(value);
        }
      }
    }
    at = close+11;
  }
  return result;
}

// Bounded range reads avoid downloading geometry or G-code. ZIP64 offsets are
// accepted only within the same small archive/file limits as ordinary ZIP.
using Reader = std::function<bool(std::uint64_t, std::size_t, std::string&)>;
inline BambuJobMetadata read(std::uint64_t size, const Reader& read_range) {
  constexpr std::size_t limit = 128 * 1024;
  const auto range = [&](std::uint64_t at, std::size_t length, std::string& out) {
    return length && length <= limit && at <= size && length <= size-at &&
        read_range(at, length, out) && out.size() == length;
  };
  if (size < 22 || size > 512ULL*1024*1024) return {};
  std::string tail;
  const auto tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(size, 65557+76));
  if (!range(size-tail_size, tail_size, tail)) return {};
  auto end = tail.rfind(std::string("PK\5\6", 4));
  if (end == tail.npos || end+22 > tail.size() || end+22+number(tail,end+20,2) != tail.size() ||
      number(tail,end+4,2) || number(tail,end+6,2)) return {};
  std::uint64_t count = number(tail,end+10,2), bytes = number(tail,end+12,4), offset = number(tail,end+16,4);
  if (number(tail,end+8,2) != count) return {};
  if (count == 65535 || bytes == 0xffffffff || offset == 0xffffffff) {
    if (end < 20 || tail.compare(end-20,4,std::string("PK\6\7",4)) ||
        number(tail,end-16,4) || number(tail,end-4,4) != 1) return {};
    std::string zip64;
    if (!range(number(tail,end-12,8),56,zip64) || zip64.compare(0,4,std::string("PK\6\6",4)) ||
        number(zip64,4,8) < 44 || number(zip64,16,4) || number(zip64,20,4) ||
        number(zip64,24,8) != number(zip64,32,8)) return {};
    count=number(zip64,32,8); bytes=number(zip64,40,8); offset=number(zip64,48,8);
  }
  if (!count || count > 512 || bytes > limit) return {};
  std::string directory;
  if (!range(offset,bytes,directory)) return {};
  std::size_t cursor=0;
  for (std::uint64_t index=0; index<count; ++index) {
    if (cursor+46 > directory.size() || directory.compare(cursor,4,std::string("PK\1\2",4))) return {};
    const auto name_size=number(directory,cursor+28,2), extra_size=number(directory,cursor+30,2);
    const auto next=cursor+46+name_size+extra_size+number(directory,cursor+32,2);
    if (next > directory.size()) return {};
    if (std::string_view(directory).substr(cursor+46,name_size) == "3D/3dmodel.model") {
      const auto flags=number(directory,cursor+8,2), method=number(directory,cursor+10,2), crc=number(directory,cursor+16,4);
      if ((flags & ~0x808ULL) || (method != 0 && method != 8) || number(directory,cursor+34,2)) return {};
      auto compressed=number(directory,cursor+20,4), raw=number(directory,cursor+24,4), local=number(directory,cursor+42,4);
      auto extra=std::string_view(directory).substr(cursor+46+name_size,extra_size);
      for (std::size_t pos=0; pos+4<=extra.size();) {
        const auto length=number(extra,pos+2,2);
        if (length > extra.size()-pos-4) return {};
        if (number(extra,pos,2)==1) {
          std::size_t field=pos+4;
          for (auto* value : {&raw,&compressed,&local}) if (*value==0xffffffff) {
            if (field+8 > pos+4+length) return {};
            *value=number(extra,field,8); field+=8;
          }
          break;
        }
        pos+=4+length;
      }
      if (!raw || raw > 512*1024 || !compressed || compressed > limit) return {};
      std::string header;
      if (!range(local,30,header) || header.compare(0,4,std::string("PK\3\4",4)) ||
          number(header,6,2)!=flags || number(header,8,2)!=method || number(header,26,2)!=name_size) return {};
      std::string name;
      if (!range(local+30,name_size,name) || name!="3D/3dmodel.model") return {};
      const auto data_offset=local+30+name_size+number(header,28,2);
      std::string data;
      if (!range(data_offset,compressed,data)) return {};
      std::string xml;
      if (method==0) { if (compressed!=raw) return {}; xml=std::move(data); }
      else {
        xml.resize(raw);
        z_stream stream{};
        stream.next_in=reinterpret_cast<Bytef*>(data.data()); stream.avail_in=data.size();
        stream.next_out=reinterpret_cast<Bytef*>(xml.data()); stream.avail_out=xml.size();
        if (inflateInit2(&stream,-MAX_WBITS)!=Z_OK) return {};
        const int status=inflate(&stream,Z_FINISH);
        const bool valid=status==Z_STREAM_END && stream.total_in==compressed && stream.total_out==raw;
        inflateEnd(&stream);
        if (!valid) return {};
      }
      if (crc32(0,reinterpret_cast<const Bytef*>(xml.data()),xml.size())!=crc) return {};
      return parse(xml);
    }
    cursor=next;
  }
  return {};
}
}  // namespace bambu_metadata
}  // namespace printdeck::platform
