#pragma once

#include <algorithm>
#include <array>
#include <limits>
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

// Some printer FTPS servers reject REST. Their bounded sequential prefix can
// still contain the complete root model entry. Never infer a title from object
// names or accept a partially inflated/CRC-unchecked metadata file.
struct PrefixDiagnostic {
  const char* reason = "header-incomplete";
  unsigned entry = 0;
  std::uint64_t offset = 0, compressed = 0, raw = 0, flags = 0;
  bool root_model = false;
  bool incomplete = true;
  std::size_t needed = 30;
};
inline std::string read_prefix_entry(std::string_view prefix, std::string_view target,
    PrefixDiagnostic* diagnostic = nullptr, std::size_t compressed_limit = 128 * 1024) {
  PrefixDiagnostic state;
  const auto reject = [&](const char* reason, bool incomplete = false,
                          std::size_t needed = 0) -> std::string {
    state.reason = reason; state.incomplete = incomplete; state.needed = needed;
    if (diagnostic) *diagnostic = state;
    return {};
  };
  // Reparse only after enough bytes arrive, doubling unknown-length input.
  // This bounds repeated inflation while keeping the transfer itself small.
  const auto more = [&](const char* reason, std::size_t start) {
    const auto available = prefix.size() - start;
    return reject(reason, true, start + std::max<std::size_t>(2048, available * 2));
  };
  std::size_t at = 0;
  std::size_t inflated_total = 0;
  for (unsigned entries = 0; entries < 512; ++entries) {
    state = {}; state.entry = entries; state.offset = at;
    if (prefix.size() - at < 30) return reject("next-header-incomplete", true, at + 30);
    if (prefix.substr(at, 4) != std::string_view("PK\3\4", 4)) return reject("local-header-signature");
    const auto flags = number(prefix, at + 6, 2);
    const auto method = number(prefix, at + 8, 2);
    auto crc = number(prefix, at + 14, 4);
    auto compressed = number(prefix, at + 18, 4);
    auto raw = number(prefix, at + 22, 4);
    const bool descriptor = (flags & 8) != 0;
    const bool zip64 = compressed == 0xffffffff || raw == 0xffffffff;
    const auto name_size = number(prefix, at + 26, 2);
    const auto extra_size = number(prefix, at + 28, 2);
    const auto data_offset = at + 30 + name_size + extra_size;
    state.flags = flags; state.compressed = compressed; state.raw = raw;
    if (data_offset > prefix.size()) return reject("entry-header-incomplete", true, data_offset);
    state.root_model = prefix.substr(at + 30, name_size) == target;
    if (flags & ~0x808ULL) return reject("unsupported-flags");
    auto extra = prefix.substr(at + 30 + name_size, extra_size);
    for (std::size_t pos = 0; pos + 4 <= extra.size();) {
      const auto length = number(extra, pos + 2, 2);
      if (length > extra.size() - pos - 4) return reject("extra-field-invalid");
      if (number(extra, pos, 2) == 1) {
        std::size_t field = pos + 4;
        for (auto* value : {&raw, &compressed}) if (*value == 0xffffffff) {
          if (field + 8 > pos + 4 + length) return reject("zip64-field-incomplete");
          *value = number(extra, field, 8); field += 8;
        }
        break;
      }
      pos += 4 + length;
    }
    state.compressed = compressed; state.raw = raw;
    std::string xml;
    std::size_t descriptor_bytes = 0;
    if (descriptor) {
      const auto data = prefix.substr(data_offset);
      const unsigned size_width = zip64 ? 8 : 4;
      const std::size_t descriptor_size = 4 + size_width * 2;
      if (method == 8) {
        z_stream stream{};
        if (data.size() > std::numeric_limits<uInt>::max()) return reject("input-limit");
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        stream.avail_in = static_cast<uInt>(data.size());
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return reject("inflate-init");
        std::array<Bytef, 2048> output{};
        uLong checksum = crc32(0, nullptr, 0);
        int status = Z_OK;
        bool over_limit = false;
        do {
          stream.next_out = output.data(); stream.avail_out = output.size();
          const auto before_in = stream.total_in, before_out = stream.total_out;
          status = inflate(&stream, Z_NO_FLUSH);
          const auto produced = output.size() - stream.avail_out;
          checksum = crc32(checksum, output.data(), produced);
          // Validate skipped entries without retaining their expanded content.
          over_limit = stream.total_out + inflated_total > 4 * 1024 * 1024 ||
              (state.root_model && (stream.total_out > 512 * 1024 || stream.total_in > compressed_limit));
          if (over_limit) break;
          if (state.root_model) xml.append(reinterpret_cast<const char*>(output.data()), produced);
          if (stream.total_in == before_in && stream.total_out == before_out) break;
        } while (status == Z_OK);
        compressed = stream.total_in; raw = stream.total_out; crc = checksum;
        const bool truncated = (status == Z_OK || status == Z_BUF_ERROR) && stream.avail_in == 0;
        inflateEnd(&stream);
        state.compressed = compressed; state.raw = raw;
        if (over_limit) return reject("inflate-size-limit");
        if (status != Z_STREAM_END) {
          if (truncated) return more("deflate-incomplete", data_offset);
          return reject("inflate-invalid");
        }
        inflated_total += raw;
        auto pos = static_cast<std::size_t>(compressed);
        if (data.size() - pos < 4) return reject("descriptor-incomplete", true, data_offset + pos + descriptor_size);
        const bool signed_descriptor = number(data, pos, 4) == 0x08074b50;
        if (signed_descriptor) pos += 4;
        if (data.size() - pos < descriptor_size)
          return reject("descriptor-incomplete", true, data_offset + pos + descriptor_size);
        if (number(data, pos, 4) != crc || number(data, pos + 4, size_width) != compressed ||
            number(data, pos + 4 + size_width, size_width) != raw) return reject("descriptor-mismatch");
        descriptor_bytes = descriptor_size + (signed_descriptor ? 4 : 0);
      } else if (method == 0) {
        // Stored streams have no deflate terminator. Accept a descriptor only
        // when both sizes match its exact offset and its CRC verifies the data.
        bool found = false;
        for (std::size_t pos = 0; pos + descriptor_size <= data.size(); ++pos) {
          const bool signed_descriptor = number(data, pos, 4) == 0x08074b50;
          const auto fields = pos + (signed_descriptor ? 4 : 0);
          if (fields + descriptor_size > data.size()) continue;
          if (number(data, fields + 4, size_width) != pos ||
              number(data, fields + 4 + size_width, size_width) != pos) continue;
          const auto checksum = crc32(0, reinterpret_cast<const Bytef*>(data.data()), pos);
          if (number(data, fields, 4) != checksum) continue;
          compressed = raw = pos; crc = checksum;
          descriptor_bytes = descriptor_size + (signed_descriptor ? 4 : 0);
          found = true; break;
        }
        if (!found) return more("stored-descriptor-incomplete", data_offset);
        if (state.root_model) {
          if (raw > compressed_limit) return reject("root-model-compressed-limit");
          xml.assign(data.substr(0, raw));
        }
      } else return reject("compression-unsupported");
    } else {
      if (compressed > prefix.size() - data_offset)
        return reject("entry-data-incomplete", true, data_offset + compressed);
      if (state.root_model) {
        if (!raw || !compressed) return reject("root-model-empty");
        if (raw > 512 * 1024) return reject("root-model-raw-limit");
        if (compressed > compressed_limit) return reject("root-model-compressed-limit");
        auto data = prefix.substr(data_offset, compressed);
        if (method == 0) {
          if (raw != compressed) return reject("stored-size-mismatch");
          xml.assign(data);
        } else if (method == 8) {
          xml.resize(raw);
          z_stream stream{};
          stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
          stream.avail_in = data.size();
          stream.next_out = reinterpret_cast<Bytef*>(xml.data());
          stream.avail_out = xml.size();
          if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return reject("inflate-init");
          const int status = inflate(&stream, Z_FINISH);
          const bool valid = status == Z_STREAM_END && stream.total_in == compressed && stream.total_out == raw;
          inflateEnd(&stream);
          if (!valid) return reject("inflate-invalid");
        } else return reject("compression-unsupported");
        if (crc32(0, reinterpret_cast<const Bytef*>(xml.data()), xml.size()) != crc) return reject("crc-mismatch");
      }
    }
    if (state.root_model) {
      state.compressed = compressed; state.raw = raw;
      state.reason = "complete";
      state.incomplete = false;
      if (diagnostic) *diagnostic = state;
      return xml;
    }
    at = data_offset + compressed + descriptor_bytes;
  }
  return reject("entry-count-limit");
}

inline BambuJobMetadata read_prefix(std::string_view prefix, PrefixDiagnostic* diagnostic = nullptr) {
  auto bytes = read_prefix_entry(prefix, "3D/3dmodel.model", diagnostic);
  auto metadata = parse(bytes);
  if (diagnostic && !bytes.empty() && metadata.title.empty()) diagnostic->reason = "root-title-empty";
  return metadata;
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
