#pragma once
#include "printdeck/core/job_state.hpp"
namespace printdeck::core {
constexpr bool print_preview_page(int page, int depth, int subpage,
                                   int count, bool resin, bool camera_cleanup) {
  return !camera_cleanup && page == 0 && depth == 1 &&
         subpage == (count > (resin ? 6 : 4) ? 1 : 0);
}
constexpr bool print_preview_job_active(JobPhase phase) {
  return phase == JobPhase::preparing || phase == JobPhase::printing || phase == JobPhase::paused;
}
}  // namespace printdeck::core
