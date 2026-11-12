// node tools/validate.mjs [path]   - validates registry/libraries.json (or the file named).
// Exit 0 = valid, 1 = invalid (every error printed), 2 = unreadable. Reads one file, writes none.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { validateRegistryText } from '../src/validate.js';

const here = fileURLToPath(new URL('.', import.meta.url));
const path = process.argv[2] || `${here}../registry/libraries.json`;
let text;
try { text = readFileSync(path, 'utf8'); } catch (e) { console.error(`cannot read ${path}: ${e.message}`); process.exit(2); }
const r = validateRegistryText(text);
if (!r.ok) {
  console.error(`INVALID: ${path}`);
  for (const e of r.errors) console.error(`  - ${e}`);
  process.exit(1);
}
const rows = r.registry.libraries;
const count = (s) => rows.filter((x) => x.status === s).length;
console.log(`valid: ${rows.length} libraries (registry_version ${r.registry.registry_version}; ok ${count('ok')}, unverified ${count('unverified')}, moved ${count('moved')}, broken ${count('broken')})`);
