// Full list of the user's saved EMG readings (replaces dumping them in the log).
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../app_log.dart';
import '../services/db.dart';
import '../theme.dart';
import '../widgets/glass_button.dart';

class ReadingsScreen extends StatefulWidget {
  const ReadingsScreen({super.key, required this.uid});
  final String uid;

  @override
  State<ReadingsScreen> createState() => _ReadingsScreenState();
}

class _ReadingsScreenState extends State<ReadingsScreen> {
  static const _pageSize = 50;
  int _limit = _pageSize;
  late Stream<List<Reading>> _stream = watchReadings(widget.uid, limit: _limit);

  void _loadMore() => setState(() {
        _limit += _pageSize;
        _stream = watchReadings(widget.uid, limit: _limit);
      });

  @override
  Widget build(BuildContext context) {
    return AnnotatedRegion<SystemUiOverlayStyle>(
      value: SystemUiOverlayStyle.light,
      child: Scaffold(
        backgroundColor: GG.background,
        appBar: AppBar(
          backgroundColor: GG.background,
          foregroundColor: Colors.white,
          elevation: 0,
          scrolledUnderElevation: 0,
          title: const Text(
            'Saved readings',
            style: TextStyle(fontWeight: FontWeight.w700),
          ),
        ),
        body: StreamBuilder<List<Reading>>(
          stream: _stream,
          builder: (context, snap) {
            if (snap.hasError) {
              return _Message('Could not load readings.\n'
                  '${errorMessage(snap.error!)}');
            }
            if (!snap.hasData) {
              return const Center(child: CircularProgressIndicator());
            }
            final rows = snap.data!;
            if (rows.isEmpty) {
              return const _Message('No saved readings yet.\n'
                  'Connect to the arm and tap "Save reading to cloud".');
            }
            final mayHaveMore = rows.length >= _limit;
            return ListView.builder(
              padding: const EdgeInsets.fromLTRB(20, 8, 20, 40),
              itemCount: rows.length + 1,
              itemBuilder: (context, i) {
                if (i == rows.length) {
                  return Padding(
                    padding: const EdgeInsets.only(top: 8),
                    child: mayHaveMore
                        ? GlassButton(title: 'Load more', onPressed: _loadMore)
                        : Text(
                            '${rows.length} reading${rows.length == 1 ? '' : 's'}',
                            textAlign: TextAlign.center,
                            style: GG.log,
                          ),
                  );
                }
                return _ReadingCard(rows[i]);
              },
            );
          },
        ),
      ),
    );
  }
}

class _ReadingCard extends StatelessWidget {
  const _ReadingCard(this.r);
  final Reading r;

  @override
  Widget build(BuildContext context) {
    final channels = <Widget>[];
    for (var i = 0; i < r.raw.length; i++) {
      final volts = i < r.volts.length ? r.volts[i].toStringAsFixed(3) : '?';
      channels.add(Padding(
        padding: const EdgeInsets.only(top: 6),
        child: Row(
          children: [
            SizedBox(
              width: 56,
              child: Text('ADC${i + 1}', style: GG.sectionLabel),
            ),
            Expanded(child: Text('${r.raw[i]}', style: GG.cardText)),
            Text('$volts V', style: GG.cardText),
          ],
        ),
      ));
    }

    return Container(
      margin: const EdgeInsets.symmetric(vertical: 6),
      padding: const EdgeInsets.fromLTRB(18, 14, 18, 16),
      decoration: GG.card(),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            r.createdAt == null ? 'Saving…' : formatTimestamp(r.createdAt!),
            style: const TextStyle(
              color: Colors.white,
              fontSize: 15,
              fontWeight: FontWeight.w700,
            ),
          ),
          ...channels,
        ],
      ),
    );
  }
}

class _Message extends StatelessWidget {
  const _Message(this.text);
  final String text;

  @override
  Widget build(BuildContext context) => Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Text(text, textAlign: TextAlign.center, style: GG.subtitle),
        ),
      );
}

const _months = [
  'Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', //
  'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec',
];

/// e.g. "Nov 10, 2026 · 3:07:42 PM" in the phone's local time.
String formatTimestamp(DateTime t) {
  final l = t.toLocal();
  final h = l.hour % 12 == 0 ? 12 : l.hour % 12;
  String two(int n) => n.toString().padLeft(2, '0');
  final ampm = l.hour < 12 ? 'AM' : 'PM';
  return '${_months[l.month - 1]} ${l.day}, ${l.year} · '
      '$h:${two(l.minute)}:${two(l.second)} $ampm';
}
