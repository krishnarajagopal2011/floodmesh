import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../src/protocol.dart';

String two(int n) => n.toString().padLeft(2, '0');

/// Local date/time, e.g. 2026-09-23 14:05.
String fmtLocal(int unixSeconds) {
  final t =
      DateTime.fromMillisecondsSinceEpoch(unixSeconds * 1000).toLocal();
  return '${t.year}-${two(t.month)}-${two(t.day)} ${two(t.hour)}:${two(t.minute)}';
}

/// "in 9 days 23 hours", "in 12 minutes", "expired 3 hours ago".
String fmtRelative(int targetUnix, int nowUnix) {
  var d = targetUnix - nowUnix;
  final past = d < 0;
  d = d.abs();
  String s;
  if (d >= 86400) {
    final days = d ~/ 86400;
    final hours = (d % 86400) ~/ 3600;
    s = '$days day${days == 1 ? '' : 's'}'
        '${hours > 0 ? ' $hours hour${hours == 1 ? '' : 's'}' : ''}';
  } else if (d >= 3600) {
    final hours = d ~/ 3600;
    final mins = (d % 3600) ~/ 60;
    s = '$hours hour${hours == 1 ? '' : 's'} $mins min';
  } else {
    final mins = (d + 59) ~/ 60;
    s = '$mins minute${mins == 1 ? '' : 's'}';
  }
  return past ? 'expired $s ago' : 'in $s';
}

int nowUnix() => DateTime.now().millisecondsSinceEpoch ~/ 1000;

Color roleColor(String role) => switch (role) {
      roleResponder => Colors.orange.shade700,
      roleRelay => Colors.blue.shade700,
      _ => Colors.grey.shade600,
    };

class RoleChip extends StatelessWidget {
  const RoleChip(this.role, {super.key});
  final String role;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
      decoration: BoxDecoration(
        color: roleColor(role),
        borderRadius: BorderRadius.circular(20),
      ),
      child: Text(
        role.isEmpty ? '?' : role.toUpperCase(),
        style: const TextStyle(
            color: Colors.white, fontWeight: FontWeight.bold, fontSize: 16),
      ),
    );
  }
}

class StatusBanner extends StatelessWidget {
  const StatusBanner(
      {super.key, required this.text, required this.error, this.onClose});
  final String text;
  final bool error;
  final VoidCallback? onClose;

  @override
  Widget build(BuildContext context) {
    final bg = error ? Colors.red.shade700 : Colors.green.shade700;
    return Material(
      color: bg,
      borderRadius: BorderRadius.circular(8),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Row(children: [
          Icon(error ? Icons.error : Icons.check_circle,
              color: Colors.white, size: 28),
          const SizedBox(width: 12),
          Expanded(
            child: Text(text,
                style: const TextStyle(color: Colors.white, fontSize: 16)),
          ),
          if (onClose != null)
            IconButton(
              onPressed: onClose,
              icon: const Icon(Icons.close, color: Colors.white),
            ),
        ]),
      ),
    );
  }
}

Future<void> copyText(BuildContext context, String text, String what) async {
  await Clipboard.setData(ClipboardData(text: text));
  if (context.mounted) {
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text('$what copied')));
  }
}

Future<bool> confirm(BuildContext context,
    {required String title,
    required String body,
    String ok = 'OK',
    bool danger = false}) async {
  final r = await showDialog<bool>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(title),
      content: Text(body),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(c, false),
            child: const Text('Cancel')),
        FilledButton(
          style: danger
              ? FilledButton.styleFrom(
                  backgroundColor: Colors.red.shade700,
                  minimumSize: const Size(88, 48))
              : FilledButton.styleFrom(minimumSize: const Size(88, 48)),
          onPressed: () => Navigator.pop(c, true),
          child: Text(ok),
        ),
      ],
    ),
  );
  return r ?? false;
}
