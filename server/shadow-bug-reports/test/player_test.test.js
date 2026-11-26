import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';

import { handleAdmin, resetAdminLimits } from '../src/admin.js';
import { handlePublic } from '../src/api.js';
import { REPORT_STATUSES, readFeedback, readStatusChange } from '../src/reports.js';
import { PROTOCOL, ReportService } from '../src/service.js';
import { sqliteStore } from '../local/host.js';
import { sha256Hex } from '../src/sha256.js';
import { ENV, TOKEN, makeEnv, post, submit } from './harness.js';
import { dashboardFor, settle } from './dom.js';

const claim = (filed) => ({ ticket: filed.body.ticket, receipt: filed.body.receipt });
const lookup = async (env, path, body, options) => {
  const response = await handlePublic(post(path, body, options), {}, env.desk);
  return { status: response.status, body: await response.json() };
};
const statusOf = async (env, filed) => (await lookup(env, '/v1/reports/status', { reports: [claim(filed)] })).body.reports[0];
const answer = (env, filed, revision, result, extra = {}, options) =>
  lookup(env, '/v1/reports/feedback', { ...claim(filed), revision, result, launcher_version: '0.1.0', game_build: 'CUSA03762 1.00 Korea', ...extra }, options);
const save = async (env, ticket, body) => {
  resetAdminLimits();
  const response = await handleAdmin(new Request(`https://reports.example/admin/api/reports/${ticket}/status`, {
    method: 'POST', headers: { Authorization: `Bearer ${TOKEN}` }, body: JSON.stringify(body),
  }), ENV, env.desk, () => {});
  return { status: response.status, body: await response.json() };
};
const adminReport = async (env, ticket) => {
  resetAdminLimits();
  const response = await handleAdmin(new Request(`https://reports.example/admin/api/reports/${ticket}`, {
    headers: { Authorization: `Bearer ${TOKEN}` } }), ENV, env.desk, () => {});
  return response.json();
};
const requestTest = (env, filed, instructions = 'Update to 0.1.1 and play the first level again.') =>
  save(env, filed.body.ticket, { status: 'awaiting_player_test', fixed_version: '', player_message: instructions });

test('every report status has a stable wire value and is accepted by the admin', async () => {
  assert.deepEqual(REPORT_STATUSES, ['new', 'investigating', 'awaiting_player_test', 'fixed', 'cannot_reproduce', 'duplicate', 'not_a_bug', 'closed']);
  const env = makeEnv();
  const filed = await submit(env);
  for (const status of REPORT_STATUSES) {
    env.advance(1000);
    const body = { status, player_message: status === 'awaiting_player_test' ? 'Try again with 0.1.1.' : '' };
    const saved = await save(env, filed.body.ticket, body);
    assert.equal(saved.status, 200, status);
    assert.equal((await statusOf(env, filed)).status, status);
  }
});

test('a fixed version belongs to Fixed only, and a player test needs its instructions', () => {
  assert.deepEqual(readStatusChange({ status: 'fixed', fixed_version: '0.1.1' }).change, { status: 'fixed', fixed_version: '0.1.1' });
  assert.deepEqual(readStatusChange({ status: 'fixed' }).change, { status: 'fixed' });
  assert.equal(readStatusChange({ status: 'fixed', fixed_version: 'not a version' }).detail, 'fixed_version');
  for (const status of ['new', 'investigating', 'closed', 'duplicate']) {
    assert.equal(readStatusChange({ status, fixed_version: '0.1.1' }).detail, 'fixed_version', status);
    assert.equal(readStatusChange({ status, fixed_version: '' }).change.fixed_version, '');
  }
  assert.equal(readStatusChange({ status: 'awaiting_player_test' }).detail, 'test_instructions');
  assert.equal(readStatusChange({ status: 'awaiting_player_test', player_message: '   ' }).detail, 'test_instructions');
  assert.equal(readStatusChange({ status: 'awaiting_player_test', player_message: ' Try it. ' }).change.player_message, 'Try it.');
});

test('a developer message is kept unless the admin clears it, and only a new message moves its revision', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  env.advance(1000);
  await save(env, filed.body.ticket, { status: 'investigating', player_message: 'Looking at it.' });
  const replied = await statusOf(env, filed);
  assert.equal(replied.player_message, 'Looking at it.');
  assert.equal(replied.message_updated_at, env.clock.now);
  env.advance(1000);
  await save(env, filed.body.ticket, { status: 'cannot_reproduce' });
  const statusOnly = await statusOf(env, filed);
  assert.equal(statusOnly.player_message, 'Looking at it.');
  assert.equal(statusOnly.message_updated_at, replied.message_updated_at);
  assert.ok(statusOnly.status_updated_at > replied.status_updated_at);
  env.advance(1000);
  await save(env, filed.body.ticket, { status: 'cannot_reproduce', player_message: '' });
  const cleared = await statusOf(env, filed);
  assert.equal(cleared.player_message, undefined);
  assert.equal(cleared.message_updated_at, undefined);
});

test('an unread reply is a message revision: two saves of the same message in one moment are still two revisions', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  const first = env.service.setStatus(filed.body.ticket, { status: 'investigating', player_message: 'A' });
  const second = env.service.setStatus(filed.body.ticket, { status: 'investigating', player_message: 'B' });
  const third = env.service.setStatus(filed.body.ticket, { status: 'investigating', player_message: 'A' });
  assert.ok(first.message_updated_at < second.message_updated_at && second.message_updated_at < third.message_updated_at);
  assert.ok(first.status_updated_at < second.status_updated_at && second.status_updated_at < third.status_updated_at);
});

test('Awaiting player test asks the owner, and each answer is tied to the request it answers', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  env.advance(1000);
  const request = (await requestTest(env, filed)).body;
  assert.equal(request.player_feedback, 'pending');
  const asked = await statusOf(env, filed);
  assert.equal(asked.status, 'awaiting_player_test');
  assert.equal(asked.test_feedback, 'pending');
  assert.equal(asked.player_message, 'Update to 0.1.1 and play the first level again.');
  assert.equal(asked.test_feedback_at, undefined);

  env.advance(1000);
  const given = await answer(env, filed, asked.status_updated_at, 'fixed');
  assert.equal(given.status, 200);
  assert.deepEqual(given.body, { ticket: filed.body.ticket, revision: asked.status_updated_at, test_feedback: 'fixed', test_feedback_at: env.clock.now });
  const after = await statusOf(env, filed);
  assert.equal(after.status, 'awaiting_player_test');
  assert.equal(after.test_feedback, 'fixed');
  assert.equal(after.status_updated_at, asked.status_updated_at);

  const stored = await adminReport(env, filed.body.ticket);
  assert.equal(stored.status, 'awaiting_player_test');
  assert.equal(stored.player_feedback, 'fixed');
  assert.equal(stored.last_feedback, 'fixed');
  assert.equal(stored.last_feedback_at, env.clock.now);
  assert.equal(stored.last_feedback_revision, asked.status_updated_at);
  assert.equal(stored.last_feedback_launcher_version, '0.1.0');
  assert.equal(stored.last_feedback_game_build, 'CUSA03762 1.00 Korea');
});

test('the first answer is final, a repeat is safe, and old or unknown requests are refused with stable codes', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  env.advance(1000);
  await requestTest(env, filed);
  const revision = (await statusOf(env, filed)).status_updated_at;
  assert.equal((await answer(env, filed, revision, 'not_fixed')).status, 200);
  const repeat = await answer(env, filed, revision, 'not_fixed');
  assert.equal(repeat.status, 200);
  assert.equal(repeat.body.test_feedback, 'not_fixed');
  const changed = await answer(env, filed, revision, 'fixed');
  assert.equal(changed.status, 409);
  assert.deepEqual(changed.body, { error: 'already_answered', protocol: PROTOCOL });
  const stale = await answer(env, filed, revision - 1, 'fixed');
  assert.equal(stale.status, 409);
  assert.equal(stale.body.error, 'stale_request');
  const wrong = await answer(env, filed, revision, 'fixed', { receipt: 'e'.repeat(64) });
  assert.deepEqual([wrong.status, wrong.body.error], [404, 'not_found']);
  const unknown = await answer(env, filed, revision, 'fixed', { ticket: 'SOB-ZZZZZZ' });
  assert.deepEqual([unknown.status, unknown.body.error], [404, 'not_found']);
  assert.equal((await statusOf(env, filed)).test_feedback, 'not_fixed');
});

test('a new test request reads as pending, and a delayed answer to the old one cannot reach it', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  await requestTest(env, filed, 'Try 0.1.1.');
  const first = (await statusOf(env, filed)).status_updated_at;
  await answer(env, filed, first, 'not_fixed');
  await requestTest(env, filed, 'Try 0.1.2.');
  const second = await statusOf(env, filed);
  assert.ok(second.status_updated_at > first, 'a request in the same millisecond still gets its own revision');
  assert.equal(second.test_feedback, 'pending');
  const late = await answer(env, filed, first, 'fixed');
  assert.equal(late.body.error, 'stale_request');
  assert.equal((await statusOf(env, filed)).test_feedback, 'pending');
  const stored = await adminReport(env, filed.body.ticket);
  assert.equal(stored.player_feedback, 'pending');
  assert.equal(stored.last_feedback, 'not_fixed');
  assert.equal(stored.last_feedback_revision, first);
  assert.equal((await answer(env, filed, second.status_updated_at, 'fixed')).status, 200);
});

test('an answer never sets the status, and once the request is closed it is stale', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  await requestTest(env, filed);
  const revision = (await statusOf(env, filed)).status_updated_at;
  await answer(env, filed, revision, 'fixed');
  assert.equal((await statusOf(env, filed)).status, 'awaiting_player_test');
  env.advance(1000);
  await save(env, filed.body.ticket, { status: 'fixed', fixed_version: '0.1.1' });
  const fixed = await statusOf(env, filed);
  assert.equal(fixed.status, 'fixed');
  assert.equal(fixed.test_feedback, undefined);
  assert.equal(fixed.player_message, 'Update to 0.1.1 and play the first level again.');
  assert.equal((await answer(env, filed, revision, 'not_fixed')).body.error, 'stale_request');
  assert.equal((await answer(env, filed, fixed.status_updated_at, 'not_fixed')).body.error, 'stale_request');
  const stored = await adminReport(env, filed.body.ticket);
  assert.equal(stored.player_feedback, '');
  assert.equal(stored.last_feedback, 'fixed');
});

test('malformed answers are refused before the store is asked, and none carries free text', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  const base = { ...claim(filed), revision: 5, result: 'fixed' };
  assert.equal(readFeedback(base).feedback.result, 'fixed');
  for (const [body, detail] of [
    [{ ...base, comment: 'it works' }, 'comment'], [{ ...base, receipt: undefined }, 'receipt'], [{ ...base, receipt: 'short' }, 'receipt'],
    [{ ...base, revision: 0 }, 'revision'], [{ ...base, revision: '5' }, 'revision'], [{ ...base, result: 'maybe' }, 'result'],
    [{ ...base, result: 'pending' }, 'result'], [{ ...base, launcher_version: 'not a version!' }, 'launcher_version'],
    [{ ...base, game_build: 'a\nb' }, 'game_build'], [[], 'body'],
  ]) {
    assert.equal(readFeedback(JSON.parse(JSON.stringify(body))).detail, detail, JSON.stringify(body).slice(0, 60));
  }
  const refused = await lookup(env, '/v1/reports/feedback', { ...base, result: 'maybe' });
  assert.deepEqual(refused.body, { error: 'bad_request', detail: 'result', protocol: PROTOCOL });
  const missing = await lookup(env, '/v1/reports/feedback', { ticket: filed.body.ticket, revision: 5, result: 'fixed' });
  assert.deepEqual([missing.status, missing.body.detail], [400, 'receipt']);
});

test('answers have their own limit, never use the report quota, and log no receipt', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  await requestTest(env, filed);
  const revision = (await statusOf(env, filed)).status_updated_at;
  const codes = [];
  for (let i = 0; i < 12; i++) codes.push((await answer(env, filed, revision, 'fixed')).status);
  assert.deepEqual(codes.slice(0, 10), Array(10).fill(200));
  assert.equal(codes[10], 429);
  const quota = env.rows('report_quota').length;
  assert.equal(quota, 1);
  assert.equal((await submit(env, {}, { ip: '203.0.113.99' })).response.status, 201);
  assert.ok(env.logs.some((line) => line === 'player test answer: ok'));
  for (const line of env.logs) assert.ok(!line.includes(filed.body.receipt) && !line.includes(filed.body.ticket));
});

test('deleting a report removes its answers, and later answers or lookups find nothing', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  await requestTest(env, filed);
  const revision = (await statusOf(env, filed)).status_updated_at;
  await answer(env, filed, revision, 'fixed');
  assert.equal(env.service.remove(filed.body.ticket), true);
  assert.equal(env.rows('report_tracking').length, 0);
  assert.equal((await answer(env, filed, revision, 'fixed')).body.error, 'not_found');
  assert.deepEqual((await lookup(env, '/v1/reports/status', { reports: [claim(filed)] })).body.reports, []);
});

test('a database written by the previous Worker opens without a reset and its reports read with safe defaults', async () => {
  const store = sqliteStore();
  store.run('CREATE TABLE meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)');
  store.run("INSERT INTO meta (k, v) VALUES ('salt', 'old-salt')");
  store.run(`CREATE TABLE reports (ticket TEXT PRIMARY KEY, ms INTEGER NOT NULL, launcher_version TEXT NOT NULL, game_build TEXT NOT NULL,
    region TEXT NOT NULL, game_version TEXT NOT NULL, cusa TEXT NOT NULL, eboot_sha256 TEXT NOT NULL, operating_system TEXT NOT NULL,
    cpu TEXT NOT NULL, renderer_selected TEXT NOT NULL, renderer TEXT NOT NULL, gpu TEXT NOT NULL, driver TEXT NOT NULL,
    render_resolution TEXT NOT NULL, description TEXT NOT NULL, log_file TEXT NOT NULL, log_clipped TEXT NOT NULL, log_text TEXT NOT NULL,
    session_end TEXT NOT NULL DEFAULT '', session_detail TEXT NOT NULL DEFAULT '', session_stage TEXT NOT NULL DEFAULT '')`);
  store.run(`CREATE TABLE report_tracking (ticket TEXT PRIMARY KEY, receipt_hash TEXT NOT NULL, submitted_ms INTEGER NOT NULL,
    status TEXT NOT NULL, status_ms INTEGER NOT NULL, fixed_version TEXT NOT NULL, player_message TEXT NOT NULL)`);
  const now = 1_700_000_000_000;
  store.run(`INSERT INTO reports VALUES ('SOB-AAAAAA', ?, '0.1.0', 'CUSA03762 1.00 Korea', 'Korea', '1.00', 'CUSA03762', '', 'Windows 11',
    '', 'auto', 'Vulkan', 'GPU', '', '', 'An old report from before player tests', 'shadow_of_the_beast_001.log', 'no', 'log', 'crash', '', '')`, now);
  const receipt = 'a'.repeat(64);
  store.run(`INSERT INTO report_tracking VALUES ('SOB-AAAAAA', ?, ?, 'investigating', ?, '', 'Looking at it.')`,
    sha256Hex(`old-salt|receipt|${receipt}`), now, now);
  const service = new ReportService({ sql: store, now: () => now + 1000, randomBytes: (n) => new Uint8Array(n).fill(7), log: () => {} });
  const [old] = service.statuses([{ ticket: 'SOB-AAAAAA', receipt }], '203.0.113.1').reports;
  assert.deepEqual(old, { ticket: 'SOB-AAAAAA', status: 'investigating', status_updated_at: now, player_message: 'Looking at it.' });
  const listed = service.list().reports[0];
  assert.equal(listed.player_feedback, '');
  assert.equal(listed.dump_status, 'none');
  const shown = service.get('SOB-AAAAAA');
  assert.equal(shown.message_updated_at, 0);
  assert.equal(shown.last_feedback, undefined);
  assert.equal(shown.dump_status, 'none');
  const saved = service.setStatus('SOB-AAAAAA', { status: 'awaiting_player_test', fixed_version: '', player_message: 'Try 0.1.1.' });
  assert.equal(saved.player_feedback, 'pending');
  const answered = service.feedback({ claim: { ticket: 'SOB-AAAAAA', receipt }, revision: saved.status_updated_at, result: 'fixed',
    launcher_version: '0.1.0', game_build: '' }, '203.0.113.1');
  assert.equal(answered.state.test_feedback, 'fixed');
  new ReportService({ sql: store, now: () => now, randomBytes: (n) => new Uint8Array(n), log: () => {} });
  assert.equal(service.get('SOB-AAAAAA').last_feedback, 'fixed');
});

// SOTB_CPP_REQUEST names the launcher's report request; its feedback test leaves the answer it sends beside it.
const requestFile = process.env.SOTB_CPP_REQUEST;
test('the answer the launcher builds is accepted for the open request', { skip: requestFile ? false : 'set SOTB_CPP_REQUEST to a request file the launcher built' }, async () => {
  const launcher = JSON.parse(readFileSync(join(dirname(requestFile), 'bug-report-feedback-request.json'), 'utf8'));
  assert.deepEqual(Object.keys(launcher).sort(), ['game_build', 'launcher_version', 'receipt', 'result', 'revision', 'ticket']);
  const env = makeEnv();
  const filed = await submit(env);
  await requestTest(env, filed);
  const revision = (await statusOf(env, filed)).status_updated_at;
  const sent = await lookup(env, '/v1/reports/feedback', { ...launcher, ...claim(filed), revision });
  assert.equal(sent.status, 200);
  assert.equal(sent.body.test_feedback, launcher.result);
  const stored = await adminReport(env, filed.body.ticket);
  assert.equal(stored.last_feedback_launcher_version, launcher.launcher_version);
  assert.equal(stored.last_feedback_game_build, launcher.game_build);
});

test('the dashboard shows player feedback beside the status, filters by it and never changes a report while filtering', async () => {
  const env = makeEnv();
  const pending = await submit(env, { description: 'Waiting for the player to answer' }, { ip: '203.0.113.1' });
  env.advance(1000);
  const fixed = await submit(env, { description: 'The player confirmed the fix' }, { ip: '203.0.113.2' });
  env.advance(1000);
  const broken = await submit(env, { description: 'The player says it still happens' }, { ip: '203.0.113.3' });
  env.advance(1000);
  await submit(env, { description: 'A report nobody touched yet' }, { ip: '203.0.113.4' });
  for (const filed of [pending, fixed, broken]) await requestTest(env, filed);
  await answer(env, fixed, (await statusOf(env, fixed)).status_updated_at, 'fixed');
  await answer(env, broken, (await statusOf(env, broken)).status_updated_at, 'not_fixed');
  const before = env.everything();
  const page = await dashboardFor(env);
  const feedbackOf = (ticket) => page.rows('list').find((row) => row[0] === ticket)?.[2];
  assert.equal(feedbackOf(pending.body.ticket), 'Awaiting response');
  assert.equal(feedbackOf(fixed.body.ticket), 'Player confirmed fixed');
  assert.equal(feedbackOf(broken.body.ticket), 'Still happening');
  const badges = page.nodes.get('list').find((n) => n.tag === 'span' && /^badge /.test(n.className)).map((n) => n.className);
  assert.deepEqual(new Set(badges), new Set(['badge pending', 'badge fixed', 'badge not_fixed']));
  assert.equal(page.text('c-new-status'), '1');
  assert.equal(page.text('c-awaiting_player_test'), '3');
  for (const [value, tickets] of [['pending', [pending]], ['fixed', [fixed]], ['not_fixed', [broken]], ['', [pending, fixed, broken]]]) {
    page.nodes.get('filter-feedback').value = value;
    page.nodes.get('filter-feedback').fire('change');
    const shown = page.rows('list').slice(1).map((row) => row[0]);
    for (const filed of tickets) assert.ok(shown.includes(filed.body.ticket), value);
    assert.equal(shown.length, value ? 1 : 4);
  }
  assert.equal(env.everything(), before);
  assert.equal(page.requests.filter((r) => r.init.method === 'POST').length, 0);
});

test('the dashboard shows the answer with the versions used, and asks for instructions before a test request', async () => {
  const env = makeEnv();
  const filed = await submit(env);
  await requestTest(env, filed);
  await answer(env, filed, (await statusOf(env, filed)).status_updated_at, 'not_fixed', { launcher_version: '0.1.1', game_build: 'CUSA02871 1.00 USA' });
  const page = await dashboardFor(env);
  page.nodes.get('list').find((n) => n.tag === 'button')[0].click();
  await settle();
  assert.equal(page.nodes.get('feedback').hidden, false);
  assert.equal(page.text('feedback-state'), 'Still happening');
  const facts = page.text('feedback-facts');
  assert.ok(facts.includes('AnswerStill happening') && facts.includes('Launcher version used0.1.1') && facts.includes('Game build usedCUSA02871 1.00 USA'));
  assert.equal(page.text('message-label'), 'What the player should test');

  page.nodes.get('message').value = '  ';
  page.nodes.get('status-save').click();
  await settle();
  assert.equal(page.text('status-note'), 'Write what the player should test first.');
  assert.equal(page.requests.filter((r) => r.init.method === 'POST').length, 0);

  page.nodes.get('message').value = 'Try again with 0.1.2.';
  page.nodes.get('status-save').click();
  await settle();
  assert.equal(page.text('status-note'), 'Saved.');
  assert.equal(page.text('feedback-state'), 'Awaiting response');
  assert.ok(page.text('feedback-facts').includes('Earlier answerStill happening'));
  assert.equal(page.rows('list')[1][2], 'Awaiting response');
  assert.equal(page.requests.filter((r) => r.init.method === 'POST').length, 1);
});
