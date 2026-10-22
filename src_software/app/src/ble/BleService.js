import { BleManager } from 'react-native-ble-plx';
import { PermissionsAndroid, Platform } from 'react-native';
import { Buffer } from 'buffer'; // handles raw bytes
import { SERVICE_UUID, EMG_UUID, SERVO_UUID, CALIB_UUID } from './uuids'; // the addresses of the ESP32's mailboxes

// the BLE library sends data as base64 text, but the ESP32 deals in raw bytes.
const b64 = (bytes) => Buffer.from(bytes).toString('base64');   // bytes -> text (sending)
const unb64 = (s) => Buffer.from(s, 'base64');                  // text -> bytes (receiving)

class BleService {
  manager = new BleManager(); // the object that controls the phone's Bluetooth
  device = null; // the connected ESP32 (empty until we connect)

  // android requires the user to allow Bluetooth.
  async requestPermissions() {
    if (Platform.OS !== 'android') return true; // iPhone handles this by itself
    const res = await PermissionsAndroid.requestMultiple(
      Platform.Version >= 31
        ? [PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN, PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT]
        : [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION]
    );
    return Object.values(res).every((v) => v === 'granted'); // true only if they all said yes
  }

  // look for devices advertising our service UUID, so only the GatorGrasp shows up and not every Bluetooth gadget nearby
  // each time one is found, onDevice(device) gets called
  // returns a function you can call to stop scanning
  scan(onDevice, onError) {
    this.manager.startDeviceScan([SERVICE_UUID], null, (err, dev) => {
      if (err) return onError?.(err);
      if (dev) onDevice(dev);
    });
    return () => this.manager.stopDeviceScan();
  }

  // open a connection to the chosen device by its id.
  async connect(id, onDisconnect) {
    this.manager.stopDeviceScan(); // stop scanning, since scanning while connecting is flaky
    const d = await this.manager.connectToDevice(id, { requestMTU: 64 }); // request bigger packets
    await d.discoverAllServicesAndCharacteristics(); // ask the ESP32 what mailboxes it has (required before using them)
    d.onDisconnected(() => { this.device = null; onDisconnect?.(); }); // run this if the connection drops
    this.device = d; // remember it so the functions below can use it
    return d;
  }

  async disconnect() { if (this.device) await this.device.cancelConnection(); }

  // the ESP32 pushes 6 bytes (3 sensors x 2 bytes) about 50 times a second
  // we split them into 3 numbers (0-4095) and hand them to cb([a, b, c]).
  // readUInt16LE = "read 2 bytes as one number, low byte first" (that's how the ESP32 stores them)
  subscribeEmg(cb, onError) {
    return this.device.monitorCharacteristicForService(SERVICE_UUID, EMG_UUID, (err, c) => {
      if (err) return onError?.(err);
      const buf = unb64(c.value);
      cb([buf.readUInt16LE(0), buf.readUInt16LE(2), buf.readUInt16LE(4)]);
    });
  }

  // send 6 angles (0-180), one per servo, as 6 bytes
  // Math.max/min keeps each angle in range so we never send junk
  writeServos(angles) {
    const clamped = angles.map((a) => Math.max(0, Math.min(180, Math.round(a))));
    return this.device.writeCharacteristicWithResponseForService(
      SERVICE_UUID, SERVO_UUID, b64(clamped));
  }

  // the arm stores 12 bytes = for each of the 3 sensors, a min and a max
  // we turn them back into { emgMin: [a, b, c], emgMax: [a, b, c] }.
  async readCalibration() {
    const c = await this.device.readCharacteristicForService(SERVICE_UUID, CALIB_UUID);
    const buf = unb64(c.value);
    const emgMin = [], emgMax = [];
    for (let i = 0; i < 3; i++) {
      emgMin.push(buf.readUInt16LE(i * 4)); // sensor i's min is at byte i*4
      emgMax.push(buf.readUInt16LE(i * 4 + 2)); // its max is the next 2 bytes
    }
    return { emgMin, emgMax };
  }

  // pack the min/max numbers into 12 bytes and send them
  writeCalibration({ emgMin, emgMax }) {
    const buf = Buffer.alloc(12);
    for (let i = 0; i < 3; i++) {
      buf.writeUInt16LE(emgMin[i], i * 4);
      buf.writeUInt16LE(emgMax[i], i * 4 + 2);
    }
    return this.device.writeCharacteristicWithResponseForService(SERVICE_UUID, CALIB_UUID, b64(buf));
  }
}

// export one shared copy so every screen uses the same Bluetooth connection.
export default new BleService();