import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gatorgrasp/widgets/glass_button.dart';

void main() {
  testWidgets('GlassButton shows its title and fires onPressed',
      (tester) async {
    var taps = 0;
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: GlassButton(title: 'Scan', onPressed: () => taps++),
      ),
    ));

    expect(find.text('Scan'), findsOneWidget);
    await tester.tap(find.text('Scan'));
    await tester.pump(const Duration(milliseconds: 200));
    expect(taps, 1);
  });
}
