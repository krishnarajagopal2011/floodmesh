import 'package:flutter/material.dart';

import '../src/storage.dart';
import 'widgets.dart';

class HistoryScreen extends StatefulWidget {
  const HistoryScreen({super.key});

  @override
  State<HistoryScreen> createState() => _HistoryScreenState();
}

class _HistoryScreenState extends State<HistoryScreen> {
  List<HistoryEntry>? _entries;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final e = await HistoryStore.load();
    if (mounted) setState(() => _entries = e.reversed.toList());
  }

  @override
  Widget build(BuildContext context) {
    final entries = _entries;
    final now = nowUnix();
    return Scaffold(
      appBar: AppBar(
        title: const Text('Registration history'),
        actions: [
          IconButton(
            tooltip: 'Clear history',
            icon: const Icon(Icons.delete_outline),
            onPressed: entries == null || entries.isEmpty
                ? null
                : () async {
                    final ok = await confirm(context,
                        title: 'Clear history?',
                        body: 'This only clears the list on this phone. '
                            'Units keep their roles.',
                        ok: 'Clear',
                        danger: true);
                    if (!ok) return;
                    await HistoryStore.clear();
                    await _load();
                  },
          ),
        ],
      ),
      body: entries == null
          ? const Center(child: CircularProgressIndicator())
          : Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Padding(
                  padding: const EdgeInsets.all(16),
                  child: FilledButton.icon(
                    onPressed: entries.isEmpty
                        ? null
                        : () => copyText(
                            context,
                            HistoryStore.toCsv(entries.reversed.toList()),
                            'CSV'),
                    icon: const Icon(Icons.copy),
                    label: const Text('Copy as CSV'),
                  ),
                ),
                Expanded(
                  child: entries.isEmpty
                      ? const Center(child: Text('No registrations yet.'))
                      : ListView.separated(
                          itemCount: entries.length,
                          separatorBuilder: (_, _) =>
                              const Divider(height: 1),
                          itemBuilder: (_, i) {
                            final e = entries[i];
                            final exp = e.exp == 0
                                ? ''
                                : '\nexpires ${fmtLocal(e.exp)} '
                                    '(${fmtRelative(e.exp, now)})'
                                    '${e.cert ? '' : ' — NO CERT'}';
                            return ListTile(
                              title: Text(e.cs,
                                  style: const TextStyle(
                                      fontSize: 20,
                                      fontWeight: FontWeight.w600)),
                              subtitle: Text('${fmtLocal(e.time)}$exp'),
                              trailing: RoleChip(e.role),
                              isThreeLine: e.exp != 0,
                            );
                          },
                        ),
                ),
              ],
            ),
    );
  }
}
