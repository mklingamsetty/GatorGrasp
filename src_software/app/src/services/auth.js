import {
  createUserWithEmailAndPassword, signInWithEmailAndPassword,
  signOut, onAuthStateChanged,
} from 'firebase/auth';
import { doc, setDoc, serverTimestamp } from 'firebase/firestore';
import { auth, db } from '../config/firebase';

export async function register(email, password) {
  const cred = await createUserWithEmailAndPassword(auth, email, password);
  await setDoc(doc(db, 'users', cred.user.uid), { email, createdAt: serverTimestamp() });
  return cred.user;
}
export const login = (email, password) =>
  signInWithEmailAndPassword(auth, email, password).then((c) => c.user);
export const logout = () => signOut(auth);
export const onUser = (cb) => onAuthStateChanged(auth, cb);
