// Port of app(archived)/src/components/ErrorPopup.js
import 'package:flutter/material.dart';

import '../theme.dart';
import 'glass_button.dart';

Future<void> showErrorPopup(
  BuildContext context, {
  required String title,
  required String message,
}) {
  return showGeneralDialog(
    context: context,
    barrierDismissible: true, // tap backdrop to close, like the Expo version
    barrierLabel: 'Dismiss',
    barrierColor: Colors.black.withAlpha(153),
    transitionDuration: const Duration(milliseconds: 200),
    transitionBuilder: (_, anim, __, child) =>
        FadeTransition(opacity: anim, child: child),
    pageBuilder: (ctx, _, __) => Center(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 32),
        child: Material(
          color: Colors.transparent,
          child: Container(
            padding: const EdgeInsets.all(22),
            decoration: BoxDecoration(
              color: GG.popup,
              border: Border.all(color: GG.white(0.25)),
              borderRadius: BorderRadius.circular(24),
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  title,
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 20,
                    fontWeight: FontWeight.w700,
                  ),
                ),
                const SizedBox(height: 8),
                Text(
                  message,
                  textAlign: TextAlign.center,
                  style: TextStyle(color: GG.white(0.8), fontSize: 15),
                ),
                const SizedBox(height: 12),
                GlassButton(
                  title: 'OK',
                  variant: GlassVariant.primary,
                  onPressed: () => Navigator.of(ctx).pop(),
                ),
              ],
            ),
          ),
        ),
      ),
    ),
  );
}
