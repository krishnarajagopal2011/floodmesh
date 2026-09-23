import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:universal_ble/universal_ble.dart';

import '../src/ble_client.dart';
import '../src/crypto.dart';
import '../src/protocol.dart';
import '../src/storage.dart';
import 'widgets.dart';

class DeviceScreen extends StatefulWidget {
  const DeviceScreen(
      {super.key,
      required this.deviceId,
      required this.name,
      required this.admin});
  final String deviceId;
  final String name;
  final P256KeyPair admin;

  @override
  State<DeviceScreen> createState() => _DeviceScreenState();
}

class _Partial implements Exception {
  _Partial(this.msg);
  final String msg;
  @override
  String toString() => msg;
}

class _CmdFailed implements Exception {
  _CmdFailed(this.code);
  final String? code;
}

class _DeviceScreenState extends State<DeviceScreen> {
  late final ProvisioningClient _client = ProvisioningClient(widget.deviceId);
  StreamSubscription<bool>? _connSub;
  DeviceInfo? _info;
  String? _pin;
  bool _connected = false;
  bool _connecting = false;
  bool _busy = false;
  String _busyText = '';
  String? _bannerText;
  bool _bannerError = false;
  ResponderDuration _duration = kResponderDurations.first;
  Timer? _tick;

  @override
  void initState() {
    super.initState();
    _connSub = UniversalBle.connectionStream(widget.deviceId).listen((c) {
      if (!mounted) return;
      setState(() => _connected = c);
      if (!c && !_connecting) {
        _banner('Disconnected from the unit.', true);
      }
    });
    // Refresh the expiry countdown.
    _tick = Timer.periodic(const Duration(seconds: 30), (_) {
      if (mounted) setState(() {});
    });
    _connect();
  }

  @override
  void dispose() {
    _tick?.cancel();
    _connSub?.cancel();
    _client.disconnect();
    super.dispose();
  }

  void _banner(String text, bool error) {
    setState(() {
      _bannerText = text;
      _bannerError = error;
    });
  }

  Future<void> _connect() async {
    setState(() {
      _connecting = true;
      _bannerText = null;
    });
    try {
      await _client.connect();
      final info = await _client.readInfo();
      if (!mounted) return;
      setState(() {
        _info = info;
        _connected = true;
      });
      if (_pin == null) await _askPin();
    } catch (e) {
      _banner('Could not connect or read INFO: $e', true);
    } finally {
      if (mounted) setState(() => _connecting = false);
    }
  }

  Future<void> _refresh() async {
    try {
      final info = await _client.readInfo();
      if (mounted) setState(() => _info = info);
    } catch (e) {
      _banner('Could not read INFO: $e', true);
    }
  }

  Future<bool> _askPin() async {
    final ctl = TextEditingController(text: _pin ?? '');
    final pin = await showDialog<String>(
      context: context,
      barrierDismissible: false,
      builder: (c) => AlertDialog(
        title: const Text('PIN from the unit screen'),
        content: TextField(
          controller: ctl,
          autofocus: true,
          keyboardType: TextInputType.number,
          maxLength: 6,
          inputFormatters: [FilteringTextInputFormatter.digitsOnly],
          style: const TextStyle(fontSize: 32, letterSpacing: 8),
          textAlign: TextAlign.center,
          decoration: const InputDecoration(hintText: '000000'),
          onSubmitted: (v) {
            if (isValidPin(v)) Navigator.pop(c, v);
          },
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(c), child: const Text('Cancel')),
          FilledButton(
            style: FilledButton.styleFrom(minimumSize: const Size(88, 48)),
            onPressed: () {
              if (isValidPin(ctl.text)) Navigator.pop(c, ctl.text);
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
    ctl.dispose();
    if (pin != null && mounted) setState(() => _pin = pin);
    return pin != null;
  }

  /// Reads a fresh INFO (for the nonce), builds a command with it, sends it
  /// and returns the successful response; throws [_CmdFailed] on ok:false.
  Future<CmdResponse> _command(String Function(DeviceInfo info) build) async {
    final info = await _client.readInfo();
    if (mounted) setState(() => _info = info);
    final resp = await _client.send(build(info));
    if (!resp.ok) throw _CmdFailed(resp.err);
    return resp;
  }

  Future<void> _run(String busyText, Future<String> Function() body,
      {bool refreshAfter = true}) async {
    if (_busy) return;
    if (_pin == null && !await _askPin()) return;
    setState(() {
      _busy = true;
      _busyText = busyText;
      _bannerText = null;
    });
    try {
      final ok = await body();
      _banner(ok, false);
    } on _CmdFailed catch (e) {
      _banner('Failed: ${errorText(e.code)}', true);
      if (e.code == 'auth') setState(() => _pin = null);
    } on _Partial catch (e) {
      _banner(e.msg, true);
    } catch (e) {
      _banner('Error: $e', true);
    } finally {
      if (mounted) {
        setState(() => _busy = false);
        if (_connected && refreshAfter) await _refresh();
      }
    }
  }

  // ------------------------------------------------------------------ actions

  Future<void> _installAdmin() async {
    final info = _info!;
    final ok = await confirm(context,
        title: 'Install admin key?',
        body: 'Unit ${info.cs} will permanently trust admin key '
            '${widget.admin.fingerprint}. Only a factory reset on the unit '
            'can remove it.',
        ok: 'Install');
    if (!ok) return;
    await _run('Installing admin key…', () async {
      final r = await _command((i) => buildSetAdmin(
          pubHex: widget.admin.publicHex, pin: _pin!, nonce: i.nonce));
      if (r.adminFp != null && r.adminFp != widget.admin.fingerprint) {
        return 'Admin key installed, but the unit reports fingerprint '
            '${r.adminFp} (expected ${widget.admin.fingerprint}).';
      }
      return 'Admin key installed on ${info.cs}.';
    });
  }

  Future<void> _makeResponder() async {
    final info = _info!;
    final dur = _duration;
    final previewExp = nowUnix() + dur.seconds;
    final ok = await confirm(context,
        title: 'Make ${info.cs} a responder?',
        body: 'Expires ${fmtLocal(previewExp)} (${dur.label}). A new '
            'responder key is generated inside the unit.',
        ok: 'Make responder');
    if (!ok) return;
    await _run('Registering responder…', () async {
      final now = nowUnix();
      final exp = now + dur.seconds;
      final r1 = await _command((i) => buildSetRole(
          admin: widget.admin,
          cs: i.cs,
          role: roleResponder,
          now: now,
          exp: exp,
          pin: _pin!,
          nonce: i.nonce));
      // set_role succeeded: the nonce has changed. _command re-reads INFO.
      setState(() => _busyText = 'Installing certificate…');
      var devPub = r1.devPub.toLowerCase();
      bool cert = false;
      String? certErr;
      try {
        await _command((i) {
          if (devPub.isEmpty) devPub = i.devPub.toLowerCase();
          if (!isLowerHex(devPub, 130)) {
            throw StateError('Unit returned no valid devPub');
          }
          if (i.devPub.isNotEmpty && i.devPub.toLowerCase() != devPub) {
            throw StateError('devPub in INFO differs from set_role response');
          }
          return buildSetCert(
              admin: widget.admin,
              cs: i.cs,
              exp: exp,
              devPubHex: devPub,
              pin: _pin!,
              nonce: i.nonce);
        });
        cert = true;
      } on _CmdFailed catch (e) {
        certErr = errorText(e.code);
      } catch (e) {
        certErr = '$e';
      }
      await HistoryStore.add(HistoryEntry(
          cs: info.cs, role: roleResponder, exp: exp, time: now, cert: cert));
      if (!cert) {
        throw _Partial('Unit is now a responder, but the certificate was NOT '
            'stored: $certErr Try "Make responder" again.');
      }
      return '${info.cs} is a responder until ${fmtLocal(exp)}. '
          'Certificate stored.';
    });
  }

  Future<void> _makePlain(String role) async {
    final info = _info!;
    final revoking = info.role == roleResponder;
    final ok = await confirm(context,
        title: role == roleRelay
            ? 'Make ${info.cs} a relay?'
            : 'Make ${info.cs} a civilian?',
        body: revoking
            ? 'This revokes its responder role and deletes its responder key.'
            : 'The role is changed immediately.',
        ok: 'Yes',
        danger: revoking);
    if (!ok) return;
    await _run('Setting role…', () async {
      final now = nowUnix();
      await _command((i) => buildSetRole(
          admin: widget.admin,
          cs: i.cs,
          role: role,
          now: now,
          exp: 0,
          pin: _pin!,
          nonce: i.nonce));
      await HistoryStore.add(
          HistoryEntry(cs: info.cs, role: role, exp: 0, time: now));
      return '${info.cs} is now a $role.';
    });
  }

  Future<void> _reboot() async {
    await _run('Rebooting unit…', () async {
      await _command((i) => buildReboot(pin: _pin!, nonce: i.nonce));
      return 'Unit is rebooting into normal mode.';
    }, refreshAfter: false);
    if (!mounted || _bannerError) return;
    await _client.disconnect();
    if (mounted) Navigator.pop(context);
  }

  // ----------------------------------------------------------------------- UI

  Widget _row(String k, Widget v) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          SizedBox(
              width: 110,
              child: Text(k,
                  style: const TextStyle(
                      fontWeight: FontWeight.w600, fontSize: 16))),
          Expanded(child: v),
        ]),
      );

  Widget _text(String s, {Color? color, bool mono = false}) => Text(s,
      style: TextStyle(
          fontSize: 16,
          color: color,
          fontFamily: mono ? 'monospace' : null,
          fontWeight: color != null ? FontWeight.bold : null));

  Widget _infoCard(DeviceInfo info) {
    final phoneNow = nowUnix();
    final mismatch = info.hasAdmin && info.adminFp != widget.admin.fingerprint;
    Widget expiry;
    if (info.role == roleResponder && info.exp > 0) {
      final left = info.exp - phoneNow;
      expiry = _text('${fmtLocal(info.exp)}\n${fmtRelative(info.exp, phoneNow)}',
          color: left <= 0
              ? Colors.red.shade700
              : left < 2 * 86400
                  ? Colors.orange.shade800
                  : null);
    } else {
      expiry = _text('—');
    }
    String unitTime;
    if (info.now == 0) {
      unitTime = 'not set';
    } else {
      final drift = info.now - phoneNow;
      unitTime = '${fmtLocal(info.now)} '
          '(${drift.abs() < 120 ? 'in sync' : '${drift > 0 ? '+' : ''}${drift}s vs phone'})';
    }
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(children: [
              Expanded(
                child: Text(info.cs,
                    style: const TextStyle(
                        fontSize: 32, fontWeight: FontWeight.bold)),
              ),
              RoleChip(info.role),
            ]),
            const SizedBox(height: 8),
            _row('Firmware', _text('${info.fw} (${info.board})')),
            _row('Expiry', expiry),
            if (info.role == roleResponder)
              _row(
                  'Certificate',
                  _text(info.hasCert ? 'stored' : 'MISSING',
                      color: info.hasCert ? null : Colors.red.shade700)),
            _row(
              'Admin',
              info.hasAdmin
                  ? _text(
                      '${info.admin}  ${info.adminFp}'
                      '${mismatch ? '\nNOT this app\'s key (${widget.admin.fingerprint})' : '\nmatches this app'}',
                      color: mismatch ? Colors.red.shade700 : Colors.green.shade700,
                      mono: true)
                  : _text('none — install the admin key',
                      color: Colors.orange.shade800),
            ),
            _row('Unit time', _text(unitTime)),
            if (info.fails > 0)
              _row(
                  'PIN fails',
                  _text('${info.fails} of 5 (mode exits at 5)',
                      color: Colors.red.shade700)),
          ],
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final info = _info;
    final canAct = info != null && _connected && !_busy && !_connecting;
    final mismatch =
        info != null && info.hasAdmin && info.adminFp != widget.admin.fingerprint;
    return Scaffold(
      appBar: AppBar(
        title: Text(widget.name),
        actions: [
          IconButton(
            tooltip: 'Re-read INFO',
            onPressed: canAct ? _refresh : null,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          if (_connecting) ...[
            const LinearProgressIndicator(),
            const SizedBox(height: 8),
            const Text('Connecting…', style: TextStyle(fontSize: 18)),
          ],
          if (_busy) ...[
            const LinearProgressIndicator(),
            const SizedBox(height: 8),
            Text(_busyText, style: const TextStyle(fontSize: 18)),
            const SizedBox(height: 8),
          ],
          if (_bannerText != null) ...[
            StatusBanner(
                text: _bannerText!,
                error: _bannerError,
                onClose: () => setState(() => _bannerText = null)),
            const SizedBox(height: 12),
          ],
          if (!_connected && !_connecting) ...[
            FilledButton.icon(
              onPressed: _connect,
              icon: const Icon(Icons.bluetooth_connected),
              label: const Text('Reconnect'),
            ),
            const SizedBox(height: 12),
          ],
          if (info != null) ...[
            if (mismatch) ...[
              const StatusBanner(
                  text: 'This unit trusts a DIFFERENT admin key. Role changes '
                      'from this phone will be rejected.',
                  error: true),
              const SizedBox(height: 12),
            ],
            _infoCard(info),
            const SizedBox(height: 8),
            ListTile(
              contentPadding: EdgeInsets.zero,
              leading: const Icon(Icons.pin, size: 32),
              title: Text(_pin == null ? 'PIN not entered' : 'PIN: $_pin',
                  style: const TextStyle(fontSize: 18)),
              trailing: TextButton(
                  onPressed: _busy ? null : _askPin,
                  child: const Text('Change')),
            ),
            const SizedBox(height: 8),
            if (!info.hasAdmin) ...[
              FilledButton.icon(
                onPressed: canAct ? _installAdmin : null,
                icon: const Icon(Icons.key),
                label: const Text('Install admin key'),
              ),
              const SizedBox(height: 16),
            ],
            Text('Responder duration',
                style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 8),
            SegmentedButton<ResponderDuration>(
              segments: [
                for (final d in kResponderDurations)
                  ButtonSegment(
                      value: d,
                      label: Text(d.seconds == 600 ? '10 min test' : d.label)),
              ],
              selected: {_duration},
              onSelectionChanged: _busy
                  ? null
                  : (s) => setState(() => _duration = s.first),
            ),
            const SizedBox(height: 12),
            FilledButton.icon(
              style: FilledButton.styleFrom(
                  backgroundColor: roleColor(roleResponder)),
              onPressed: canAct && info.hasAdmin ? _makeResponder : null,
              icon: const Icon(Icons.local_police),
              label: const Text('Make responder'),
            ),
            const SizedBox(height: 12),
            FilledButton.icon(
              style:
                  FilledButton.styleFrom(backgroundColor: roleColor(roleRelay)),
              onPressed:
                  canAct && info.hasAdmin ? () => _makePlain(roleRelay) : null,
              icon: const Icon(Icons.cell_tower),
              label: const Text('Make relay'),
            ),
            const SizedBox(height: 12),
            FilledButton.icon(
              style: FilledButton.styleFrom(
                  backgroundColor: roleColor(roleCivilian)),
              onPressed: canAct && info.hasAdmin
                  ? () => _makePlain(roleCivilian)
                  : null,
              icon: const Icon(Icons.person),
              label: const Text('Make civilian (revoke)'),
            ),
            const SizedBox(height: 24),
            OutlinedButton.icon(
              onPressed: canAct ? _reboot : null,
              icon: const Icon(Icons.restart_alt),
              label: const Text('Done (reboot unit)'),
            ),
          ],
        ],
      ),
    );
  }
}
