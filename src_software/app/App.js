// THROWAWAY debug harness - proves auth + BLE + Firestore work end to end.
// Styling pass: GatorGrasp title + glass buttons. Logic is unchanged.
import React, { useEffect, useRef, useState } from 'react';
import { View, Text, TextInput, ScrollView, StyleSheet, StatusBar } from 'react-native';
import { onUser, login, register, logout } from './src/services/auth';
import { saveReading, listReadings } from './src/services/db';
import ble from './src/ble/BleService';

// Rough conversion: 12-bit ADC, 12 dB attenuation (~0-3.1 V usable). Not calibrated.
const toVolts = (raw) => ((raw / 4095) * 3.1).toFixed(2);
import GlassButton from './src/components/GlassButton';

export default function App() {
  const [user, setUser] = useState(null);
  const [email, setEmail] = useState('');
  const [pw, setPw] = useState('');
  const [devices, setDevices] = useState({});
  const [connected, setConnected] = useState(false);
  const [emg, setEmg] = useState([0, 0]);
  const [log, setLog] = useState('');
  const emgRef = useRef([0, 0]);
  const emgSub = useRef(null);

  const say = (m) => setLog((l) => `${m}\n${l}`.slice(0, 2000));
  const guard = (fn) => async () => { try { await fn(); } catch (e) { say(`ERR: ${e.message}`); } };

  useEffect(() => onUser(setUser), []);

  const scan = guard(async () => {
    if (!(await ble.requestPermissions())) return say('BLE permission denied');
    setDevices({});
    ble.scan((d) => setDevices((p) => ({ ...p, [d.id]: d })), (e) => say(`ERR: ${e.message}`));
    say('scanning...');
  });

  const connect = (id) => guard(async () => {
    say('connecting...');
    await ble.connect(id, () => { setConnected(false); say('disconnected'); });
    setConnected(true);
    emgSub.current = ble.subscribeEmg(
      (v) => { emgRef.current = v; setEmg(v); },
      (e) => say(`ADC ERR: ${e.message}`));
    say('connected!');
  })();

  const servo = (angle) => guard(async () => {
    if (!connected) return say('Not connected - tap a Connect button first');
    await ble.writeServos([angle, angle, angle, angle, angle, angle]);
    say(`sent ${angle} degrees`);
  })();

  const saveToCloud = guard(async () => {
    if (!connected) return say('Not connected - no reading to save');
    await saveReading(user.uid, emgRef.current);
    say(`saved to cloud: ${emgRef.current.join(', ')}`);
  });

  const loadFromCloud = guard(async () => {
    const rows = await listReadings(user.uid, 5);
    if (!rows.length) return say('no saved readings yet');
    const lines = rows.map((r) => {
      const t = r.createdAt ? r.createdAt.toDate().toLocaleString() : 'pending';
      return `${t}  volts: ${(r.volts || []).join(' | ')}  raw: ${(r.raw || []).join(' | ')}`;
    });
    say(`--- last ${rows.length} saved readings ---\n${lines.join('\n')}`);
  });

  if (!user) {
    return (
      <View style={styles.screen}>
        <StatusBar barStyle="light-content" />
        <View style={styles.loginWrap}>
          <Text style={styles.title}>GatorGrasp</Text>
          <Text style={styles.subtitle}>Sign in to your arm</Text>

          <TextInput
            style={styles.input}
            placeholder="email"
            placeholderTextColor="rgba(255,255,255,0.45)"
            autoCapitalize="none"
            value={email}
            onChangeText={setEmail}
          />
          <TextInput
            style={styles.input}
            placeholder="password"
            placeholderTextColor="rgba(255,255,255,0.45)"
            secureTextEntry
            value={pw}
            onChangeText={setPw}
          />

          <GlassButton title="Login" variant="primary" onPress={guard(() => login(email, pw))} />
          <GlassButton title="Register" onPress={guard(() => register(email, pw))} />
          <Text style={styles.log}>{log}</Text>
        </View>
      </View>
    );
  }

  return (
    <View style={styles.screen}>
      <StatusBar barStyle="light-content" />
      <ScrollView contentContainerStyle={styles.content}>
        <Text style={styles.title}>GatorGrasp</Text>
        <Text style={styles.subtitle}>{user.email}</Text>

        <GlassButton title="Logout" variant="danger" onPress={logout} />

        <Text style={styles.sectionLabel}>CONNECTION</Text>
        <GlassButton title="Scan" variant="primary" onPress={scan} />
        {Object.values(devices).map((d) => (
          <GlassButton key={d.id} title={`Connect ${d.name || d.id}`} onPress={() => connect(d.id)} />
        ))}

        <View style={styles.card}>
          <Text style={styles.cardText}>Connected: {String(connected)}</Text>
          <Text style={styles.cardText}>EMG: {emg.join(' | ')}</Text>
        </View>

        <Text style={styles.sectionLabel}>HAND CONTROL</Text>
        <GlassButton title="Open hand (all 0)" onPress={guard(() => ble.writeServos([0,0,0,0,0,0]))} />
        <GlassButton title="Close hand (all 180)" onPress={guard(() => ble.writeServos([180,180,180,180,180,180]))} />

        <Text style={styles.sectionLabel}>CALIBRATION</Text>
        <GlassButton title="Calibration: arm -> cloud" onPress={syncUp} />
        <GlassButton title="Calibration: cloud -> arm" onPress={syncDown} />

        <Text style={styles.log}>{log}</Text>
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  screen: {
    flex: 1,
    backgroundColor: '#0B1F4D', // deep blue so the glass buttons have something to "float" on
  },
  loginWrap: {
    flex: 1,
    justifyContent: 'center',
    paddingHorizontal: 32,
  },
  content: {
    paddingHorizontal: 28,
    paddingTop: 70,
    paddingBottom: 50,
  },
  title: {
    color: '#fff',
    fontSize: 40,
    fontWeight: '800',
    letterSpacing: 0.5,
    textAlign: 'center',
  },
  subtitle: {
    color: 'rgba(255,255,255,0.65)',
    fontSize: 15,
    textAlign: 'center',
    marginTop: 4,
    marginBottom: 24,
  },
  input: {
    color: '#fff',
    backgroundColor: 'rgba(255,255,255,0.10)',
    borderColor: 'rgba(255,255,255,0.25)',
    borderWidth: 1,
    borderRadius: 24,
    paddingVertical: 14,
    paddingHorizontal: 20,
    marginVertical: 6,
    fontSize: 16,
  },
  sectionLabel: {
    color: 'rgba(255,255,255,0.5)',
    fontSize: 12,
    fontWeight: '700',
    letterSpacing: 1.5,
    marginTop: 22,
    marginBottom: 4,
    marginLeft: 8,
  },
  card: {
    backgroundColor: 'rgba(255,255,255,0.08)',
    borderColor: 'rgba(255,255,255,0.2)',
    borderWidth: 1,
    borderRadius: 24,
    padding: 18,
    marginTop: 10,
  },
  cardText: {
    color: '#fff',
    fontSize: 15,
    marginVertical: 2,
  },
  log: {
    color: 'rgba(255,255,255,0.55)',
    fontSize: 12,
    marginTop: 20,
  },
});
