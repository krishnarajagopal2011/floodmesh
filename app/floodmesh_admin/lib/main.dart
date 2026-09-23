import 'package:flutter/material.dart';

import 'src/crypto.dart';
import 'src/storage.dart';
import 'ui/identity_screens.dart';
import 'ui/scan_screen.dart';

void main() {
  runApp(const FloodMeshAdminApp());
}

class FloodMeshAdminApp extends StatelessWidget {
  const FloodMeshAdminApp({super.key});

  @override
  Widget build(BuildContext context) {
    ThemeData theme(Brightness b) {
      final base = ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(
            seedColor: const Color(0xFF0B5CAD), brightness: b),
      );
      return base.copyWith(
        filledButtonTheme: FilledButtonThemeData(
          style: FilledButton.styleFrom(
            minimumSize: const Size.fromHeight(56),
            textStyle:
                const TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
          ),
        ),
        outlinedButtonTheme: OutlinedButtonThemeData(
          style: OutlinedButton.styleFrom(
            minimumSize: const Size.fromHeight(56),
            textStyle: const TextStyle(fontSize: 18),
          ),
        ),
        inputDecorationTheme:
            const InputDecorationTheme(border: OutlineInputBorder()),
      );
    }

    return MaterialApp(
      title: 'FloodMesh Admin',
      theme: theme(Brightness.light),
      darkTheme: theme(Brightness.dark),
      home: const RootGate(),
    );
  }
}

/// Decides between first-run, unlock and the main (scan) screen.
class RootGate extends StatefulWidget {
  const RootGate({super.key});

  @override
  State<RootGate> createState() => _RootGateState();
}

class _RootGateState extends State<RootGate> {
  bool _loading = true;
  String? _loadError;
  EncryptedIdentity? _stored;
  P256KeyPair? _admin;

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _reload() async {
    setState(() {
      _loading = true;
      _loadError = null;
    });
    try {
      final s = await IdentityStore.load();
      setState(() {
        _stored = s;
        _admin = null;
        _loading = false;
      });
    } catch (e) {
      setState(() {
        _loadError = 'Could not read the stored admin identity: $e';
        _loading = false;
      });
    }
  }

  void _unlocked(P256KeyPair kp, EncryptedIdentity enc) {
    setState(() {
      _stored = enc;
      _admin = kp;
    });
  }

  @override
  Widget build(BuildContext context) {
    if (_loading) {
      return const Scaffold(body: Center(child: CircularProgressIndicator()));
    }
    if (_loadError != null) {
      return Scaffold(
        appBar: AppBar(title: const Text('FloodMesh Admin')),
        body: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(children: [
            Text(_loadError!, style: const TextStyle(color: Colors.red)),
            const SizedBox(height: 16),
            ResetIdentityButton(onReset: _reload),
          ]),
        ),
      );
    }
    final admin = _admin;
    final stored = _stored;
    if (stored == null) {
      return CreateIdentityScreen(onCreated: _unlocked);
    }
    if (admin == null) {
      return UnlockScreen(
          stored: stored, onUnlocked: _unlocked, onReset: _reload);
    }
    return ScanScreen(
      admin: admin,
      stored: stored,
      onLock: () => setState(() => _admin = null),
    );
  }
}
