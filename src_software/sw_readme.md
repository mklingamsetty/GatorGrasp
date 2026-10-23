# Software

## Current status
| Piece | Status |
|---|---|
| Firmware: BLE advertising, connect, EMG notify, servo write, calibration read/write | Working (tested with nRF Connect) |
| Firmware: 1 servo on GPIO 4 | Working |
| Firmware: EMG-to-servo mapping, saving calibration to flash, 6 servos | Not done (removed to keep the test version minimal) |
| App: Firebase login/register, calibration profile sync | Written, login not yet tested end to end |
| App: BLE scan/connect/EMG/servo | Written, **not yet tested on a real Android phone** |

## File Tree
```
src_software/
  firmware/            ESP-IDF project (C)
    main/main.c        everything: BLE, ADC, servo
    sdkconfig.defaults
  app/                 React Native (Expo dev build)
    App.js             throwaway debug screen
    app.json
    firestore.rules
    src/
      ble/uuids.js     BLE UUIDs (must match main.c)
      ble/BleService.js
      services/auth.js
      services/db.js
      config/firebase.js
```

## BLE interface
| Characteristic | UUID ends in | Access | Data |
|---|---|---|---|
| EMG | `…0002` | read, notify | 3 x uint16 little-endian (raw ADC, 0-4095), 50 Hz |
| Servo | `…0003` | write | First byte = angle 0-180 (the app sends 6 bytes; only the first is used for now) |
| Calibration | `…0004` | read, write | 12 bytes: 3 x {min uint16, max uint16}, little-endian. RAM only for now |

The UUIDs exist in two places, `firmware/main/main.c` and `app/src/ble/uuids.js`. Change both together.

---


## Mobile app (Windows, Android)
BLE needs native code, so **Expo Go does not work**. We use a **development build** installed on a physical Android phone. Emulators have no Bluetooth, so they can only test login/Firebase.

### One-time setup
1. Install **Node.js (LTS)**.
2. Install **Android Studio** (default options). Open it once so it downloads the Android SDK.
3. Set environment variables (Windows -> "Edit the system environment variables"):
   - `JAVA_HOME` = `C:\Program Files\Android\Android Studio\jbr` (the folder containing `bin`)
   - `ANDROID_HOME` = `C:\Users\<you>\AppData\Local\Android\Sdk`
   - Add to `Path`: `%JAVA_HOME%\bin` and `%ANDROID_HOME%\platform-tools`
4. **Close and reopen your terminal**, then check: `java -version` and `adb version`.
5. On the phone: Settings -> About phone -> tap Build number 7 times -> Developer options -> enable **USB debugging**. Plug in via USB and accept the prompt. `adb devices` should list it.

### Build and run
```
cd src_software/app
npm install
npx expo prebuild
npx expo run:android
```
The first build takes a long time (it downloads the NDK and other tools). After that it's much faster. The app installs on the connected phone and the Metro dev server starts. Leave that terminal open.

### How builds work
- **JavaScript changes** (anything in `App.js` or `src/`) reload instantly with no rebuild.
- **Rebuild only when** you change `app.json`, add or remove a native package, or change anything in `android/`: run `npx expo prebuild` then `npx expo run:android` again.
- **Everyone builds on their own machine** and installs onto their own phone. Builds don't conflict, so there's no need to take turns.
