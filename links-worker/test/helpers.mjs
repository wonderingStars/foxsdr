// Shared stand-ins for the tests. Nothing here touches the network: a scripted fetch, a fake KV
// namespace, and a loader for MUTANT copies of the source (the break-it checks).
import { cpSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const SRC_DIR = fileURLToPath(new URL('../src/', import.meta.url));

export function row(over = {}) {
  return {
    id: 'demo-lib',
    name: 'Demo library',
    publisher: 'Demo Publisher',
    files: ['demo.dll'],
    min_version: '1.0',
    page_url: 'https://demo.example/page',
    download: null,
    licence: 'MIT (demo)',
    fetch_allowed: 'no (unverified)',
    fetch_evidence: 'not assessed (demo)',
    install_kind: 'installer',
    allowed_hosts: ['demo.example'],
    status: 'ok',
    last_checked: null,
    last_changed: '2026-11-12',
    ...over,
  };
}

export function registry(rows = [row()], over = {}) {
  return { schema_version: 1, registry_version: 3, updated: '2026-11-12', libraries: rows, ...over };
}

export function registryText(reg = registry()) {
  return JSON.stringify(reg, null, 2) + '\n';
}

// A scripted fetch. `script` maps "METHOD url" -> a response spec or a function(init) -> spec.
// A spec is { status, headers, throws } ; a missing script entry throws a network error.
// Records every call in .calls so a test can assert what was (and was NOT) requested.
export function scriptedFetch(script) {
  const calls = [];
  const fn = async (url, init = {}) => {
    const method = init.method || 'GET';
    calls.push({ url: String(url), method, headers: init.headers || {}, signal: init.signal });
    let spec = script[`${method} ${url}`] ?? script[`* ${url}`];
    if (typeof spec === 'function') spec = spec(init);
    if (!spec) throw new TypeError('no such stand-in address: ' + url);
    if (spec.hangs) {
      // honour the abort signal exactly as fetch does
      return new Promise((_, reject) => {
        init.signal?.addEventListener('abort', () => reject(init.signal.reason));
      });
    }
    if (spec.throws) throw spec.throws;
    return new Response(spec.body ?? null, { status: spec.status ?? 200, headers: spec.headers ?? {} });
  };
  fn.calls = calls;
  return fn;
}

export function fakeKv(initial = null) {
  const store = new Map();
  if (initial) store.set('state', JSON.stringify(initial));
  return {
    store,
    puts: 0,
    gets: 0,
    failGet: false,
    async get(key, type) {
      this.gets += 1;
      if (this.failGet) throw new Error('KV unavailable');
      const v = store.get(key);
      if (v === undefined) return null;
      return type === 'json' ? JSON.parse(v) : v;
    },
    async put(key, value) {
      this.puts += 1;
      store.set(key, value);
    },
  };
}

// Copies src/ to a scratch folder, applies ONE text replacement to one file (and fails if the
// text is not there, so a mutant can never silently equal the original), and imports it.
export async function loadMutant(file, from, to) {
  const dir = mkdtempSync(join(tmpdir(), 'lw-mutant-'));
  cpSync(SRC_DIR, dir, { recursive: true });
  writeFileSync(join(dir, 'package.json'), '{"type":"module"}');
  const path = join(dir, file);
  const text = readFileSync(path, 'utf8');
  if (!text.includes(from)) throw new Error(`mutation target not found in ${file}: ${from}`);
  writeFileSync(path, text.replace(from, to));
  return import(pathToFileURL(path).href + `?m=${Math.random()}`);
}
