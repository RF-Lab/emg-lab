#include "nn_model.h"
#include "emg_model.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char* TAG = "TFLite";

static float input_scale;
static int32_t input_zero_point;

namespace {
    const tflite::Model* model = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    TfLiteTensor* input = nullptr;
    TfLiteTensor* output = nullptr;

    constexpr int kTensorArenaSize = 128 * 1024;
    uint8_t tensor_arena[kTensorArenaSize] __attribute__((aligned(16)));
}

extern "C" void init_1d_cnn(void) {
    tflite::InitializeTarget();

    model = tflite::GetModel(emg_model_tflite);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "Version mismatch!");
        return;
    }

    static tflite::MicroMutableOpResolver<14> resolver;
    resolver.AddReshape();
    resolver.AddConv2D();
    resolver.AddMaxPool2D();
    resolver.AddMean();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();
    resolver.AddShape();
    resolver.AddStridedSlice();
    resolver.AddPack();
    resolver.AddExpandDims();
    resolver.AddSub();
    resolver.AddMul();
    resolver.AddQuantize();
    resolver.AddDequantize();

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tensor_arena, kTensorArenaSize);
    interpreter = &static_interpreter;

    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        ESP_LOGE(TAG, "Tensor arena size is not enough!");
        return;
    }

    ESP_LOGI(TAG, "1D-CNN started! Memory used: %d bytes of %d available",
             interpreter->arena_used_bytes(), kTensorArenaSize);

    input = interpreter->input(0);
    output = interpreter->output(0);

    input_scale = input->params.scale;
    input_zero_point = input->params.zero_point;
    ESP_LOGI(TAG, "Scale: %f, ZeroPoint: %ld", input_scale, input_zero_point);
}

extern "C" void invoke_1d_cnn(nn_sample_t *window, output_vector_packet_t *output) {
    if (!interpreter) {
        ESP_LOGE(TAG, "1D-CNN is not initialized!");
        return;
    }

    TfLiteTensor* input_tensor = interpreter->input(0);

    for (int i = 0; i < WINDOW_SIZE; i++) {
        for (int ch = 0; ch < CONFIG_AD1299_NUM_CH; ch++) {
            float raw_val = window[i].channels[ch]; 
            int32_t quantized_val = (int32_t)roundf(raw_val / input_scale) + input_zero_point;
            
            input_tensor->data.int8[i * CONFIG_AD1299_NUM_CH + ch] = (int8_t)quantized_val;
        }
    }

    if (interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "Inference error!");
        return;
    }

    TfLiteTensor* output_tensor_tflite = interpreter->output(0);
    int8_t* output_buffer = output_tensor_tflite->data.int8;
    
    float out_scale = output_tensor_tflite->params.scale;
    int32_t out_zero_point = output_tensor_tflite->params.zero_point;

    for (int ch = 0; ch < NUM_CLASSES; ch++) {
        float probability = (float)(output_buffer[ch] - out_zero_point) * out_scale;
        int32_t percentage = (int32_t)roundf(probability * 100.0f);

        output->output_vector[ch] = (int8_t)percentage;
    }
}