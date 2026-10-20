-- MegaOpen cloud D1 schema. Idempotent: safe to run again.
CREATE TABLE IF NOT EXISTS batches (
  id    INTEGER PRIMARY KEY,
  ts    INTEGER NOT NULL,          -- server time, epoch ms
  dev   TEXT    NOT NULL,
  fw    TEXT,
  boot  INTEGER,
  up    INTEGER,
  reset TEXT,
  heap  INTEGER,
  rssi  INTEGER,
  state TEXT,                      -- JSON
  lines TEXT                       -- newline-joined log lines
);
CREATE INDEX IF NOT EXISTS batches_dev_ts ON batches(dev, ts);
CREATE INDEX IF NOT EXISTS batches_ts ON batches(ts);

CREATE TABLE IF NOT EXISTS devices (
  dev        TEXT PRIMARY KEY,
  last_ts    INTEGER,              -- last ingest, server epoch ms
  fw         TEXT,
  boot       INTEGER,
  up         INTEGER,
  state      TEXT,                 -- JSON of last ingest
  last_check INTEGER               -- last GET /v1/manifest, server epoch ms
);
