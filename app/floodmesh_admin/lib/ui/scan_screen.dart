import 'dart:async';

import 'package:flutter/material.dart';
import 'package:universal_ble/universal_ble.dart';

import '../src/crypto.dart';
import '../src/protocol.dart';
import 'device_screen.dart';
import 'history_screen.dart';
import 'identity_screens.dart';
import 'widgets.dart';

class ScanScreen extends StatefulWidget {
  const ScanScreen(
      {super.key,
      required this.admin,
      required this.stored,
      required this.onLock});
  final P256KeyPair admin;
  final EncryptedIdentity stored;
  final VoidCallback onLock;

  @override
  State<ScanScreen> createState() => _ScanScreenState();
}

class _ScanScreenState extends State<ScanScreen> {
  final Map<String, BleDevice> _found = {};
  bool _scanning = false;
  String? _error;
  Timer? _stopTimer;

  @override
  void dispose() {
    _stopTimer?.cancel();
    UniversalBle.onScanResult = null;
    if (_scanning) {
      UniversalBle.stopScan().catchError((_) {});
    }
    super.dispose();
  }

  Future<void> _startScan() async {
    setState(() {
      _error = null;
      _found.clear();
    });
    try {
      // Android 12+: BLUETOOTH_SCAN + BLUETOOTH_CONNECT.
      // Android <= 11: location (declared with maxSdkVersion 30).
      await UniversalBle.requestPermissions(withAndroidFineLocation: false);
    } catch (e) {
      setState(() => _error =
          'Bluetooth permission was denied. Allow "Nearby devices" (and '
          'Location on Android 11 or older) in the app settings.');
      return;
    }
    final state = await UniversalBle.getBluetoothAvailabilityState();
    if (state != AvailabilityState.poweredOn) {
      setState(() => _error = 'Bluetooth is off or unavailable. Turn it on.');
      return;
    }
    UniversalBle.onScanResult = (d) {
      if (!mounted) return;
      // The name may be in the scan response only; keep one we already saw.
      final old = _found[d.deviceId];
      if ((d.name == null || d.name!.isEmpty) && old?.name != null) {
        d.name = old!.name;
      }
      setState(() => _found[d.deviceId] = d);
    };
    try {
      await UniversalBle.startScan(
        scanFilter: ScanFilter(withServices: [kServiceUuid]),
        platformConfig: PlatformConfig(
          android: AndroidOptions(
            // ESP32 NimBLE advertises with legacy (BLE 4.x) PDUs.
            legacy: true,
            scanMode: AndroidScanMode.lowLatency,
            requestLocationPermission: false,
          ),
        ),
      );
      setState(() => _scanning = true);
      _stopTimer?.cancel();
      _stopTimer = Timer(const Duration(seconds: 20), _stopScan);
    } catch (e) {
      setState(() => _error = 'Scan failed: $e');
    }
  }

  Future<void> _stopScan() async {
    _stopTimer?.cancel();
    try {
      await UniversalBle.stopScan();
    } catch (_) {}
    if (mounted) setState(() => _scanning = false);
  }

  Future<void> _open(BleDevice d) async {
    await _stopScan();
    if (!mounted) return;
    await Navigator.push(
      context,
      MaterialPageRoute(
        builder: (_) => DeviceScreen(
          deviceId: d.deviceId,
          name: d.name ?? d.deviceId,
          admin: widget.admin,
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final devices = _found.values.toList()
      ..sort((a, b) => (b.rssi ?? -999).compareTo(a.rssi ?? -999));
    return Scaffold(
      appBar: AppBar(
        title: const Text('FloodMesh Admin'),
        actions: [
          IconButton(
            tooltip: 'History',
            icon: const Icon(Icons.history),
            onPressed: () => Navigator.push(context,
                MaterialPageRoute(builder: (_) => const HistoryScreen())),
          ),
          IconButton(
            tooltip: 'Admin key',
            icon: const Icon(Icons.key),
            onPressed: () => Navigator.push(
                context,
                MaterialPageRoute(
                    builder: (_) => AdminKeyScreen(stored: widget.stored))),
          ),
          IconButton(
            tooltip: 'Lock',
            icon: const Icon(Icons.lock),
            onPressed: widget.onLock,
          ),
        ],
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text('Admin fingerprint: ${widget.admin.fingerprint}',
                style: const TextStyle(fontFamily: 'monospace')),
            const SizedBox(height: 8),
            const Text(
              'On the unit: hold # while powering on. It shows PROVISIONING '
              'and a 6-digit PIN.',
            ),
            const SizedBox(height: 12),
            FilledButton.icon(
              onPressed: _scanning ? _stopScan : _startScan,
              icon: Icon(_scanning ? Icons.stop : Icons.bluetooth_searching),
              label: Text(_scanning ? 'Stop scan' : 'Scan for units'),
            ),
            if (_scanning) const LinearProgressIndicator(),
            const SizedBox(height: 12),
            if (_error != null) StatusBanner(text: _error!, error: true),
            Expanded(
              child: devices.isEmpty
                  ? Center(
                      child: Text(_scanning
                          ? 'Looking for units in provisioning mode…'
                          : 'No units found yet.'))
                  : ListView.separated(
                      itemCount: devices.length,
                      separatorBuilder: (_, _) => const Divider(height: 1),
                      itemBuilder: (_, i) {
                        final d = devices[i];
                        return ListTile(
                          minTileHeight: 72,
                          leading: const Icon(Icons.sensors, size: 36),
                          title: Text(d.name ?? '(no name)',
                              style: const TextStyle(
                                  fontSize: 20, fontWeight: FontWeight.w600)),
                          subtitle: Text(d.deviceId),
                          trailing: Text('${d.rssi ?? '?'} dBm',
                              style: const TextStyle(fontSize: 16)),
                          onTap: () => _open(d),
                        );
                      },
                    ),
            ),
          ],
        ),
      ),
    );
  }
}
