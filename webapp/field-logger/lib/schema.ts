/**
 * Database schema for the field logger (docs/field-logger-protocol.md).
 *
 * One list of idempotent statements, run on first use by every server
 * process and by `npm run db:migrate`. Plain SQL that both Neon Postgres and
 * PGlite accept, so local tests exercise the same schema as production.
 *
 * Column changes: add them here as `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`
 * statements after the CREATE TABLE, never by editing an existing column,
 * so a database created by an older build is upgraded in place.
 *
 * This file is also imported by scripts/migrate.ts under Node's own type
 * stripping, so it must stay plain (no path aliases, no enums).
 */
export const SCHEMA: readonly string[] = [
  // One row per accepted ingest request, including re-sends. It is the
  // "last seen" signal for a unit even when a request carries no new records.
  `CREATE TABLE IF NOT EXISTS uploads (
    id           bigserial PRIMARY KEY,
    unit         text        NOT NULL,
    received_at  timestamptz NOT NULL,
    v            integer     NOT NULL,
    mac          text,
    fw           text,
    image        text,
    boot         bigint      NOT NULL,
    up_ms        bigint      NOT NULL,
    device_epoch bigint      NOT NULL,
    dropped      bigint,
    n_records    integer     NOT NULL,
    n_new        integer     NOT NULL,
    min_seq      bigint,
    max_seq      bigint,
    body_bytes   integer     NOT NULL
  )`,
  `CREATE INDEX IF NOT EXISTS uploads_unit_received ON uploads (unit, received_at DESC)`,
  // Records of the request that collided with a different record already
  // stored under the same seq (see record_conflicts below).
  `ALTER TABLE uploads ADD COLUMN IF NOT EXISTS n_conflicts integer`,

  // One row per unit record, stored once per (unit, seq) so re-sends are
  // harmless (protocol section 4). Every field the protocol names has its own
  // column; anything else the unit sends is kept in `extra`, and so is a
  // known field that arrived with the wrong JSON type.
  //   ts           computed time (protocol section 5): the unit's epoch when
  //                its clock was set, else receive time minus uptime delta
  //                for records of the request's boot, else NULL
  //   ts_src       'unit' | 'uptime' | NULL, which rule produced ts
  //   device_epoch the unit's own claim, 0 when its clock was not set
  //   received_at  when the server first stored the record
  `CREATE TABLE IF NOT EXISTS records (
    unit         text        NOT NULL,
    seq          bigint      NOT NULL,
    boot         bigint      NOT NULL,
    ms           bigint      NOT NULL,
    device_epoch bigint      NOT NULL,
    ts           timestamptz,
    ts_src       text,
    received_at  timestamptz NOT NULL,
    k            text        NOT NULL,
    t            text,
    from_call    text,
    msg_id       text,
    hops         integer,
    rssi         double precision,
    snr          double precision,
    text         text,
    ch           integer,
    ok           boolean,
    reason       text,
    fw           text,
    image        text,
    state        text,
    n            integer,
    batt_v       double precision,
    batt_pct     integer,
    powered      boolean,
    air_pm       integer,
    heard        integer,
    sleep_pct    integer,
    role         text,
    extra        jsonb,
    PRIMARY KEY (unit, seq)
  )`,
  `CREATE INDEX IF NOT EXISTS records_unit_ts ON records (unit, ts)`,
  `CREATE INDEX IF NOT EXISTS records_unit_k_seq ON records (unit, k, seq DESC)`,
  // The factory MAC of the board that uploaded the record (from the upload's
  // envelope). A call sign names a unit, not a board: a spare board can be
  // given `callsign A`, and the analysis must still tell the two apart.
  `ALTER TABLE records ADD COLUMN IF NOT EXISTS mac text`,

  // Seq collisions (protocol section 4): a record whose (unit, seq) is
  // already stored with different content. That happens when seq restarts
  // at 1, after a board's flash is erased or when a spare board takes over a
  // call sign. The record is kept here, with the board's MAC, rather than
  // dropped as a duplicate; the unit is flagged on the admin page. Same
  // columns as records; mac is '' when the upload did not say, so the unique
  // key below also makes a re-sent collision a no-op.
  `CREATE TABLE IF NOT EXISTS record_conflicts (
    id           bigserial PRIMARY KEY,
    unit         text        NOT NULL,
    seq          bigint      NOT NULL,
    boot         bigint      NOT NULL,
    ms           bigint      NOT NULL,
    device_epoch bigint      NOT NULL,
    ts           timestamptz,
    ts_src       text,
    received_at  timestamptz NOT NULL,
    k            text        NOT NULL,
    t            text,
    from_call    text,
    msg_id       text,
    hops         integer,
    rssi         double precision,
    snr          double precision,
    text         text,
    ch           integer,
    ok           boolean,
    reason       text,
    fw           text,
    image        text,
    state        text,
    n            integer,
    batt_v       double precision,
    batt_pct     integer,
    powered      boolean,
    air_pm       integer,
    heard        integer,
    sleep_pct    integer,
    role         text,
    extra        jsonb,
    mac          text        NOT NULL DEFAULT ''
  )`,
  `CREATE UNIQUE INDEX IF NOT EXISTS record_conflicts_key ON record_conflicts (unit, seq, boot, ms, k, mac)`,

  // Volunteers' phone positions, only while they chose to share them.
  // ts is the phone's fix time; received_at is the server's clock.
  `CREATE TABLE IF NOT EXISTS locations (
    id           bigserial PRIMARY KEY,
    unit         text             NOT NULL,
    ts           timestamptz      NOT NULL,
    lat          double precision NOT NULL,
    lon          double precision NOT NULL,
    accuracy     double precision,
    altitude     double precision,
    received_at  timestamptz      NOT NULL DEFAULT now()
  )`,
  `CREATE INDEX IF NOT EXISTS locations_unit_ts ON locations (unit, ts)`,

  // Free-text observations typed on the volunteer page ("moved to terrace").
  `CREATE TABLE IF NOT EXISTS notes (
    id           bigserial PRIMARY KEY,
    unit         text        NOT NULL,
    ts           timestamptz NOT NULL DEFAULT now(),
    author       text        NOT NULL,
    text         text        NOT NULL
  )`,
  `CREATE INDEX IF NOT EXISTS notes_unit_ts ON notes (unit, ts DESC)`,

  // Login rate limiting (lib/ratelimit.ts): one counter per client IP and
  // per account for the current 15-minute window, kept in the database
  // rather than in memory because on Vercel every request may land on a
  // different instance. One row per key, so a single upsert counts an
  // attempt atomically however many arrive at once.
  `CREATE TABLE IF NOT EXISTS login_limits (
    key          text        PRIMARY KEY,
    window_start timestamptz NOT NULL,
    n            integer     NOT NULL
  )`,
  // Replaced by login_limits (its check-then-insert let concurrent guesses
  // through). It only ever held rate-limit state.
  `DROP TABLE IF EXISTS login_failures`,
];
