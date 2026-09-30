#include "printdeck/platform/preview_png.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include "png.h"

namespace printdeck::platform {
namespace {
constexpr std::size_t kEncodedLimit = 1024 * 1024;
constexpr unsigned kSourceEdgeLimit = 2048;
constexpr unsigned kOutputEdgeLimit = 512;
struct State {
  const std::uint8_t* input;
  std::size_t remaining;
  std::size_t used, limit;
  png_structp png;
  png_infop info;
  std::uint8_t* row;
  std::uint8_t* pixels;
};
struct alignas(std::max_align_t) Allocation { std::size_t bytes; };
png_voidp allocate(png_structp png, png_alloc_size_t bytes) {
  auto* state = static_cast<State*>(png_get_mem_ptr(png));
  if (!state || bytes > std::numeric_limits<std::size_t>::max() - sizeof(Allocation)) return nullptr;
  const auto total = std::size_t(bytes) + sizeof(Allocation);
  if (state->used > state->limit || total > state->limit - state->used) return nullptr;
  auto* header = static_cast<Allocation*>(std::malloc(total));
  if (!header) return nullptr;
  header->bytes = total; state->used += total;
  return header + 1;
}
void release(png_structp png, png_voidp pointer) {
  if (!pointer) return;
  auto* state = static_cast<State*>(png_get_mem_ptr(png));
  auto* header = static_cast<Allocation*>(pointer) - 1;
  state->used -= header->bytes;
  std::free(header);
}
void error(png_structp png, png_const_charp) { png_longjmp(png, 1); }
void warning(png_structp, png_const_charp) {}
void read(png_structp png, png_bytep bytes, png_size_t size) {
  auto* state = static_cast<State*>(png_get_io_ptr(png));
  if (size > state->remaining) png_error(png, "Truncated preview");
  std::memcpy(bytes, state->input, size);
  state->input += size; state->remaining -= size;
}
void cleanup(State* state) {
  if (state->png) png_destroy_read_struct(&state->png, &state->info, nullptr);
  std::free(state->row); std::free(state->pixels);
}
}
bool decode_bounded_preview_png(std::span<const std::uint8_t> encoded,
    PreviewPixels& output, unsigned maximum_edge, std::size_t memory_budget) {
  output = {};
  if (encoded.size() < 33 || encoded.size() > kEncodedLimit ||
      !maximum_edge || maximum_edge > kOutputEdgeLimit || memory_budget < 4096 ||
      png_sig_cmp(encoded.data(), 0, 8)) return false;
  // All mutable decoder state lives on the heap: longjmp cannot invalidate
  // modified automatic variables or bypass C++ resource destructors.
  std::unique_ptr<State> state(new (std::nothrow) State{});
  if (!state) return false;
  state->input = encoded.data(); state->remaining = encoded.size();
  state->limit = memory_budget;
  state->png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, nullptr, error,
                                       warning, state.get(), allocate, release);
  if (!state->png) return false;
  if (setjmp(png_jmpbuf(state->png))) { cleanup(state.get()); return false; }
  state->info = png_create_info_struct(state->png);
  if (!state->info) png_error(state->png, "Preview metadata allocation failed");
  png_set_read_fn(state->png, state.get(), read);
  png_set_user_limits(state->png, kSourceEdgeLimit, kSourceEdgeLimit);
  png_set_chunk_malloc_max(state->png, 32 * 1024);
  png_set_chunk_cache_max(state->png, 8);
  png_set_keep_unknown_chunks(state->png, PNG_HANDLE_CHUNK_NEVER, nullptr, 0);
  png_read_info(state->png, state->info);
  const auto width = png_get_image_width(state->png, state->info);
  const auto height = png_get_image_height(state->png, state->info);
  if (!width || !height || width > kSourceEdgeLimit || height > kSourceEdgeLimit)
    png_error(state->png, "Preview dimensions exceeded");
  const auto edge = std::max(width, height);
  const unsigned out_width = edge > maximum_edge ? std::max(1U, width * maximum_edge / edge) : width;
  const unsigned out_height = edge > maximum_edge ? std::max(1U, height * maximum_edge / edge) : height;
  const bool interlaced = png_get_interlace_type(state->png, state->info) != PNG_INTERLACE_NONE;
  if (interlaced && edge > maximum_edge)
    png_error(state->png, "Interlaced preview exceeds bounded output");
  const auto depth = png_get_bit_depth(state->png, state->info);
  const auto color = png_get_color_type(state->png, state->info);
  if (depth == 16) png_set_strip_16(state->png);
  if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(state->png);
  if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(state->png);
  const bool transparent = png_get_valid(state->png, state->info, PNG_INFO_tRNS);
  if (transparent) png_set_tRNS_to_alpha(state->png);
  if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(state->png);
  if (!(color & PNG_COLOR_MASK_ALPHA) && !transparent) png_set_add_alpha(state->png, 255, PNG_FILLER_AFTER);
  png_set_bgr(state->png);
  const auto passes = png_set_interlace_handling(state->png);
  png_read_update_info(state->png, state->info);
  const std::size_t row_bytes = width * 4U;
  const std::size_t output_bytes = std::size_t(out_width) * out_height * 4U;
  if (png_get_rowbytes(state->png, state->info) != row_bytes ||
      row_bytes + output_bytes > state->limit ||
      state->used > state->limit - row_bytes - output_bytes)
    png_error(state->png, "Preview memory budget exceeded");
  state->limit -= row_bytes + output_bytes;
  state->pixels = static_cast<std::uint8_t*>(std::calloc(1, output_bytes));
  state->row = static_cast<std::uint8_t*>(std::malloc(row_bytes));
  if (!state->pixels || !state->row) png_error(state->png, "Preview allocation failed");
  for (int pass = 0; pass < passes; ++pass) {
    unsigned next_y = 0;
    for (unsigned y = 0; y < height; ++y) {
      if (interlaced) {
        // Small interlaced images combine passes directly in the bounded output.
        png_read_row(state->png, state->pixels + y * row_bytes, nullptr);
      } else {
        png_read_row(state->png, state->row, nullptr);
        if (next_y < out_height && y == next_y * height / out_height) {
          for (unsigned x = 0; x < out_width; ++x)
            std::memcpy(state->pixels + (std::size_t(next_y) * out_width + x) * 4,
                        state->row + (x * width / out_width) * 4, 4);
          ++next_y;
        }
      }
    }
  }
  png_read_end(state->png, nullptr);
  output.storage.reset(state->pixels); state->pixels = nullptr;
  output.bytes = output_bytes; output.width = out_width; output.height = out_height;
  cleanup(state.get());
  return true;
}
}  // namespace printdeck::platform
