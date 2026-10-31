# GatorGrasp

CEN4907C CPE Senior Design Project

Team members:
- Murali Lingamsetty
- Chase Prasad
- Cher Huang
- Benjamin Smith
- Trevor Turnquist

Current Bugs/Issues:
- **Murali Lingamsetty:**
    * EdgeImpulse Library is currently plateauing at an 88.3% accuracy; testing different Neural Network architectures and model designs (like tuning the Dense layer configurations or testing 1D-CNN) to increase validation accuracy past 90%
    * Working on reducing the memory size of the EdgeImpulse model to ensure it cleanly fits on the ESP32-S3's internal SRAM without starving other peripherals of memory.
    * The current 3D printed hand model is too flimsy and must be edited/re-printed to handle the high static and dynamic tension loads from the tendon lines.
    * Attempting to resolve the normalization bug in `prepare_edge_impulse_data.py` where per-file normalization accidentally amplifies 60Hz power-line hum on relaxed channels up to full 1.0 scale.
- **Chase Prasad:** 
- **Cher Huang:** 
- **Benjamin Smith:** 
- **Trevor Turnquist:** 
