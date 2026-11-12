import 'package:flutter_test/flutter_test.dart';
import 'package:gatorgrasp/ble/packets.dart';

void main() {
  group('EMG', () {
    test('parses 4 x uint16 little-endian (current firmware)', () {
      // 0x0FFF = 4095, 0x0102 = 258, 0x0000 = 0, 0x0800 = 2048
      expect(
        parseEmg([0xFF, 0x0F, 0x02, 0x01, 0x00, 0x00, 0x00, 0x08]),
        [4095, 258, 0, 2048],
      );
    });

    test('still parses 2-channel packets from older firmware', () {
      expect(parseEmg([0xFF, 0x0F, 0x02, 0x01]), [4095, 258]);
    });

    test('ignores a trailing odd byte', () {
      expect(parseEmg([0x01, 0x00, 0x02]), [1]);
    });

    test('rejects packets too short for one channel', () {
      expect(() => parseEmg([1]), throwsFormatException);
    });

    test('pin table matches the channel count', () {
      expect(emgChannelGpios.length, emgChannelCount);
    });
  });

  group('servos', () {
    test('rounds and clamps to 0..180', () {
      expect(encodeServos([-10, 0, 89.6, 180, 999]), [0, 0, 90, 180, 180]);
    });
  });

  group('calibration', () {
    test('matches the firmware default layout (4 channels, 16 bytes)', () {
      // firmware/main/main.c: { 0,0,0xFF,0x0F } x 4  -> min 0, max 4095
      final c = parseCalibration([
        for (var i = 0; i < 4; i++) ...[0, 0, 0xFF, 0x0F],
      ]);
      expect(c.emgMin, [0, 0, 0, 0]);
      expect(c.emgMax, [4095, 4095, 4095, 4095]);
    });

    test('round-trips', () {
      final c = Calibration(
        emgMin: [1, 300, 65535, 7],
        emgMax: [2, 4095, 0, 8],
      );
      final bytes = encodeCalibration(c);
      expect(bytes.length, emgChannelCount * 4);
      final back = parseCalibration(bytes);
      expect(back.emgMin, c.emgMin);
      expect(back.emgMax, c.emgMax);
    });

    test('rejects lengths that are not a multiple of 4', () {
      expect(() => parseCalibration([0, 0, 0xFF]), throwsFormatException);
    });
  });

  test('toVolts uses the 3.1 V reference', () {
    expect(toVolts(0), 0);
    expect(toVolts(4095), closeTo(3.1, 1e-9));
  });
}
