// Login branch of app(archived)/App.js
import 'package:flutter/material.dart';

import '../app_log.dart';
import '../services/auth.dart' as auth;
import '../services/auth_errors.dart';
import '../theme.dart';
import '../widgets/error_popup.dart';
import '../widgets/glass_button.dart';

class LoginScreen extends StatefulWidget {
  const LoginScreen({super.key, required this.log});
  final AppLog log;

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

  /// Auth errors get the friendly popup (authErrorInfo) and are also logged.
  Future<void> _run(Future<void> Function() fn) async {
    try {
      await fn();
    } catch (e) {
      final info = authErrorInfo(e);
      widget.log.say('ERR: ${info.message}');
      if (mounted) {
        await showErrorPopup(context, title: info.title, message: info.message);
      }
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
              title: 'Login',
              variant: GlassVariant.primary,
              onPressed: () =>
                  _run(() => auth.login(_email.text.trim(), _pw.text)),
            ),
            GlassButton(
              title: 'Register',
              onPressed: () =>
                  _run(() => auth.register(_email.text.trim(), _pw.text)),
            ),
            const SizedBox(height: 6),
            ListenableBuilder(
              listenable: widget.log,
              builder: (_, __) => Text(widget.log.text, style: GG.log),
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
