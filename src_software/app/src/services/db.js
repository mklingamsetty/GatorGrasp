// Firestore layout:  users/{uid}  and  users/{uid}/profiles/{profileId}
// profile = { name, emgMin:[3], emgMax:[3], updatedAt }
import { collection, doc, addDoc, setDoc, getDocs, query, orderBy, serverTimestamp } from 'firebase/firestore';
import { db } from '../config/firebase';

const profiles = (uid) => collection(db, 'users', uid, 'profiles');

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
