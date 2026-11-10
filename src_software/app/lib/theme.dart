import 'package:flutter/material.dart';

/// Shared look, mirroring the StyleSheet in app(archived)/App.js.
class GG {
  static const background =
      Color(0xFF0B1F4D); // deep blue for the glass to float on
  static const popup = Color(0xFF14295F);
  static const orange = Color(0xFFFA4616);

  static Color white(double alpha) => Color.fromRGBO(255, 255, 255, alpha);

  static const title = TextStyle(
    color: Colors.white,
    fontSize: 40,
    fontWeight: FontWeight.w800,
    letterSpacing: 0.5,
  );

  static final subtitle = TextStyle(color: white(0.65), fontSize: 15);

  static final sectionLabel = TextStyle(
    color: white(0.5),
    fontSize: 12,
    fontWeight: FontWeight.w700,
    letterSpacing: 1.5,
  );

  static const cardText = TextStyle(color: Colors.white, fontSize: 15);

  static final log = TextStyle(color: white(0.55), fontSize: 12);

  static BoxDecoration card() => BoxDecoration(
        color: white(0.08),
        border: Border.all(color: white(0.2)),
        borderRadius: BorderRadius.circular(24),
      );
}
