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
        setState(_devices.clear);
        await _ble.scan(
          (r) {
            if (mounted) setState(() => _devices[r.device.remoteId.str] = r);
          },
          (e) => _log.say('ERR: ${errorMessage(e)}'),
        );
        _log.say('scanning...');
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

  Future<void> _loadFromCloud() => _log.guard(() async {
        final rows = await listReadings(widget.user.uid, n: 5);
        if (rows.isEmpty) {
          _log.say('no saved readings yet');
          return;
        }
        final lines = rows.map((r) {
          final t =
              r.createdAt?.toLocal().toString().split('.').first ?? 'pending';
          return '$t  volts: ${r.volts.join(' | ')}  raw: ${r.raw.join(' | ')}';
        });
        _log.say(
            '--- last ${rows.length} saved readings ---\n${lines.join('\n')}');
      });

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
        const SizedBox(height: 24),
        GlassButton(
          title: 'Logout',
          variant: GlassVariant.danger,
          onPressed: () => _log.guard(auth.logout),
        ),
        _section('CONNECTION'),
        GlassButton(
          title: 'Scan',
          variant: GlassVariant.primary,
          onPressed: _scan,
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
        GlassButton(
            title: 'Load readings from cloud', onPressed: _loadFromCloud),
        _section('LOGS'),
        ListenableBuilder(
          listenable: _log,
          builder: (_, __) => Padding(
            padding: const EdgeInsets.only(top: 6),
            child: Text(_log.text, style: GG.log),
          ),
        ),
      ],
    );
  }
}
