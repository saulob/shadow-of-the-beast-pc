// The report service behind the Durable Object: the store, the abuse limits and the random values. No Cloudflare import,
// so Node runs it in the tests. Every method answers { code: 'ok', ... } or { code } with what went wrong.
import { PROTECTION, RateLimiter } from './limits.js';
import { ReportDesk, isMinidump, ticketCode } from './reports.js';
import { toHex } from './sha256.js';

export const SERVICE = 'shadow-bug-reports';
export const PROTOCOL = 2;
export const WORKER_BUILD = '2026-11-25-shadow-bug-reports';

const progressOf = (dump, stored) => ({ stored: dump.complete ? dump.bytes : stored, bytes: dump.bytes, complete: Boolean(dump.complete) });
const NO_DUMP = { code: 'bad_request', detail: 'no crash dump was announced for this report' };

// host: { sql: { all, get, run, transaction? }, now(), randomBytes(n), log(text) }
export class ReportService {
  constructor(host) {
    this.host = host;
    this.desk = new ReportDesk(host.sql, this.salt());
    this.guards = {
      submitAll: new RateLimiter(PROTECTION.submitAll),
      submitAddress: new RateLimiter(PROTECTION.submitAddress),
      lookupAll: new RateLimiter(PROTECTION.lookupAll),
      statusAddress: new RateLimiter(PROTECTION.statusAddress),
      readAddress: new RateLimiter(PROTECTION.readAddress),
      dumpAll: new RateLimiter(PROTECTION.dumpAll),
      dumpAddress: new RateLimiter(PROTECTION.dumpAddress),
    };
  }

  atomically(work) {
    return (this.host.sql.transaction ?? ((inner) => inner()))(work);
  }

  // The service's own random salt for the keys of the quota and the receipts; it never leaves the database.
  salt() {
    const { sql } = this.host;
    sql.run('CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)');
    const stored = sql.get("SELECT v FROM meta WHERE k = 'salt'");
    if (stored) return stored.v;
    const salt = toHex(this.host.randomBytes(32));
    sql.run("INSERT INTO meta (k, v) VALUES ('salt', ?)", salt);
    return salt;
  }

  limited(guards) {
    const now = this.host.now();
    if (!guards.every(([guard, key]) => this.guards[guard].allows(key, now))) return true;
    for (const [guard, key] of guards) this.guards[guard].take(key, now);
    return false;
  }

  newTicket() {
    let ticket;
    do ticket = ticketCode(this.host.randomBytes(6));
    while (this.desk.taken(ticket));
    return ticket;
  }

  // A request repeated with the submission key of a report already filed (its answer was lost) gets that report back:
  // no second report, no second quota entry.
  submit(report, address, dump = null, submission = null) {
    if (this.limited([['submitAll', 'all'], ['submitAddress', address]])) return { code: 'limit' };
    const now = this.host.now();
    const filed = this.atomically(() => {
      const earlier = submission && this.desk.submitted(submission, now);
      if (earlier) {
        return { code: 'ok', ticket: earlier.ticket, receipt: this.desk.submissionReceipt(submission), submitted_at: earlier.ms,
          dump: Boolean(this.desk.dump(earlier.ticket)), repeated: true };
      }
      const refusal = this.desk.refusal(address, now);
      if (refusal) return { code: refusal };
      const ticket = this.newTicket();
      const receipt = submission ? this.desk.submissionReceipt(submission) : toHex(this.host.randomBytes(32));
      this.desk.file(report, ticket, address, now, dump);
      this.desk.track(ticket, receipt, now);
      if (submission) this.desk.rememberSubmission(submission, ticket, now);
      return { code: 'ok', ticket, receipt, submitted_at: now, dump: Boolean(dump), repeated: false };
    });
    if (filed.code !== 'ok') this.host.log(`report refused: ${filed.code}`);
    return filed;
  }

  // A dump whose bytes all arrived but whose check never finished is checked now.
  async dumpStatus(claim, address) {
    if (this.limited([['dumpAll', 'all'], ['dumpAddress', address]])) return { code: 'limit' };
    this.desk.purge(this.host.now());
    if (!this.desk.claimed(claim)) return { code: 'not_found' };
    const dump = this.desk.dump(claim.ticket);
    if (!dump) return NO_DUMP;
    const stored = this.desk.dumpStored(claim.ticket);
    if (!dump.complete && stored === dump.bytes) return this.finishDump(claim.ticket);
    return { code: 'ok', ...progressOf(dump, stored) };
  }

  // Only an append exactly at the end of what is stored; 'conflict' answers with where that is.
  async dumpChunk(claim, offset, bytes, address) {
    if (this.limited([['dumpAll', 'all'], ['dumpAddress', address]])) return { code: 'limit' };
    this.desk.purge(this.host.now());
    if (!this.desk.claimed(claim)) return { code: 'not_found' };
    const step = this.atomically(() => {
      const dump = this.desk.dump(claim.ticket);
      if (!dump) return NO_DUMP;
      const stored = this.desk.dumpStored(claim.ticket);
      if (dump.complete || offset !== stored) return { code: 'conflict', ...progressOf(dump, stored) };
      if (offset + bytes.length > dump.bytes) return { code: 'bad_request', detail: 'past the announced size' };
      if (offset === 0 && !isMinidump(bytes)) {
        this.desk.dropDump(claim.ticket);
        return { code: 'bad_request', detail: 'not_minidump' };
      }
      this.desk.appendDump(claim.ticket, offset, bytes);
      return { code: 'ok', stored: offset + bytes.length, bytes: dump.bytes, complete: false };
    });
    if (step.code !== 'ok' || step.stored !== step.bytes) return step;
    return this.finishDump(claim.ticket);
  }

  async finishDump(ticket) {
    const verdict = await this.desk.completeDump(ticket);
    if (verdict === 'missing') return NO_DUMP;
    if (verdict !== 'complete') {
      this.host.log(`crash dump refused: ${verdict}`);
      return { code: 'bad_request', detail: verdict };
    }
    const dump = this.desk.dump(ticket);
    return { code: 'ok', stored: dump.bytes, bytes: dump.bytes, complete: true };
  }

  dump(ticket) {
    this.desk.purge(this.host.now());
    return this.desk.dumpBytes(ticket);
  }

  statuses(claims, address) {
    if (this.limited([['lookupAll', 'all'], ['statusAddress', address]])) return { code: 'limit' };
    this.desk.purge(this.host.now());
    return { code: 'ok', reports: this.desk.statuses(claims) };
  }

  read(claim, address) {
    if (this.limited([['lookupAll', 'all'], ['readAddress', address]])) return { code: 'limit' };
    this.desk.purge(this.host.now());
    const report = this.desk.report(claim);
    return report ? { code: 'ok', report } : { code: 'not_found' };
  }

  list() {
    return this.desk.list(this.host.now());
  }

  get(ticket) {
    this.desk.purge(this.host.now());
    const found = this.desk.get(ticket);
    return found ? { ...found, ...this.desk.status(ticket), ...this.desk.dumpSummary(ticket) } : null;
  }

  setStatus(ticket, change) {
    return this.desk.setStatus(ticket, change, this.host.now());
  }

  remove(ticket) {
    return this.desk.remove(ticket);
  }
}
