// Shown briefly when the app opens, before the login/home screen.
import 'package:flutter/material.dart';

import '../theme.dart';

class SplashScreen extends StatefulWidget {
  const SplashScreen({super.key});

  @override
  State<SplashScreen> createState() => _SplashScreenState();
}

class _SplashScreenState extends State<SplashScreen>
    with SingleTickerProviderStateMixin {
  late final _anim = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 900),
  )..forward();

  late final _fade = CurvedAnimation(parent: _anim, curve: Curves.easeOut);
  late final _scale = Tween(begin: 0.85, end: 1.0)
      .animate(CurvedAnimation(parent: _anim, curve: Curves.easeOutBack));

  @override
  void dispose() {
    _anim.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Center(
      child: FadeTransition(
        opacity: _fade,
        child: ScaleTransition(
          scale: _scale,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const AppLogo(size: 120),
              const SizedBox(height: 28),
              const Text('GatorGrasp', style: GG.title),
              const SizedBox(height: 6),
              Text('Prosthetic arm companion', style: GG.subtitle),
            ],
          ),
        ),
      ),
    );
  }
}

/// TEMPORARY logo: an orange glass circle with a hand icon.
/// Replace with an Image.asset(...) once there's a real logo.
class AppLogo extends StatelessWidget {
  const AppLogo({super.key, this.size = 96});
  final double size;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: const Color.fromRGBO(250, 70, 22, 0.55),
        border: Border.all(
          color: const Color.fromRGBO(255, 170, 140, 0.6),
          width: 2,
        ),
        boxShadow: [
          BoxShadow(
            color: GG.orange.withAlpha(110),
            blurRadius: 32,
            offset: const Offset(0, 10),
          ),
        ],
      ),
      child:
          Icon(Icons.front_hand_rounded, size: size * 0.5, color: Colors.white),
    );
  }
}
