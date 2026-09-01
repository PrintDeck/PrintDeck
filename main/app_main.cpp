#include "printdeck/platform/runtime.hpp"

extern "C" void app_main() {
  static printdeck::platform::Runtime runtime;
  runtime.start();
}

