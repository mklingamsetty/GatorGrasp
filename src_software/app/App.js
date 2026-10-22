// THROWAWAY debug harness - proves auth + BLE + Firestore work end to end.
// Frontend teammate: replace this with real screens; keep calling the services in src/.
import React, { useEffect, useRef, useState } from 'react';
import { View, Text, TextInput, Button, ScrollView } from 'react-native';
import { onUser, login, register, logout } from './src/services/auth';
import { saveProfile, listProfiles } from './src/services/db';
import ble from './src/ble/BleService';

export default function App() {
  const [user, setUser] = useState(null);
  const [email, setEmail] = useState('');
  const [pw, setPw] = useState('');
  const [devices, setDevices] = useState({});
  const [connected, setConnected] = useState(false);
  const [emg, setEmg] = useState([0, 0, 0]);
  const [log, setLog] = useState('');
  const stopScan = useRef(null);
  const emgSub = useRef(null);

  const say = (m) => setLog((l) => `${m}\n${l}`.slice(0, 800));
  const guard = (fn) => async () => { try { await fn(); } catch (e) { say(`ERR: ${e.message}`); } };

  useEffect(() => onUser(setUser), []);

  const scan = guard(async () => {
    if (!(await ble.requestPermissions())) return say('BLE permission denied');
    setDevices({});
    stopScan.current = ble.scan((d) => setDevices((p) => ({ ...p, [d.id]: d })), (e) => say(e.message));
    say('scanning...');
  });

  const connect = (id) => guard(async () => {
    await ble.connect(id, () => { setConnected(false); say('disconnected'); });
    setConnected(true);
    emgSub.current = ble.subscribeEmg(setEmg, (e) => say(e.message));
    say('connected');
  })();

  const syncUp = guard(async () => {
    const cal = await ble.readCalibration();
    await saveProfile(user.uid, { name: 'default', ...cal });
    say(`saved to cloud: ${JSON.stringify(cal)}`);
  });

  const syncDown = guard(async () => {
    const [p] = await listProfiles(user.uid);
    if (!p) return say('no profiles');
    await ble.writeCalibration(p);
    say(`pushed to arm: ${p.name}`);
  });

  if (!user) {
    return (
      <View style={{ padding: 40 }}>
        <TextInput placeholder="email" autoCapitalize="none" value={email} onChangeText={setEmail} />
        <TextInput placeholder="password" secureTextEntry value={pw} onChangeText={setPw} />
        <Button title="Login" onPress={guard(() => login(email, pw))} />
        <Button title="Register" onPress={guard(() => register(email, pw))} />
        <Text>{log}</Text>
      </View>
    );
  }

  return (
    <ScrollView contentContainerStyle={{ padding: 40 }}>
      <Text>{user.email}</Text>
      <Button title="Logout" onPress={logout} />
      <Button title="Scan" onPress={scan} />
      {Object.values(devices).map((d) => (
        <Button key={d.id} title={`Connect ${d.name || d.id}`} onPress={() => connect(d.id)} />
      ))}
      <Text>Connected: {String(connected)}</Text>
      <Text>EMG: {emg.join(' | ')}</Text>
      <Button title="Open hand (all 0)" onPress={guard(() => ble.writeServos([0,0,0,0,0,0]))} />
      <Button title="Close hand (all 180)" onPress={guard(() => ble.writeServos([180,180,180,180,180,180]))} />
      <Button title="Calibration: arm -> cloud" onPress={syncUp} />
      <Button title="Calibration: cloud -> arm" onPress={syncDown} />
      <Text>{log}</Text>
    </ScrollView>
  );
}
