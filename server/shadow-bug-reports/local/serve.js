// Development server for the Worker's handlers on 127.0.0.1 only: try /admin and the API without Cloudflare. It needs
// SOTB_BUG_REPORT_ADMIN_TOKEN in the environment (24 or more characters); SOTB_LOCAL_DB names a SQLite file, in memory
// otherwise. The launcher only talks HTTPS, so it cannot reach this server.
import http from 'node:http';
import { Readable } from 'node:stream';

import { handleAdmin, isAdminPath } from '../src/admin.js';
import { handlePublic } from '../src/api.js';
import { localService } from './host.js';

const port = Number(process.env.PORT ?? 8787);
const log = (text) => console.log(text);
const { service } = localService({ db: process.env.SOTB_LOCAL_DB ?? ':memory:', log });
const desk = {
  submit: async (report, address, dump, submission) => service.submit(report, address, dump, submission),
  statuses: async (claims, address) => service.statuses(claims, address),
  read: async (claim, address) => service.read(claim, address),
  dumpStatus: async (claim, address) => service.dumpStatus(claim, address),
  dumpChunk: async (claim, offset, bytes, address) => service.dumpChunk(claim, offset, bytes, address),
  dump: async (ticket) => service.dump(ticket),
  list: async () => service.list(),
  get: async (ticket) => service.get(ticket),
  setStatus: async (ticket, change) => service.setStatus(ticket, change),
  remove: async (ticket) => service.remove(ticket),
};
const env = { SOTB_BUG_REPORT_ADMIN_TOKEN: process.env.SOTB_BUG_REPORT_ADMIN_TOKEN };

const server = http.createServer(async (incoming, outgoing) => {
  const hasBody = incoming.method !== 'GET' && incoming.method !== 'HEAD';
  const request = new Request(`http://127.0.0.1:${port}${incoming.url}`, {
    method: incoming.method,
    headers: incoming.headers,
    body: hasBody ? Readable.toWeb(incoming) : undefined,
    duplex: 'half',
  });
  const url = new URL(request.url);
  const response = isAdminPath(url.pathname)
    ? await handleAdmin(request, env, desk, log)
    : await handlePublic(request, env, desk, log);
  outgoing.writeHead(response.status, Object.fromEntries(response.headers));
  outgoing.end(Buffer.from(await response.arrayBuffer()));
});

server.listen(port, '127.0.0.1', () => log(`shadow-bug-reports (local): http://127.0.0.1:${port}/admin`));
