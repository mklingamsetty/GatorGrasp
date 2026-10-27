// THROWAWAY test harness: login + BLE connect + servo buttons + ADC readout/save.
import React, { useEffect, useRef, useState } from 'react';
import { View, Text, TextInput, Button, ScrollView } from 'react-native';
import { onUser, login, register, logout } from './src/services/auth';
import { saveReading, listReadings } from './src/services/db';
import ble from './src/ble/BleService';

// Rough conversion: 12-bit ADC, 12 dB attenuation (~0-3.1 V usable). Not calibrated.
const toVolts = (raw) => ((raw / 4095) * 3.1).toFixed(2);

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
      <View style={{ padding: 40, paddingTop: 80 }}>
        <TextInput placeholder="email" autoCapitalize="none" value={email} onChangeText={setEmail} />
        <TextInput placeholder="password" secureTextEntry value={pw} onChangeText={setPw} />
        <Button title="Login" onPress={guard(() => login(email, pw))} />
        <Button title="Register" onPress={guard(() => register(email, pw))} />
        <Text style={{ marginTop: 20 }}>{log}</Text>
      </View>
    );
  }

  return (
    <ScrollView contentContainerStyle={{ padding: 40, paddingTop: 80 }}>
      <Button title="Scan" onPress={scan} />
      {Object.values(devices).map((d) => (
        <Button key={d.id} title={`Connect ${d.name || d.id}`} onPress={() => connect(d.id)} />
      ))}

      <Button title="0 degrees" onPress={() => servo(0)} />
      <Button title="90 degrees" onPress={() => servo(90)} />
      <Button title="180 degrees" onPress={() => servo(180)} />

      <Button title="Save reading to cloud" onPress={saveToCloud} />
      <Button title="Load readings from cloud" onPress={loadFromCloud} />
      <Button title="Logout" onPress={logout} />

      <Text style={{ fontSize: 18, marginTop: 20 }}>Connected: {String(connected)}</Text>
      <Text>ADC1 (GPIO1): {emg[0]} ({toVolts(emg[0])} V)</Text>
      <Text>ADC2 (GPIO11): {emg[1]} ({toVolts(emg[1])} V)</Text>
      <Text style={{ marginTop: 20 }}>LOG:{'\n'}{log}</Text>
    </ScrollView>
  );
}