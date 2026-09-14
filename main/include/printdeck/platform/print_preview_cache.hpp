#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace printdeck::platform {

// One bounded PNG/BMP per printer/job identity. The caller owns flash-safe I/O.
class PrintPreviewCache {
 public:
  static constexpr std::size_t kMaximumBytes = 1024 * 1024;
  explicit PrintPreviewCache(std::string directory = "/assets/print-previews")
      : directory_(std::move(directory)) {}
  // Incremental file I/O for the shared internal-stack worker. The caller
  // retains the input/output storage and calls cancel on that same worker.
  // No filesystem lock or ImageWorkspaceLock is retained between steps.
  class Transfer {
   public:
    enum class Result { pending, complete, failed };
    static constexpr std::size_t kChunkBytes = 4096;
    Transfer() = default;
    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;
    ~Transfer();
    void begin_write(const PrintPreviewCache&, std::string_view,
                     std::span<const std::uint8_t>);
    void begin_read(const PrintPreviewCache&, std::string_view,
                    std::span<std::uint8_t>);
    Result step();
    void cancel();
   private:
    enum class Stage { idle, prepare, scan, evict, space, open_write,
                       write_data, commit, open_read, read_data };
    Result fail();
    Stage stage_ = Stage::idle;
    std::string directory_, destination_, staging_;
    std::span<const std::uint8_t> input_;
    std::span<std::uint8_t> output_;
    FILE* file_ = nullptr;
    DIR* directory_handle_ = nullptr;
    std::uint64_t identity_ = 0, checksum_ = 0, expected_checksum_ = 0;
    std::size_t offset_ = 0, used_ = 0;
    bool owns_staging_ = false;
  };
  std::size_t size(std::string_view key) const;
  bool read(std::string_view key, std::span<std::uint8_t> bytes) const;
  bool write(std::string_view key, std::span<const std::uint8_t> bytes) const;
  void remove(std::string_view key) const;

 private:
  std::string path(std::string_view key) const;
  std::string directory_;
};

}  // namespace printdeck::platform
