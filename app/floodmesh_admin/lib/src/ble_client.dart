// Thin wrapper over universal_ble for the FMREG v1 GATT service.

import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:universal_ble/universal_ble.dart';

import 'protocol.dart';

class ProvisioningClient {
  ProvisioningClient(this.deviceId);

  final String deviceId;
  StreamSubscription<Uint8List>? _respSub;
  final StreamController<String> _resp = StreamController<String>.broadcast();
  int mtu = 23;

  Future<void> connect() async {
    await UniversalBle.connect(deviceId,
        timeout: const Duration(seconds: 20),
        platformConfig: ConnectionPlatformConfig(
            android: AndroidConnectionOptions(closeGattOnDetach: true)));
    try {
      mtu = await UniversalBle.requestMtu(deviceId, kRequestedMtu);
    } catch (_) {
      // Best effort; long reads/writes still work at the default MTU.
    }
    await UniversalBle.discoverServices(deviceId);
    _respSub = UniversalBle.characteristicValueStream(deviceId, kRespCharUuid)
        .listen((v) => _resp.add(utf8.decode(v, allowMalformed: true)));
    // Subscribe to RESP notifications before any command is written.
    await UniversalBle.subscribeNotifications(
        deviceId, kServiceUuid, kRespCharUuid);
  }

  Future<void> disconnect() async {
    await _respSub?.cancel();
    _respSub = null;
    try {
      await UniversalBle.disconnect(deviceId);
    } catch (_) {}
  }

  Future<DeviceInfo> readInfo() async {
    final raw = await UniversalBle.read(deviceId, kServiceUuid, kInfoCharUuid);
    return DeviceInfo.parse(utf8.decode(raw));
  }

  Future<String> _readResp() async {
    final raw = await UniversalBle.read(deviceId, kServiceUuid, kRespCharUuid);
    return utf8.decode(raw);
  }

  /// Writes [cmdJson] to CMD and waits for the RESP notification; if none
  /// arrives within 3 s (or it is truncated/unparseable), reads RESP.
  Future<CmdResponse> send(String cmdJson,
      {Duration wait = const Duration(seconds: 3)}) async {
    final completer = Completer<CmdResponse>();
    final sub = _resp.stream.listen((text) {
      if (completer.isCompleted) return;
      try {
        completer.complete(CmdResponse.parse(text));
      } catch (_) {
        // Probably truncated to MTU-3; read the full value instead.
        completer.complete(_readResp().then(CmdResponse.parse));
      }
    });
    try {
      await UniversalBle.write(deviceId, kServiceUuid, kCmdCharUuid,
          Uint8List.fromList(utf8.encode(cmdJson)));
      return await completer.future.timeout(wait, onTimeout: () async {
        return CmdResponse.parse(await _readResp());
      });
    } finally {
      await sub.cancel();
    }
  }
}
