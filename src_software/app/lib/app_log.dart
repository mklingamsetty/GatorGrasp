import 'package:firebase_core/firebase_core.dart';
import 'package:flutter/foundation.dart';

/// The on-screen log from the Expo app: newest first, capped at 2000 chars.
/// Shared by the login and home screens so it survives logging in/out.
class AppLog extends ChangeNotifier {
  String _text = '';
  String get text => _text;

  void say(String m) {
    final next = '$m\n$_text';
    _text = next.length > 2000 ? next.substring(0, 2000) : next;
    notifyListeners();
  }

  /// Runs [fn] and logs `ERR: <message>` instead of throwing.
  Future<void> guard(Future<void> Function() fn) async {
    try {
      await fn();
    } catch (e) {
      say('ERR: ${errorMessage(e)}');
    }
  }
}

String errorMessage(Object e) {
  if (e is FirebaseException) return e.message ?? e.code;
  if (e is StateError) return e.message;
  if (e is FormatException) return e.message;
  return e.toString();
}
