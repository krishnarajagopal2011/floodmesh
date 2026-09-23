// FMREG v1 BLE provisioning protocol: constants, INFO parsing, command and
// signed-message builders. Pure Dart, no Flutter imports.
//
// Source of truth: docs/ble-provisioning-protocol.md. Change that document
// first, then this file.

import 'dart:convert';

import 'crypto.dart';

// §2 GATT layout
const String kServiceUuid = '6e3f0001-8a4c-4b8e-9f10-f10d3e5a0001';
const String kInfoCharUuid = '6e3f0002-8a4c-4b8e-9f10-f10d3e5a0001';
const String kCmdCharUuid = '6e3f0003-8a4c-4b8e-9f10-f10d3e5a0001';
const String kRespCharUuid = '6e3f0004-8a4c-4b8e-9f10-f10d3e5a0001';

const int kRequestedMtu = 517;
const int kMaxCmdBytes = 600;
const String kDeviceNamePrefix = 'FM-';

/// §5.2: exp <= now + FM_RESPONDER_MAX_S.
const int kResponderMaxSeconds = 864000;

const String roleCivilian = 'civilian';
const String roleRelay = 'relay';
const String roleResponder = 'responder';
const List<String> kRoles = [roleCivilian, roleRelay, roleResponder];

const String opSetAdmin = 'set_admin';
const String opSetRole = 'set_role';
const String opSetCert = 'set_cert';
const String opReboot = 'reboot';

/// Parsed INFO characteristic (§3).
class DeviceInfo {
  DeviceInfo({
    required this.v,
    required this.cs,
    required this.fw,
    required this.board,
    required this.role,
    required this.exp,
    required this.now,
    required this.admin,
    required this.adminFp,
    required this.nonce,
    required this.devPub,
    required this.hasCert,
    required this.fails,
  });

  final int v;
  final String cs;
  final String fw;
  final String board;
  final String role;
  final int exp;
  final int now;

  /// `none` | `compiled` | `stored`
  final String admin;
  final String adminFp;
  final String nonce;
  final String devPub;
  final bool hasCert;
  final int fails;

  bool get hasAdmin => admin != 'none';

  static DeviceInfo parse(String jsonText) {
    final m = jsonDecode(jsonText);
    if (m is! Map<String, dynamic>) {
      throw const FormatException('INFO is not a JSON object');
    }
    int i(String k) => (m[k] as num?)?.toInt() ?? 0;
    String s(String k) => (m[k] as String?) ?? '';
    final info = DeviceInfo(
      v: i('v'),
      cs: s('cs'),
      fw: s('fw'),
      board: s('board'),
      role: s('role'),
      exp: i('exp'),
      now: i('now'),
      admin: s('admin'),
      adminFp: s('adminFp'),
      nonce: s('nonce'),
      devPub: s('devPub'),
      hasCert: m['hasCert'] == true,
      fails: i('fails'),
    );
    if (info.v != 1) {
      throw FormatException('Unsupported protocol version ${info.v}');
    }
    if (info.cs.isEmpty) throw const FormatException('INFO has no call sign');
    if (info.nonce.isEmpty) throw const FormatException('INFO has no nonce');
    return info;
  }
}

/// Parsed RESP characteristic.
class CmdResponse {
  CmdResponse(this.ok, this.err, this.raw);

  final bool ok;
  final String? err;
  final Map<String, dynamic> raw;

  String? get adminFp => raw['adminFp'] as String?;
  String? get role => raw['role'] as String?;
  int get exp => (raw['exp'] as num?)?.toInt() ?? 0;
  String get devPub => (raw['devPub'] as String?) ?? '';

  static CmdResponse parse(String jsonText) {
    final m = jsonDecode(jsonText);
    if (m is! Map<String, dynamic> || m['ok'] is! bool) {
      throw const FormatException('RESP is not a valid response object');
    }
    return CmdResponse(m['ok'] as bool, m['err'] as String?, m);
  }
}

// ---------------------------------------------------------------------------
// §4.1 PIN proof
// ---------------------------------------------------------------------------

bool isValidPin(String pin) => RegExp(r'^[0-9]{6}$').hasMatch(pin);

/// lowercase hex( HMAC-SHA256( key = PIN ASCII, msg = nonce + "|" + op ) ).
/// The nonce is used exactly as the 32-char hex string from INFO.
String authFor(String pin, String nonce, String op) =>
    toHex(hmacSha256(ascii.encode(pin), utf8.encode('$nonce|$op')));

// ---------------------------------------------------------------------------
// Signed messages
// ---------------------------------------------------------------------------

/// §5.2: `FMREG1|<cs>|<role>|<now>|<exp>|<nonce>`
String regMessage(String cs, String role, int now, int exp, String nonce) =>
    'FMREG1|$cs|$role|$now|$exp|$nonce';

/// §5.3: `FMCERT1|<cs>|responder|<exp>|<devPub hex, lowercase>`
String certMessage(String cs, int exp, String devPubHex) =>
    'FMCERT1|$cs|$roleResponder|$exp|${devPubHex.toLowerCase()}';

// ---------------------------------------------------------------------------
// Commands (§5). Field order follows the document.
// ---------------------------------------------------------------------------

String _enc(Map<String, Object> m) {
  final s = jsonEncode(m);
  if (utf8.encode(s).length > kMaxCmdBytes) {
    throw StateError('Command exceeds $kMaxCmdBytes bytes');
  }
  return s;
}

String buildSetAdmin(
        {required String pubHex, required String pin, required String nonce}) =>
    _enc({
      'op': opSetAdmin,
      'pub': pubHex.toLowerCase(),
      'auth': authFor(pin, nonce, opSetAdmin),
    });

/// Builds and signs `set_role`. For civilian/relay [exp] must be 0.
String buildSetRole({
  required P256KeyPair admin,
  required String cs,
  required String role,
  required int now,
  required int exp,
  required String pin,
  required String nonce,
}) {
  if (!kRoles.contains(role)) throw ArgumentError('bad role $role');
  if (role == roleResponder) {
    if (!(now < exp && exp <= now + kResponderMaxSeconds)) {
      throw ArgumentError('responder exp must satisfy now < exp <= now+10d');
    }
  } else if (exp != 0) {
    throw ArgumentError('exp must be 0 for $role');
  }
  final sig = admin.signHex(regMessage(cs, role, now, exp, nonce));
  return _enc({
    'op': opSetRole,
    'role': role,
    'now': now,
    'exp': exp,
    'sig': sig,
    'auth': authFor(pin, nonce, opSetRole),
  });
}

String buildSetCert({
  required P256KeyPair admin,
  required String cs,
  required int exp,
  required String devPubHex,
  required String pin,
  required String nonce,
}) {
  final sig = admin.signHex(certMessage(cs, exp, devPubHex));
  return _enc({
    'op': opSetCert,
    'exp': exp,
    'sig': sig,
    'auth': authFor(pin, nonce, opSetCert),
  });
}

String buildReboot({required String pin, required String nonce}) =>
    _enc({'op': opReboot, 'auth': authFor(pin, nonce, opReboot)});

// ---------------------------------------------------------------------------
// §5.5 Errors
// ---------------------------------------------------------------------------

const Map<String, String> kErrorText = {
  'json': 'The unit could not parse the command (JSON error).',
  'op': 'The unit does not know this command.',
  'auth': 'Wrong PIN. Check the 6 digits on the unit\'s screen and re-enter.',
  'locked': 'Too many wrong PINs. The unit left provisioning mode; '
      'restart it in provisioning mode.',
  'no_admin': 'The unit has no admin key. Use "Install admin key" first.',
  'admin_set': 'The unit already has an admin key. It can only be removed by '
      'a factory reset on the unit.',
  'badkey': 'The unit rejected the public key.',
  'sig': 'Admin signature rejected. The unit probably trusts a different '
      'admin key (check the fingerprint), or the nonce was stale; retry.',
  'cs': 'Call sign mismatch.',
  'role': 'Invalid role.',
  'exp': 'Invalid expiry (must be in the future and at most 10 days).',
  'nocert_state': 'The unit is not a responder with a fresh key, so it '
      'cannot take a certificate. Make it a responder again.',
  'storage': 'The unit failed to save to flash. Try again.',
};

String errorText(String? code) {
  if (code == null || code.isEmpty) return 'Unknown error.';
  return kErrorText[code] ?? 'Unit error "$code".';
}

// ---------------------------------------------------------------------------
// Durations offered for responders
// ---------------------------------------------------------------------------

class ResponderDuration {
  const ResponderDuration(this.label, this.seconds);
  final String label;
  final int seconds;
}

const List<ResponderDuration> kResponderDurations = [
  ResponderDuration('10 days', 864000),
  ResponderDuration('1 day', 86400),
  ResponderDuration('10-minute test expiry', 600),
];
