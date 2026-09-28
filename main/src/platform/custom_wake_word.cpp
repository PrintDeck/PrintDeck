#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "frontend.h"
#include "frontend_util.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

extern const unsigned char wake_model_start[] asm("_binary_hey_printdeck_tflite_start");
namespace {
constexpr char kTag[] = "custom-wake";
constexpr size_t kArenaBytes = 128 * 1024;
constexpr size_t kVariableBytes = 16 * 1024;
constexpr int kFrameSamples = 512;

// All mutable state belongs to the core-0 voice worker. This adapter uses the
// existing lifecycle; MultiNet remains the command recognizer after a wake.
struct Detector {
  FrontendState frontend{};
  tflite::MicroMutableOpResolver<20> ops;
  uint8_t* arena = nullptr;
  uint8_t* variables = nullptr;
  std::unique_ptr<tflite::MicroInterpreter> engine;
  unsigned stride = 0;
  unsigned invocations = 0;
  unsigned probability_index = 0;
  float probabilities[5]{};
  bool failed = false;
  int64_t measured_us = 0;
  int64_t worst_us = 0;
  unsigned chunks = 0;
  unsigned peak = 0;
  float peak_score = 0;

  ~Detector() {
    engine.reset();
    heap_caps_free(arena);
    heap_caps_free(variables);
    FrontendFreeStateContents(&frontend);
  }

  bool init() {
#define ADD_OP(name) if (ops.Add##name() != kTfLiteOk) return false
    ADD_OP(CallOnce); ADD_OP(VarHandle); ADD_OP(Reshape);
    ADD_OP(ReadVariable); ADD_OP(StridedSlice); ADD_OP(Concatenation);
    ADD_OP(AssignVariable); ADD_OP(Conv2D); ADD_OP(Mul); ADD_OP(Add);
    ADD_OP(Mean); ADD_OP(FullyConnected); ADD_OP(Logistic); ADD_OP(Quantize);
    ADD_OP(DepthwiseConv2D); ADD_OP(AveragePool2D); ADD_OP(MaxPool2D);
    ADD_OP(Pad); ADD_OP(Pack); ADD_OP(SplitV);
#undef ADD_OP
    FrontendConfig config{};
    FrontendFillConfigWithDefaults(&config);
    config.window.size_ms = 30;
    config.window.step_size_ms = 10;
    config.filterbank.num_channels = 40;
    config.filterbank.lower_band_limit = 125;
    config.filterbank.upper_band_limit = 7500;
    config.noise_reduction.smoothing_bits = 10;
    config.noise_reduction.even_smoothing = 0.025F;
    config.noise_reduction.odd_smoothing = 0.06F;
    config.noise_reduction.min_signal_remaining = 0.05F;
    config.pcan_gain_control.enable_pcan = 1;
    config.pcan_gain_control.strength = 0.95F;
    config.pcan_gain_control.offset = 80;
    config.pcan_gain_control.gain_bits = 21;
    config.log_scale.enable_log = 1;
    config.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&config, &frontend, 16000)) return false;
    arena = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    variables = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, kVariableBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arena || !variables) return false;
    auto* allocator = tflite::MicroAllocator::Create(variables, kVariableBytes);
    if (!allocator) return false;
    auto* resources = tflite::MicroResourceVariables::Create(allocator, 20);
    if (!resources) return false;
    const auto* model = tflite::GetModel(wake_model_start);
    if (model->version() != TFLITE_SCHEMA_VERSION) return false;
    engine.reset(new (std::nothrow) tflite::MicroInterpreter(model, ops, arena, kArenaBytes, resources));
    if (!engine || engine->AllocateTensors() != kTfLiteOk) return false;
    const auto* input = engine->input(0);
    const auto* output = engine->output(0);
    if (input->type != kTfLiteInt8 || input->dims->size != 3 ||
        input->dims->data[0] != 1 || input->dims->data[1] != 3 || input->dims->data[2] != 40 ||
        input->params.scale <= 0 || output->type != kTfLiteUInt8 || output->bytes != 1 ||
        output->params.scale <= 0) return false;
    ESP_LOGI(kTag, "Prototype arena used=%u reserved=%u variable=%u, input scale=%.8f zero=%ld",
             static_cast<unsigned>(engine->arena_used_bytes()), static_cast<unsigned>(kArenaBytes),
             static_cast<unsigned>(kVariableBytes), static_cast<double>(input->params.scale),
             static_cast<long>(input->params.zero_point));
    return true;
  }

  void reset() {
    FrontendReset(&frontend);
    if (engine->Reset() != kTfLiteOk) failed = true;
    stride = invocations = probability_index = 0;
    std::fill(std::begin(probabilities), std::end(probabilities), 0.0F);
  }

  wakenet_state_t detect(const int16_t* samples) {
    if (failed) return WAKENET_CHANNEL_VERIFIED;
    const auto begin = esp_timer_get_time();
    for (int i = 0; i < kFrameSamples; ++i) peak = std::max(peak, static_cast<unsigned>(std::abs(static_cast<int>(samples[i]))));
    size_t offset = 0;
    bool detected = false;
    while (offset < kFrameSamples) {
      size_t consumed = 0;
      auto features = FrontendProcessSamples(&frontend, samples + offset, kFrameSamples - offset, &consumed);
      if (consumed == 0) { failed = true; break; }
      offset += consumed;
      if (!features.size) continue;
      if (features.size != 40) { failed = true; break; }
      auto* input = engine->input(0);
      for (size_t i = 0; i < 40; ++i) {
        // The training frontend scales raw features by 1/25.6. Quantize with
        // the actual model metadata, including saturating the input range.
        const float value = features.values[i] * 0.0390625F / input->params.scale + input->params.zero_point;
        input->data.int8[stride * 40 + i] = static_cast<int8_t>(std::clamp(std::nearbyint(value), -128.0F, 127.0F));
      }
      if (++stride < 3) continue;
      stride = 0;
      if (engine->Invoke() != kTfLiteOk) { failed = true; break; }
      ++invocations;
      auto* output = engine->output(0);
      const float score = (static_cast<int>(output->data.uint8[0]) - output->params.zero_point) * output->params.scale;
      // Same 84-inference startup exclusion and five-score mean as evaluation.
      if (invocations <= 84) continue;
      probabilities[probability_index++ % 5] = score;
      float mean = 0;
      for (float p : probabilities) mean += p * 0.2F;
      peak_score = std::max(peak_score, mean);
      if (probability_index >= 5 && mean >= 0.92F) {
        ESP_LOGI(kTag, "Trigger score=%.3f", static_cast<double>(mean));
        detected = true;
      }
    }
    const auto elapsed = esp_timer_get_time() - begin;
    measured_us += elapsed;
    worst_us = std::max(worst_us, elapsed);
    if (++chunks % 256 == 0) {
      ESP_LOGI(kTag, "Audio peak=%u score=%.3f processing max=%lld us", peak,
               static_cast<double>(peak_score), static_cast<long long>(worst_us));
      peak = 0; peak_score = 0;
    }
    if (failed) {
      ESP_LOGE(kTag, "Inference stopped after a frontend or model error");
      return WAKENET_CHANNEL_VERIFIED;
    }
    return detected ? WAKENET_DETECTED : WAKENET_NO_DETECT;
  }
};
Detector* cast(model_iface_data_t* data) { return reinterpret_cast<Detector*>(data); }
model_iface_data_t* create(const void*, det_mode_t) {
  auto model = std::unique_ptr<Detector>(new (std::nothrow) Detector);
  if (!model || !model->init()) return nullptr;
  return reinterpret_cast<model_iface_data_t*>(model.release());
}
void destroy(model_iface_data_t* data) {
  auto* d = cast(data);
  ESP_LOGI(kTag, "Processing worst chunk=%lld us (budget 32000 us)", static_cast<long long>(d->worst_us));
  delete d;
}
esp_wn_iface_t make_interface() {
  esp_wn_iface_t api{};
  api.create = create;
  api.get_samp_chunksize = +[](model_iface_data_t*) { return kFrameSamples; };
  api.get_samp_rate = +[](model_iface_data_t*) { return 16000; };
  api.detect = +[](model_iface_data_t* d, int16_t* s) { return cast(d)->detect(s); };
  api.clean = +[](model_iface_data_t* d) { cast(d)->reset(); };
  api.destroy = destroy;
  return api;
}
} // namespace
extern "C" const esp_wn_iface_t printdeck_custom_wake_word = make_interface();
