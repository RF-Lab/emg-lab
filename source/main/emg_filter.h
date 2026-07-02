#ifndef EMG_FILTER_H
#define EMG_FILTER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float x1;
    float y1;
} dc_filter_state_t;

typedef struct {
    float x1;
    float x2;
    float y1;
    float y2;
} notch_filter_state_t;

typedef struct {
    dc_filter_state_t dc;
    notch_filter_state_t notch;
} channel_filter_t;

typedef struct {
    float b0, b1, b2;
    float a1, a2;
} notch_coeffs_t;

void emg_filter_init(float sample_rate, float notch_freq, float q_factor, float dc_r);

float emg_filter_apply(int ch, float input);

#endif // EMG_FILTER_H