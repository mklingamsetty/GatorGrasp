import {
  collection, doc, addDoc, setDoc, getDocs, query, orderBy, limit, serverTimestamp,
} from 'firebase/firestore';
import { db } from '../config/firebase';

const profiles = (uid) => collection(db, 'users', uid, 'profiles');
const readings = (uid) => collection(db, 'users', uid, 'readings');

export async function saveProfile(uid, profile, id = null) {
  const data = { ...profile, updatedAt: serverTimestamp() };
  if (id) { await setDoc(doc(profiles(uid), id), data, { merge: true }); return id; }
  const ref = await addDoc(profiles(uid), data);
  return ref.id;
}

export async function listProfiles(uid) {
  const snap = await getDocs(query(profiles(uid), orderBy('updatedAt', 'desc')));
  return snap.docs.map((d) => ({ id: d.id, ...d.data() }));
}

// raw = [ch0, ch1, ch2] ADC counts (0..4095). Voltage is an approximate conversion.
export async function saveReading(uid, raw) {
  const volts = raw.map((r) => Number(((r / 4095) * 3.1).toFixed(3)));
  const ref = await addDoc(readings(uid), { raw, volts, createdAt: serverTimestamp() });
  return ref.id;
}

// newest first. Each item: { id, raw:[..], volts:[..], createdAt: Timestamp|null }
export async function listReadings(uid, n = 5) {
  const snap = await getDocs(query(readings(uid), orderBy('createdAt', 'desc'), limit(n)));
  return snap.docs.map((x) => ({ id: x.id, ...x.data() }));
}