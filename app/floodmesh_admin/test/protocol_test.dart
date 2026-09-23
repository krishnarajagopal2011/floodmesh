import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:floodmesh_admin/src/crypto.dart';
import 'package:floodmesh_admin/src/protocol.dart';

void main() {
  const nonce = '00112233445566778899aabbccddeeff';

  test('UUIDs match the protocol document', () {
    expect(kServiceUuid, '6e3f0001-8a4c-4b8e-9f10-f10d3e5a0001');
    expect(kInfoCharUuid, '6e3f0002-8a4c-4b8e-9f10-f10d3e5a0001');
    expect(kCmdCharUuid, '6e3f0003-8a4c-4b8e-9f10-f10d3e5a0001');
    expect(kRespCharUuid, '6e3f0004-8a4c-4b8e-9f10-f10d3e5a0001');
  });

  group('auth HMAC (vectors computed with Python hmac/hashlib)', () {
    test('set_role', () {
      expect(authFor('123456', nonce, 'set_role'),
          'f42200c02f3369696f5e9599713fcb45bcc89b67c90f8f2ec04340429284a49b');
    });
    test('reboot', () {
      expect(authFor('048213', '9f2c1b7e0a4d5c6f8e9d0a1b2c3d4e5f', 'reboot'),
          '07e96e5d24a5735d173d68964db8f57f17839738cbd107b295e88722f8f36b34');
    });
    test('PIN validation', () {
      expect(isValidPin('012345'), isTrue);
      expect(isValidPin('12345'), isFalse);
      expect(isValidPin('12345a'), isFalse);
    });
  });

  group('signed message strings', () {
    test('FMREG1', () {
      expect(regMessage('K7Q2M', 'responder', 1790000000, 1790864000, nonce),
          'FMREG1|K7Q2M|responder|1790000000|1790864000|$nonce');
      expect(regMessage('K7Q2M', 'relay', 1790000000, 0, nonce),
          'FMREG1|K7Q2M|relay|1790000000|0|$nonce');
    });
    test('FMCERT1 lowercases devPub', () {
      final pub = '04${'AB' * 64}';
      expect(certMessage('K7Q2M', 1790864000, pub),
          'FMCERT1|K7Q2M|responder|1790864000|04${'ab' * 64}');
    });
  });

  group('commands', () {
    final admin = P256KeyPair.generate();

    test('set_admin', () {
      final m = jsonDecode(
          buildSetAdmin(pubHex: admin.publicHex, pin: '123456', nonce: nonce))
          as Map<String, dynamic>;
      expect(m.keys.toList(), ['op', 'pub', 'auth']);
      expect(m['op'], 'set_admin');
      expect(m['pub'], admin.publicHex);
      expect(m['auth'], authFor('123456', nonce, 'set_admin'));
    });

    test('set_role responder: fields and verifiable signature', () {
      final s = buildSetRole(
          admin: admin,
          cs: 'K7Q2M',
          role: 'responder',
          now: 1790000000,
          exp: 1790864000,
          pin: '123456',
          nonce: nonce);
      expect(utf8.encode(s).length, lessThanOrEqualTo(kMaxCmdBytes));
      final m = jsonDecode(s) as Map<String, dynamic>;
      expect(m.keys.toList(), ['op', 'role', 'now', 'exp', 'sig', 'auth']);
      expect(m['op'], 'set_role');
      expect(m['now'], 1790000000);
      expect(m['exp'], 1790864000);
      expect(isLowerHex(m['sig'] as String, 128), isTrue);
      expect(m['auth'],
          'f42200c02f3369696f5e9599713fcb45bcc89b67c90f8f2ec04340429284a49b');
      expect(
          verifySignature(
              admin.publicUncompressed,
              'FMREG1|K7Q2M|responder|1790000000|1790864000|$nonce',
              fromHex(m['sig'] as String)),
          isTrue);
    });

    test('set_role expiry rules', () {
      String build(String role, int exp) => buildSetRole(
          admin: admin,
          cs: 'K7Q2M',
          role: role,
          now: 1000,
          exp: exp,
          pin: '123456',
          nonce: nonce);
      expect(() => build('responder', 1000), throwsArgumentError);
      expect(() => build('responder', 1000 + 864001), throwsArgumentError);
      build('responder', 1000 + 864000);
      expect(() => build('relay', 5), throwsArgumentError);
      expect(jsonDecode(build('civilian', 0))['exp'], 0);
      expect(() => build('admin', 0), throwsArgumentError);
    });

    test('set_cert', () {
      final dev = P256KeyPair.generate();
      final s = buildSetCert(
          admin: admin,
          cs: 'K7Q2M',
          exp: 1790864000,
          devPubHex: dev.publicHex.toUpperCase(),
          pin: '123456',
          nonce: nonce);
      final m = jsonDecode(s) as Map<String, dynamic>;
      expect(m.keys.toList(), ['op', 'exp', 'sig', 'auth']);
      expect(m['op'], 'set_cert');
      expect(m['auth'], authFor('123456', nonce, 'set_cert'));
      expect(
          verifySignature(
              admin.publicUncompressed,
              'FMCERT1|K7Q2M|responder|1790864000|${dev.publicHex}',
              fromHex(m['sig'] as String)),
          isTrue);
    });

    test('reboot', () {
      expect(buildReboot(pin: '048213', nonce: '9f2c1b7e0a4d5c6f8e9d0a1b2c3d4e5f'),
          '{"op":"reboot","auth":"07e96e5d24a5735d173d68964db8f57f17839738cbd107b295e88722f8f36b34"}');
    });
  });

  group('parsing', () {
    test('INFO example from the document', () {
      final info = DeviceInfo.parse('{"v":1,"cs":"K7Q2M","fw":"0.4.0",'
          '"board":"proto_v2","role":"civilian","exp":0,"now":0,'
          '"admin":"none","adminFp":"","nonce":"$nonce","devPub":"",'
          '"hasCert":false,"fails":0}');
      expect(info.cs, 'K7Q2M');
      expect(info.role, 'civilian');
      expect(info.hasAdmin, isFalse);
      expect(info.nonce, nonce);
    });
    test('RESP', () {
      final r = CmdResponse.parse(
          '{"ok":true,"role":"responder","exp":1790864000,"devPub":"04ab"}');
      expect(r.ok, isTrue);
      expect(r.exp, 1790864000);
      expect(r.devPub, '04ab');
      final e = CmdResponse.parse('{"ok":false,"err":"sig"}');
      expect(e.ok, isFalse);
      expect(e.err, 'sig');
      expect(errorText('auth'), contains('PIN'));
    });
    test('every documented error code has friendly text', () {
      for (final c in [
        'json', 'op', 'auth', 'locked', 'no_admin', 'admin_set', 'badkey',
        'sig', 'cs', 'role', 'exp', 'nocert_state', 'storage'
      ]) {
        expect(kErrorText.containsKey(c), isTrue, reason: c);
      }
    });
  });
}
