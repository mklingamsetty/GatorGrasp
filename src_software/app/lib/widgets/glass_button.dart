// Port of app(archived)/src/components/GlassButton.js
import 'package:flutter/material.dart';

import '../theme.dart';

enum GlassVariant { glass, primary, danger }

class GlassButton extends StatefulWidget {
  const GlassButton({
    super.key,
    required this.title,
    required this.onPressed,
    this.variant = GlassVariant.glass,
  });

  final String title;
  final VoidCallback? onPressed;
  final GlassVariant variant;

  @override
  State<GlassButton> createState() => _GlassButtonState();
}

class _GlassButtonState extends State<GlassButton> {
  bool _pressed = false;

  void _setPressed(bool v) {
    if (_pressed != v) setState(() => _pressed = v);
  }

  @override
  Widget build(BuildContext context) {
    final (bg, border, shadow) = switch (widget.variant) {
      GlassVariant.glass => (
          GG.white(0.12),
          GG.white(0.28),
          Colors.black.withAlpha(64)
        ),
      GlassVariant.primary => (
          const Color.fromRGBO(250, 70, 22, 0.55),
          const Color.fromRGBO(255, 170, 140, 0.6),
          GG.orange.withAlpha(128),
        ),
      GlassVariant.danger => (
          const Color.fromRGBO(255, 80, 80, 0.18),
          const Color.fromRGBO(255, 120, 120, 0.4),
          Colors.black.withAlpha(64),
        ),
    };

    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Semantics(
        button: true,
        label: widget.title,
        child: GestureDetector(
          onTapDown: (_) => _setPressed(true),
          onTapUp: (_) => _setPressed(false),
          onTapCancel: () => _setPressed(false),
          onTap: widget.onPressed,
          child: AnimatedScale(
            scale: _pressed ? 0.97 : 1,
            duration: const Duration(milliseconds: 90),
            child: AnimatedOpacity(
              opacity: _pressed ? 0.75 : 1,
              duration: const Duration(milliseconds: 90),
              child: Container(
                width: double.infinity,
                padding:
                    const EdgeInsets.symmetric(vertical: 15, horizontal: 22),
                decoration: BoxDecoration(
                  color: bg,
                  border: Border.all(color: border),
                  borderRadius: BorderRadius.circular(28),
                  boxShadow: [
                    BoxShadow(
                      color: shadow,
                      offset: const Offset(0, 8),
                      blurRadius: 16,
                    ),
                  ],
                ),
                alignment: Alignment.center,
                child: Text(
                  widget.title,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 16,
                    fontWeight: FontWeight.w600,
                    letterSpacing: 0.3,
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}
