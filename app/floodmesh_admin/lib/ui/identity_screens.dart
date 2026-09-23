import 'package:flutter/material.dart';

import '../src/crypto.dart';
import '../src/storage.dart';
import 'widgets.dart';

typedef OnIdentity = void Function(P256KeyPair kp, EncryptedIdentity enc);

const String kFirmwareNote =
    "Compile this into firmware as FM_ADMIN_PUBKEY_HEX, or install it on each "
    "unit with 'Install admin key'.";

// ---------------------------------------------------------------------------
// First run
// ---------------------------------------------------------------------------

class CreateIdentityScreen extends StatefulWidget {
  const CreateIdentityScreen({super.key, required this.onCreated});
  final OnIdentity onCreated;

  @override
  State<CreateIdentityScreen> createState() => _CreateIdentityScreenState();
}

class _CreateIdentityScreenState extends State<CreateIdentityScreen> {
  final _pass = TextEditingController();
  final _pass2 = TextEditingController();
  bool _busy = false;
  String? _error;
  (P256KeyPair, EncryptedIdentity)? _created;

  @override
  void dispose() {
    _pass.dispose();
    _pass2.dispose();
    super.dispose();
  }

  Future<void> _create() async {
    final p = _pass.text;
    if (p.length < 8) {
      setState(() => _error = 'Passphrase must be at least 8 characters.');
      return;
    }
    if (p != _pass2.text) {
      setState(() => _error = 'The two passphrases do not match.');
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final r = await IdentityStore.create(p);
      setState(() => _created = r);
    } catch (e) {
      setState(() => _error = 'Could not create identity: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final created = _created;
    return Scaffold(
      appBar: AppBar(title: const Text('Create admin identity')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: created != null
            ? [
                const StatusBanner(
                    text: 'Admin identity created and stored encrypted.',
                    error: false),
                const SizedBox(height: 16),
                AdminKeyCard(publicHex: created.$1.publicHex),
                const SizedBox(height: 16),
                const Text(
                  'Write your passphrase down and keep it safe. Without it the '
                  'admin key cannot be used. Export a backup from the Admin key '
                  'screen after you continue.',
                ),
                const SizedBox(height: 24),
                FilledButton(
                  onPressed: () => widget.onCreated(created.$1, created.$2),
                  child: const Text('Continue'),
                ),
              ]
            : [
                const Text(
                  'This phone will hold the FloodMesh super-admin key. It signs '
                  'every role change. Choose a passphrase (at least 8 '
                  'characters); it encrypts the key on this phone.',
                  style: TextStyle(fontSize: 16),
                ),
                const SizedBox(height: 16),
                TextField(
                  controller: _pass,
                  obscureText: true,
                  enabled: !_busy,
                  decoration: const InputDecoration(labelText: 'Passphrase'),
                ),
                const SizedBox(height: 12),
                TextField(
                  controller: _pass2,
                  obscureText: true,
                  enabled: !_busy,
                  decoration:
                      const InputDecoration(labelText: 'Confirm passphrase'),
                ),
                const SizedBox(height: 16),
                if (_error != null) ...[
                  StatusBanner(text: _error!, error: true),
                  const SizedBox(height: 16),
                ],
                FilledButton(
                  onPressed: _busy ? null : _create,
                  child: _busy
                      ? const Text('Generating key…')
                      : const Text('Create admin key'),
                ),
                const SizedBox(height: 12),
                OutlinedButton(
                  onPressed: _busy
                      ? null
                      : () => Navigator.push(
                            context,
                            MaterialPageRoute(
                              builder: (_) => ImportBackupScreen(
                                onImported: (kp, enc) {
                                  Navigator.pop(context);
                                  widget.onCreated(kp, enc);
                                },
                              ),
                            ),
                          ),
                  child: const Text('Import a backup instead'),
                ),
              ],
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Unlock
// ---------------------------------------------------------------------------

class UnlockScreen extends StatefulWidget {
  const UnlockScreen(
      {super.key,
      required this.stored,
      required this.onUnlocked,
      required this.onReset});
  final EncryptedIdentity stored;
  final OnIdentity onUnlocked;
  final VoidCallback onReset;

  @override
  State<UnlockScreen> createState() => _UnlockScreenState();
}

class _UnlockScreenState extends State<UnlockScreen> {
  final _pass = TextEditingController();
  bool _busy = false;
  String? _error;

  @override
  void dispose() {
    _pass.dispose();
    super.dispose();
  }

  Future<void> _unlock() async {
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final kp = await IdentityStore.unlock(widget.stored, _pass.text);
      _pass.clear();
      widget.onUnlocked(kp, widget.stored);
    } on WrongPassphraseException {
      setState(() => _error = 'Wrong passphrase.');
    } catch (e) {
      setState(() => _error = 'Unlock failed: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('FloodMesh Admin')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text('Admin key fingerprint: ${widget.stored.fingerprint}',
              style: const TextStyle(fontFamily: 'monospace', fontSize: 16)),
          const SizedBox(height: 16),
          TextField(
            controller: _pass,
            obscureText: true,
            enabled: !_busy,
            autofocus: true,
            onSubmitted: (_) => _unlock(),
            decoration: const InputDecoration(labelText: 'Admin passphrase'),
          ),
          const SizedBox(height: 16),
          if (_error != null) ...[
            StatusBanner(text: _error!, error: true),
            const SizedBox(height: 16),
          ],
          FilledButton(
            onPressed: _busy ? null : _unlock,
            child: Text(_busy ? 'Unlocking…' : 'Unlock'),
          ),
          const SizedBox(height: 32),
          OutlinedButton(
            onPressed: _busy
                ? null
                : () => Navigator.push(
                      context,
                      MaterialPageRoute(
                        builder: (_) => ImportBackupScreen(
                          replacing: true,
                          onImported: (kp, enc) {
                            Navigator.pop(context);
                            widget.onUnlocked(kp, enc);
                          },
                        ),
                      ),
                    ),
            child: const Text('Import a backup'),
          ),
          const SizedBox(height: 12),
          ResetIdentityButton(onReset: widget.onReset),
        ],
      ),
    );
  }
}

class ResetIdentityButton extends StatelessWidget {
  const ResetIdentityButton({super.key, required this.onReset});
  final VoidCallback onReset;

  @override
  Widget build(BuildContext context) {
    return OutlinedButton(
      style: OutlinedButton.styleFrom(foregroundColor: Colors.red.shade700),
      onPressed: () async {
        final ok = await confirm(
          context,
          title: 'Reset admin identity?',
          body: 'This permanently DELETES the admin private key from this '
              'phone. Units that trust this key (compiled in or installed) '
              'will reject every role change from a new key until they are '
              'factory-reset on the unit or reflashed. Existing responder '
              'certificates stay valid until they expire.\n\n'
              'Only continue if you have a backup or really want a new key.',
          ok: 'Delete key',
          danger: true,
        );
        if (!ok || !context.mounted) return;
        final again = await confirm(
          context,
          title: 'Are you sure?',
          body: 'There is no undo.',
          ok: 'Yes, delete',
          danger: true,
        );
        if (!again) return;
        await IdentityStore.reset();
        onReset();
      },
      child: const Text('Reset admin identity'),
    );
  }
}

// ---------------------------------------------------------------------------
// Import / export
// ---------------------------------------------------------------------------

class ImportBackupScreen extends StatefulWidget {
  const ImportBackupScreen(
      {super.key, required this.onImported, this.replacing = false});
  final OnIdentity onImported;
  final bool replacing;

  @override
  State<ImportBackupScreen> createState() => _ImportBackupScreenState();
}

class _ImportBackupScreenState extends State<ImportBackupScreen> {
  final _blob = TextEditingController();
  final _pass = TextEditingController();
  bool _busy = false;
  String? _error;

  @override
  void dispose() {
    _blob.dispose();
    _pass.dispose();
    super.dispose();
  }

  Future<void> _import() async {
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final enc = EncryptedIdentity.fromBackupBlob(_blob.text);
      final kp = await IdentityStore.unlock(enc, _pass.text);
      if (widget.replacing && mounted) {
        final ok = await confirm(context,
            title: 'Replace stored identity?',
            body: 'The admin key on this phone will be replaced by the backup '
                '(fingerprint ${enc.fingerprint}).',
            ok: 'Replace',
            danger: true);
        if (!ok) return;
      }
      await IdentityStore.save(enc);
      widget.onImported(kp, enc);
    } on WrongPassphraseException {
      setState(() => _error = 'Wrong passphrase for this backup.');
    } on FormatException catch (e) {
      setState(() => _error = 'Not a valid backup: ${e.message}');
    } catch (e) {
      setState(() => _error = 'Import failed: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Import backup')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          TextField(
            controller: _blob,
            minLines: 4,
            maxLines: 8,
            enabled: !_busy,
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12),
            decoration:
                const InputDecoration(labelText: 'Paste the backup text'),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _pass,
            obscureText: true,
            enabled: !_busy,
            decoration:
                const InputDecoration(labelText: 'Passphrase of the backup'),
          ),
          const SizedBox(height: 16),
          if (_error != null) ...[
            StatusBanner(text: _error!, error: true),
            const SizedBox(height: 16),
          ],
          FilledButton(
            onPressed: _busy ? null : _import,
            child: Text(_busy ? 'Checking…' : 'Import'),
          ),
        ],
      ),
    );
  }
}

class AdminKeyCard extends StatelessWidget {
  const AdminKeyCard({super.key, required this.publicHex});
  final String publicHex;

  @override
  Widget build(BuildContext context) {
    final fp = publicKeyFingerprint(fromHex(publicHex));
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('Admin public key',
                style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
            const SizedBox(height: 8),
            SelectableText(publicHex,
                style: const TextStyle(fontFamily: 'monospace', fontSize: 13)),
            const SizedBox(height: 12),
            const Text('Fingerprint',
                style: TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
            SelectableText(fp,
                style: const TextStyle(fontFamily: 'monospace', fontSize: 20)),
            const SizedBox(height: 12),
            FilledButton.icon(
              onPressed: () => copyText(context, publicHex, 'Public key'),
              icon: const Icon(Icons.copy),
              label: const Text('Copy public key'),
            ),
            const SizedBox(height: 12),
            const Text(kFirmwareNote),
          ],
        ),
      ),
    );
  }
}

class AdminKeyScreen extends StatelessWidget {
  const AdminKeyScreen({super.key, required this.stored});
  final EncryptedIdentity stored;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Admin key')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          AdminKeyCard(publicHex: stored.publicHex),
          const SizedBox(height: 24),
          OutlinedButton.icon(
            icon: const Icon(Icons.backup),
            label: const Text('Export encrypted backup'),
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute(
                  builder: (_) => _ExportScreen(blob: stored.toBackupBlob())),
            ),
          ),
        ],
      ),
    );
  }
}

class _ExportScreen extends StatelessWidget {
  const _ExportScreen({required this.blob});
  final String blob;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Encrypted backup')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          const Text(
            'This text is the admin private key encrypted with your '
            'passphrase. Store it somewhere safe (e.g. a password manager). '
            'Anyone who has it AND your passphrase can act as super admin, so '
            'use a strong passphrase.',
          ),
          const SizedBox(height: 16),
          SelectableText(blob,
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
          const SizedBox(height: 16),
          FilledButton.icon(
            onPressed: () => copyText(context, blob, 'Backup'),
            icon: const Icon(Icons.copy),
            label: const Text('Copy backup'),
          ),
        ],
      ),
    );
  }
}
