// Problem reports filed from the launcher's Bug Report page. A filed report holds only the fields below, all of them
// shown to the player first. Who filed it stays apart: the quota keeps salted hashes of the connection's address for a
// day, and ownership is a random receipt of which only a hash is kept.
import { digestsMatch, sha256Hex, toHex } from './sha256.js';

const HOUR_MS = 3600 * 1000;
const DAY_MS = 24 * HOUR_MS;

export const REPORT = {
  bodyBytes: 192 * 1024,
  descriptionChars: 500,
  descriptionMinChars: 10,
  logBytes: 64 * 1024,
  metadataChars: 64,
  detailChars: 96,
  sessionDetailChars: 400,
  previewChars: 140,
  perAddressPerHour: 3,
  perAddressPerDay: 10,
  perDay: 300,
  kept: 500,
  keptDays: 180,
  messageChars: 500,
  statusBatch: 30,
  statusKept: 5000,
  statusKeptDays: 365,
  // A dump holds thread stacks and module lists, no heap (src/crash_dump.c); the launcher refuses more as well.
  dumpBytes: 8 * 1024 * 1024,
  dumpMinBytes: 1024,
  dumpChunkBytes: 1024 * 1024,
  dumpsKept: 100,
  dumpPendingMs: DAY_MS,
  submissionKeptMs: DAY_MS,
};

export const REPORT_STATUSES = ['new', 'investigating', 'fixed', 'cannot_reproduce', 'duplicate', 'not_a_bug', 'closed'];
export const SESSION_ENDS = ['unknown', 'clean', 'crash', 'device_lost', 'stopped', 'startup_failure', 'unsupported_build'];

const LOG_FILE = /^shadow_of_the_beast_[0-9]{3,10}\.log$/;
const HEX64 = /^[0-9a-f]{64}$/;
const DUMP_KEYS = ['dump_file', 'dump_bytes', 'dump_sha256'];
const REQUEST_EXTRAS = new Set([...DUMP_KEYS, 'submission']);
const CHUNK_KEYS = new Set(['ticket', 'receipt', 'offset', 'data']);
const BASE64 = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;
const MINIDUMP_MAGIC = [0x4d, 0x44, 0x4d, 0x50];
const VERSION = /^[0-9A-Za-z][0-9A-Za-z.+-]{0,31}$/;
const TICKET = /^SOB-[A-HJ-NP-Z2-9]{6}$/;
const RECEIPT = /^[0-9a-f]{64}$/;
const STATUS_FIELDS = new Set(['status', 'fixed_version', 'player_message']);
const TICKET_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

// SOB-XXXXXX from six random bytes; the alphabet has 32 letters, so each byte maps without bias.
export const ticketCode = (bytes) => `SOB-${Array.from(bytes.subarray(0, 6), (b) => TICKET_ALPHABET[b % 32]).join('')}`;

export const isTicket = (text) => typeof text === 'string' && TICKET.test(text);

// Built from the two validated names only, so nothing the client sent beyond them reaches a header.
export function logDownloadName(report) {
  if (!isTicket(report.ticket)) return 'shadow-of-the-beast-report.log';
  return LOG_FILE.test(report.log_file) ? `${report.ticket}-${report.log_file}` : `${report.ticket}.log`;
}

// The dump of a log is named after it; the service never takes the launcher's word for the name.
export const dumpNameOf = (logFile) => `${logFile.slice(0, -4)}.dmp`;

export function dumpDownloadName(report) {
  if (!isTicket(report.ticket)) return 'shadow-of-the-beast-report.dmp';
  return LOG_FILE.test(report.log_file) ? `${report.ticket}-${dumpNameOf(report.log_file)}` : `${report.ticket}.dmp`;
}

export const isMinidump = (bytes) => bytes.length >= MINIDUMP_MAGIC.length && MINIDUMP_MAGIC.every((b, i) => bytes[i] === b);

const utf8Bytes = (text) => new TextEncoder().encode(text).length;

// What the launcher's filter leaves in text: line feeds and tabs, no other control characters, no carriage returns.
// eslint-disable-next-line no-control-regex
const HAS_FORBIDDEN = /[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/;
// eslint-disable-next-line no-control-regex
const HAS_CONTROL = /[\u0000-\u001f\u007f-\u009f]/;

class Refusal extends Error {
  constructor(code, detail) {
    super(detail);
    this.code = code;
  }
}

function text(msg, field, maxChars, { optional = false, pattern = null } = {}) {
  const value = msg[field];
  if (typeof value !== 'string' || HAS_CONTROL.test(value) || value !== value.trim()) throw new Refusal('bad_request', field);
  if (!value && !optional) throw new Refusal('bad_request', field);
  if (Array.from(value).length > maxChars) throw new Refusal('too_large', field);
  if (value && pattern && !pattern.test(value)) throw new Refusal('bad_request', field);
  return value;
}

function description(msg) {
  const value = msg.description;
  if (typeof value !== 'string' || HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'description');
  if (Array.from(value.trim()).length < REPORT.descriptionMinChars) throw new Refusal('bad_request', 'description too short');
  if (Array.from(value).length > REPORT.descriptionChars) throw new Refusal('too_large', 'description');
  return value;
}

function logText(msg) {
  const value = msg.log_text;
  if (typeof value !== 'string') throw new Refusal('bad_request', 'log_text');
  if (utf8Bytes(value) > REPORT.logBytes) throw new Refusal('too_large', 'log_text');
  if (HAS_FORBIDDEN.test(value) || value.includes('\r')) throw new Refusal('bad_request', 'log_text');
  if (!value.trim()) throw new Refusal('bad_request', 'empty log');
  return value;
}

function yesNo(msg, field) {
  if (msg[field] !== 'yes' && msg[field] !== 'no') throw new Refusal('bad_request', field);
  return msg[field];
}

const meta = (field, options) => (msg) => text(msg, field, REPORT.metadataChars, options);
const detail = (field) => (msg) => text(msg, field, REPORT.detailChars, { optional: true });

// A field newer launchers send: a launcher released before it existed leaves it out, and its reports are still taken.
const newer = (field, fallback, read) => (msg) => (Object.hasOwn(msg, field) ? read(msg) : fallback);

function sessionEnd(msg) {
  if (!SESSION_ENDS.includes(msg.session_end)) throw new Refusal('bad_request', 'session_end');
  return msg.session_end;
}

export const READERS = {
  description,
  launcher_version: meta('launcher_version', { pattern: VERSION }),
  game_build: meta('game_build'),
  region: meta('region', { optional: true }),
  game_version: meta('game_version', { optional: true, pattern: /^[0-9][0-9.]{0,15}$/ }),
  cusa: meta('cusa', { optional: true, pattern: /^CUSA[0-9]{5}$/ }),
  eboot_sha256: meta('eboot_sha256', { optional: true, pattern: /^[0-9a-f]{64}$/ }),
  operating_system: meta('operating_system'),
  cpu: detail('cpu'),
  renderer_selected: meta('renderer_selected', { optional: true }),
  renderer: meta('renderer', { optional: true }),
  gpu: detail('gpu'),
  driver: detail('driver'),
  render_resolution: meta('render_resolution', { optional: true }),
  session_end: newer('session_end', 'unknown', sessionEnd),
  session_detail: newer('session_detail', '', (msg) => text(msg, 'session_detail', REPORT.sessionDetailChars, { optional: true })),
  session_stage: newer('session_stage', '', meta('session_stage', { optional: true })),
  log_file: meta('log_file', { pattern: LOG_FILE }),
  log_clipped: (msg) => yesNo(msg, 'log_clipped'),
  log_text: logText,
};

// The announced crash dump of a report, or null. Its name must be the selected log's, so it names nothing else.
function readDump(msg, logFile) {
  const given = DUMP_KEYS.filter((key) => Object.hasOwn(msg, key));
  if (given.length === 0) return null;
  if (given.length !== DUMP_KEYS.length) throw new Refusal('bad_request', 'dump_file, dump_bytes and dump_sha256 go together');
  if (msg.dump_file !== dumpNameOf(logFile)) throw new Refusal('bad_request', 'dump_file');
  const bytes = msg.dump_bytes;
  if (!Number.isSafeInteger(bytes) || bytes < REPORT.dumpMinBytes) throw new Refusal('bad_request', 'dump_bytes');
  if (bytes > REPORT.dumpBytes) throw new Refusal('too_large', 'dump_bytes');
  if (typeof msg.dump_sha256 !== 'string' || !HEX64.test(msg.dump_sha256)) throw new Refusal('bad_request', 'dump_sha256');
  return { file: dumpNameOf(logFile), bytes, sha256: msg.dump_sha256 };
}

function readSubmission(msg) {
  if (!Object.hasOwn(msg, 'submission')) return null;
  if (typeof msg.submission !== 'string' || !HEX64.test(msg.submission)) throw new Refusal('bad_request', 'submission');
  return msg.submission;
}

// The stored fields of a request, or { code, detail } for the answer. Nothing here trusts a size the client states, and
// a field the page could not have shown is refused rather than dropped: unknown_field names a launcher newer than this.
export function readReport(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) return { code: 'bad_request', detail: 'body' };
  for (const key of Object.keys(body)) {
    if (!Object.hasOwn(READERS, key) && !REQUEST_EXTRAS.has(key)) return { code: 'unknown_field', detail: key.slice(0, REPORT.metadataChars) };
  }
  try {
    const report = {};
    for (const [field, read] of Object.entries(READERS)) report[field] = read(body);
    return { report, dump: readDump(body, report.log_file), submission: readSubmission(body) };
  } catch (error) {
    if (error instanceof Refusal) return { code: error.code, detail: error.message };
    throw error;
  }
}

function decodeBase64(text) {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

// One chunk of a crash dump: { claim, offset, bytes } or { code, detail }. The bytes must be standard base64 and fit one
// chunk; where they go is decided against what is stored, never by the client alone.
export function readDumpChunk(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) return { code: 'bad_request', detail: 'body' };
  for (const key of Object.keys(body)) if (!CHUNK_KEYS.has(key)) return { code: 'bad_request', detail: key };
  const claim = readClaim(body);
  if (!claim) return { code: 'bad_request', detail: 'receipt' };
  const { offset, data } = body;
  if (!Number.isSafeInteger(offset) || offset < 0 || offset >= REPORT.dumpBytes) return { code: 'bad_request', detail: 'offset' };
  if (typeof data !== 'string' || data.length === 0) return { code: 'bad_request', detail: 'data' };
  if (data.length > Math.ceil(REPORT.dumpChunkBytes / 3) * 4) return { code: 'too_large', detail: 'data' };
  if (!BASE64.test(data)) return { code: 'bad_request', detail: 'data' };
  const bytes = decodeBase64(data);
  if (bytes.length === 0 || bytes.length > REPORT.dumpChunkBytes) return { code: 'too_large', detail: 'data' };
  return { claim, offset, bytes };
}

// One report's receipt from a lookup, or null. Nothing here says whether the report exists.
export function readClaim(entry) {
  if (entry === null || typeof entry !== 'object' || Array.isArray(entry)) return null;
  const { ticket, receipt } = entry;
  return isTicket(ticket) && typeof receipt === 'string' && RECEIPT.test(receipt) ? { ticket, receipt } : null;
}

export function readReadClaim(body) {
  if (body === null || typeof body !== 'object' || Object.keys(body).some((key) => key !== 'ticket' && key !== 'receipt')) return null;
  return readClaim(body);
}

// The claims of a status request, or null when it is malformed or too long.
export function readClaims(body) {
  if (body === null || typeof body !== 'object' || Object.keys(body).some((key) => key !== 'reports')) return null;
  const { reports } = body;
  if (!Array.isArray(reports) || reports.length > REPORT.statusBatch) return null;
  const claims = reports.map(readClaim);
  return claims.includes(null) ? null : claims;
}

// An admin's change: { change } or { detail }. A fixed version exists only on a Fixed report, so a stale one never shows.
export function readStatusChange(body) {
  if (body === null || typeof body !== 'object' || Array.isArray(body)) return { detail: 'body' };
  for (const key of Object.keys(body)) if (!STATUS_FIELDS.has(key)) return { detail: key };
  const { status, fixed_version: version = '', player_message: message = '' } = body;
  if (!REPORT_STATUSES.includes(status)) return { detail: 'status' };
  if (typeof version !== 'string' || (status === 'fixed' && version && !VERSION.test(version))) return { detail: 'fixed_version' };
  if (typeof message !== 'string' || HAS_FORBIDDEN.test(message) || message.includes('\r')) return { detail: 'player_message' };
  if (Array.from(message).length > REPORT.messageChars) return { detail: 'player_message' };
  return { change: { status, fixed_version: status === 'fixed' ? version : '', player_message: message.trim() } };
}

const LIST_COLUMNS = `reports.ticket AS ticket, ms, launcher_version, game_build, region, game_version, operating_system, cpu,
  renderer_selected, renderer, gpu, driver, render_resolution, session_end, log_file, log_clipped,
  substr(description, 1, ${REPORT.previewChars}) AS preview, COALESCE(t.status, 'new') AS status,
  COALESCE(t.status_ms, ms) AS status_updated_at,
  COALESCE((SELECT CASE complete WHEN 1 THEN 'stored' ELSE 'uploading' END FROM report_dumps d WHERE d.ticket = reports.ticket),
    'none') AS dump_status`;

const SESSION_COLUMNS = ['session_end', 'session_detail', 'session_stage'];

const statusOf = (row) => ({
  status: row.status,
  status_updated_at: row.status_ms,
  fixed_version: row.fixed_version,
  player_message: row.player_message,
});

// The report store. `sql` is { all, get, run } over a SQLite database.
export class ReportDesk {
  constructor(sql, salt) {
    this.sql = sql;
    this.salt = salt;
    sql.run(`CREATE TABLE IF NOT EXISTS reports (ticket TEXT PRIMARY KEY, ms INTEGER NOT NULL,
      launcher_version TEXT NOT NULL, game_build TEXT NOT NULL, region TEXT NOT NULL, game_version TEXT NOT NULL,
      cusa TEXT NOT NULL, eboot_sha256 TEXT NOT NULL, operating_system TEXT NOT NULL, cpu TEXT NOT NULL,
      renderer_selected TEXT NOT NULL, renderer TEXT NOT NULL, gpu TEXT NOT NULL, driver TEXT NOT NULL,
      render_resolution TEXT NOT NULL, description TEXT NOT NULL, log_file TEXT NOT NULL, log_clipped TEXT NOT NULL, log_text TEXT NOT NULL)`);
    sql.run('CREATE TABLE IF NOT EXISTS report_quota (address_key TEXT NOT NULL, ms INTEGER NOT NULL)');
    sql.run(`CREATE TABLE IF NOT EXISTS report_tracking (ticket TEXT PRIMARY KEY, receipt_hash TEXT NOT NULL,
      submitted_ms INTEGER NOT NULL, status TEXT NOT NULL, status_ms INTEGER NOT NULL, fixed_version TEXT NOT NULL,
      player_message TEXT NOT NULL)`);
    // A database made before the session fields gains them; its older reports read as empty.
    for (const column of SESSION_COLUMNS) {
      try {
        sql.get(`SELECT ${column} FROM reports LIMIT 0`);
      } catch {
        sql.run(`ALTER TABLE reports ADD COLUMN ${column} TEXT NOT NULL DEFAULT ''`);
      }
    }
    sql.run(`CREATE TABLE IF NOT EXISTS report_dumps (ticket TEXT PRIMARY KEY, file TEXT NOT NULL, bytes INTEGER NOT NULL,
      sha256 TEXT NOT NULL, complete INTEGER NOT NULL, ms INTEGER NOT NULL)`);
    sql.run(`CREATE TABLE IF NOT EXISTS report_dump_chunks (ticket TEXT NOT NULL, pos INTEGER NOT NULL, data BLOB NOT NULL,
      PRIMARY KEY (ticket, pos))`);
    sql.run('CREATE TABLE IF NOT EXISTS report_submissions (key TEXT PRIMARY KEY, ticket TEXT NOT NULL, ms INTEGER NOT NULL)');
  }

  key(kind, value) {
    return sha256Hex(`${this.salt}|${kind}|${value}`).slice(0, 32);
  }

  // The receipt of a report filed with a submission key: the same key always gives the same receipt, so a repeated
  // request can be answered again although only a hash of the receipt is stored.
  submissionReceipt(submission) {
    return sha256Hex(`${this.salt}|submission-receipt|${submission}`);
  }

  submitted(submission, now) {
    return this.sql.get(`SELECT s.ticket AS ticket, r.ms AS ms FROM report_submissions s JOIN reports r ON r.ticket = s.ticket
      WHERE s.key = ? AND s.ms > ?`, this.key('submission', submission), now - REPORT.submissionKeptMs) ?? null;
  }

  rememberSubmission(submission, ticket, now) {
    this.sql.run('INSERT INTO report_submissions (key, ticket, ms) VALUES (?, ?, ?)', this.key('submission', submission), ticket, now);
  }

  // The answer code that refuses a report now, or null. The windows roll: a report leaves them one hour and one day after
  // it was accepted. The shorter window comes first, because that is the one to wait for.
  refusal(address, now) {
    this.sql.run('DELETE FROM report_quota WHERE ms <= ?', now - DAY_MS);
    const key = this.key('address', address);
    const used = this.sql.get(`SELECT COUNT(*) AS everyone, COALESCE(SUM(address_key = ?), 0) AS address_day,
      COALESCE(SUM(address_key = ? AND ms > ?), 0) AS address_hour FROM report_quota`, key, key, now - HOUR_MS);
    if (used.address_hour >= REPORT.perAddressPerHour) return 'hourly_limit';
    if (used.address_day >= REPORT.perAddressPerDay) return 'daily_limit';
    if (used.everyone >= REPORT.perDay) return 'limit';
    return null;
  }

  file(report, ticket, address, now, dump = null) {
    this.purge(now);
    this.sql.run(`INSERT INTO reports (ticket, ms, launcher_version, game_build, region, game_version, cusa, eboot_sha256,
      operating_system, cpu, renderer_selected, renderer, gpu, driver, render_resolution, description, log_file, log_clipped,
      log_text, session_end, session_detail, session_stage) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    ticket, now, report.launcher_version, report.game_build, report.region, report.game_version, report.cusa, report.eboot_sha256,
    report.operating_system, report.cpu, report.renderer_selected, report.renderer, report.gpu, report.driver,
    report.render_resolution, report.description, report.log_file, report.log_clipped, report.log_text, report.session_end,
    report.session_detail, report.session_stage);
    this.sql.run('INSERT INTO report_quota (address_key, ms) VALUES (?, ?)', this.key('address', address), now);
    this.sql.run(`DELETE FROM reports WHERE rowid IN
      (SELECT rowid FROM reports ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, REPORT.kept);
    if (dump) {
      this.sql.run('INSERT INTO report_dumps (ticket, file, bytes, sha256, complete, ms) VALUES (?, ?, ?, ?, 0, ?)', ticket, dump.file,
        dump.bytes, dump.sha256, now);
    }
    this.trimDumps(now);
  }

  // Reports and their statuses leave after their own number of days, whatever the count limits say; a dump and a
  // submission key never outlive their report.
  purge(now) {
    this.sql.run('DELETE FROM reports WHERE ms <= ?', now - REPORT.keptDays * DAY_MS);
    this.sql.run('DELETE FROM report_tracking WHERE submitted_ms <= ?', now - REPORT.statusKeptDays * DAY_MS);
    this.sql.run('DELETE FROM report_submissions WHERE ms <= ? OR ticket NOT IN (SELECT ticket FROM reports)', now - REPORT.submissionKeptMs);
    this.trimDumps(now);
  }

  trimDumps(now) {
    this.sql.run('DELETE FROM report_dumps WHERE complete = 0 AND ms <= ?', now - REPORT.dumpPendingMs);
    this.sql.run('DELETE FROM report_dumps WHERE ticket NOT IN (SELECT ticket FROM reports)');
    this.sql.run(`DELETE FROM report_dumps WHERE rowid IN
      (SELECT rowid FROM report_dumps ORDER BY ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, REPORT.dumpsKept);
    this.sql.run('DELETE FROM report_dump_chunks WHERE ticket NOT IN (SELECT ticket FROM report_dumps)');
  }

  dump(ticket) {
    return this.sql.get('SELECT * FROM report_dumps WHERE ticket = ?', ticket) ?? null;
  }

  dumpStored(ticket) {
    return this.sql.get('SELECT COALESCE(MAX(pos + LENGTH(data)), 0) AS n FROM report_dump_chunks WHERE ticket = ?', ticket).n;
  }

  // A chunk goes at the end of what is stored; a second writer to the same place fails on the key.
  appendDump(ticket, pos, bytes) {
    this.sql.run('INSERT INTO report_dump_chunks (ticket, pos, data) VALUES (?, ?, ?)', ticket, pos, bytes);
  }

  dropDump(ticket) {
    this.sql.run('DELETE FROM report_dumps WHERE ticket = ?', ticket);
    this.sql.run('DELETE FROM report_dump_chunks WHERE ticket = ?', ticket);
  }

  dumpBytes(ticket, partial = false) {
    const dump = this.dump(ticket);
    if (!dump || (!dump.complete && !partial)) return null;
    const out = new Uint8Array(dump.bytes);
    let at = 0;
    for (const chunk of this.sql.all('SELECT pos, data FROM report_dump_chunks WHERE ticket = ? ORDER BY pos', ticket)) {
      const data = new Uint8Array(chunk.data);
      if (chunk.pos !== at || at + data.length > dump.bytes) return null;
      out.set(data, at);
      at += data.length;
    }
    return at === dump.bytes ? out : null;
  }

  // 'complete' once every announced byte arrived, begins like a minidump and has the announced SHA-256; anything else
  // drops the dump. The digest is awaited, so the state is read again before it changes.
  async completeDump(ticket) {
    const dump = this.dump(ticket);
    if (!dump) return 'missing';
    if (dump.complete) return 'complete';
    const bytes = this.dumpBytes(ticket, true);
    if (!bytes) return 'incomplete';
    const verdict = !isMinidump(bytes) ? 'not_minidump'
      : toHex(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))) !== dump.sha256 ? 'dump_sha256' : 'complete';
    const now = this.dump(ticket);
    if (!now || now.complete) return now ? 'complete' : 'missing';
    if (verdict !== 'complete') {
      this.dropDump(ticket);
      return verdict;
    }
    this.sql.run('UPDATE report_dumps SET complete = 1 WHERE ticket = ?', ticket);
    return 'complete';
  }

  dumpSummary(ticket) {
    const dump = this.dump(ticket);
    return { dump_status: dump ? (dump.complete ? 'stored' : 'uploading') : 'none', dump_file: dump?.file ?? '', dump_bytes: dump?.bytes ?? 0 };
  }

  receiptHash(receipt) {
    return sha256Hex(`${this.salt}|receipt|${receipt}`);
  }

  // A status outlives the log it came with, so its table has a longer limit of its own.
  track(ticket, receipt, now) {
    this.sql.run(`INSERT INTO report_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, ?, ?, 'new', ?, '', '')`, ticket, this.receiptHash(receipt), now, now);
    this.sql.run(`DELETE FROM report_tracking WHERE rowid IN
      (SELECT rowid FROM report_tracking ORDER BY submitted_ms DESC, rowid DESC LIMIT -1 OFFSET ?)`, REPORT.statusKept);
  }

  // Every claim costs the same hash, so a wrong receipt and an unknown ticket cannot be told apart.
  claimed({ ticket, receipt }) {
    const hash = this.receiptHash(receipt);
    const row = this.sql.get('SELECT * FROM report_tracking WHERE ticket = ?', ticket);
    return row && digestsMatch(row.receipt_hash, hash) ? row : null;
  }

  // What the launcher may know of the claimed reports; a claim that fails is left out.
  statuses(claims) {
    return claims.map((claim) => this.claimed(claim)).filter(Boolean).map((row) => ({
      ticket: row.ticket,
      status: row.status,
      status_updated_at: row.status_ms,
      ...(row.fixed_version ? { fixed_version: row.fixed_version } : {}),
      ...(row.player_message ? { player_message: row.player_message } : {}),
    }));
  }

  // The stored report for its owner, or null once its log is gone.
  report(claim) {
    const row = this.claimed(claim);
    const stored = row && this.get(row.ticket);
    if (!stored) return null;
    const { ms, ...report } = stored;
    return { ...report, submitted_at: ms };
  }

  status(ticket) {
    const row = this.sql.get('SELECT * FROM report_tracking WHERE ticket = ?', ticket);
    if (row) return statusOf(row);
    const stored = this.sql.get('SELECT ms FROM reports WHERE ticket = ?', ticket);
    return stored ? { status: 'new', status_updated_at: stored.ms, fixed_version: '', player_message: '' } : null;
  }

  // The status after the change, or null for an unknown report. A save that changes nothing keeps its time.
  setStatus(ticket, change, now) {
    const current = this.status(ticket);
    if (!current) return null;
    if (Object.keys(change).every((field) => current[field] === change[field])) return current;
    const submitted = this.sql.get('SELECT ms FROM reports WHERE ticket = ?', ticket)?.ms ?? current.status_updated_at;
    this.sql.run(`INSERT INTO report_tracking (ticket, receipt_hash, submitted_ms, status, status_ms, fixed_version, player_message)
      VALUES (?, '', ?, ?, ?, ?, ?) ON CONFLICT(ticket) DO UPDATE SET status = excluded.status, status_ms = excluded.status_ms,
      fixed_version = excluded.fixed_version, player_message = excluded.player_message`, ticket, submitted, change.status, now,
    change.fixed_version, change.player_message);
    return this.status(ticket);
  }

  // The whole report: its fields and log, and the status and receipt hash that let its owner look it up. False when
  // nothing was stored under the ticket.
  remove(ticket) {
    const atomically = this.sql.transaction ?? ((work) => work());
    return atomically(() => {
      const existed = this.taken(ticket);
      this.sql.run('DELETE FROM reports WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM report_tracking WHERE ticket = ?', ticket);
      this.sql.run('DELETE FROM report_submissions WHERE ticket = ?', ticket);
      this.dropDump(ticket);
      return existed;
    });
  }

  taken(ticket) {
    return Boolean(this.sql.get(`SELECT 1 AS x FROM reports WHERE ticket = ?
      UNION ALL SELECT 1 AS x FROM report_tracking WHERE ticket = ?`, ticket, ticket));
  }

  list(now) {
    this.purge(now);
    return {
      reports: this.sql.all(`SELECT ${LIST_COLUMNS} FROM reports LEFT JOIN report_tracking t ON t.ticket = reports.ticket
        ORDER BY ms DESC, reports.rowid DESC`),
      kept: REPORT.kept,
    };
  }

  get(ticket) {
    return this.sql.get('SELECT * FROM reports WHERE ticket = ?', ticket) ?? null;
  }
}
