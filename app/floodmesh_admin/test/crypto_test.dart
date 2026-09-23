import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:floodmesh_admin/src/crypto.dart';

void main() {
  group('hex / bigint', () {
    test('round trip', () {
      final b = fromHex('00ff10ab');
      expect(b, [0, 255, 16, 171]);
      expect(toHex(b), '00ff10ab');
    });
    test('left padding', () {
      expect(toHex(bigIntToBytes(BigInt.from(1), 32)), '${'00' * 31}01');
    });
    test('sha256 known vector', () {
      expect(toHex(sha256(utf8.encode('abc'))),
          'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    });
  });

  group('P-256 keys', () {
    test('generated key is 65-byte uncompressed and on the curve', () {
      final kp = P256KeyPair.generate();
      expect(kp.publicUncompressed.length, 65);
      expect(kp.publicUncompressed[0], 0x04);
      expect(isLowerHex(kp.publicHex, 130), isTrue);
      expect(kp.fingerprint.length, 16);
      expect(kp.fingerprint, toHex(sha256(kp.publicUncompressed)).substring(0, 16));
      decodeUncompressed(kp.publicUncompressed); // throws if not on curve
      expect(P256KeyPair.fromPrivateBytes(kp.privateBytes).publicHex,
          kp.publicHex);
    });

    // RFC 6979 appendix A.2.5 (P-256, SHA-256, message "sample").
    const rfcX =
        'c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721';
    const rfcPub = '04'
        '60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6'
        '7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299';
    const rfcSig =
        'efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716'
        'f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8';

    test('public key from private matches RFC 6979 vector', () {
      final kp = P256KeyPair.fromPrivateBytes(fromHex(rfcX));
      expect(kp.publicHex, rfcPub);
    });

    test('deterministic signature matches RFC 6979 vector', () {
      final kp = P256KeyPair.fromPrivateBytes(fromHex(rfcX));
      expect(kp.signHex('sample'), rfcSig);
      expect(verifySignature(fromHex(rfcPub), 'sample', fromHex(rfcSig)),
          isTrue);
    });

    test('sign then verify round trip; signature is 64 bytes', () {
      final kp = P256KeyPair.generate();
      for (var i = 0; i < 20; i++) {
        final msg = 'FMREG1|K7Q2M|responder|1790000000|${1790000000 + i}|'
            '00112233445566778899aabbccddeeff';
        final sig = kp.sign(msg);
        expect(sig.length, 64);
        expect(kp.signHex(msg).length, 128);
        expect(isLowerHex(kp.signHex(msg), 128), isTrue);
        expect(verifySignature(kp.publicUncompressed, msg, sig), isTrue);
        expect(verifySignature(kp.publicUncompressed, '${msg}x', sig), isFalse);
      }
    });

    test('point not on the curve is rejected', () {
      final bad = fromHex(rfcPub);
      bad[64] ^= 1;
      expect(() => decodeUncompressed(bad), throwsFormatException);
      expect(() => decodeUncompressed(fromHex(rfcPub).sublist(0, 64)),
          throwsFormatException);
    });

    test('verification fails with another key', () {
      final a = P256KeyPair.generate();
      final b = P256KeyPair.generate();
      final sig = a.sign('hello');
      expect(verifySignature(b.publicUncompressed, 'hello', sig), isFalse);
    });
  });

  group('encrypted identity', () {
    // Fewer iterations keep the suite fast; one test uses the real count.
    test('encrypt -> decrypt round trip, wrong passphrase fails', () {
      final kp = P256KeyPair.generate();
      final enc = EncryptedIdentity.encrypt(kp, 'correct horse', iterations: 1000);
      expect(enc.salt.length, 16);
      expect(enc.nonce.length, 12);
      expect(enc.ciphertext.length, 32 + 16);
      expect(enc.decrypt('correct horse').publicHex, kp.publicHex);
      expect(() => enc.decrypt('correct horsf'),
          throwsA(isA<WrongPassphraseException>()));
      expect(() => enc.decrypt(''), throwsA(isA<WrongPassphraseException>()));
    });

    test('backup blob round trip', () {
      final kp = P256KeyPair.generate();
      final enc = EncryptedIdentity.encrypt(kp, 'passphrase1', iterations: 1000);
      final blob = enc.toBackupBlob();
      final back = EncryptedIdentity.fromBackupBlob('  $blob\n');
      expect(back.publicHex, kp.publicHex);
      expect(back.iterations, 1000);
      expect(back.decrypt('passphrase1').privateBytes, kp.privateBytes);
      expect(() => EncryptedIdentity.fromBackupBlob('not a backup!'),
          throwsFormatException);
    });

    test('tampered public key is rejected', () {
      final kp = P256KeyPair.generate();
      final other = P256KeyPair.generate();
      final enc = EncryptedIdentity.encrypt(kp, 'passphrase1', iterations: 1000);
      final forged = EncryptedIdentity(
          salt: enc.salt,
          nonce: enc.nonce,
          ciphertext: enc.ciphertext,
          publicHex: other.publicHex,
          iterations: 1000);
      expect(() => forged.decrypt('passphrase1'),
          throwsA(isA<WrongPassphraseException>()));
    });

    test('default uses 200000 PBKDF2 iterations', () {
      final kp = P256KeyPair.generate();
      final enc = EncryptedIdentity.encrypt(kp, 'passphrase1');
      expect(enc.iterations, 200000);
      expect(enc.toJson()['iter'], 200000);
      expect(enc.decrypt('passphrase1').publicHex, kp.publicHex);
    });

    test('PBKDF2-HMAC-SHA256 known vector', () {
      // RFC 7914 §11 / widely published: P="passwd", S="salt", c=1, dkLen=64
      // first 32 bytes.
      expect(
          toHex(deriveKey('passwd', utf8.encode('salt'), iterations: 1)),
          '55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc');
    });
  });
}
