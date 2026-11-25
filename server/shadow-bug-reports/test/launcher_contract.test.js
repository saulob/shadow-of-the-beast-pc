// The report contract between the launcher (launcher/incident_report.cpp, launcher/incident_client.cpp) and this service,
// field by field, for every supported build and both platforms. A Worker that refuses a field the launcher sends must fail
// here before it is deployed.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

import { handleAdmin } from '../src/admin.js';
import { handlePublic } from '../src/api.js';
import { READERS, REPORT } from '../src/reports.js';
import { PROTOCOL } from '../src/service.js';
import { ENV, MARKER, TOKEN, fixture, makeEnv, post, report, submit } from './harness.js';

const LAUNCHER = join(import.meta.dirname, '..', '..', '..', 'launcher');
const source = (file) => readFileSync(join(LAUNCHER, file), 'utf8');

// Fields a launcher released before them may leave out; every other field is required.
const NEWER_FIELDS = ['session_end', 'session_detail', 'session_stage'];

function launcherFieldKeys() {
  const text = source('incident_report.cpp');
  const table = text.slice(text.indexOf('kFieldNames[] = {'), text.indexOf('};', text.indexOf('kFieldNames[] = {')));
  const constants = Object.fromEntries([...text.matchAll(/constexpr char (k\w+Key)\[\] = "(\w+)";/g)].map((m) => [m[1], m[2]]));
  return [...table.matchAll(/\{(?:"(\w+)"|(k\w+Key)), "/g)].map((m) => m[1] ?? constants[m[2]]);
}

function supportedBuilds() {
  const text = source('game_profile.cpp');
  return [...text.matchAll(/\.title_id = "(CUSA\d{5})",[\s\S]*?\.version = "([\d.]+)",[\s\S]*?\.label = "([^"]+)",\s*\.region = "([^"]+)"/g)]
    .map(([, cusa, version, label, region]) => ({ cusa, version, label, region }));
}

const send = (env, body, options) => handlePublic(post('/v1/reports', body, options), {}, env.desk);
const answer = async (response) => ({ status: response.status, body: await response.json() });

test('the launcher sends exactly the fields this service reads, under the protocol /health names', async () => {
  assert.deepEqual(launcherFieldKeys().sort(), Object.keys(READERS).sort());
  assert.ok(PROTOCOL >= 2, 'session fields, dumps and submission keys need protocol 2');
  const health = await (await handlePublic(new Request('https://reports.example/health'), {}, null)).json();
  assert.equal(health.protocol, PROTOCOL);
});

test('a report with every field the launcher sends, a submission key and session fields is accepted (the protocol 1 refusal)', async () => {
  const env = makeEnv();
  const { status, body } = await answer(await send(env, report({ submission: 'c'.repeat(64) })));
  assert.equal(status, 201);
  const stored = env.service.get(body.ticket);
  assert.equal(stored.session_end, fixture().session_end);
  assert.equal(stored.session_stage, fixture().session_stage);
});

test('every supported build is accepted with its own label, region, title id and version', async () => {
  const builds = supportedBuilds();
  assert.deepEqual(builds.map((b) => b.cusa).sort(), ['CUSA00951', 'CUSA02871', 'CUSA03762']);
  for (const build of builds) {
    const env = makeEnv();
    const filed = await submit(env, { game_build: build.label, region: build.region, cusa: build.cusa, game_version: build.version });
    assert.equal(filed.response.status, 201, build.label);
    assert.equal(env.service.get(filed.body.ticket).game_build, build.label);
  }
});

test('Windows and Linux reports differ only in the operating system and are both accepted', async () => {
  for (const os of ['Windows 11 (build 26200)', 'Windows 10 (build 19045)', 'Linux', 'Linux: Ubuntu 24.04.1 LTS', 'Linux: SteamOS']) {
    const env = makeEnv();
    const filed = await submit(env, { operating_system: os });
    assert.equal(filed.response.status, 201, os);
    assert.equal(env.service.get(filed.body.ticket).operating_system, os);
  }
});

test('the port version is any version-like name, not one release', async () => {
  for (const version of ['0.1.0', '0.2.0', '1.0.0', '1.0.0-rc.1', '10.20.30+build.7']) {
    const env = makeEnv();
    assert.equal((await submit(env, { launcher_version: version })).response.status, 201, version);
  }
  for (const version of ['', ' 0.1.0', 'v 0.1.0', '0.1.0\n', '.0.1.0', 'x'.repeat(33)]) {
    const env = makeEnv();
    const { status, body } = await answer(await send(env, report({ launcher_version: version })));
    assert.ok(status === 400 || status === 413, JSON.stringify(version));
    assert.equal(body.detail, 'launcher_version');
  }
});

test('only a JSON request is read', async () => {
  const env = makeEnv();
  for (const type of ['application/json', 'application/json; charset=utf-8', 'Application/JSON']) {
    assert.equal((await send(makeEnv(), report(), { headers: { 'Content-Type': type } })).status, 201, type);
  }
  const plain = await answer(await send(env, report(), { headers: { 'Content-Type': 'text/plain' } }));
  assert.deepEqual(plain, { status: 415, body: { error: 'unsupported_media_type', protocol: PROTOCOL } });
  const broken = await answer(await send(env, null, { raw: '{"description":' }));
  assert.deepEqual(broken, { status: 400, body: { error: 'bad_request', detail: 'invalid JSON', protocol: PROTOCOL } });
  assert.equal(env.rows('reports').length, 0);
});

test('a missing field is refused by name, except the fields older launchers do not send', async () => {
  for (const key of Object.keys(fixture())) {
    const env = makeEnv();
    const body = report();
    delete body[key];
    const result = await answer(await send(env, body));
    if (NEWER_FIELDS.includes(key)) {
      assert.equal(result.status, 201, key);
    } else {
      assert.deepEqual(result, { status: 400, body: { error: 'bad_request', detail: key, protocol: PROTOCOL } }, key);
      assert.equal(env.rows('reports').length, 0, key);
    }
  }
});

test('refusals name the field with a stable code and never echo what was sent', async () => {
  const cases = [
    [{ description: `${MARKER} ${'d'.repeat(REPORT.descriptionChars)}` }, 413, 'too_large', 'description'],
    [{ description: 'short' }, 400, 'bad_request', 'description too short'],
    [{ log_text: `${MARKER}\n${'l'.repeat(REPORT.logBytes)}` }, 413, 'too_large', 'log_text'],
    [{ log_text: ' \n ' }, 400, 'bad_request', 'empty log'],
    [{ operating_system: '' }, 400, 'bad_request', 'operating_system'],
    [{ operating_system: `Linux ${MARKER}\u0007` }, 400, 'bad_request', 'operating_system'],
    [{ game_build: '' }, 400, 'bad_request', 'game_build'],
    [{ cusa: `CUSA0${MARKER}` }, 400, 'bad_request', 'cusa'],
    [{ game_version: 'v1.00' }, 400, 'bad_request', 'game_version'],
    [{ session_end: MARKER }, 400, 'bad_request', 'session_end'],
    [{ submission: MARKER }, 400, 'bad_request', 'submission'],
    [{ platform: MARKER }, 400, 'unknown_field', 'platform'],
  ];
  for (const [extra, status, code, detail] of cases) {
    const env = makeEnv();
    const response = await send(env, report(extra));
    const text = await response.text();
    assert.equal(response.status, status, detail);
    assert.deepEqual(JSON.parse(text), { error: code, detail, protocol: PROTOCOL });
    assert.ok(!text.includes(MARKER), detail);
    assert.equal(env.rows('reports').length, 0, detail);
  }
});

test('a limit, a refusal and an unavailable store answer with different statuses and codes', async () => {
  const env = makeEnv();
  for (let i = 0; i < REPORT.perAddressPerHour; i++) assert.equal((await submit(env)).response.status, 201);
  const limited = await submit(env);
  assert.equal(limited.response.status, 429);
  assert.equal(limited.body.error, 'hourly_limit');
  assert.equal(limited.response.headers.get('Retry-After'), '60');
  const refused = await answer(await send(env, report({ description: 'short' })));
  assert.equal(refused.status, 400);
  const broken = { submit: async () => { throw new Error('storage exploded'); } };
  const down = await handlePublic(post('/v1/reports', report()), {}, broken);
  assert.equal(down.status, 502);
  assert.deepEqual(await down.json(), { error: 'unavailable', protocol: PROTOCOL });
});

test('an accepted report is stored once, listed on /admin and opens with its description and filtered log', async () => {
  const env = makeEnv();
  const key = 'd'.repeat(64);
  const first = await submit(env, { submission: key });
  const again = await submit(env, { submission: key });
  assert.equal(first.response.status, 201);
  assert.equal(again.response.status, 200);
  assert.equal(again.body.ticket, first.body.ticket);
  assert.equal(env.rows('reports').length, 1);

  const adminGet = (path) => handleAdmin(new Request(`https://reports.example${path}`, { headers: { Authorization: `Bearer ${TOKEN}` } }), ENV, env.desk, () => {});
  const list = await (await adminGet('/admin/api/reports')).json();
  assert.deepEqual(list.reports.map((row) => row.ticket), [first.body.ticket]);
  const opened = await (await adminGet(`/admin/api/reports/${first.body.ticket}`)).json();
  assert.equal(opened.description, report().description);
  assert.equal(opened.log_text, fixture().log_text);
  const anonymous = await handleAdmin(new Request('https://reports.example/admin/api/reports'), ENV, env.desk, () => {});
  assert.equal(anonymous.status, 401);
});
