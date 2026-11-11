# GatorGrasp Full-System Dual-Core + Low-Power Integration Test

Combined test project built from `sandbox/murali/EdgeImpulse_Testing/Hand_Gesture_ESP32S3_Test` for the **Hosyond ESP32-S3-WROOM-1 N16R8**, integrating:

1. **4-Channel MyoWare 2.0 ADC1 Sampling (Core 0)** via `adc_continuous` DMA + curve-fitting voltage calibration.
2. **Edge Impulse ML Inference (`GatorGrasp_EMG_V2`, Core 1)** over a 300-sample x 4-channel rolling EMG window.
3. **Dorhea PCA9685 16-Channel 12-Bit PWM Servo Driver over I2C** driving 6 MG995 servos (5 fingers + wrist rotation).
4. **NimBLE BLE Server (`GatorGrasp`, Core 0)** streaming 4-channel ADC readings and accepting manual servo overrides / calibration from the companion app (`src_software/app`).
5. **Low-Power Mode (DFS + Automatic Light Sleep)** with Tickless Idle, BLE controller modem sleep (`MAIN_XTAL`), and duty-cycled ADC DMA bursts.

## Pin Assignment (ESP32-S3-WROOM-1 N16R8)

| Function | ESP32-S3 Pin | Notes |
| --- | --- | --- |
| MyoWare 1 (`emg1`) | `GPIO 4` (`ADC1_CH3`) | Connect to MyoWare Arduino Shield analog channel 0 |
| MyoWare 2 (`emg2`) | `GPIO 5` (`ADC1_CH4`) | Connect to MyoWare Arduino Shield analog channel 1 |
| MyoWare 3 (`emg3`) | `GPIO 6` (`ADC1_CH5`) | Connect to MyoWare Arduino Shield analog channel 2 |
| MyoWare 4 (`emg4`) | `GPIO 7` (`ADC1_CH6`) | Connect to MyoWare Arduino Shield analog channel 3 |
| PCA9685 I2C `SDA` | `GPIO 8` | Connect to Dorhea PCA9685 `SDA` |
| PCA9685 I2C `SCL` | `GPIO 9` | Connect to Dorhea PCA9685 `SCL` |
| UART0 Console (`COM6`) | `GPIO 43/44` (internal CH343) | Survives Automatic Light Sleep (use the UART USB port) |

> **Hardware Safety Notes**
> - **MyoWare Power = 3.3V:** Power the MyoWare 2.0 sensors / Arduino Shield from **3.3V** (not 5V) so analog outputs stay within the ESP32-S3 ADC input range.
> - **PCA9685 Power Split:** Connect PCA9685 **`VCC`** to ESP32-S3 **3.3V** (for 3.3V I2C logic), and connect PCA9685 **`V+`** screw terminal to a dedicated **5V–6V external battery/supply** capable of handling 6x MG995 stall current. **Share a common GND** across ESP32-S3, MyoWare, PCA9685, and the servo PSU.
> - **Reserved Pins Avoided:** `GPIO 33–37` (Octal PSRAM on N16R8), `GPIO 0/3/45/46` (strapping pins), `GPIO 19/20` (native USB), `GPIO 43/44` (UART0).

## PCA9685 Servo Channel Mapping (`0x40`, 50 Hz)

- `Ch 0`: Thumb
- `Ch 1`: Index
- `Ch 2`: Middle
- `Ch 3`: Ring
- `Ch 4`: Pinky
- `Ch 5`: Wrist Rotation

## Building & Monitoring in VS Code (ESP-IDF v6.1)

1. Open `sandbox/chase/full_system_low_power_test` in VS Code (preconfigured for `esp32s3` and `COM6` in `.vscode/settings.json`).
2. Run **ESP-IDF: Build, Flash and Start a Monitor on your Device** (plugged into the board's **UART** USB port on `COM6`).
3. Watch the telemetry output for:
   - **Boot Self-Test:** Verifies the 5 recorded Zenodo ground-truth vectors pass under DFS + dual-core setup.
   - **ML Inference Time & Dropped Frames:** Checks whether Core 1 keeps up without dropping ADC bursts (`Drop: 0`).
   - **LightSleep %:** Confirms the chip actually enters Automatic Light Sleep between cycles while keeping BLE and PCA9685 PWM alive.
   - **SRAM Min Free:** Verifies safe internal heap headroom under simultaneous BLE + ADC DMA + Edge Impulse operation.
