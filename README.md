# GatorGrasp

CEN4907C CPE Senior Design Project

Team members:
- Murali Lingamsetty
- Chase Prasad
- Cher Huang
- Benjamin Smith
- Trevor Turnquist

## 1. Completed Work & Effort (Pre-Alpha Milestone)
This pre-alpha build demonstrates the foundational end-to-end architecture of the GatorGrasp prosthetic system, representing approximately 20 hours of effort per team member (~100 total person-hours) tracked via time-stamped GitHub commits and online documentation:
- **Mechanical Assembly & Tendon Routing:** 3D-printed the right-hand chassis using PLA/PETG, assembled the finger joints, and routed high-tension fishing wire tendons to 5 independent servos.
- **Experimentation & Calibration:** Calibrated fishing line tension and servo horn angles so a single shared PWM control signal curls all five fingers simultaneously without line snapping or mechanical binding.
- **Embedded Sensing & Actuation (ESP32-S3):** Configured the Hosyond ESP32-S3 N16R8 to sample raw/envelope analog voltages from the MyoWare 2.0 sensor via ADC1 and trigger a shared hardware PWM output when the user's muscle contraction exceeds the curling threshold.
- **BLE Companion App & Cloud Persistence:** Built the barebones smartphone interface supporting user login (username/password), Bluetooth Low Energy (BLE) pairing with the ESP32-S3, reception of the `"GRASP"` prompt and live MyoWare analog readings, and profile synchronization to a Firebase database.

## 2. System Architecture

<img width="5046" height="3620" alt="MyoWare ESP32-S3 Control Architecture-2026-10-31-115627" src="https://github.com/user-attachments/assets/0308001b-788a-47fc-baef-d3ae37c5764b" />

## 3. Current Bugs/Issues:
- **Murali Lingamsetty:**
    * EdgeImpulse Library is currently plateauing at an 88.3% accuracy; testing different Neural Network architectures and model designs (like tuning the Dense layer configurations or testing 1D-CNN) to increase validation accuracy past 90%
    * Working on reducing the memory size of the EdgeImpulse model to ensure it cleanly fits on the ESP32-S3's internal SRAM without starving other peripherals of memory.
    * The current 3D printed hand model is too flimsy and must be edited/re-printed to handle the high static and dynamic tension loads from the tendon lines.
    * Attempting to resolve the normalization bug in `prepare_edge_impulse_data.py` where per-file normalization accidentally amplifies 60Hz power-line hum on relaxed channels up to full 1.0 scale.
- **Chase Prasad:**
   * The MyoWare sensor data being returned shows a much lower difference in voltage values between the resting and flexed forearm states than expected )only a ~200 mV difference vs an expected ~1 V difference)
   * Must determine whether sensor data is consistent enough across different users or if a user-specific configuration for the arm is required
   * ESP32-S3 board has trouble driving more than 2 PWM signals while in low-power mode; have not tested with more than 3 servos in regular operating mode
- **Cher Huang:** 
- **Benjamin Smith:** 
- **Trevor Turnquist:**
     * MyoWare Sensor is prone to grounding issues, providing an incorrect signal if the user is not properly grounded (e.g. touching metal).
     * The sensor is very sensitive, requiring some form of debouncing, creating a tradeoff between smoother actuation and reactivity.
     * We do not currently have enough electrodes to test the full four MyoWare sensor setup. The cost for each day we test will be about $12 (12 electrodes at $1 each).
