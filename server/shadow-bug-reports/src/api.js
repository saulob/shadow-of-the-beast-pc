// The public report API, plain HTTPS and JSON. The launcher opens one short request when the player sends a report or
// asks about one; nothing else reaches the service. No Cloudflare import, so Node runs it in the tests.
import { ipGroup } from './limits.js';
import { REPORT, readClaims, readDumpChunk, readReadClaim, readReport } from './reports.js';
import { PROTOCOL, SERVICE, WORKER_BUILD } from './service.js';

export const API_HEADERS = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'",
};

const RETRY_AFTER_SECONDS = '60';
const MAX_LOOKUP_BODY_BYTES = 8 * 1024;
const MAX_CHUNK_BODY_BYTES = Math.ceil(REPORT.dumpChunkBytes / 3) * 4 + 1024;

const STATUS_OF_CODE = {
  ok: 200,
  bad_request: 400,
  not_found: 404,
  conflict: 409,
  too_large: 413,
  unsupported_media_type: 415,
  hourly_limit: 429,
  daily_limit: 429,
  limit: 429,
  origin_not_allowed: 403,
  unavailable: 502,
};

export function reply(body, status = 200, extra = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...API_HEADERS, 'Content-Type': 'application/json; charset=utf-8', ...extra },
  });
}

function failure(code, detail) {
  const status = STATUS_OF_CODE[code] ?? 500;
  return reply(detail === undefined ? { error: code } : { error: code, detail }, status,
    status === 429 ? { 'Retry-After': RETRY_AFTER_SECONDS } : {});
}

export const clientAddress = (request) => ipGroup(request.headers.get('CF-Connecting-IP') ?? 'local');

// The text of the body, or a refusal. A declared size is checked first and the stream is cut at the limit, so a client
// that lies about it or sends none cannot make the service hold more than the limit.
export async function readBounded(request, maxBytes) {
  const declared = request.headers.get('Content-Length');
  if (declared !== null && !(/^[0-9]{1,12}$/.test(declared) && Number(declared) <= maxBytes)) return { code: 'too_large' };
  if (!request.body) return { text: '' };
  const reader = request.body.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > maxBytes) {
      await reader.cancel();
      return { code: 'too_large' };
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let at = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, at);
    at += chunk.byteLength;
  }
  try {
    return { text: new TextDecoder('utf-8', { fatal: true }).decode(bytes) };
  } catch {
    return { code: 'bad_request', detail: 'encoding' };
  }
}

const isJson = (request) => /^application\/json(\s*;|$)/i.test(request.headers.get('Content-Type') ?? '');

// A dump arrives in several requests in a row, so its requests are counted apart from reports and lookups at the edge.
const ROUTES = {
  '/v1/reports': { maxBytes: REPORT.bodyBytes, kind: 'submit', edge: 'report' },
  '/v1/reports/status': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'status', edge: 'report' },
  '/v1/reports/read': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'read', edge: 'report' },
  '/v1/reports/dump': { maxBytes: MAX_LOOKUP_BODY_BYTES, kind: 'dump', edge: 'dump' },
  '/v1/reports/dump/chunk': { maxBytes: MAX_CHUNK_BODY_BYTES, kind: 'chunk', edge: 'dump' },
};

async function edgeAllows(limiter, key) {
  if (!limiter) return true;
  try {
    return (await limiter.limit({ key })).success !== false;
  } catch {
    return true;
  }
}

const dumpProgress = (answer) => ({ stored: answer.stored, bytes: answer.bytes, complete: answer.complete });

// desk: { submit(report, address, dump, submission), statuses(claims, address), read(claim, address),
// dumpStatus(claim, address), dumpChunk(claim, offset, bytes, address) } as promises.
export async function handlePublic(request, env, desk, log = () => {}) {
  const url = new URL(request.url);
  if (url.pathname === '/' || url.pathname === '/health') {
    if (request.method !== 'GET') return reply({ error: 'method_not_allowed' }, 405, { Allow: 'GET' });
    return reply({ service: SERVICE, protocol: PROTOCOL, status: 'ok', worker_build: WORKER_BUILD });
  }
  const route = ROUTES[url.pathname];
  if (!route) return failure('not_found');
  if (request.method !== 'POST') return reply({ error: 'method_not_allowed' }, 405, { Allow: 'POST' });
  if (request.headers.has('Origin')) return failure('origin_not_allowed');
  if (!isJson(request)) return failure('unsupported_media_type');
  const address = clientAddress(request);
  if (!(await edgeAllows(env.REPORT_LIMITER, `${route.edge}:${address}`))) return failure('limit');
  const body = await readBounded(request, route.maxBytes);
  if (body.code) return failure(body.code, body.detail);
  let parsed;
  try {
    parsed = JSON.parse(body.text);
  } catch {
    return failure('bad_request', 'invalid JSON');
  }
  try {
    if (route.kind === 'submit') {
      const { report, dump, submission, code, detail } = readReport(parsed);
      if (!report) return failure(code, detail);
      const filed = await desk.submit(report, address, dump, submission);
      if (filed.code !== 'ok') return failure(filed.code);
      return reply({
        ticket: filed.ticket, receipt: filed.receipt, submitted_at: filed.submitted_at,
        ...(filed.dump && { dump: 'expected', dump_chunk_bytes: REPORT.dumpChunkBytes }),
      }, filed.repeated ? 200 : 201);
    }
    if (route.kind === 'dump' || route.kind === 'chunk') {
      const chunk = route.kind === 'chunk' ? readDumpChunk(parsed) : null;
      const claim = chunk ? chunk.claim : readReadClaim(parsed);
      if (chunk?.code) return failure(chunk.code, chunk.detail);
      if (!claim) return failure('bad_request', 'receipt');
      const answer = chunk ? await desk.dumpChunk(claim, chunk.offset, chunk.bytes, address) : await desk.dumpStatus(claim, address);
      if (answer.code === 'ok') return reply(dumpProgress(answer));
      if (answer.code === 'conflict') return reply({ error: 'offset', ...dumpProgress(answer) }, 409);
      return failure(answer.code, answer.detail);
    }
    if (route.kind === 'status') {
      const claims = readClaims(parsed);
      if (!claims) return failure('bad_request', 'reports');
      const answer = await desk.statuses(claims, address);
      return answer.code === 'ok' ? reply({ reports: answer.reports }) : failure(answer.code);
    }
    const claim = readReadClaim(parsed);
    if (!claim) return failure('bad_request', 'receipt');
    const answer = await desk.read(claim, address);
    return answer.code === 'ok' ? reply({ report: answer.report }) : failure(answer.code);
  } catch (error) {
    log(`reports: desk unavailable: ${error?.message ?? error}`);
    return failure('unavailable');
  }
}
