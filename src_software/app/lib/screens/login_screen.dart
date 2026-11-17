// Login branch of app(archived)/App.js
import 'package:flutter/material.dart';

import '../services/auth.dart' as auth;
import '../services/auth_errors.dart';
import '../theme.dart';
import '../widgets/error_popup.dart';
import '../widgets/glass_button.dart';

class LoginScreen extends StatefulWidget {
  const LoginScreen({super.key});

  @override
  State<LoginScreen> createState() => _LoginScreenState();
}

class _LoginScreenState extends State<LoginScreen> {
  final _email = TextEditingController();
  final _pw = TextEditingController();

  @override
  void dispose() {
    _email.dispose();
    _pw.dispose();
    super.dispose();
  }

  /// Which action is in progress ('login' / 'register'), or null.
  String? _busy;

  static final _emailPattern = RegExp(r'^[^@\s]+@[^@\s]+\.[^@\s]+$');

  /// Catches empty/malformed input before calling Firebase, which otherwise
  /// returns unhelpful errors for empty fields.
  AuthErrorInfo? _validate({required bool registering}) {
    final email = _email.text.trim();
    final pw = _pw.text;
    if (email.isEmpty && pw.isEmpty) {
      return const AuthErrorInfo(
          'Missing details', 'Enter your email and password.');
    }
    if (email.isEmpty) {
      return const AuthErrorInfo('Missing email', 'Enter your email address.');
    }
    if (!_emailPattern.hasMatch(email)) {
      return const AuthErrorInfo(
          'Invalid email', "That doesn't look like a valid email address.");
    }
    if (pw.isEmpty) {
      return const AuthErrorInfo('Missing password', 'Enter your password.');
    }
    if (registering && pw.length < 6) {
      return const AuthErrorInfo(
          'Weak password', 'Password must be at least 6 characters.');
    }
    return null;
  }

  /// Errors are shown only as a popup (not in the log).
  Future<void> _submit({required bool registering}) async {
    if (_busy != null) return;
    FocusScope.of(context).unfocus();

    final problem = _validate(registering: registering);
    if (problem != null) {
      await showErrorPopup(context,
          title: problem.title, message: problem.message);
      return;
    }

    setState(() => _busy = registering ? 'register' : 'login');
    try {
      final email = _email.text.trim();
      if (registering) {
        await auth.register(email, _pw.text);
      } else {
        await auth.login(email, _pw.text);
      }
      // On success the auth gate swaps this screen out automatically.
    } catch (e) {
      final info = authErrorInfo(e);
      if (mounted) {
        await showErrorPopup(context, title: info.title, message: info.message);
      }
    } finally {
      if (mounted) setState(() => _busy = null);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Center(
      child: SingleChildScrollView(
        padding: const EdgeInsets.symmetric(horizontal: 32),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const Text('GatorGrasp',
                style: GG.title, textAlign: TextAlign.center),
            const SizedBox(height: 4),
            Text('Sign in to your arm',
                style: GG.subtitle, textAlign: TextAlign.center),
            const SizedBox(height: 24),
            _GlassField(
              controller: _email,
              hint: 'email',
              keyboardType: TextInputType.emailAddress,
            ),
            _GlassField(controller: _pw, hint: 'password', obscure: true),
            const SizedBox(height: 6),
            GlassButton(
              title: _busy == 'login' ? 'Logging in…' : 'Login',
              variant: GlassVariant.primary,
              onPressed:
                  _busy == null ? () => _submit(registering: false) : null,
            ),
            GlassButton(
              title: _busy == 'register' ? 'Creating account…' : 'Register',
              onPressed:
                  _busy == null ? () => _submit(registering: true) : null,
            ),
          ],
        ),
      ),
    );
  }
}

class _GlassField extends StatelessWidget {
  const _GlassField({
    required this.controller,
    required this.hint,
    this.obscure = false,
    this.keyboardType,
  });

  final TextEditingController controller;
  final String hint;
  final bool obscure;
  final TextInputType? keyboardType;

  @override
  Widget build(BuildContext context) {
    final border = OutlineInputBorder(
      borderRadius: BorderRadius.circular(24),
      borderSide: BorderSide(color: GG.white(0.25)),
    );
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: TextField(
        controller: controller,
        obscureText: obscure,
        keyboardType: keyboardType,
        autocorrect: false,
        enableSuggestions: !obscure,
        textCapitalization: TextCapitalization.none,
        style: const TextStyle(color: Colors.white, fontSize: 16),
        cursorColor: Colors.white,
        decoration: InputDecoration(
          hintText: hint,
          hintStyle: TextStyle(color: GG.white(0.45)),
          filled: true,
          fillColor: GG.white(0.10),
          contentPadding:
              const EdgeInsets.symmetric(vertical: 14, horizontal: 20),
          enabledBorder: border,
          focusedBorder: border.copyWith(
            borderSide: BorderSide(color: GG.white(0.5)),
          ),
        ),
      ),
    );
  }
}
