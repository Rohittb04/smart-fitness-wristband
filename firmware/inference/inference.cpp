#include "inference.h"
#include "config.h"
#include "model.h"   // heart_rate_lstm_tflite[]

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ─── TFLite objects (file-local) ──────────────────────────────────────────────
constexpr int kTensorArenaSize = 12 * 1024;
static uint8_t tensor_arena[kTensorArenaSize];

static tflite::MicroInterpreter* interpreter = nullptr;
static TfLiteTensor*             inputTensor  = nullptr;
static TfLiteTensor*             outputTensor = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
void initInference() {
  tflite::InitializeTarget();

  const tflite::Model* model = tflite::GetModel(heart_rate_lstm_tflite);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("[TFLite] Schema version mismatch — halting");
    while (1);
  }

  // Register only the ops the model actually uses
  static tflite::MicroMutableOpResolver<4> resolver;
  resolver.AddFullyConnected();
  resolver.AddReshape();
  resolver.AddQuantize();
  resolver.AddDequantize();

  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, kTensorArenaSize);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("[TFLite] Tensor allocation failed — halting");
    while (1);
  }

  inputTensor  = interpreter->input(0);
  outputTensor = interpreter->output(0);
  Serial.println("[TFLite] Model ready");
}

// ─────────────────────────────────────────────────────────────────────────────
// Task: run LSTM inference once per second
// ─────────────────────────────────────────────────────────────────────────────
void InferenceTask(void *pvParameters) {
  while (1) {
    // Copy circular buffer into model input in correct chronological order
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      for (int t = 0; t < LSTM_TIMESTEPS; t++) {
        int src = (input_buffer_idx + t) % LSTM_TIMESTEPS; // oldest → newest
        for (int f = 0; f < LSTM_FEATURES; f++) {
          inputTensor->data.f[t * LSTM_FEATURES + f] = input_buffer[src][f];
        }
      }
      xSemaphoreGive(xMutex);
    }

    if (interpreter->Invoke() == kTfLiteOk) {
      if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
        current_heart_rate = outputTensor->data.f[0];
        xSemaphoreGive(xMutex);
      }
      Serial.printf("[LSTM] Predicted HR: %.2f bpm\n", current_heart_rate);
    } else {
      Serial.println("[LSTM] Invoke failed!");
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
