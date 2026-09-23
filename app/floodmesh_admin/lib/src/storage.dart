// Persistence: encrypted admin identity (flutter_secure_storage) and the
// local registration history (shared_preferences).

import 'dart:convert';
import 'dart:isolate';

import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'crypto.dart';

class IdentityStore {
  static const _key = 'fm_admin_identity_v1';
  static const _storage = FlutterSecureStorage();

  static Future<EncryptedIdentity?> load() async {
    final s = await _storage.read(key: _key);
    if (s == null) return null;
    return EncryptedIdentity.fromJsonString(s);
  }

  static Future<void> save(EncryptedIdentity id) =>
      _storage.write(key: _key, value: id.toJsonString());

  static Future<void> reset() => _storage.delete(key: _key);

  /// Generates a new key pair and encrypts it (PBKDF2 runs off the UI thread).
  static Future<(P256KeyPair, EncryptedIdentity)> create(
      String passphrase) async {
    final result = await Isolate.run(() {
      final kp = P256KeyPair.generate();
      final enc = EncryptedIdentity.encrypt(kp, passphrase);
      return (kp.privateBytes, enc.toJsonString());
    });
    final enc = EncryptedIdentity.fromJsonString(result.$2);
    await save(enc);
    return (P256KeyPair.fromPrivateBytes(result.$1), enc);
  }

  /// Decrypts off the UI thread. Throws [WrongPassphraseException].
  static Future<P256KeyPair> unlock(
      EncryptedIdentity enc, String passphrase) async {
    final json = enc.toJsonString();
    final priv = await Isolate.run(() =>
        EncryptedIdentity.fromJsonString(json).decrypt(passphrase).privateBytes);
    return P256KeyPair.fromPrivateBytes(priv);
  }
}

class HistoryEntry {
  HistoryEntry({
    required this.cs,
    required this.role,
    required this.exp,
    required this.time,
    this.cert = false,
  });

  final String cs;
  final String role;

  /// Unix seconds, 0 unless responder.
  final int exp;

  /// Unix seconds when the registration was done.
  final int time;

  /// Responder certificate installed.
  final bool cert;

  Map<String, Object> toJson() =>
      {'cs': cs, 'role': role, 'exp': exp, 'time': time, 'cert': cert};

  factory HistoryEntry.fromJson(Map<String, dynamic> m) => HistoryEntry(
        cs: m['cs'] as String,
        role: m['role'] as String,
        exp: (m['exp'] as num).toInt(),
        time: (m['time'] as num).toInt(),
        cert: m['cert'] == true,
      );
}

class HistoryStore {
  static const _key = 'fm_history_v1';

  static Future<List<HistoryEntry>> load() async {
    final p = await SharedPreferences.getInstance();
    final list = p.getStringList(_key) ?? const [];
    return list
        .map((s) => HistoryEntry.fromJson(jsonDecode(s) as Map<String, dynamic>))
        .toList();
  }

  static Future<void> add(HistoryEntry e) async {
    final p = await SharedPreferences.getInstance();
    final list = p.getStringList(_key) ?? <String>[];
    list.add(jsonEncode(e.toJson()));
    await p.setStringList(_key, list);
  }

  static Future<void> clear() async {
    final p = await SharedPreferences.getInstance();
    await p.remove(_key);
  }

  static String toCsv(List<HistoryEntry> entries) {
    final sb = StringBuffer('time_utc,call_sign,role,exp_unix,exp_utc,cert\n');
    for (final e in entries) {
      final t = DateTime.fromMillisecondsSinceEpoch(e.time * 1000, isUtc: true);
      final x = e.exp == 0
          ? ''
          : DateTime.fromMillisecondsSinceEpoch(e.exp * 1000, isUtc: true)
              .toIso8601String();
      sb.writeln(
          '${t.toIso8601String()},${e.cs},${e.role},${e.exp},$x,${e.cert}');
    }
    return sb.toString();
  }
}
