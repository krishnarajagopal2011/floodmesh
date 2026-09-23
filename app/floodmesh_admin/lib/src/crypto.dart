// Crypto primitives for the FloodMesh admin app.
//
// Pure Dart (no Flutter imports) so it can be unit-tested with `flutter test`
// and reasoned about independently of the UI. Everything here must
// interoperate with mbedTLS on the ESP32-S3 unit; see
// docs/ble-provisioning-protocol.md §4.
//
// - ECDSA P-256 (secp256r1) with SHA-256, deterministic k (RFC 6979).
// - Public key: 65-byte uncompressed point 04 || X || Y, lowercase hex.
// - Signature: raw r || s, each big-endian left-padded to 32 bytes, lowercase hex.
// - Private key at rest: AES-256-GCM, key = PBKDF2-HMAC-SHA256(passphrase,
//   16-byte random salt, 200000 iterations).

import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import 'package:pointycastle/export.dart';

/// PBKDF2 iteration count for the admin passphrase.
const int kPbkdf2Iterations = 200000;

/// Lengths used in the encrypted identity blob.
const int kSaltLength = 16;
const int kGcmNonceLength = 12;
const int kGcmTagBits = 128;

final ECDomainParameters _p256 = ECCurve_secp256r1();
final BigInt _p256Prime = BigInt.parse(
    'ffffffff00000001000000000000000000000000ffffffffffffffffffffffff',
    radix: 16);

// ---------------------------------------------------------------------------
// Hex / bigint helpers
// ---------------------------------------------------------------------------

String toHex(List<int> bytes) {
  final sb = StringBuffer();
  for (final b in bytes) {
    sb.write((b & 0xff).toRadixString(16).padLeft(2, '0'));
  }
  return sb.toString();
}

Uint8List fromHex(String hex) {
  final h = hex.trim();
  if (h.length.isOdd) {
    throw const FormatException('Hex string has odd length');
  }
  final out = Uint8List(h.length ~/ 2);
  for (var i = 0; i < out.length; i++) {
    final v = int.tryParse(h.substring(2 * i, 2 * i + 2), radix: 16);
    if (v == null) throw const FormatException('Invalid hex character');
    out[i] = v;
  }
  return out;
}

bool isLowerHex(String s, int length) =>
    s.length == length && RegExp(r'^[0-9a-f]*$').hasMatch(s);

/// Big-endian unsigned encoding of [n], left-padded with zeros to [length].
Uint8List bigIntToBytes(BigInt n, int length) {
  if (n.isNegative) throw ArgumentError('negative');
  final out = Uint8List(length);
  var v = n;
  for (var i = length - 1; i >= 0; i--) {
    out[i] = (v & BigInt.from(0xff)).toInt();
    v = v >> 8;
  }
  if (v != BigInt.zero) throw ArgumentError('value does not fit in $length bytes');
  return out;
}

BigInt bytesToBigInt(List<int> bytes) {
  var r = BigInt.zero;
  for (final b in bytes) {
    r = (r << 8) | BigInt.from(b & 0xff);
  }
  return r;
}

// ---------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------

SecureRandom newSecureRandom() {
  final seedSource = Random.secure();
  final seed = Uint8List.fromList(
      List<int>.generate(32, (_) => seedSource.nextInt(256)));
  final r = FortunaRandom();
  r.seed(KeyParameter(seed));
  return r;
}

Uint8List randomBytes(int n) {
  final src = Random.secure();
  return Uint8List.fromList(List<int>.generate(n, (_) => src.nextInt(256)));
}

// ---------------------------------------------------------------------------
// Hashes / MACs
// ---------------------------------------------------------------------------

Uint8List sha256(List<int> data) =>
    SHA256Digest().process(Uint8List.fromList(data));

Uint8List hmacSha256(List<int> key, List<int> msg) {
  final mac = HMac(SHA256Digest(), 64)
    ..init(KeyParameter(Uint8List.fromList(key)));
  return mac.process(Uint8List.fromList(msg));
}

// ---------------------------------------------------------------------------
// P-256 keys and ECDSA
// ---------------------------------------------------------------------------

/// An admin (or any) P-256 key pair.
class P256KeyPair {
  P256KeyPair(this.d) : publicUncompressed = _publicFromPrivate(d);

  /// Private scalar.
  final BigInt d;

  /// 65-byte uncompressed public key (04 || X || Y).
  final Uint8List publicUncompressed;

  String get publicHex => toHex(publicUncompressed);

  /// First 16 lowercase hex chars of SHA-256(65-byte public key).
  String get fingerprint => publicKeyFingerprint(publicUncompressed);

  /// 32-byte big-endian private scalar.
  Uint8List get privateBytes => bigIntToBytes(d, 32);

  static P256KeyPair generate() {
    final gen = ECKeyGenerator()
      ..init(ParametersWithRandom(
          ECKeyGeneratorParameters(_p256), newSecureRandom()));
    final pair = gen.generateKeyPair();
    return P256KeyPair(pair.privateKey.d!);
  }

  static P256KeyPair fromPrivateBytes(List<int> bytes) {
    if (bytes.length != 32) {
      throw const FormatException('Private key must be 32 bytes');
    }
    final d = bytesToBigInt(bytes);
    if (d == BigInt.zero || d >= _p256.n) {
      throw const FormatException('Private key out of range');
    }
    return P256KeyPair(d);
  }

  static Uint8List _publicFromPrivate(BigInt d) {
    final q = (_p256.G * d)!;
    return encodeUncompressed(q);
  }

  /// Signs the ASCII/UTF-8 [message] (SHA-256 is applied inside) and
  /// returns the 64-byte raw r || s signature.
  Uint8List sign(String message) => signBytes(utf8.encode(message));

  Uint8List signBytes(List<int> message) {
    // Deterministic ECDSA (RFC 6979): the k-MAC must be HMAC with the same
    // digest used to hash the message.
    final signer = ECDSASigner(SHA256Digest(), HMac(SHA256Digest(), 64))
      ..init(true, PrivateKeyParameter<ECPrivateKey>(ECPrivateKey(d, _p256)));
    final sig =
        signer.generateSignature(Uint8List.fromList(message)) as ECSignature;
    final out = Uint8List(64);
    out.setRange(0, 32, bigIntToBytes(sig.r, 32));
    out.setRange(32, 64, bigIntToBytes(sig.s, 32));
    return out;
  }

  /// Lowercase hex of [sign] — 128 characters.
  String signHex(String message) => toHex(sign(message));
}

Uint8List encodeUncompressed(ECPoint q) {
  final x = q.x!.toBigInteger()!;
  final y = q.y!.toBigInteger()!;
  final out = Uint8List(65);
  out[0] = 0x04;
  out.setRange(1, 33, bigIntToBytes(x, 32));
  out.setRange(33, 65, bigIntToBytes(y, 32));
  return out;
}

/// Decodes a 65-byte uncompressed P-256 point and checks it is on the curve.
ECPoint decodeUncompressed(List<int> bytes) {
  if (bytes.length != 65 || bytes[0] != 0x04) {
    throw const FormatException('Public key must be 65 bytes starting 0x04');
  }
  final x = bytesToBigInt(bytes.sublist(1, 33));
  final y = bytesToBigInt(bytes.sublist(33, 65));
  if (x >= _p256Prime || y >= _p256Prime) {
    throw const FormatException('Public key coordinate out of range');
  }
  final b = _p256.curve.b!.toBigInteger()!;
  // P-256: y^2 = x^3 - 3x + b (mod p)
  final lhs = (y * y) % _p256Prime;
  final rhs = (x * x * x - BigInt.from(3) * x + b) % _p256Prime;
  if (lhs != rhs) {
    throw const FormatException('Public key is not on the P-256 curve');
  }
  final p = _p256.curve.createPoint(x, y);
  if (p.isInfinity) {
    throw const FormatException('Invalid public key point');
  }
  return p;
}

String publicKeyFingerprint(List<int> uncompressed65) =>
    toHex(sha256(uncompressed65)).substring(0, 16);

/// Verifies a 64-byte raw r || s signature over [message].
bool verifySignature(
    List<int> publicUncompressed, String message, List<int> sig64) {
  if (sig64.length != 64) return false;
  final q = decodeUncompressed(publicUncompressed);
  final r = bytesToBigInt(sig64.sublist(0, 32));
  final s = bytesToBigInt(sig64.sublist(32));
  final verifier = ECDSASigner(SHA256Digest())
    ..init(false, PublicKeyParameter<ECPublicKey>(ECPublicKey(q, _p256)));
  return verifier.verifySignature(
      Uint8List.fromList(utf8.encode(message)), ECSignature(r, s));
}

// ---------------------------------------------------------------------------
// Passphrase-encrypted private key
// ---------------------------------------------------------------------------

class WrongPassphraseException implements Exception {
  @override
  String toString() => 'Wrong passphrase (or corrupted identity data)';
}

Uint8List deriveKey(String passphrase, List<int> salt,
    {int iterations = kPbkdf2Iterations}) {
  final kdf = PBKDF2KeyDerivator(HMac(SHA256Digest(), 64))
    ..init(Pbkdf2Parameters(Uint8List.fromList(salt), iterations, 32));
  return kdf.process(Uint8List.fromList(utf8.encode(passphrase)));
}

Uint8List _aesGcm(bool encrypt, List<int> key, List<int> nonce, List<int> input,
    List<int> aad) {
  final c = GCMBlockCipher(AESEngine())
    ..init(
        encrypt,
        AEADParameters(KeyParameter(Uint8List.fromList(key)), kGcmTagBits,
            Uint8List.fromList(nonce), Uint8List.fromList(aad)));
  return c.process(Uint8List.fromList(input));
}

/// The stored, encrypted admin identity. Serialised as JSON; the backup
/// "blob" is base64(utf8(json)).
class EncryptedIdentity {
  EncryptedIdentity({
    required this.salt,
    required this.nonce,
    required this.ciphertext,
    required this.publicHex,
    this.iterations = kPbkdf2Iterations,
  });

  final Uint8List salt;
  final Uint8List nonce;

  /// AES-256-GCM ciphertext of the 32-byte private scalar, with the 16-byte
  /// tag appended. AAD = the public key hex (binds key pair together).
  final Uint8List ciphertext;
  final String publicHex;
  final int iterations;

  String get fingerprint => publicKeyFingerprint(fromHex(publicHex));

  static const _aadPrefix = 'FMADMIN1|';

  /// Encrypts [kp] under [passphrase]. Slow (PBKDF2, 200k iterations).
  static EncryptedIdentity encrypt(P256KeyPair kp, String passphrase,
      {int iterations = kPbkdf2Iterations}) {
    final salt = randomBytes(kSaltLength);
    final nonce = randomBytes(kGcmNonceLength);
    final key = deriveKey(passphrase, salt, iterations: iterations);
    final ct = _aesGcm(true, key, nonce, kp.privateBytes,
        utf8.encode(_aadPrefix + kp.publicHex));
    return EncryptedIdentity(
        salt: salt,
        nonce: nonce,
        ciphertext: ct,
        publicHex: kp.publicHex,
        iterations: iterations);
  }

  /// Decrypts; throws [WrongPassphraseException] if the passphrase is wrong
  /// or the data was tampered with. Slow (PBKDF2).
  P256KeyPair decrypt(String passphrase) {
    final key = deriveKey(passphrase, salt, iterations: iterations);
    Uint8List plain;
    try {
      plain = _aesGcm(
          false, key, nonce, ciphertext, utf8.encode(_aadPrefix + publicHex));
    } on InvalidCipherTextException {
      throw WrongPassphraseException();
    } on ArgumentError {
      throw WrongPassphraseException();
    }
    final kp = P256KeyPair.fromPrivateBytes(plain);
    if (kp.publicHex != publicHex) {
      // Should be impossible with GCM + AAD, but check the pair anyway.
      throw WrongPassphraseException();
    }
    return kp;
  }

  Map<String, Object> toJson() => {
        'v': 1,
        'kdf': 'pbkdf2-hmac-sha256',
        'iter': iterations,
        'salt': base64.encode(salt),
        'nonce': base64.encode(nonce),
        'ct': base64.encode(ciphertext),
        'pub': publicHex,
      };

  String toJsonString() => jsonEncode(toJson());

  factory EncryptedIdentity.fromJsonString(String s) {
    final m = jsonDecode(s);
    if (m is! Map<String, dynamic> || m['v'] != 1) {
      throw const FormatException('Unknown identity format');
    }
    final pub = m['pub'] as String;
    if (!isLowerHex(pub, 130)) {
      throw const FormatException('Bad public key in identity');
    }
    decodeUncompressed(fromHex(pub));
    return EncryptedIdentity(
      salt: base64.decode(m['salt'] as String),
      nonce: base64.decode(m['nonce'] as String),
      ciphertext: base64.decode(m['ct'] as String),
      publicHex: pub,
      iterations: m['iter'] as int,
    );
  }

  /// Backup blob: base64 of the JSON form. Safe to copy around; it is only
  /// as strong as the passphrase.
  String toBackupBlob() => base64.encode(utf8.encode(toJsonString()));

  factory EncryptedIdentity.fromBackupBlob(String blob) {
    final cleaned = blob.replaceAll(RegExp(r'\s'), '');
    String json;
    try {
      json = utf8.decode(base64.decode(cleaned));
    } on FormatException {
      throw const FormatException('Backup is not valid base64');
    }
    return EncryptedIdentity.fromJsonString(json);
  }
}
