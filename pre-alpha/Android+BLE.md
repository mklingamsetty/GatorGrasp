# How to Set Up the Android BLE Environment

## Prerequisites

You only have to do the following once:

1. Install Node.js (LTS) and Android Studio (let it download the Android SDK).
2. Set these Windows environment variables, then open a new terminal:
   - `JAVA_HOME` = `C:\Program Files\Android\Android Studio\jbr`
   - `ANDROID_HOME` = `C:\Users\<you>\AppData\Local\Android\Sdk`
   - Add `%ANDROID_HOME%\platform-tools` to `Path`
3. In the project folder: `npm install`

On the Android phone:

1. Settings > About phone > tap Build number 7 times
2. Settings > Developer options > turn on USB debugging
3. Plug the phone into the PC with a data cable and tap Allow on the phone.
4. Run `adb devices`. The phone should show as `device`.

## Build and install (first time or after changing app.json / native packages)

Do this in `src_software\app`:

```bash
npx expo prebuild
npx expo run:android --device
```

The app installs on the phone and opens. It takes a while the first time.

```bash
npx expo start --dev-client
adb reverse tcp:8081 tcp:8081
```

Open the GatorGrasp app on the phone. If it doesn't load, enter `http://localhost:8081` in the app launcher.
Edits to JavaScript files reload automatically. Re-run `adb reverse` each time you replug the phone.

## Test BLE

1. Flash the ESP32 in `src_software\firmware`: and power it on.
2. Make sure no other app (like nRF Connect) is connected to it.
3. In the app: log in or register a new account, tap **Scan**, allow Bluetooth permission.
4. Tap **Connect GatorGrasp**. The log should say `connected!` and the ADC values should update.
5. Tap **0 / 90 / 180 degrees**. The servo should move, and the ESP32 serial monitor prints `servo -> ... deg`.
6. Tap **Save reading to cloud**, then **Load readings from cloud**. Saved readings appear in the log.
