import assert from 'node:assert/strict';
import { test } from 'node:test';

import { handleAdmin, resetAdminLimits } from '../src/admin.js';
import { handlePublic } from '../src/api.js';
import { READERS, REPORT, readReport } from '../src/reports.js';
import { ENV, TOKEN, fixture, makeEnv, post, submit } from './harness.js';

test('region is one of the fields the service reads, with the canonical request carrying it', () => {
  assert.ok(Object.hasOwn(READERS, 'region'));
  assert.equal(fixture().region, 'Korea');
  assert.equal(readReport(fixture()).report.region, 'Korea');
});

test('region is optional: an empty text stores an empty region', () => {
  assert.equal(readReport({ ...fixture(), region: '' }).report.region, '');
});

test('region follows the metadata rules: length limit, no control characters, no padding, text only', () => {
  const bad = (region, code) => assert.equal(readReport({ ...fixture(), region }).code, code, JSON.stringify(region));
  assert.equal(readReport({ ...fixture(), region: 'r'.repeat(REPORT.metadataChars) }).code, undefined);
  assert.equal(readReport({ ...fixture(), region: '€'.repeat(REPORT.metadataChars) }).code, undefined);
  bad('r'.repeat(REPORT.metadataChars + 1), 'too_large');
  bad('€'.repeat(REPORT.metadataChars + 1), 'too_large');
  for (const control of ['\n', '\t', '\r', '\u0000', '\u0007', '\u007f', '\u0085']) bad(`E${control}U`, 'bad_request');
  bad(' EU', 'bad_request');
  bad('EU ', 'bad_request');
  for (const value of [7, null, true, ['EU'], { a: 1 }]) bad(value, 'bad_request');
  assert.equal(readReport({ ...fixture(), region: 'bad\u0007' }).detail, 'region');
});

test('a filed region is stored, read back by its owner, and shown in the admin list and report', async () => {
  const env = makeEnv();
  const filed = await submit(env, { region: 'HK' });
  assert.equal(filed.response.status, 201);
  assert.equal(env.service.get(filed.body.ticket).region, 'HK');
  const read = await (await handlePublic(post('/v1/reports/read', { ticket: filed.body.ticket, receipt: filed.body.receipt }), {}, env.desk)).json();
  assert.equal(read.report.region, 'HK');
  resetAdminLimits();
  const admin = (path) => handleAdmin(new Request(`https://reports.example${path}`, { headers: { Authorization: `Bearer ${TOKEN}` } }), ENV, env.desk);
  assert.equal((await (await admin('/admin/api/reports')).json()).reports[0].region, 'HK');
  assert.equal((await (await admin(`/admin/api/reports/${filed.body.ticket}`)).json()).region, 'HK');
});

test('a request with a bad region is refused by the API and nothing is stored', async () => {
  const env = makeEnv();
  for (const region of ['x'.repeat(65), 'a\u0007b', 'a\nb']) {
    const { response } = await submit(env, { region }, { ip: '198.51.100.1' });
    assert.ok([400, 413].includes(response.status), region);
  }
  assert.equal(env.rows('reports').length, 0);
  assert.equal((await submit(env, { region: '' }, { ip: '198.51.100.2' })).response.status, 201);
});

