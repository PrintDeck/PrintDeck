#include "printdeck/platform/ctb_preview.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#ifdef ESP_PLATFORM
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#elif defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#include <openssl/sha.h>
#endif

namespace printdeck::platform {
namespace {
std::uint32_t u32(const std::uint8_t* p) {
  return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint16_t u16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
void put32(std::uint8_t* p, std::uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = v >> (i * 8); }
bool within(std::uint64_t offset, std::uint64_t length, std::uint64_t end) {
  return offset <= end && length <= end - offset;
}
bool decrypt(std::span<std::uint8_t> bytes) {
  // Fixed CTB format constants, not a user's printer or account credentials.
  constexpr std::uint8_t key[32] = {0xd0,0x5b,0x8e,0x33,0x71,0xde,0x3d,0x1a,0xe5,0x4f,0x22,0xdd,0xdf,0x5b,0xfd,0x94,0xab,0x5d,0x64,0x3a,0x9d,0x7e,0xbf,0xaf,0x42,0x03,0xf3,0x10,0xd8,0x52,0x2a,0xea};
  std::uint8_t iv[16] = {0x0f,0x01,0x0a,0x05,0x05,0x0b,0x06,0x07,0x08,0x06,0x0a,0x0c,0x0c,0x0d,0x09,0x0f};
  if (bytes.empty() || bytes.size() % 16) return false;
#ifdef ESP_PLATFORM
  mbedtls_aes_context context; mbedtls_aes_init(&context);
  const bool ok = mbedtls_aes_setkey_dec(&context, key, 256) == 0 &&
      mbedtls_aes_crypt_cbc(&context, MBEDTLS_AES_DECRYPT, bytes.size(), iv, bytes.data(), bytes.data()) == 0;
  mbedtls_aes_free(&context); return ok;
#elif defined(__APPLE__)
  std::size_t count = 0;
  return CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, key, sizeof(key), iv,
      bytes.data(), bytes.size(), bytes.data(), bytes.size(), &count) == kCCSuccess && count == bytes.size();
#else
  auto* context = EVP_CIPHER_CTX_new(); if (!context) return false;
  int count = 0, last = 0;
  bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_cbc(), nullptr, key, iv) == 1 &&
      EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
      EVP_DecryptUpdate(context, bytes.data(), &count, bytes.data(), bytes.size()) == 1 &&
      EVP_DecryptFinal_ex(context, bytes.data() + count, &last) == 1 && count + last == int(bytes.size());
  EVP_CIPHER_CTX_free(context); return ok;
#endif
}
bool signature_matches(std::span<const std::uint8_t> settings, std::span<const std::uint8_t> signature) {
  std::array<std::uint8_t, 32> hash{};
#ifdef ESP_PLATFORM
  if (mbedtls_sha256(settings.data(), 8, hash.data(), 0) != 0) return false;
#elif defined(__APPLE__)
  CC_SHA256(settings.data(), 8, hash.data());
#else
  SHA256(settings.data(), 8, hash.data());
#endif
  return std::equal(hash.begin(), hash.end(), signature.begin());
}
std::vector<std::uint8_t> bitmap(unsigned w, unsigned h) {
  const unsigned stride = (w * 3 + 3) & ~3U;
  std::vector<std::uint8_t> result(54 + stride * h);
  result[0] = 'B'; result[1] = 'M'; put32(result.data() + 2, result.size());
  put32(result.data() + 10, 54); put32(result.data() + 14, 40);
  put32(result.data() + 18, w); put32(result.data() + 22, 0U - h);
  result[26] = 1; result[28] = 24;
  return result;
}
void output_size(unsigned w, unsigned h, unsigned& ow, unsigned& oh) {
  constexpr unsigned maximum = 320;
  ow = w; oh = h;
  if (w > maximum || h > maximum) {
    if (w >= h) { ow = maximum; oh = std::max(1U, h * maximum / w); }
    else { oh = maximum; ow = std::max(1U, w * maximum / h); }
  }
}
}

std::optional<std::string> ctb_preview_path(std::string_view path) {
  if (path.empty() || path.size() > 512 || path.front() != '/') return {};
  std::string normalized;
  while (!path.empty()) {
    if (path.front() == '/') { path.remove_prefix(1); continue; }
    const auto slash = path.find('/'); const auto part = path.substr(0, slash);
    if (part == "." || part == "..") return {};
    normalized += '/';
    for (unsigned char c : part) {
      if (c < 32 || c == 127 || c == '\\') return {};
      // Escape every URL metacharacter, including literal percent signs.
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') normalized += char(c);
      else { constexpr char hex[] = "0123456789ABCDEF"; normalized += '%'; normalized += hex[c >> 4]; normalized += hex[c & 15]; }
    }
    path.remove_prefix(slash == path.npos ? path.size() : slash);
  }
  auto suffix = normalized.substr(normalized.size() > 4 ? normalized.size() - 4 : 0);
  for (auto& c : suffix) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  if (suffix != ".ctb" || !(normalized.starts_with("/media/emmc/") ||
      normalized.starts_with("/media/emmc0/") || normalized.starts_with("/media/usb/"))) return {};
  return normalized;
}
bool ctb_preview_range(std::string_view header, std::uint64_t offset, std::size_t length, std::uint64_t size) {
  return length && within(offset, length, size) && header == "bytes " + std::to_string(offset) + "-" +
      std::to_string(offset + length - 1) + "/" + std::to_string(size);
}
bool ctb_exposure_preview_matches(std::string_view source, std::string_view current,
    unsigned source_index, unsigned current_index, bool exposing,
    std::uint64_t status_at, std::uint64_t image_at, std::uint64_t now, std::uint64_t hold_until) {
  return (exposing ? source_index == current_index : now < hold_until) && !source.empty() && source == current &&
      status_at > image_at && now >= status_at && now - status_at <= 2000;
}
bool ctb_preview_header(const CtbRead& read, std::uint64_t size, CtbHeader& out) {
  out = {};
  if (size < 368 || size > 16ULL * 1024 * 1024 * 1024) return false;
  std::array<std::uint8_t, 48> raw{};
  if (!read(0, raw) || u32(raw.data()) != 0x12fd0107 || u32(raw.data() + 4) != 288 ||
      u32(raw.data() + 8) != 48 || u32(raw.data() + 12) != 0 ||
      (u32(raw.data() + 16) != 4 && u32(raw.data() + 16) != 5) || u32(raw.data() + 20) != 32) return false;
  const std::uint64_t sig = u32(raw.data() + 24);
  if (sig < 336 || !within(sig, 32, size)) return false;
  std::array<std::uint8_t, 288> settings{}; std::array<std::uint8_t, 32> signature{};
  if (!read(48, settings) || !read(sig, signature) || !decrypt(settings) || !decrypt(signature) ||
      !signature_matches(settings, signature)) return false;
  CtbHeader value;
  value.size = size; value.signature = sig; value.table = u32(settings.data() + 8);
  value.width = u32(settings.data() + 56); value.height = u32(settings.data() + 60);
  value.layers = u32(settings.data() + 64); value.seed = u32(settings.data() + 128);
  value.model[0] = u32(settings.data() + 68); value.model[1] = u32(settings.data() + 72);
  if (!value.width || !value.height || value.width > 32768 || value.height > 32768 ||
      std::uint64_t(value.width) * value.height > 160000000 || !value.layers || value.layers > 65535 ||
      value.table < 336 || !within(value.table, std::uint64_t(value.layers) * 16, sig)) return false;
  out = value; return true;
}

std::vector<std::uint8_t> ctb_model_preview(const CtbRead& read, const CtbHeader& h, const CtbCancel& cancelled) {
  const auto descriptor = h.model[0] ? h.model[0] : h.model[1];
  if (cancelled() || descriptor < 336 || !within(descriptor, 16, h.table)) return {};
  std::array<std::uint8_t, 16> raw{}; if (!read(descriptor, raw)) return {};
  const auto w = u32(raw.data()), height = u32(raw.data() + 4), offset = u32(raw.data() + 8), length = u32(raw.data() + 12);
  if (!w || !height || w > 2048 || height > 2048 || w * height > 2000000 ||
      !length || length > kCtbImageLimit || length % 2 || offset < descriptor + 16 || !within(offset, length, h.table)) return {};
  const auto other = h.model[0] ? h.model[1] : 0;
  if (other && (other < 336 || !within(other, 16, h.table) ||
      !(other + 16 <= descriptor || descriptor + 16 <= other) ||
      !(other + 16 <= offset || offset + length <= other))) return {};
  std::vector<std::uint8_t> bytes(length); if (!read(offset, bytes)) return {};
  unsigned ow, oh; output_size(w, height, ow, oh); auto result = bitmap(ow, oh);
  const auto stride = (ow * 3 + 3) & ~3U;
  std::uint32_t position = 0, runs = 0;
  for (std::size_t i = 0; i < bytes.size();) {
    if (++runs > 1000000 || ((runs & 1023) == 1 && cancelled())) return {};
    const auto code = u16(bytes.data() + i); i += 2; unsigned count = 1;
    if (code & 0x20) {
      if (bytes.size() - i < 2) return {};
      const auto repeat = u16(bytes.data() + i); i += 2;
      if ((repeat & 0xf000) != 0x3000) return {};
      count = (repeat & 0xfff) + 1;
    }
    if (count > w * height - position) return {};
    // Nearest sample preserves the slicer's RGB model colors and row order.
    for (unsigned end = position + count; position < end; ++position) {
      const auto x = position % w, y = position / w;
      const auto ox = x * ow / w, oy = y * oh / height;
      if (x != (ox * w + ow - 1) / ow || y != (oy * height + oh - 1) / oh) continue;
      auto* pixel = result.data() + 54 + oy * stride + ox * 3;
      pixel[0] = (code & 31) << 3; pixel[1] = ((code >> 6) & 31) << 3; pixel[2] = ((code >> 11) & 31) << 3;
    }
  }
  return position == w * height && !cancelled() ? result : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> ctb_layer_preview(const CtbRead& read, const CtbHeader& h, std::uint32_t index, const CtbCancel& cancelled) {
  if (cancelled() || index >= h.layers) return {};
  std::array<std::uint8_t, 32> pointers{};
  const auto count = index + 1 < h.layers ? 32 : 16;
  if (!read(h.table + std::uint64_t(index) * 16, std::span(pointers).first(count))) return {};
  const auto pointer = [&](const std::uint8_t* p) -> std::uint64_t {
    const auto offset = u32(p) | (std::uint64_t(u32(p + 4)) << 32);
    return u32(p + 8) == 88 && offset >= h.table + std::uint64_t(h.layers) * 16 &&
        within(offset, 88, h.signature) ? offset : 0;
  };
  const auto offset = pointer(pointers.data());
  const auto next = count == 32 ? pointer(pointers.data() + 16) : h.signature;
  if (!offset || !next || !within(offset, 88, next)) return {};
  std::array<std::uint8_t, 88> record{};
  if (!read(offset, record) || u32(record.data()) != 88) return {};
  const auto image = u32(record.data() + 16) | (std::uint64_t(u32(record.data() + 20)) << 32);
  const auto length = u32(record.data() + 24), aes_offset = u32(record.data() + 32), aes_length = u32(record.data() + 36);
  if (!length || length > kCtbImageLimit || image < offset + 88 || !within(image, length, next) ||
      aes_length % 16 || !within(aes_offset, aes_length, length) || (aes_length && length < 512 && length % 16)) return {};
  std::vector<std::uint8_t> bytes(length);
  if (!read(image, bytes) || (aes_length && !decrypt(std::span(bytes).subspan(aes_offset, aes_length)))) return {};
  if (h.seed) {
    const std::uint32_t step = h.seed * 0x2d83cdacU + 0xd8a83423U;
    std::uint32_t key = (index * 0x1e1530cdU + 0xec3d47cdU) * step;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] ^= key >> (8 * (i % 4)); if (i % 4 == 3) key += step;
    }
  }
  unsigned ow, oh; output_size(h.width, h.height, ow, oh); auto result = bitmap(ow, oh);
  const auto stride = (ow * 3 + 3) & ~3U;
  const auto pixels = h.width * h.height;
  std::uint32_t position = 0, runs = 0;
  for (std::size_t i = 0; i < bytes.size();) {
    if (++runs > 1000000 || ((runs & 1023) == 1 && cancelled())) return {};
    const auto code = bytes[i++]; unsigned length = 1;
    if (code & 128) {
      if (i == bytes.size()) return {};
      const auto prefix = bytes[i++]; unsigned extra = 0;
      if (prefix < 128) length = prefix;
      else if (prefix < 192) { extra = 1; length = prefix & 63; }
      else if (prefix < 224) { extra = 2; length = prefix & 31; }
      else if (prefix < 240) { extra = 3; length = prefix & 15; }
      else return {};
      if (extra > bytes.size() - i) return {};
      while (extra--) length = (length << 8) | bytes[i++];
    }
    if (!length || length > pixels - position) return {};
    const auto gray = code & 127; const auto value = gray ? (gray << 1) | 1 : 0;
    const auto end = position + length;
    if (!value) { position = end; continue; }
    while (position < end) {
      const auto row = position / h.width, x = position % h.width;
      if ((row & 63) == 0 && cancelled()) return {};
      const auto row_end = std::min(end, (row + 1) * h.width);
      const auto first = x * ow / h.width, last = (row_end - row * h.width - 1) * ow / h.width;
      auto* output = result.data() + 54 + row * oh / h.height * stride;
      for (unsigned ox = first; ox <= last; ++ox)
        for (unsigned c = 0; c < 3; ++c) output[ox * 3 + c] = std::max<unsigned>(output[ox * 3 + c], value);
      position = row_end;
    }
  }
  return position == pixels && !cancelled() ? result : std::vector<std::uint8_t>{};
}
}  // namespace printdeck::platform
