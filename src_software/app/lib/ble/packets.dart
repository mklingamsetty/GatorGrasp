// Pure encode/decode helpers for the GatorGrasp BLE protocol.
// Kept free of plugin imports so they can be unit tested.
// Must match src_software/firmware/main/main.c.
import 'dart:typed_data';

/// GPIO pin for each EMG channel, in packet order (firmware `emg_gpios[]`).
const emgChannelGpios = [4, 5, 6, 7];

/// Number of EMG channels the current firmware sends.
const emgChannelCount = 4;

/// ADC counts (12-bit, 0..4095) -> volts, using the board's ~3.1 V reference.
double toVolts(int raw) => raw / 4095 * 3.1;

/// EMG notify payload: one uint16 LE per channel (8 bytes for 4 channels).
/// Accepts any even length so older 2-channel firmware still parses.
List<int> parseEmg(List<int> bytes) {
  if (bytes.length < 2) {
    throw FormatException('EMG packet too short (${bytes.length} bytes)');
  }
  final d = ByteData.sublistView(Uint8List.fromList(bytes));
  return [
    for (var i = 0; i + 1 < bytes.length; i += 2) d.getUint16(i, Endian.little),
  ];
}

/// Servo angles -> one uint8 per servo, rounded and clamped to 0..180.
List<int> encodeServos(List<num> angles) =>
    angles.map((a) => a.round().clamp(0, 180)).toList();

class Calibration {
  Calibration({required this.emgMin, required this.emgMax})
      : assert(emgMin.length == emgMax.length);

  final List<int> emgMin; // one per channel
  final List<int> emgMax; // one per channel

  int get channels => emgMin.length;

  @override
  String toString() => 'Calibration(min: $emgMin, max: $emgMax)';
}

/// 4 bytes per channel: min[i] at i*4 and max[i] at i*4+2 (uint16 LE).
/// Current firmware: 4 channels = 16 bytes.
Calibration parseCalibration(List<int> bytes) {
  if (bytes.length < 4 || bytes.length % 4 != 0) {
    throw FormatException('Bad calibration packet (${bytes.length} bytes)');
  }
  final n = bytes.length ~/ 4;
  final d = ByteData.sublistView(Uint8List.fromList(bytes));
  return Calibration(
    emgMin: [for (var i = 0; i < n; i++) d.getUint16(i * 4, Endian.little)],
    emgMax: [for (var i = 0; i < n; i++) d.getUint16(i * 4 + 2, Endian.little)],
  );
}

/// The firmware only accepts exactly emgChannelCount * 4 bytes.
List<int> encodeCalibration(Calibration c) {
  final d = ByteData(c.channels * 4);
  for (var i = 0; i < c.channels; i++) {
    d.setUint16(i * 4, c.emgMin[i], Endian.little);
    d.setUint16(i * 4 + 2, c.emgMax[i], Endian.little);
  }
  return d.buffer.asUint8List();
}
