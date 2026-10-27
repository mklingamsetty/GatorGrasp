import { BleManager } from 'react-native-ble-plx';
import { PermissionsAndroid, Platform } from 'react-native';
import { Buffer } from 'buffer';
import { SERVICE_UUID, EMG_UUID, SERVO_UUID, CALIB_UUID } from './uuids';

const b64 = (bytes) => Buffer.from(bytes).toString('base64');
const unb64 = (s) => Buffer.from(s, 'base64');

class BleService {
  manager = new BleManager();
  device = null;

  async requestPermissions() {
    if (Platform.OS !== 'android') return true;
    const res = await PermissionsAndroid.requestMultiple(
      Platform.Version >= 31
        ? [PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN, PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT]
        : [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION]
    );
    return Object.values(res).every((v) => v === 'granted');
  }

  scan(onDevice, onError) {
    this.manager.startDeviceScan([SERVICE_UUID], null, (err, dev) => {
      if (err) return onError?.(err);
      if (dev) onDevice(dev);
    });
    return () => this.manager.stopDeviceScan();
  }

  async connect(id, onDisconnect) {
    this.manager.stopDeviceScan();
    const d = await this.manager.connectToDevice(id, { requestMTU: 64 });
    await d.discoverAllServicesAndCharacteristics();
    d.onDisconnected(() => { this.device = null; onDisconnect?.(); });
    this.device = d;
    return d;
  }

  async disconnect() { if (this.device) await this.device.cancelConnection(); }

  // cb([adc1, adc2]) - 2 x uint16 LE. returns subscription (call .remove())
  subscribeEmg(cb, onError) {
    return this.device.monitorCharacteristicForService(SERVICE_UUID, EMG_UUID, (err, c) => {
      if (err) return onError?.(err);
      const buf = unb64(c.value);
      cb([buf.readUInt16LE(0), buf.readUInt16LE(2)]);
    });
  }

  writeServos(angles) {
    const clamped = angles.map((a) => Math.max(0, Math.min(180, Math.round(a))));
    return this.device.writeCharacteristicWithResponseForService(
      SERVICE_UUID, SERVO_UUID, b64(clamped));
  }

  async readCalibration() {
    const c = await this.device.readCharacteristicForService(SERVICE_UUID, CALIB_UUID);
    const buf = unb64(c.value);
    const emgMin = [], emgMax = [];
    for (let i = 0; i < 3; i++) { emgMin.push(buf.readUInt16LE(i * 4)); emgMax.push(buf.readUInt16LE(i * 4 + 2)); }
    return { emgMin, emgMax };
  }

  writeCalibration({ emgMin, emgMax }) {
    const buf = Buffer.alloc(12);
    for (let i = 0; i < 3; i++) { buf.writeUInt16LE(emgMin[i], i * 4); buf.writeUInt16LE(emgMax[i], i * 4 + 2); }
    return this.device.writeCharacteristicWithResponseForService(SERVICE_UUID, CALIB_UUID, b64(buf));
  }
}

export default new BleService();