// Port of app(archived)/src/services/auth.js
import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';

final _auth = FirebaseAuth.instance;

Future<User> register(String email, String password) async {
  final cred = await _auth.createUserWithEmailAndPassword(
    email: email,
    password: password,
  );
  final user = cred.user!;
  await FirebaseFirestore.instance.collection('users').doc(user.uid).set({
    'email': email,
    'createdAt': FieldValue.serverTimestamp(),
  });
  return user;
}

Future<User> login(String email, String password) async {
  final cred = await _auth.signInWithEmailAndPassword(
    email: email,
    password: password,
  );
  return cred.user!;
}

Future<void> logout() => _auth.signOut();

Stream<User?> onUser() => _auth.authStateChanges();
