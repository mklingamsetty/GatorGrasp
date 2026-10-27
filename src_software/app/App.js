// THROWAWAY debug harness - proves auth + BLE + Firestore work end to end.
// Styling pass: GatorGrasp title + glass buttons. Logic is unchanged.
import React, { useEffect, useRef, useState } from 'react';
import { View, Text, TextInput, ScrollView, StyleSheet, StatusBar } from 'react-native';
import { onUser, login, register, logout } from './src/services/auth';
import { saveProfile, listProfiles } from './src/services/db';
import ble from './src/ble/BleService';
import GlassButton from './src/components/GlassButton';

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
