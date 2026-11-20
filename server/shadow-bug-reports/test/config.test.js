import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

const ROOT = join(import.meta.dirname, '..');
const toml = readFileSync(join(ROOT, 'wrangler.toml'), 'utf8').replace(/\r\n/g, '\n');
const withoutComments = toml.split('\n').filter((line) => !line.trim().startsWith('#')).join('\n');
const SOURCES = readdirSync(join(ROOT, 'src')).map((name) => readFileSync(join(ROOT, 'src', name), 'utf8'));

const setting = (name) => new RegExp(`^${name}\\s*=\\s*"([^"]*)"`, 'm').exec(withoutComments)?.[1];

test('the Worker has its own name, entry point and Durable Object', () => {
  assert.equal(setting('name'), 'shadow-bug-reports');
  assert.equal(setting('main'), 'src/worker.js');
  assert.match(withoutComments, /^workers_dev\s*=\s*true$/m);
  assert.match(withoutComments, /\[\[durable_objects\.bindings\]\]\s+name = "REPORTS"\s+class_name = "BugReportDesk"/);
  assert.match(withoutComments, /\[\[migrations\]\]\s+tag = "v1"\s+new_sqlite_classes = \["BugReportDesk"\]/);
  assert.ok(!/deleted_classes|renamed_classes/.test(withoutComments));
});

test('the edge limit binding uses the namespace reserved for this Worker, 20 requests a minute', () => {
  assert.match(withoutComments, /\[\[ratelimits\]\]\s+name = "REPORT_LIMITER"/);
  const id = setting('namespace_id');
  assert.equal(id, '5006');
  assert.ok(!['1001', '1301', '2002', '2701', '3003', '3601', '4004', '5005'].includes(id), `${id} belongs to another Worker`);
  assert.match(withoutComments, /simple = { limit = 20, period = 60 }/);
  assert.ok(!/SET_UNIQUE|REPLACE|TODO|CHANGEME|PLACEHOLDER|YOUR_/.test(withoutComments));
});

test('the configuration names no other project, account or secret', () => {
  assert.ok(!/account_id|api[_-]?token|secret|\[vars\]|routes?\s*=|\bzone/i.test(withoutComments));
  assert.ok(!/13|dragon|dcp|tsr|lobby/i.test(withoutComments.replace(/[0-9]{4}/g, '')));
});

test('the only secret read is the Shadow admin token', () => {
  const names = new Set(SOURCES.flatMap((source) => [...source.matchAll(/\benv\.([A-Z][A-Z_]+)/g)].map((m) => m[1])));
  assert.deepEqual([...names].sort(), ['REPORTS', 'REPORT_LIMITER', 'SOTB_BUG_REPORT_ADMIN_TOKEN']);
});

test('the service answers to its own name', () => {
  const service = SOURCES.join('\n');
  assert.ok(service.includes("'shadow-bug-reports'"));
  assert.ok(!/13-sentinels|13S-|TSR_|dragon|DCP_/i.test(service));
});
