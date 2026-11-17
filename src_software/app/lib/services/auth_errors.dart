// Port of app(archived)/src/services/authErrors.js.
// FlutterFire codes have no "auth/" prefix.
import 'package:firebase_auth/firebase_auth.dart';

class AuthErrorInfo {
  const AuthErrorInfo(this.title, this.message);
  final String title;
  final String message;
}

AuthErrorInfo authErrorInfo(Object e) {
  final code = e is FirebaseAuthException ? e.code : null;
  switch (code) {
    // Returned for empty fields (the login screen also checks these first).
    case 'missing-email':
      return const AuthErrorInfo('Missing email', 'Enter your email address.');
    case 'missing-password':
      return const AuthErrorInfo('Missing password', 'Enter your password.');
    case 'channel-error':
      return const AuthErrorInfo(
          'Missing details', 'Enter your email and password.');
    case 'invalid-email':
      return const AuthErrorInfo(
          'Invalid email', "That doesn't look like a valid email address.");
    case 'user-not-found':
      return const AuthErrorInfo('Account not found',
          'No account exists for that email. Check the spelling or tap Register to create one.');
    case 'wrong-password':
      return const AuthErrorInfo('Incorrect password',
          'The password you entered is wrong. Please try again.');
    case 'invalid-credential':
    case 'invalid-login-credentials':
    case 'INVALID_LOGIN_CREDENTIALS':
      return const AuthErrorInfo('Login failed',
          'Incorrect email or password, or no account exists for that email.');
    case 'user-disabled':
      return const AuthErrorInfo(
          'Account disabled', 'This account has been disabled.');
    case 'too-many-requests':
      return const AuthErrorInfo('Too many attempts',
          'Please wait a few minutes before trying again.');
    case 'network-request-failed':
      return const AuthErrorInfo(
          'No connection', 'Check your internet connection and try again.');
    case 'email-already-in-use':
      return const AuthErrorInfo('Email already registered',
          'An account with that email already exists. Try logging in instead.');
    case 'weak-password':
      return const AuthErrorInfo(
          'Weak password', 'Password must be at least 6 characters.');
    default:
      // Never show raw platform/channel text (e.g. "dev.flutter.pigeon...").
      // Firebase's own message is OK if it's a normal sentence.
      final msg = e is FirebaseAuthException ? e.message : null;
      final readable = msg != null &&
          msg.isNotEmpty &&
          !msg.contains('dev.flutter') &&
          !msg.contains('PlatformException');
      return AuthErrorInfo(
        'Something went wrong',
        readable ? msg : 'Please try again in a moment.',
      );
  }
}
