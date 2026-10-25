set(EI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Library")

# Gather Edge Impulse SDK, TFLite Micro, and compiled model sources
file(GLOB_RECURSE EI_SDK_SRCS
    "${EI_DIR}/edge-impulse-sdk/dsp/*.cpp"
    "${EI_DIR}/edge-impulse-sdk/dsp/*.c"
    "${EI_DIR}/edge-impulse-sdk/classifier/*.cpp"
    "${EI_DIR}/edge-impulse-sdk/classifier/*.c"
    "${EI_DIR}/edge-impulse-sdk/tensorflow/*.cc"
    "${EI_DIR}/edge-impulse-sdk/tensorflow/*.cpp"
    "${EI_DIR}/edge-impulse-sdk/tensorflow/*.c"
    "${EI_DIR}/edge-impulse-sdk/third_party/*.c"
    "${EI_DIR}/edge-impulse-sdk/third_party/*.cc"
    "${EI_DIR}/edge-impulse-sdk/third_party/*.cpp"
    "${EI_DIR}/tflite-model/*.cpp"
    "${EI_DIR}/model-parameters/*.cpp"
)

# Only compile the top-level Espressif porting files (avoids POSIX duplicate symbols)
file(GLOB EI_PORTING_SRCS
    "${EI_DIR}/edge-impulse-sdk/porting/espressif/*.c"
    "${EI_DIR}/edge-impulse-sdk/porting/espressif/*.cpp"
)

file(GLOB EI_DSP_SRCS
    "${EI_DIR}/edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/misc/*.cpp"
    "${EI_DIR}/edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/float/*.c"
)

# Check which include directories exist in this SDK version before registering
set(CANDIDATE_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${EI_DIR}"
    "${EI_DIR}/edge-impulse-sdk"
    "${EI_DIR}/edge-impulse-sdk/dsp/kissfft"
    "${EI_DIR}/edge-impulse-sdk/third_party"
    "${EI_DIR}/edge-impulse-sdk/third_party/flatbuffers/include"
    "${EI_DIR}/edge-impulse-sdk/third_party/gemmlowp"
    "${EI_DIR}/edge-impulse-sdk/third_party/ruy"
    "${EI_DIR}/edge-impulse-sdk/CMSIS/DSP/Include"
    "${EI_DIR}/edge-impulse-sdk/CMSIS/Core/Include"
    "${EI_DIR}/edge-impulse-sdk/porting/espressif/ESP-NN/include"
    "${EI_DIR}/edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include"
    "${EI_DIR}/edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/include"
    "${EI_DIR}/model-parameters"
    "${EI_DIR}/tflite-model"
)

set(VALID_INCLUDES "")
foreach(dir ${CANDIDATE_INCLUDES})
    if(IS_DIRECTORY "${dir}")
        list(APPEND VALID_INCLUDES "${dir}")
    endif()
endforeach()

idf_component_register(
    SRCS "main.cpp" ${EI_SDK_SRCS} ${EI_PORTING_SRCS} ${EI_DSP_SRCS}
    INCLUDE_DIRS ${VALID_INCLUDES}
    REQUIRES esp_timer driver freertos log
)

# Prevent ESP-IDF v6.1 -Werror from halting on Edge Impulse SDK warnings
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-deprecated-declarations
    -DEIDSP_QUANTIZE_FILTERBANK=0
    -DEIDSP_USE_ESP_DSP=1
    -O3
)