// Crash dumps attached to a report: announced with the report, uploaded in chunks with the report's receipt, resumed from
// what the service holds, checked when the last byte arrives, downloaded from the dashboard, and gone with their report.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';

import { handleAdmin, resetAdminLimits } from '../src/admin.js';
import { handlePublic } from '../src/api.js';
import { REPORT, dumpDownloadName, readDumpChunk, readReport } from '../src/reports.js';
import { DAY, ENV, TOKEN, fixture, makeEnv, post, report, submit } from './harness.js';

const DUMP_NAME = 'shadow_of_the_beast_026.dmp';
const CHUNK = REPORT.dumpChunkBytes;

function minidump(size, seed = 7) {
  const bytes = new Uint8Array(size);
  let state = seed;
  for (let i = 0; i < size; i++) {
    state = (state * 1103515245 + 12345) >>> 0;
    bytes[i] = state >>> 24;
  }
  bytes.set([0x4d, 0x44, 0x4d, 0x50]);
  return bytes;
}

const sha = (bytes) => createHash('sha256').update(bytes).digest('hex');
const b64 = (bytes) => Buffer.from(bytes).toString('base64');
const announce = (bytes) => ({ dump_file: DUMP_NAME, dump_bytes: bytes.length, dump_sha256: sha(bytes) });
const claimOf = (filed) => ({ ticket: filed.body.ticket, receipt: filed.body.receipt });

async function call(env, path, body, options = {}) {
  const response = await handlePublic(post(path, body, options), {}, env.desk);
  return { status: response.status, body: await response.json() };
}

const status = (env, claim, options) => call(env, '/v1/reports/dump', claim, options);
const chunk = (env, claim, offset, bytes, options) =>
  call(env, '/v1/reports/dump/chunk', { ...claim, offset, data: b64(bytes) }, options);

async function uploadAll(env, claim, bytes, options) {
  let last;
  for (let at = 0; at < bytes.length; at += CHUNK) last = await chunk(env, claim, at, bytes.subarray(at, Math.min(at + CHUNK, bytes.length)), options);
  return last;
}

function admin(env, path, init = {}) {
  resetAdminLimits();
  const headers = { Authorization: `Bearer ${TOKEN}`, ...(init.headers ?? {}) };
  return handleAdmin(new Request(`https://reports.example${path}`, { ...init, headers }), ENV, env.desk, () => {});
}

test('a report announces its dump and the answer asks for it, with the chunk size', async () => {
  const env = makeEnv();
  const bytes = minidump(2 * CHUNK + 777);
  const filed = await submit(env, announce(bytes));
  assert.equal(filed.response.status, 201);
  assert.equal(filed.body.dump, 'expected');
  assert.equal(filed.body.dump_chunk_bytes, CHUNK);
  const [row] = env.rows('report_dumps');
  assert.equal(row.ticket, filed.body.ticket);
  assert.equal(row.file, DUMP_NAME);
  assert.equal(row.bytes, bytes.length);
  assert.equal(row.complete, 0);
  const plain = await submit(makeEnv());
  assert.equal(plain.body.dump, undefined);
});

test('the upload begins at zero, takes chunks in order, resumes where the service stopped and completes once', async () => {
  const env = makeEnv();
  const bytes = minidump(2 * CHUNK + 777);
  const filed = await submit(env, announce(bytes));
  const claim = claimOf(filed);
  assert.deepEqual((await status(env, claim)).body, { stored: 0, bytes: bytes.length, complete: false });

  const first = await chunk(env, claim, 0, bytes.subarray(0, CHUNK));
  assert.deepEqual([first.status, first.body], [200, { stored: CHUNK, bytes: bytes.length, complete: false }]);

  // The connection dropped: the launcher asks again and continues from the confirmed byte.
  const resumed = await status(env, claim);
  assert.deepEqual(resumed.body, { stored: CHUNK, bytes: bytes.length, complete: false });
  const second = await chunk(env, claim, resumed.body.stored, bytes.subarray(CHUNK, 2 * CHUNK));
  assert.equal(second.body.stored, 2 * CHUNK);
  const last = await chunk(env, claim, 2 * CHUNK, bytes.subarray(2 * CHUNK));
  assert.deepEqual([last.status, last.body], [200, { stored: bytes.length, bytes: bytes.length, complete: true }]);
  assert.deepEqual((await status(env, claim)).body, { stored: bytes.length, bytes: bytes.length, complete: true });

  assert.equal(env.rows('reports').length, 1);
  assert.equal(env.rows('report_quota').length, 1);
  assert.equal(env.rows('report_dumps')[0].complete, 1);
  assert.equal(env.service.get(filed.body.ticket).dump_status, 'stored');
});

test('a chunk already stored, a gap, an earlier offset and bytes past the announced size are refused', async () => {
  const env = makeEnv();
  const bytes = minidump(2 * CHUNK + 100);
  const claim = claimOf(await submit(env, announce(bytes)));
  await chunk(env, claim, 0, bytes.subarray(0, CHUNK));

  const replay = await chunk(env, claim, 0, bytes.subarray(0, CHUNK));
  assert.deepEqual([replay.status, replay.body], [409, { error: 'offset', stored: CHUNK, bytes: bytes.length, complete: false }]);
  const ahead = await chunk(env, claim, 2 * CHUNK, bytes.subarray(2 * CHUNK));
  assert.deepEqual([ahead.status, ahead.body.stored], [409, CHUNK]);
  const behind = await chunk(env, claim, 10, bytes.subarray(10, 20));
  assert.deepEqual([behind.status, behind.body.stored], [409, CHUNK]);
  assert.equal((await chunk(env, claim, CHUNK, bytes.subarray(CHUNK, 2 * CHUNK))).status, 200);
  const over = await chunk(env, claim, 2 * CHUNK, minidump(200));
  assert.deepEqual([over.status, over.body.detail], [400, 'past the announced size']);

  const stored = env.store.all('SELECT pos, LENGTH(data) AS n FROM report_dump_chunks ORDER BY pos');
  assert.deepEqual(stored, [{ pos: 0, n: CHUNK }, { pos: CHUNK, n: CHUNK }]);
  assert.equal(env.rows('report_dumps')[0].complete, 0);
});

test('bytes that are not a minidump or not the announced dump are refused and the announcement is dropped', async () => {
  const env = makeEnv();
  const bytes = minidump(CHUNK + 50);
  const claim = claimOf(await submit(env, announce(bytes)));
  const wrong = bytes.slice(0, CHUNK);
  wrong.set([0x4d, 0x5a, 0x90, 0x00]);
  const refused = await chunk(env, claim, 0, wrong);
  assert.deepEqual([refused.status, refused.body.detail], [400, 'not_minidump']);
  assert.equal(env.rows('report_dumps').length, 0);
  assert.equal(env.rows('report_dump_chunks').length, 0);
  assert.equal((await status(env, claim)).status, 400);

  const other = makeEnv();
  const announced = minidump(CHUNK + 50, 1);
  const sent = minidump(CHUNK + 50, 2);
  const otherClaim = claimOf(await submit(other, announce(announced)));
  const last = await uploadAll(other, otherClaim, sent);
  assert.deepEqual([last.status, last.body.detail], [400, 'dump_sha256']);
  assert.equal(other.rows('report_dumps').length, 0);
  assert.equal(other.rows('report_dump_chunks').length, 0);
  assert.equal(other.rows('reports').length, 1);
});

test('a dump whose first bytes changed in storage is refused when it completes', async () => {
  const env = makeEnv();
  const bytes = minidump(CHUNK + 50);
  const claim = claimOf(await submit(env, announce(bytes)));
  await chunk(env, claim, 0, bytes.subarray(0, CHUNK));
  const tampered = bytes.slice(0, CHUNK);
  tampered[0] = 0;
  env.store.run('UPDATE report_dump_chunks SET data = ? WHERE pos = 0', tampered);
  const last = await chunk(env, claim, CHUNK, bytes.subarray(CHUNK));
  assert.deepEqual([last.status, last.body.detail], [400, 'not_minidump']);
  assert.equal(env.rows('report_dumps').length, 0);
});

test('an upload that stops short stays unfinished, is never offered for download, and leaves after a day', async () => {
  const env = makeEnv();
  const bytes = minidump(CHUNK + 50);
  const filed = await submit(env, announce(bytes));
  await chunk(env, claimOf(filed), 0, bytes.subarray(0, CHUNK));
  assert.equal(env.service.get(filed.body.ticket).dump_status, 'uploading');
  assert.equal(env.service.dump(filed.body.ticket), null);
  assert.equal((await admin(env, `/admin/api/reports/${filed.body.ticket}/dump`)).status, 404);
  env.advance(DAY - 1000);
  env.service.list();
  assert.equal(env.rows('report_dump_chunks').length, 1);
  env.advance(2000);
  env.service.list();
  assert.equal(env.rows('report_dumps').length, 0);
  assert.equal(env.rows('report_dump_chunks').length, 0);
  assert.equal(env.rows('reports').length, 1);
});

test('a finished dump cannot be changed', async () => {
  const env = makeEnv();
  const bytes = minidump(CHUNK + 50);
  const claim = claimOf(await submit(env, announce(bytes)));
  await uploadAll(env, claim, bytes);
  const before = env.everything();
  const again = await chunk(env, claim, 0, minidump(CHUNK, 9));
  assert.deepEqual([again.status, again.body], [409, { error: 'offset', stored: bytes.length, bytes: bytes.length, complete: true }]);
  const tail = await chunk(env, claim, bytes.length - 10, minidump(10, 3));
  assert.equal(tail.status, 409);
  assert.equal(env.everything(), before);
});

test('only the owner of a report with an announced dump may upload, and the request shape is strict', async () => {
  const env = makeEnv();
  const bytes = minidump(CHUNK + 50);
  const filed = await submit(env, announce(bytes));
  const claim = claimOf(filed);
  const wrongReceipt = { ...claim, receipt: 'f'.repeat(64) };
  assert.equal((await status(env, wrongReceipt)).status, 404);
  assert.equal((await chunk(env, wrongReceipt, 0, bytes.subarray(0, 10))).status, 404);
  assert.equal((await status(env, { ticket: 'SOB-AAAAAA', receipt: claim.receipt })).status, 404);

  const plain = await submit(env, {}, { ip: '203.0.113.99' });
  const none = await status(env, claimOf(plain));
  assert.deepEqual([none.status, none.body.error], [400, 'bad_request']);
  assert.equal((await chunk(env, claimOf(plain), 0, bytes.subarray(0, 10))).status, 400);

  assert.equal(readDumpChunk({ ...claim, offset: 0, data: 'not base64!' }).code, 'bad_request');
  assert.equal(readDumpChunk({ ...claim, offset: 0, data: 'QUJD' + '=' }).code, 'bad_request');
  assert.equal(readDumpChunk({ ...claim, offset: -1, data: 'QUJD' }).code, 'bad_request');
  assert.equal(readDumpChunk({ ...claim, offset: 1.5, data: 'QUJD' }).code, 'bad_request');
  assert.equal(readDumpChunk({ ...claim, offset: 0, data: '' }).code, 'bad_request');
  assert.equal(readDumpChunk({ ...claim, offset: 0, data: 'QUJD', path: '../x' }).code, 'bad_request');
  assert.equal(readDumpChunk({ ...claim, offset: 0, data: b64(new Uint8Array(CHUNK + 3)) }).code, 'too_large');
  assert.equal(readDumpChunk({ ...claim, offset: 0, data: b64(new Uint8Array(CHUNK)) }).bytes.length, CHUNK);
  const raw = await handlePublic(post('/v1/reports/dump/chunk', null, { raw: '{"ticket":' }), {}, env.desk);
  assert.equal(raw.status, 400);
  const get = await handlePublic(new Request('https://reports.example/v1/reports/dump', { method: 'GET' }), {}, env.desk);
  assert.equal(get.status, 405);
});

test('the announcement names exactly the selected log\'s dump, within the size limits, with its SHA-256', () => {
  const bytes = minidump(4096);
  const ok = readReport({ ...fixture(), ...announce(bytes) });
  assert.deepEqual(ok.dump, { file: DUMP_NAME, bytes: 4096, sha256: sha(bytes) });
  const bad = (extra, code, detail) => {
    const result = readReport({ ...fixture(), ...announce(bytes), ...extra });
    assert.equal(result.code, code, JSON.stringify(extra));
    if (detail) assert.match(result.detail, detail);
  };
  bad({ dump_file: 'shadow_of_the_beast_027.dmp' }, 'bad_request', /dump_file/);
  bad({ dump_file: '..\\..\\Windows\\win.ini' }, 'bad_request', /dump_file/);
  bad({ dump_file: 'C:/Users/ana/shadow_of_the_beast_026.dmp' }, 'bad_request', /dump_file/);
  bad({ dump_bytes: 0 }, 'bad_request', /dump_bytes/);
  bad({ dump_bytes: REPORT.dumpMinBytes - 1 }, 'bad_request', /dump_bytes/);
  bad({ dump_bytes: '4096' }, 'bad_request', /dump_bytes/);
  bad({ dump_bytes: REPORT.dumpBytes + 1 }, 'too_large', /dump_bytes/);
  bad({ dump_sha256: 'A'.repeat(64) }, 'bad_request', /dump_sha256/);
  assert.equal(readReport({ ...fixture(), ...announce(bytes), dump_bytes: REPORT.dumpBytes }).dump.bytes, REPORT.dumpBytes);
  const partial = { ...fixture(), dump_file: DUMP_NAME };
  assert.equal(readReport(partial).code, 'bad_request');
  assert.equal(readReport({ ...fixture(), dump_path: '/etc/passwd' }).code, 'unknown_field');
});

test('a repeated request with the same submission key returns the report already filed', async () => {
  const env = makeEnv();
  const submission = 'a'.repeat(64);
  const bytes = minidump(CHUNK + 50);
  const first = await submit(env, { submission, ...announce(bytes) });
  assert.equal(first.response.status, 201);
  const again = await submit(env, { submission, ...announce(bytes) });
  assert.equal(again.response.status, 200);
  assert.deepEqual(again.body, first.body);
  assert.equal(env.rows('reports').length, 1);
  assert.equal(env.rows('report_quota').length, 1);
  assert.equal(env.rows('report_dumps').length, 1);

  // The receipt of the repeated answer is the report's: the dump uploads with it.
  assert.equal((await uploadAll(env, claimOf(again), bytes)).body.complete, true);

  const other = await submit(env, { submission: 'b'.repeat(64) });
  assert.notEqual(other.body.ticket, first.body.ticket);
  const without = await submit(env, {}, { ip: '203.0.113.50' });
  assert.notEqual(without.body.ticket, first.body.ticket);
  assert.equal(env.rows('reports').length, 3);

  const stored = env.everything();
  assert.ok(!stored.includes(submission) && !stored.includes(first.body.receipt));
  assert.deepEqual(env.store.all("SELECT name FROM pragma_table_info('report_submissions')").map((r) => r.name).sort(), ['key', 'ms', 'ticket']);
  assert.ok(env.rows('report_submissions').every((r) => /^[0-9a-f]{32}$/.test(r.key)));
  assert.equal(readReport({ ...fixture(), submission: 'xyz' }).code, 'bad_request');
});

test('a submission key is forgotten after a day and with its report', async () => {
  const env = makeEnv();
  const submission = 'c'.repeat(64);
  const first = await submit(env, { submission });
  env.advance(DAY + 1000);
  const later = await submit(env, { submission });
  assert.notEqual(later.body.ticket, first.body.ticket);

  const removed = makeEnv();
  const filed = await submit(removed, { submission });
  assert.equal((await admin(removed, `/admin/api/reports/${filed.body.ticket}`, { method: 'DELETE' })).status, 200);
  assert.equal(removed.rows('report_submissions').length, 0);
  const refiled = await submit(removed, { submission });
  assert.equal(refiled.response.status, 201);
  assert.equal(removed.rows('reports').length, 1);
});

test('the dashboard shows the dump and downloads exactly its bytes under a safe name, with the token only', async () => {
  const env = makeEnv();
  const bytes = minidump(2 * CHUNK + 3);
  const filed = await submit(env, announce(bytes));
  const ticket = filed.body.ticket;
  assert.equal((await admin(env, `/admin/api/reports/${ticket}/dump`)).status, 404);
  await uploadAll(env, claimOf(filed), bytes);

  const detail = await (await admin(env, `/admin/api/reports/${ticket}`)).json();
  assert.deepEqual([detail.dump_status, detail.dump_file, detail.dump_bytes], ['stored', DUMP_NAME, bytes.length]);
  const listed = await (await admin(env, '/admin/api/reports')).json();
  assert.equal(listed.reports[0].dump_status, 'stored');

  const download = await admin(env, `/admin/api/reports/${ticket}/dump`);
  assert.equal(download.status, 200);
  assert.equal(download.headers.get('Content-Type'), 'application/octet-stream');
  assert.equal(download.headers.get('Content-Disposition'), `attachment; filename="${ticket}-${DUMP_NAME}"`);
  assert.equal(download.headers.get('X-Content-Type-Options'), 'nosniff');
  const got = new Uint8Array(await download.arrayBuffer());
  assert.equal(sha(got), sha(bytes));
  assert.equal(got.length, bytes.length);

  resetAdminLimits();
  const anonymous = await handleAdmin(new Request(`https://reports.example/admin/api/reports/${ticket}/dump`), ENV, env.desk, () => {});
  assert.equal(anonymous.status, 401);
  assert.equal((await admin(env, `/admin/api/reports/${ticket}/dump`, { method: 'DELETE' })).status, 405);
  assert.equal((await admin(env, '/admin/api/reports/..%2F..%2Fetc/dump')).status, 400);
  assert.equal((await admin(env, `/admin/api/reports/${ticket}/dump/extra`)).status, 404);
  assert.equal((await admin(env, `/admin/api/reports/${ticket}/dump`, { headers: { Origin: 'https://evil.example' } })).status, 403);
  assert.equal((await admin(env, '/admin/api/reports/SOB-AAAAAA/dump')).status, 404);

  assert.equal(dumpDownloadName({ ticket, log_file: 'shadow_of_the_beast_026.log' }), `${ticket}-${DUMP_NAME}`);
  assert.equal(dumpDownloadName({ ticket, log_file: '../../x"; filename=evil.exe' }), `${ticket}.dmp`);
  assert.equal(dumpDownloadName({ ticket: 'SOB-"x', log_file: 'shadow_of_the_beast_026.log' }), 'shadow-of-the-beast-report.dmp');
});

test('deleting a report deletes its dump; an expired report takes its dump with it', async () => {
  const env = makeEnv();
  const bytes = minidump(CHUNK + 50);
  const filed = await submit(env, announce(bytes));
  await uploadAll(env, claimOf(filed), bytes);
  assert.equal((await admin(env, `/admin/api/reports/${filed.body.ticket}`, { method: 'DELETE' })).status, 200);
  assert.equal(env.rows('report_dumps').length, 0);
  assert.equal(env.rows('report_dump_chunks').length, 0);

  const aged = makeEnv();
  const old = await submit(aged, announce(bytes));
  await uploadAll(aged, claimOf(old), bytes);
  aged.advance(REPORT.keptDays * DAY + 1000);
  aged.service.list();
  assert.equal(aged.rows('reports').length, 0);
  assert.equal(aged.rows('report_dumps').length, 0);
  assert.equal(aged.rows('report_dump_chunks').length, 0);
});

test('only the newest dumps are kept', () => {
  const env = makeEnv();
  const desk = env.service.desk;
  const bytes = minidump(2048);
  const { report: stored } = readReport(report());
  for (let i = 0; i < REPORT.dumpsKept + 3; i++) {
    const ticket = `SOB-${'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'[i % 32]}${'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'[Math.floor(i / 32)]}AAAA`;
    desk.file(stored, ticket, `198.51.100.${i % 200}`, env.clock.now + i, { file: DUMP_NAME, bytes: bytes.length, sha256: sha(bytes) });
    desk.appendDump(ticket, 0, bytes);
  }
  desk.purge(env.clock.now + REPORT.dumpsKept + 3);
  assert.equal(env.rows('report_dumps').length, REPORT.dumpsKept);
  assert.equal(env.rows('report_dump_chunks').length, REPORT.dumpsKept);
  assert.ok(!env.rows('report_dumps').some((r) => r.ticket === 'SOB-AAAAAA'));
});

test('a report from a launcher without session fields is still taken; new fields are validated', async () => {
  const env = makeEnv();
  const older = { ...fixture() };
  delete older.session_end;
  delete older.session_detail;
  delete older.session_stage;
  const { report: read } = readReport(older);
  assert.deepEqual([read.session_end, read.session_detail, read.session_stage], ['unknown', '', '']);
  const response = await handlePublic(post('/v1/reports', older), {}, env.desk);
  assert.equal(response.status, 201);
  const { ticket } = await response.json();
  assert.equal(env.service.get(ticket).session_end, 'unknown');

  const filed = await submit(env, { session_end: 'device_lost', session_detail: 'GPU device lost: vkQueueSubmit (submit) result -4, tick 9', session_stage: 'first frame presented' }, { ip: '203.0.113.77' });
  const stored = env.service.get(filed.body.ticket);
  assert.deepEqual([stored.session_end, stored.session_stage], ['device_lost', 'first frame presented']);
  assert.equal(readReport({ ...fixture(), session_end: 'exploded' }).code, 'bad_request');
  assert.equal(readReport({ ...fixture(), session_detail: 'x'.repeat(REPORT.sessionDetailChars + 1) }).code, 'too_large');
  assert.equal(readReport({ ...fixture(), session_detail: 'line\nbreak' }).code, 'bad_request');
  assert.equal(readReport({ ...fixture(), session_stage: 's'.repeat(65) }).code, 'too_large');
});

test('dump requests are limited apart from reports, at the edge and in the service', async () => {
  const keys = [];
  const limiter = { limit: async ({ key }) => { keys.push(key); return { success: true }; } };
  const env = makeEnv();
  const bytes = minidump(CHUNK + 50);
  const filed = await submit(env, announce(bytes), {}, { REPORT_LIMITER: limiter });
  await handlePublic(post('/v1/reports/dump', claimOf(filed)), { REPORT_LIMITER: limiter }, env.desk);
  await handlePublic(post('/v1/reports/dump/chunk', { ...claimOf(filed), offset: 0, data: b64(bytes.subarray(0, 8)) }), { REPORT_LIMITER: limiter }, env.desk);
  assert.deepEqual(keys, ['report:203.0.113.7', 'dump:203.0.113.7', 'dump:203.0.113.7']);

  const busy = makeEnv();
  const claim = claimOf(await submit(busy, announce(bytes)));
  let last;
  for (let i = 0; i < 40; i++) last = await status(busy, claim);
  assert.deepEqual([last.status, last.body.error], [429, 'limit']);
});

// SOTB_CPP_REQUEST names the launcher's request; the launcher's upload test leaves its dump request and chunks beside it.
const requestFile = process.env.SOTB_CPP_REQUEST;
const skip = requestFile ? false : 'set SOTB_CPP_REQUEST to a request file the launcher built';

test('the dump the launcher announces and uploads is accepted and downloads as the original file', { skip }, async () => {
  const folder = dirname(requestFile);
  const requestPath = join(folder, 'bug-report-dump-request.json');
  const uploadPath = join(folder, 'bug-report-dump-upload.json');
  assert.ok(existsSync(requestPath) && existsSync(uploadPath), 'the crash-dump-upload test must run first');
  const sent = JSON.parse(readFileSync(requestPath, 'utf8'));
  const upload = JSON.parse(readFileSync(uploadPath, 'utf8'));
  assert.equal(sent.dump_sha256, upload.dump_sha256);
  assert.equal(sent.dump_bytes, upload.dump_bytes);
  const env = makeEnv();
  const response = await handlePublic(post('/v1/reports', sent), {}, env.desk);
  assert.equal(response.status, 201);
  const filed = { body: await response.json() };
  assert.equal(filed.body.dump, 'expected');
  const claim = claimOf(filed);
  let last;
  for (const body of upload.chunks) {
    assert.deepEqual(Object.keys(body).sort(), ['data', 'offset']);
    last = await call(env, '/v1/reports/dump/chunk', { ...claim, ...body });
    assert.equal(last.status, 200);
  }
  assert.equal(last.body.complete, true);
  const download = await admin(env, `/admin/api/reports/${filed.body.ticket}/dump`);
  const got = new Uint8Array(await download.arrayBuffer());
  assert.equal(got.length, upload.dump_bytes);
  assert.equal(sha(got), upload.dump_sha256);
  const stored = env.service.get(filed.body.ticket);
  assert.equal(stored.session_end, sent.session_end);
  assert.equal(stored.dump_file, sent.dump_file);
});
