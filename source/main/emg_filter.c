#include "emg_filter.h"
#include <math.h>
#include <string.h>

static channel_filter_t g_filters[CONFIG_AD1299_NUM_CH];
static notch_coeffs_t g_notch_coeffs;
static float g_dc_r = 0.99f;
static bool g_is_initialized = false;

void emg_filter_init(float sample_rate, float notch_freq, float q_factor, float dc_r) {
    g_dc_r = dc_r;

    float omega = 2.0f * M_PI * notch_freq / sample_rate;
    float sin_omega = sinf(omega);
    float cos_omega = cosf(omega);
    
    float alpha = sin_omega / (2.0f * q_factor);

    float a0 = 1.0f + alpha;
    float b0_tmp = 1.0f;
    float b1_tmp = -2.0f * cos_omega;
    float b2_tmp = 1.0f;
    float a1_tmp = -2.0f * cos_omega;
    float a2_tmp = 1.0f - alpha;

    g_notch_coeffs.b0 = b0_tmp / a0;
    g_notch_coeffs.b1 = b1_tmp / a0;
    g_notch_coeffs.b2 = b2_tmp / a0;
    g_notch_coeffs.a1 = a1_tmp / a0;
    g_notch_coeffs.a2 = a2_tmp / a0;

    memset(g_filters, 0, sizeof(g_filters));

    g_is_initialized = true;
}

float emg_filter_apply(int ch, float input) {
    if (!g_is_initialized) {
        return input;
    }

    float dc_out = input - g_filters[ch].dc.x1 + g_dc_r * g_filters[ch].dc.y1;
    
    g_filters[ch].dc.x1 = input;
    g_filters[ch].dc.y1 = dc_out;

    float notch_out = (g_notch_coeffs.b0 * dc_out) + 
                      (g_notch_coeffs.b1 * g_filters[ch].notch.x1) + 
                      (g_notch_coeffs.b2 * g_filters[ch].notch.x2) - 
                      (g_notch_coeffs.a1 * g_filters[ch].notch.y1) - 
                      (g_notch_coeffs.a2 * g_filters[ch].notch.y2);

    g_filters[ch].notch.x2 = g_filters[ch].notch.x1;
    g_filters[ch].notch.x1 = dc_out;
    g_filters[ch].notch.y2 = g_filters[ch].notch.y1;
    g_filters[ch].notch.y1 = notch_out;

    return notch_out;
}
