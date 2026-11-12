// Port of app(archived)/src/ble/BleService.js
import 'dart:async';
import 'dart:io' show Platform;

import 'package:device_info_plus/device_info_plus.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'packets.dart';
import 'uuids.dart';

class BleService {
  BleService._();
  static final BleService instance = BleService._();

  BluetoothDevice? _device;
  BluetoothCharacteristic? _emg;
  BluetoothCharacteristic? _servo;
  BluetoothCharacteristic? _calib;
  StreamSubscription<List<int>>? _emgSub;

  bool get isConnected => _device != null;

  /// True while a scan is running (emits on start/stop/timeout).
  Stream<bool> get isScanning => FlutterBluePlus.isScanning;
  bool get isScanningNow => FlutterBluePlus.isScanningNow;

  /// Android 12+: BLUETOOTH_SCAN + BLUETOOTH_CONNECT. Android <= 11: location.
  /// iOS shows its own Bluetooth prompt on first use.
  Future<bool> requestPermissions() async {
    if (!Platform.isAndroid) return true;
    final sdk = (await DeviceInfoPlugin().androidInfo).version.sdkInt;
    final perms = sdk >= 31
        ? [Permission.bluetoothScan, Permission.bluetoothConnect]
        : [Permission.locationWhenInUse];
    final res = await perms.request();
    return res.values.every((s) => s.isGranted);
  }

  /// Scans for devices advertising the GatorGrasp service. Stops on its own
  /// after [timeout], or when [stopScan]/[connect] is called.
  Future<void> scan(
    void Function(ScanResult) onDevice,
    void Function(Object) onError, {
    Duration timeout = const Duration(seconds: 15),
  }) async {
    final sub = FlutterBluePlus.onScanResults.listen(
      (results) => results.forEach(onDevice),
      onError: onError,
    );
    FlutterBluePlus.cancelWhenScanComplete(sub);
    await FlutterBluePlus.startScan(
      withServices: [Guid(serviceUuid)],
      timeout: timeout,
    );
  }

  Future<void> stopScan() => FlutterBluePlus.stopScan();

  Future<void> connect(
    BluetoothDevice device, {
    void Function()? onDisconnect,
  }) async {
    await FlutterBluePlus.stopScan();
    // mtu is Android-only; matches requestMTU: 64 in the Expo app.
    await device.connect(license: License.nonprofit, mtu: 64);

    final services = await device.discoverServices();
    final svc = services.firstWhere(
      (s) => s.uuid == Guid(serviceUuid),
      orElse: () => throw StateError('GatorGrasp service not found'),
    );
    BluetoothCharacteristic chr(String id) => svc.characteristics.firstWhere(
          (c) => c.uuid == Guid(id),
          orElse: () => throw StateError('Characteristic $id not found'),
        );
    _emg = chr(emgUuid);
    _servo = chr(servoUuid);
    _calib = chr(calibUuid);

    final connSub = device.connectionState.listen((state) {
      if (state == BluetoothConnectionState.disconnected) {
        _reset();
        onDisconnect?.call();
      }
    });
    device.cancelWhenDisconnected(connSub, delayed: true, next: true);

    _device = device;
  }

  Future<void> disconnect() async => _device?.disconnect();

  /// cb([adc1, adc2]) on every EMG notification (~20 Hz).
  Future<void> subscribeEmg(
    void Function(List<int>) cb,
    void Function(Object) onError,
  ) async {
    final c = _require(_emg);
    await _emgSub?.cancel();
    _emgSub = c.onValueReceived.listen(
      (bytes) {
        try {
          cb(parseEmg(bytes));
        } catch (e) {
          onError(e);
        }
      },
      onError: onError,
    );
    _device!.cancelWhenDisconnected(_emgSub!);
    await c.setNotifyValue(true);
  }

  Future<void> writeServos(List<num> angles) =>
      _require(_servo).write(encodeServos(angles));

  Future<Calibration> readCalibration() async =>
      parseCalibration(await _require(_calib).read());

  Future<void> writeCalibration(Calibration c) =>
      _require(_calib).write(encodeCalibration(c));

  BluetoothCharacteristic _require(BluetoothCharacteristic? c) {
    if (c == null || _device == null) throw StateError('Not connected');
    return c;
  }

  void _reset() {
    _emgSub?.cancel();
    _emgSub = null;
    _emg = _servo = _calib = null;
    _device = null;
  }
}
