# Shadow of the Beast PC port: bug report service

A Cloudflare Worker with one SQLite-backed Durable Object. The launcher's Bug Report page files reports here; only the
developer reads them, in the private dashboard at `/admin`. It is a service of its own: its own Worker, Durable Object,
database and secret, shared with no other project, and it keeps no player identities.

The production Worker is `https://shadow-bug-reports.saulobenigno.workers.dev` (dashboard at `/admin`), and it is the launcher's
default address. The steps under "Deploy" are manual and
are not run by any script, test or build in this repository.

## Architecture

| File | What |
|---|---|
| `src/worker.js` | the Worker, and the Durable Object class `BugReportDesk` that holds the service |
| `src/api.js` | the public HTTP routes: origin, media type, size, edge limit, JSON, validation |
| `src/service.js` | the service: random values, limits, the transaction that files a report |
| `src/reports.js` | the request fields, tickets, receipts, statuses, the SQL tables, retention |
| `src/limits.js` | the in-memory rate limiter (bounded, pruned) and address grouping |
| `src/admin.js`, `src/admin_page.js` | the dashboard's API and its static page |
| `src/sha256.js` | a synchronous SHA-256 |
| `local/host.js` | the Node host (`node:sqlite`) the tests use |
| `local/serve.js` | the loopback development server (`npm run dev`) |
| `test/` | the tests; `test/fixtures/request.json` is the canonical request |

The Worker checks every request, then asks the Durable Object (named `global`) by RPC; the object is reached from nowhere
else. Reports, statuses and receipt hashes live in its SQLite database.

## Endpoints

| Path | Method | Who | What |
|---|---|---|---|
| `/health` | GET | anyone | name, protocol and build; no counts |
| `/v1/reports` | POST | launcher | file a report; answers `201 {ticket, receipt, submitted_at}`, plus `dump: "expected"` when it announced a crash dump |
| `/v1/reports/status` | POST | launcher | up to 30 `{ticket, receipt}` claims; answers the status of those that match |
| `/v1/reports/read` | POST | launcher | `{ticket, receipt}`; answers the stored report to its owner |
| `/v1/reports/feedback` | POST | launcher | `{ticket, receipt, revision, result, launcher_version, game_build}`: the owner's answer to the open player test |
| `/v1/reports/dump` | POST | launcher | `{ticket, receipt}`; answers `{stored, bytes, complete}` for the report's announced dump |
| `/v1/reports/dump/chunk` | POST | launcher | `{ticket, receipt, offset, data}`: up to 1 MiB of the dump in base64, exactly at `stored` |
| `/admin` | GET | developer | the dashboard page (static; holds no data) |
| `/admin/api/reports[/<ticket>[/log\|/dump\|/status]]` | GET, POST, DELETE | developer | list, read, download the log or the minidump, set the status, delete a report; Bearer token |

Requests are JSON over HTTPS (`Content-Type: application/json`). A request that carries an `Origin` header (a web page) is
refused, there are no CORS headers, bodies are cut at 192 KiB while read, and fields are validated before the Durable
Object is touched. A field the page could not have shown is refused, not dropped. Answers: `400 bad_request`,
`400 unknown_field` (a field this Worker does not know: the launcher is newer than the deployed Worker),
`403 origin_not_allowed`, `404 not_found`, `405`, `409 offset` (a dump chunk not at `stored`, with `{stored, bytes, complete}`),
`409 stale_request | already_answered` (player tests),
`413 too_large`, `415 unsupported_media_type`, `429 hourly_limit | daily_limit | limit` (with `Retry-After`), `502 unavailable`.
An error body is `{error, detail?, protocol}`: `detail` names the refused field, never its value. A stack trace is never returned.

A ticket is `SOB-` and six characters from a 32-letter alphabet; a receipt is 32 random bytes as 64 lowercase hex
characters. The fields of a report are those of `READERS` in `src/reports.js`: description, launcher version, game build,
region, game version, title id (CUSA), executable hash, operating system, CPU, selected and active renderer, GPU, driver,
render resolution, how the session ended (`session_end`: `unknown`, `clean`, `crash`, `device_lost`, `stopped`,
`startup_failure`, `unsupported_build`), the line that names the failure (`session_detail`, up to 400 characters), the last
startup stage (`session_stage`), log file name (`shadow_of_the_beast_NNN.log`), whether the log was clipped, and the log
text. The three session fields may be absent (launchers released before them); they are then stored as `unknown` and empty.

A report may also carry `submission` (64 lowercase hex characters, one per draft in the launcher): the same request repeated
within a day, after an answer that was lost, answers `200` with the report already filed and its receipt, and files nothing.

### Crash dumps

A report may announce its session's minidump with `dump_file` (exactly the log's name with `.dmp`), `dump_bytes` (1 KiB to
8 MiB) and `dump_sha256`. The bytes follow under the report's receipt: `/v1/reports/dump` says how many are stored, and each
chunk must start exactly there; a chunk already stored, a gap or anything past the announced size is refused, so nothing is
appended twice. The first chunk must begin with `MDMP`. When the last byte arrives the whole dump must begin with `MDMP` and
have the announced SHA-256; otherwise the announcement and its bytes are dropped. Only a complete dump is shown as attached
and can be downloaded (`application/octet-stream`, `<ticket>-shadow_of_the_beast_NNN.dmp` from validated names only). There
is no other file upload.

## Admin

`/admin` is a static page; its data comes from `/admin/api/...`, and every one of those calls needs
`Authorization: Bearer <token>`. The token is the Worker secret `SOTB_BUG_REPORT_ADMIN_TOKEN` (at least 24 characters),
compared by SHA-256 digest; wrong tries are limited per address (10, then one every 30 seconds, per isolate). Without the
secret `/admin/api` answers `503 not_configured`. A call that names another origin is refused and no CORS header is sent. The
token is never in the page, the code or the logs.

The dashboard lists reports newest first with their status and player feedback, counts the reports in each status (New
included), opens one with its facts, player feedback and log, filters by status, player feedback (Awaiting response, Player
confirmed fixed, Still happening), launcher version and operating system, saves a status with a message to the player,
copies or downloads the log and the minidump, and deletes a report after a confirmation. Filtering happens in the page over
the list already loaded and changes nothing. A failed request keeps the list and the open report on screen. "Keep for this
tab" keeps the token and the open report in the tab's session storage until Sign out or until the tab closes. Everything a
player sent is shown as text, never as markup. The downloaded file is named `<ticket>-<log file>` from two validated names
only.

Statuses (wire values): New (`new`), Investigating (`investigating`), Awaiting player test (`awaiting_player_test`), Fixed
(`fixed`, with an optional version), Cannot reproduce (`cannot_reproduce`), Duplicate (`duplicate`), Not a bug
(`not_a_bug`), Closed (`closed`). Only Fixed means the problem is confirmed fixed, and only Fixed takes `fixed_version`.
The message to the player (up to 500 characters) is separate from the status: a field left out of a status change keeps its
stored value, so a message is cleared only when it is sent empty. Every saved change gets a status time later than the one
before (`status_updated_at`), and a changed message gets its own time (`message_updated_at`). A status change makes the
launcher show **Updated**; a new message revision makes it show **New reply**.

### Player tests

Awaiting player test needs the message: it is what the player should test. The launcher asks **Did this fix the issue?**
and sends `fixed` or `not_fixed` with `revision`, the `status_updated_at` it was shown. The answer is taken only for the open
request (`409 stale_request` for any other revision or status, `404 not_found` for a claim that does not match) and the first
answer is final: the same answer again answers `200` and changes nothing, another one is `409 already_answered`. Saving the
report again (a new request) reads as pending; the earlier answer stays stored and the dashboard shows it as such. An answer
never changes the status. It is stored with its time and the launcher version and game build the player used; there is no
free text. Status answers carry `test_feedback` (`pending`, `fixed` or `not_fixed`) while a request is open.

## Rate limits

| Limit | Value |
|---|---|
| reports per address | 3 an hour, 10 a day (IPv6 counted by /64) |
| reports in all | 300 a day |
| submit bursts (memory) | 40 in all, 6 per address |
| status checks (memory) | 200 in all, 20 per address |
| reads (memory) | 10 per address |
| player test answers (memory) | 10 per address, then one every 30 seconds; never the report quota |
| status request | 30 reports |
| dump requests (memory) | 120 in all, 30 per address, then one every 10 seconds |
| edge limit (`REPORT_LIMITER`) | 20 requests a minute per address, before the Durable Object; dump requests are counted apart from the others |

## Retention

- The newest 500 reports are kept, for at most 180 days.
- A report's status and the hash that lets its owner ask for it are kept up to 5000 entries and 365 days.
- Quota rows (a salted hash of the address and a time) are deleted after one day.
- At most the newest 100 crash dumps are kept, each only as long as its report; an upload nobody finished is deleted after
  one day.
- A submission key is kept only as a salted hash, for one day and never longer than its report.
- A player test answer lives in the status row and goes with it.
- A report can be deleted at any time from the dashboard: its row, log, status, message, player feedback, receipt hash,
  submission key and dump (complete or not) go together. Quota counters already spent are not given back.

## Privacy

A report holds only the fields the player saw in the launcher's preview before pressing Send report, and one privacy-filtered
log. No account, name, address, token or other identity is stored or logged. A receipt is kept only as a salted hash, and the
salt never leaves the database. The connection's address is used for the quota as a salted hash and is not stored with the
report. The Worker's log records refusals and errors, never report text, receipts, tokens or addresses. There is no
telemetry: nothing is sent unless the player presses Send report.

## Local development and tests

```powershell
cd server\shadow-bug-reports
npm test        # node --test "test/*.test.js"; Node 22.5 or newer; an in-memory database, no network, no npm packages
```

`.\tools\test.ps1` runs the launcher's Bug Report test, these tests with the launcher's own request, and
`tests\worker_script_tests.ps1` (`tools\worker.ps1` against a stand-in `npx`; nothing is deployed).

`npm run dev` serves the same handlers on `http://127.0.0.1:8787` (`PORT` changes it) to try `/admin` and the API without
Cloudflare. It needs `SOTB_BUG_REPORT_ADMIN_TOKEN` in the environment and keeps reports in memory unless `SOTB_LOCAL_DB`
names a SQLite file. The launcher only talks HTTPS, so it cannot reach this server; file reports with `curl` instead.

`test/contract.test.js` is skipped unless `SOTB_CPP_REQUEST` names a request file the launcher's test built
(`sotb-bug-report-tests`), so the request the real launcher builds is the one the Worker has to accept.
`test/config.test.js` checks `wrangler.toml` (own name, own namespace, no other project's values) and the one secret the code
reads.

## Variables

| Name | Where | What |
|---|---|---|
| `SOTB_BUG_REPORT_ADMIN_TOKEN` | Worker secret | the admin token, at least 24 characters |
| `REPORTS` | Durable Object binding | `BugReportDesk` |
| `REPORT_LIMITER` | rate-limit binding | the edge limit; `namespace_id` in `wrangler.toml` |
| `SOTB_BUG_REPORT_URL` | launcher environment | development and test override of the launcher's default service address, for one run (`https://host[:port]`) |

This Worker's `REPORT_LIMITER` uses `namespace_id = "5006"` in `wrangler.toml`. The value must stay unique in the Cloudflare
account: the other Workers use 1001, 1301, 2002, 2701, 3003, 3601, 4004 and 5005, and `test/config.test.js` refuses those.

## Deploy (manual; nothing runs it for you)

From the repository root:

```powershell
.\tools\worker.ps1 -DryRun   # validates the files, wrangler.toml, imports and namespace_id, then npx wrangler deploy --dry-run: bundles, uploads nothing
.\tools\worker.ps1           # the same checks, then npx wrangler deploy inside server\shadow-bug-reports
```

The script refuses to run Wrangler while the folder, `wrangler.toml`, `package.json`, the entry point, an imported module or `npx`
is missing, or while `namespace_id` is not an integer or holds a placeholder. It never sets a secret and prints no token.
Wrangler decides the destination from `wrangler.toml`; a Wrangler failure keeps its exit code.

Before the first deploy:

1. `npx wrangler login`, then `npx wrangler whoami` to check the account.
2. Keep `namespace_id = "5006"` in `wrangler.toml` (see Variables).
3. `.\tools\worker.ps1 -DryRun`, then `.\tools\worker.ps1`. The deploy creates the Durable Object class `BugReportDesk`
   (migration `v1`, SQLite); do not edit an applied migration.
4. Make a random token of 24 or more characters, keep it in a password manager, and run
   `npx wrangler secret put SOTB_BUG_REPORT_ADMIN_TOKEN` in `server\shadow-bug-reports`. Until the secret exists `/admin/api` answers `503`.
5. Open `/health`, then `/admin`, and send one report from the launcher. The launcher needs no setting for the production
   address; `SOTB_BUG_REPORT_URL` is only an override for development and tests.

Protocol 2 (session fields, crash dumps, submission keys) must be deployed before a launcher that sends them is released:
an older Worker refuses their keys. The existing database gains the session columns, the dump tables and the message and
player test columns on its own when the object starts; nothing is reset. A launcher released before player tests ignores
the Awaiting player test status, and a launcher with them never sees one from an older Worker.

To rotate the token, run the secret command again; nothing else needs deploying. `npx wrangler tail shadow-bug-reports`
shows the Worker's log.
