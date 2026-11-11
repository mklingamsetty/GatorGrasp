#include <cstdio>
#include <cmath>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

// Edge Impulse Classifier Headers
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"

// Real EMG Test Vectors (1800 floats: 300 samples x 6 channels)
#include "test_vectors.h"

// Buffer for raw inference window (300 samples x 6 channels = 1800 floats)
static float features_buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];

// Test cases using real recorded Zenodo dataset vectors
struct RealGestureTestCase {
    const char* expected_label;
    const float* vector_ptr;
};

static const RealGestureTestCase real_test_cases[] = {
    { "gesture_0", test_gesture_0 },
    { "gesture_1", test_gesture_1 },
    { "gesture_2", test_gesture_2 },
    { "gesture_3", test_gesture_3 },
    { "gesture_4", test_gesture_4 }
};

// Structure for simulated 4-channel analog EMG inputs (channels 5 & 6 are computed)
struct EmgSimPattern {
    const char* test_name;
    const char* description;
    float emg1;
    float emg2;
    float emg3;
    float emg4;
};

static const EmgSimPattern sim_patterns[] = {
    { "Sim 1", "Rest / Baseline Noise",          0.04f, 0.03f, 0.04f, 0.03f },
    { "Sim 2", "Ch1 Dominant Contraction",       0.85f, 0.12f, 0.10f, 0.08f },
    { "Sim 3", "Ch2 Dominant (Ch2 > Ch3 diff)",  0.15f, 0.78f, 0.15f, 0.12f },
    { "Sim 4", "Ch3 Dominant (Ch3 > Ch2 diff)",  0.12f, 0.18f, 0.82f, 0.20f },
    { "Sim 5", "Ch4 Dominant Contraction",       0.10f, 0.15f, 0.22f, 0.85f },
    { "Sim 6", "Co-Contraction (All High)",      0.70f, 0.65f, 0.68f, 0.72f }
};

/**
 * @brief Fill the 6-channel EMG inference window
 * Generates synthetic EMG bursts across 300 time steps (1 ms sampling period).
 * Channels 0..3: Base raw EMG values with subtle ripple.
 * Channel 4: emg_diff_23  = emg2 - emg3
 * Channel 5: emg_ratio_23 = (emg2 - emg3) / (emg2 + emg3 + epsilon)
 */
static void fill_emg_window(float emg1, float emg2, float emg3, float emg4) {
    for (size_t s = 0; s < EI_CLASSIFIER_RAW_SAMPLE_COUNT; s++) {
        // Simulated muscle fibrillar ripple (8 Hz modulating envelope)
        float ripple = 0.02f * sinf(2.0f * 3.14159265f * 8.0f * ((float)s / (float)EI_CLASSIFIER_FREQUENCY));
        size_t idx = s * EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME;

        float c1 = fminf(fmaxf(emg1 + ripple, 0.0f), 1.0f);
        float c2 = fminf(fmaxf(emg2 - ripple, 0.0f), 1.0f);
        float c3 = fminf(fmaxf(emg3 + ripple * 0.5f, 0.0f), 1.0f);
        float c4 = fminf(fmaxf(emg4 - ripple * 0.5f, 0.0f), 1.0f);

        features_buffer[idx + 0] = c1;
        features_buffer[idx + 1] = c2;
        features_buffer[idx + 2] = c3;
        features_buffer[idx + 3] = c4;

        if (EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME >= 6) {
            float diff_23 = c2 - c3;
            float ratio_23 = (c2 - c3) / (c2 + c3 + 1e-4f);
            features_buffer[idx + 4] = diff_23;
            features_buffer[idx + 5] = ratio_23;
        }
    }
}

/**
 * @brief Print SRAM and PSRAM memory statistics
 */
static void print_memory_stats(const char* tag) {
    size_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_sram  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    printf("[%s] SRAM Free: %lu B | Min Ever Free: %lu B | PSRAM Free: %lu B\n",
           tag,
           (unsigned long)free_sram,
           (unsigned long)min_sram,
           (unsigned long)free_psram);
}

/**
 * @brief Run inference on features_buffer and format output to serial monitor
 * @return true if expected_lbl was matched, false otherwise (or true if expected_lbl is null)
 */
static bool run_and_print_inference(const char* test_desc, const char* expected_lbl = nullptr) {
    signal_t signal;
    int err = numpy::signal_from_buffer(features_buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    if (err != 0) {
        printf("ERR: signal_from_buffer failed (%d)\n", err);
        return false;
    }

    size_t heap_before = esp_get_free_heap_size();
    int64_t t_start = esp_timer_get_time();

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
    int64_t t_total_us = esp_timer_get_time() - t_start;
    size_t heap_after = esp_get_free_heap_size();

    if (res != EI_IMPULSE_OK) {
        printf("ERR: run_classifier failed (%d)\n", res);
        return false;
    }

    // Find the class with the highest probability
    size_t best_idx = 0;
    float best_val = 0.0f;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (result.classification[i].value > best_val) {
            best_val = result.classification[i].value;
            best_idx = i;
        }
    }

    const char* pred_label = result.classification[best_idx].label;
    bool match = (expected_lbl != nullptr) ? (strcmp(pred_label, expected_lbl) == 0) : true;

    printf("--------------------------------------------------------------------------------\n");
    printf("Test     | %s\n", test_desc);
    if (expected_lbl) {
        printf("Verdict  | Expected: %-10s -> Predicted: %-10s [%s] (Conf: %.1f%%)\n",
               expected_lbl, pred_label, match ? "PASS" : "FAIL", best_val * 100.0f);
    } else {
        printf("Verdict  | Top Predicted: %-10s (Confidence: %.1f%%)\n",
               pred_label, best_val * 100.0f);
    }
    printf("Scores   | ");
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        printf("%s: %5.1f%%  ",
               result.classification[i].label,
               result.classification[i].value * 100.0f);
    }
    printf("\nTiming   | Total: %lld us | DSP: %d ms | NN: %d ms\n",
           t_total_us, result.timing.dsp, result.timing.classification);
    printf("Memory   | Heap Before: %lu B | After: %lu B | Net Delta: %ld B\n",
           (unsigned long)heap_before,
           (unsigned long)heap_after,
           (long)(heap_before - heap_after));

    return match;
}

extern "C" void app_main(void)
{
    // Allow USB CDC serial monitor time to connect
    vTaskDelay(pdMS_TO_TICKS(1500));

    printf("\n================================================================================\n");
    printf("         GatorGrasp ESP32-S3 Edge Impulse 6-Channel EMG Classifier\n");
    printf("================================================================================\n");
    printf(" Project Name:        %s\n", EI_CLASSIFIER_PROJECT_NAME);
    printf(" Project ID:          %d\n", EI_CLASSIFIER_PROJECT_ID);
    printf(" Input Frame Size:    %d floats (%d samples @ %d Hz x %d channels)\n",
           EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
           EI_CLASSIFIER_RAW_SAMPLE_COUNT,
           EI_CLASSIFIER_FREQUENCY,
           EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME);
    printf(" Channels:            %s\n", EI_CLASSIFIER_FUSION_AXES_STRING);
    printf(" DSP Block:           Flatten (avg, max, rms, stdev x 6 = %d features)\n",
           EI_CLASSIFIER_NN_INPUT_FRAME_SIZE);
    printf(" Output Classes:      %d (gesture_0 .. gesture_4)\n", EI_CLASSIFIER_LABEL_COUNT);
    printf(" TFLite Arena Size:   %d bytes\n", EI_CLASSIFIER_TFLITE_LARGEST_ARENA_SIZE);
    print_memory_stats("STARTUP");
    printf("================================================================================\n\n");

    // ==============================================================================
    // PHASE 1: Real Recorded EMG Test Vectors (Ground Truth Verification)
    // ==============================================================================
    printf("\n>>> PHASE 1: Real Recorded EMG Test Vectors (Ground Truth Verification) <<<\n");
    const size_t num_real = sizeof(real_test_cases) / sizeof(real_test_cases[0]);
    size_t passed_count = 0;

    for (size_t i = 0; i < num_real; i++) {
        memcpy(features_buffer, real_test_cases[i].vector_ptr, sizeof(features_buffer));
        char desc[64];
        snprintf(desc, sizeof(desc), "Ground Truth Vector for %s", real_test_cases[i].expected_label);
        
        bool ok = run_and_print_inference(desc, real_test_cases[i].expected_label);
        if (ok) passed_count++;

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    printf("\n--------------------------------------------------------------------------------\n");
    printf(">>> PHASE 1 SUMMARY: %u / %u TEST CASES PASSED (%.1f%%) <<<\n",
           (unsigned int)passed_count, (unsigned int)num_real,
           ((float)passed_count / (float)num_real) * 100.0f);
    printf("--------------------------------------------------------------------------------\n\n");

    // ==============================================================================
    // PHASE 2: Synthetically Simulated Real-Time EMG Patterns (Continuous Loop)
    // ==============================================================================
    printf(">>> PHASE 2: Simulated Real-Time EMG Patterns (Continuous Loop) <<<\n");
    const size_t num_sim = sizeof(sim_patterns) / sizeof(sim_patterns[0]);
    size_t sim_idx = 0;

    while (true) {
        const EmgSimPattern& pat = sim_patterns[sim_idx];
        fill_emg_window(pat.emg1, pat.emg2, pat.emg3, pat.emg4);

        char desc[128];
        snprintf(desc, sizeof(desc), "%s - %s [emg1: %.2f, emg2: %.2f, emg3: %.2f, emg4: %.2f]",
                 pat.test_name, pat.description, pat.emg1, pat.emg2, pat.emg3, pat.emg4);
        run_and_print_inference(desc, nullptr);

        sim_idx = (sim_idx + 1) % num_sim;
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}
