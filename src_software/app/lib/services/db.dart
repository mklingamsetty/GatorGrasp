// Port of app(archived)/src/services/db.js. Firestore paths are unchanged so
// data written by the Expo app still loads.
import 'package:cloud_firestore/cloud_firestore.dart';

import '../ble/packets.dart';

final _db = FirebaseFirestore.instance;

CollectionReference<Map<String, dynamic>> _profiles(String uid) =>
    _db.collection('users').doc(uid).collection('profiles');
CollectionReference<Map<String, dynamic>> _readings(String uid) =>
    _db.collection('users').doc(uid).collection('readings');

Future<String> saveProfile(
  String uid,
  Map<String, dynamic> profile, {
  String? id,
}) async {
  final data = {...profile, 'updatedAt': FieldValue.serverTimestamp()};
  if (id != null) {
    await _profiles(uid).doc(id).set(data, SetOptions(merge: true));
    return id;
  }
  final ref = await _profiles(uid).add(data);
  return ref.id;
}

Future<List<Map<String, dynamic>>> listProfiles(String uid) async {
  final snap =
      await _profiles(uid).orderBy('updatedAt', descending: true).get();
  return [
    for (final d in snap.docs) {'id': d.id, ...d.data()},
  ];
}

class Reading {
  const Reading({
    required this.id,
    required this.raw,
    required this.volts,
    this.createdAt,
  });

  final String id;
  final List<int> raw;
  final List<double> volts;
  final DateTime? createdAt; // null while the server timestamp is pending

  factory Reading.fromDoc(QueryDocumentSnapshot<Map<String, dynamic>> d) {
    final data = d.data();
    return Reading(
      id: d.id,
      raw: [
        for (final v in (data['raw'] as List? ?? const [])) (v as num).toInt()
      ],
      volts: [
        for (final v in (data['volts'] as List? ?? const []))
          (v as num).toDouble()
      ],
      createdAt: (data['createdAt'] as Timestamp?)?.toDate(),
    );
  }
}

/// raw = ADC counts (0..4095). Voltage is an approximate conversion.
Future<String> saveReading(String uid, List<int> raw) async {
  final volts =
      raw.map((r) => double.parse(toVolts(r).toStringAsFixed(3))).toList();
  final ref = await _readings(uid).add({
    'raw': raw,
    'volts': volts,
    'createdAt': FieldValue.serverTimestamp(),
  });
  return ref.id;
}

/// Live, newest-first list of the user's readings, up to [limit].
/// Updates automatically when a reading is saved.
Stream<List<Reading>> watchReadings(String uid, {int limit = 50}) =>
    _readings(uid)
        .orderBy('createdAt', descending: true)
        .limit(limit)
        .snapshots()
        .map((snap) => snap.docs.map(Reading.fromDoc).toList());

/// Newest first.
Future<List<Reading>> listReadings(String uid, {int n = 5}) async {
  final snap = await _readings(uid)
      .orderBy('createdAt', descending: true)
      .limit(n)
      .get();
  return snap.docs.map(Reading.fromDoc).toList();
}
