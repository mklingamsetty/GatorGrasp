#include <cstdio>
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"

// Edge Impulse Classifier Headers
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"

// Buffer of 1200 floats (300 samples x 4 channels: emg1, emg2, emg3, emg4)
static float features_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];

// Structure to hold 4 test EMG variables ([0.0, 1.0] normalized analog scale)
struct EmgTestPattern {
    const char* test_name;
    float emg1;
    float emg2;
    float emg3;
    float emg4;
};

// 5 test patterns cycling through simulated 4-channel analog EMG states
static const EmgTestPattern test_patterns[] = {
    { "Test A (Ch1 High / Ch3 Med)",   0.85f, 0.12f, 0.55f, 0.10f },
    { "Test B (Ch1 & Ch2 Active)",     0.78f, 0.65f, 0.12f, 0.15f },
    { "Test C (Low/Mid Contraction)",  0.25f, 0.18f, 0.35f, 0.22f },
    { "Test D (Ch3 Dominant)",         0.30f, 0.35f, 0.80f, 0.25f },
    { "Test E (Ch2 & Ch3 Dominant)",   0.18f, 0.55f, 0.62f, 0.80f }
};

// Fills the 300x4 window from the 4 EMG variables
static void fill_emg_window(float emg1, float emg2, float emg3, float emg4) {
    for (size_t s = 0; s < EI_CLASSIFIER_RAW_SAMPLE_COUNT; s++) {
        // Add a tiny 0.02 ripple so Flatten skewness/kurtosis & FFT have non-zero variance
        float ripple = 0.02f * sinf(2.0f * 3.14159265f * 8.0f * ((float)s / EI_CLASSIFIER_FREQUENCY));
        size_t idx = s * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME;

        features_buffer[idx + 0] = fminf(fmaxf(emg1 + ripple, 0.0f), 1.0f);
        features_buffer[idx + 1] = fminf(fmaxf(emg2 - ripple, 0.0f), 1.0f);
        features_buffer[idx + 2] = fminf(fmaxf(emg3 + ripple * 0.5f, 0.0f), 1.0f);
        features_buffer[idx + 3] = fminf(fmaxf(emg4 - ripple * 0.5f, 0.0f), 1.0f);
    }
}

extern "C" void app_main(void)
{
    printf("\n============================================================\n");
    printf(" GatorGrasp ESP32-S3 Edge Impulse 4-Channel EMG Test\n");
    printf(" Project: %s (ID: %d)\n", EI_CLASSIFIER_PROJECT_NAME, EI_CLASSIFIER_PROJECT_ID);
    printf(" Input Frame: %d floats (%d samples x %d channels)\n",
           EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
           EI_CLASSIFIER_RAW_SAMPLE_COUNT,
           EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME);
    printf(" Free Heap Before Inference: %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("============================================================\n\n");

    const size_t num_tests = sizeof(test_patterns) / sizeof(test_patterns[0]);
    size_t current_test = 0;

    while (true) {
        const EmgTestPattern& pat = test_patterns[current_test];

        // 1. Populate the 1200-float window using the 4 EMG variables
        fill_emg_window(pat.emg1, pat.emg2, pat.emg3, pat.emg4);

        // 2. Wrap buffer in an Edge Impulse signal_t
        signal_t signal;
        int err = numpy::signal_from_buffer(features_buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
        if (err != 0) {
            printf("ERR: signal_from_buffer failed (%d)\n", err);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // 3. Run the classifier
        ei_impulse_result_t result = { 0 };
        int64_t t_start = esp_timer_get_time();
        EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
        int64_t t_total_us = esp_timer_get_time() - t_start;

        if (res != EI_IMPULSE_OK) {
            printf("ERR: run_classifier failed (%d)\n", res);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // 4. Find the highest-probability gesture
        size_t best_idx = 0;
        float best_val = 0.0f;
        for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            if (result.classification[i].value > best_val) {
                best_val = result.classification[i].value;
                best_idx = i;
            }
        }

        // 5. Print the 4 input variables and the ESP32-S3's classification output
        printf("------------------------------------------------------------\n");
        printf("Input  | %-28s -> [emg1: %.2f, emg2: %.2f, emg3: %.2f, emg4: %.2f]\n",
               pat.test_name, pat.emg1, pat.emg2, pat.emg3, pat.emg4);
        printf("Scores | ");
        for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            printf("%s: %5.1f%%  ",
                   result.classification[i].label,
                   result.classification[i].value * 100.0f);
        }
        printf("\nResult | >> PREDICTED: %-10s (%5.1f%%) << | Time: %lld us (DSP: %d ms, NN: %d ms) | Free RAM: %lu B\n",
               result.classification[best_idx].label,
               best_val * 100.0f,
               t_total_us,
               result.timing.dsp,
               result.timing.classification,
               (unsigned long)esp_get_free_heap_size());

        // Advance to the next test pattern every 2 seconds
        current_test = (current_test + 1) % num_tests;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}