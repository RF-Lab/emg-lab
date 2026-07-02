#ifndef NN_MODEL_H
#define NN_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEW_DATA_STEP 50 // sliding window step
#define WINDOW_SIZE   200
#define NUM_CLASSES   3

typedef struct {
    int8_t output_vector[CONFIG_AD1299_NUM_CH];
} output_vector_packet_t;

typedef struct {
    float channels[CONFIG_AD1299_NUM_CH];
} nn_sample_t;

void init_1d_cnn(void);

void invoke_1d_cnn(nn_sample_t *window, output_vector_packet_t *output);

#ifdef __cplusplus
}
#endif

#endif // NN_MODEL_H