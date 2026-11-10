import 'package:flutter_test/flutter_test.dart';
import 'package:gatorgrasp/ble/packets.dart';

void main() {
  group('EMG', () {
    test('parses 2 x uint16 little-endian', () {
      // 0x0FFF = 4095, 0x0102 = 258
      expect(parseEmg([0xFF, 0x0F, 0x02, 0x01]), [4095, 258]);
    });

    test('rejects short packets', () {
      expect(() => parseEmg([1, 2, 3]), throwsFormatException);
    });
  });

  group('servos', () {
    test('rounds and clamps to 0..180', () {
      expect(encodeServos([-10, 0, 89.6, 180, 999]), [0, 0, 90, 180, 180]);
    });
  });

  group('calibration', () {
    test('matches the firmware default layout', () {
      // firmware/main/main.c: { 0,0,0xFF,0x0F } x 3  -> min 0, max 4095
      final c = parseCalibration(
          [0, 0, 0xFF, 0x0F, 0, 0, 0xFF, 0x0F, 0, 0, 0xFF, 0x0F]);
      expect(c.emgMin, [0, 0, 0]);
      expect(c.emgMax, [4095, 4095, 4095]);
    });

    test('round-trips', () {
      const c = Calibration(emgMin: [1, 300, 65535], emgMax: [2, 4095, 0]);
      final bytes = encodeCalibration(c);
      expect(bytes.length, 12);
      final back = parseCalibration(bytes);
      expect(back.emgMin, c.emgMin);
      expect(back.emgMax, c.emgMax);
    });
  });

  test('toVolts uses the 3.1 V reference', () {
    expect(toVolts(0), 0);
    expect(toVolts(4095), closeTo(3.1, 1e-9));
  });
}
