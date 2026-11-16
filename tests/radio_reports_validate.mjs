// Reads a file of messages, one JSON message per line, and prints the Worker's own verdict on each (telemetry-worker/radio-reports.js, the
// validator the deployed Worker runs): "OK <kind>" or "REFUSED <code> <kind>". Used by tests/test_radio_reporter.cpp, which hands it every message
// the application made. Nothing here is the Worker's: it only imports the validator, so the app is held to the SAME file the Worker is.
//
// usage: node radio_reports_validate.mjs <messages.jsonl>
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const rr = await import(new URL('file:///' + join(here, '..', 'telemetry-worker', 'radio-reports.js').replace(/\\/g, '/')).href);

// worker.js's scrubModel, verbatim (it is not exported there).
function scrubModel(v) {
  if (typeof v !== 'string') return '';
  const kept = [];
  let dropped = false;
  for (const t of v.split(/[\s,;]+/)) {
    if (t === '') continue;
    if (/[=:/\\@]/.test(t) || /^\d+\.\d+\.\d+\.\d+$/.test(t)) { dropped = true; continue; }
    kept.push(t);
  }
  if (kept.length === 0) return dropped ? 'other' : '';
  return kept.join(' ');
}

const text = readFileSync(process.argv[2], 'utf8');
for (const line of text.split('\n')) {
  if (line.trim() === '') continue;
  let msg;
  try { msg = JSON.parse(line); } catch { console.log('REFUSED bad-json ?'); continue; }
  const v = rr.validateRadioMessage(msg, { scrubModel });
  const kind = msg && typeof msg.kind === 'string' ? msg.kind : '?';
  console.log(v.ok ? `OK ${kind}` : `REFUSED ${v.code} ${kind}`);
}
