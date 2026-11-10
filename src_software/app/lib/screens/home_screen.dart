// Main branch of app(archived)/App.js
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../app_log.dart';
import '../ble/ble_service.dart';
import '../ble/packets.dart';
import '../services/auth.dart' as auth;
import '../services/db.dart';
import '../theme.dart';
import '../widgets/glass_button.dart';
import 'readings_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key, required this.user, required this.log});
  final User user;
  final AppLog log;

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _ble = BleService.instance;
  final Map<String, ScanResult> _devices = {};
  bool _scannedOnce = false;
  bool _connected = false;
  List<int> _emg = const [0, 0];

  AppLog get _log => widget.log;

  @override
  void dispose() {
    // Leaving this screen (logout) stops scanning and drops the arm.
    _ble.stopScan();
    _ble.disconnect();
    super.dispose();
  }

  Future<void> _scan() => _log.guard(() async {
        if (!await _ble.requestPermissions()) {
          _log.say('BLE permission denied');
          return;
        }
        setState(() {
          _devices.clear();
          _scannedOnce = true;
        });
        // The Scan button shows "Scanning…" while this runs (see build).
        await _ble.scan(
          (r) {
            if (mounted) setState(() => _devices[r.device.remoteId.str] = r);
          },
          (e) => _log.say('ERR: ${errorMessage(e)}'),
        );
      });

  Future<void> _connect(ScanResult r) => _log.guard(() async {
        _log.say('connecting...');
        await _ble.connect(r.device, onDisconnect: () {
          if (mounted) setState(() => _connected = false);
          _log.say('disconnected');
        });
        setState(() => _connected = true);
        await _ble.subscribeEmg(
          (v) {
            if (mounted) setState(() => _emg = v);
          },
          (e) => _log.say('ADC ERR: ${errorMessage(e)}'),
        );
        _log.say('connected!');
      });

  Future<void> _servo(int angle) => _log.guard(() async {
        if (!_connected) {
          _log.say('Not connected - tap a Connect button first');
          return;
        }
        await _ble.writeServos(List.filled(6, angle));
        _log.say('sent $angle degrees');
      });

  Future<void> _saveToCloud() => _log.guard(() async {
        if (!_connected) {
          _log.say('Not connected - no reading to save');
          return;
        }
        final reading = List<int>.of(_emg);
        await saveReading(widget.user.uid, reading);
        _log.say('saved to cloud: ${reading.join(', ')}');
      });

  void _openReadings() => Navigator.of(context).push(
        MaterialPageRoute<void>(
          builder: (_) => ReadingsScreen(uid: widget.user.uid),
        ),
      );

  String _deviceLabel(ScanResult r) {
    final name = r.advertisementData.advName.isNotEmpty
        ? r.advertisementData.advName
        : r.device.platformName;
    return name.isNotEmpty ? name : r.device.remoteId.str;
  }

  Widget _section(String label) => Padding(
        padding: const EdgeInsets.only(top: 22, bottom: 4, left: 8),
        child: Text(label, style: GG.sectionLabel),
      );

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.fromLTRB(28, 24, 28, 50),
      children: [
        const Text('GatorGrasp', style: GG.title, textAlign: TextAlign.center),
        const SizedBox(height: 4),
        Text(widget.user.email ?? '',
            style: GG.subtitle, textAlign: TextAlign.center),
        const SizedBox(height: 8),
        _section('CONNECTION'),
        StreamBuilder<bool>(
          stream: _ble.isScanning,
          initialData: _ble.isScanningNow,
          builder: (_, snap) {
            final scanning = snap.data ?? false;
            return Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                GlassButton(
                  title: scanning ? 'Scanning…' : 'Scan',
                  variant: GlassVariant.primary,
                  onPressed: scanning ? null : _scan,
                ),
                if (!scanning && _scannedOnce && _devices.isEmpty)
                  Padding(
                    padding: const EdgeInsets.only(top: 4, bottom: 2),
                    child: Text(
                      'No arm found. Make sure it is powered on and nearby.',
                      textAlign: TextAlign.center,
                      style: GG.log,
                    ),
                  ),
              ],
            );
          },
        ),
        for (final r in _devices.values)
          GlassButton(
            title: 'Connect ${_deviceLabel(r)}',
            onPressed: () => _connect(r),
          ),
        Container(
          margin: const EdgeInsets.only(top: 10),
          padding: const EdgeInsets.all(18),
          decoration: GG.card(),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('Connected: $_connected', style: GG.cardText),
              const SizedBox(height: 4),
              Text(
                'ADC1 (GPIO1): ${_emg[0]} (${toVolts(_emg[0]).toStringAsFixed(2)} V)',
                style: GG.cardText,
              ),
              const SizedBox(height: 4),
              Text(
                'ADC2 (GPIO11): ${_emg[1]} (${toVolts(_emg[1]).toStringAsFixed(2)} V)',
                style: GG.cardText,
              ),
            ],
          ),
        ),
        _section('SERVO TEST'),
        for (final a in const [0, 90, 180])
          GlassButton(title: '$a degrees', onPressed: () => _servo(a)),
        _section('CLOUD'),
        GlassButton(title: 'Save reading to cloud', onPressed: _saveToCloud),
        GlassButton(title: 'View saved readings', onPressed: _openReadings),
        _section('LOGS'),
        ListenableBuilder(
          listenable: _log,
          builder: (_, __) => Padding(
            padding: const EdgeInsets.only(top: 6),
            child: Text(_log.text, style: GG.log),
          ),
        ),
        const SizedBox(height: 28),
        GlassButton(
          title: 'Logout',
          variant: GlassVariant.danger,
          onPressed: () => _log.guard(auth.logout),
        ),
      ],
    );
  }
}
