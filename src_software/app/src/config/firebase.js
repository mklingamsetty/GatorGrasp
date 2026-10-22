import { initializeApp } from 'firebase/app';
import { initializeAuth, getReactNativePersistence } from 'firebase/auth';
import { getFirestore } from 'firebase/firestore';
import AsyncStorage from '@react-native-async-storage/async-storage';

const firebaseConfig = {
  apiKey: "AIzaSyAn9tWE61Dcl9DSJzWk0NOrjypiF9IEx8k",
  authDomain: "gatorgrasp-a74fe.firebaseapp.com",
  projectId: "gatorgrasp-a74fe",
  storageBucket: "gatorgrasp-a74fe.firebasestorage.app",
  messagingSenderId: "97562623316",
  appId: "1:97562623316:web:c7da79fbafea6682975cc1",
};
const app = initializeApp(firebaseConfig);
export const auth = initializeAuth(app, {persistence: getReactNativePersistence(AsyncStorage)});
export const db = getFirestore(app);
