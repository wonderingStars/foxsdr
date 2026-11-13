// Registry validation: the single gate between a hand-edited libraries.json and anything that
// serves it, bundles it or (in the program) accepts it. PURE: no platform API, no network.
//
// STRICT ON PURPOSE. An unknown field is an error (a typo such as "page_ulr" must not be a row
// that silently has no address), every address is https with no user-info and no fragment, and a
// row whose status is `unverified` may carry NO address at all (the owner's rule: invent none).
//
// The same rules are specified for the program in CLIENT-CONTRACT.md; the C++ side must refuse
// exactly what this refuses. registry/schema.json documents the shape; test/schema-sync.test.mjs
// holds it to the constants below.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

export const SCHEMA_VERSION = 1;
export const MAX_REGISTRY_BYTES = 256 * 1024;
export const MAX_ROWS = 200;

export const STATUSES = ['ok', 'moved', 'broken', 'unverified'];
export const FETCH_ALLOWED = ['yes', 'no (patent)', 'no (terms)', 'no (unverified)'];
export const INSTALL_KINDS = ['installer', 'copy-in', 'helper-program'];

export const TOP_FIELDS = ['schema_version', 'registry_version', 'updated', 'libraries'];
export const ROW_FIELDS = [
  'id', 'name', 'publisher', 'files', 'min_version', 'page_url', 'download', 'licence',
  'fetch_allowed', 'fetch_evidence', 'install_kind', 'allowed_hosts', 'status', 'status_note',
  'last_checked', 'last_changed',
];
export const OPTIONAL_ROW_FIELDS = ['status_note'];
export const DOWNLOAD_FIELDS = ['url', 'size_bytes', 'sha256'];

const ID_RE = /^[a-z0-9]+(-[a-z0-9]+)*$/;
const DATE_RE = /^(\d{4})-(\d{2})-(\d{2})$/;
const SHA256_RE = /^[0-9a-f]{64}$/;
const VERSION_RE = /^\d+(\.\d+){0,3}$/;
// A DNS name: labels of letters, digits and hyphens, at least one dot, lower case, no trailing dot.
const HOST_RE = /^(?=.{4,253}$)([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$/;
const FILE_RE = /^[A-Za-z0-9][A-Za-z0-9 ._+-]{0,98}$/;

export function isHost(h) {
  return typeof h === 'string' && HOST_RE.test(h);
}

export function isDate(s) {
  if (typeof s !== 'string') return false;
  const m = DATE_RE.exec(s);
  if (!m) return false;
  const d = new Date(Date.UTC(+m[1], +m[2] - 1, +m[3]));
  return d.getUTCFullYear() === +m[1] && d.getUTCMonth() === +m[2] - 1 && d.getUTCDate() === +m[3];
}

// Parses an address a registry may carry. Returns { host } or { error }.
export function parseAddress(text) {
  if (typeof text !== 'string' || text.length === 0 || text.length > 500) {
    return { error: 'must be a string of 1..500 characters' };
  }
  if (/[\s\u0000-\u001f\u007f]/.test(text)) return { error: 'must not contain whitespace or control characters' };
  if (!text.startsWith('https://')) return { error: 'must start with https://' };
  let u;
  try { u = new URL(text); } catch { return { error: 'is not a valid address' }; }
  if (u.protocol !== 'https:') return { error: 'must be https' };
  if (u.username !== '' || u.password !== '') return { error: 'must not carry a user name or password' };
  if (u.port !== '') return { error: 'must not name a port' };
  if (u.hash !== '' || text.includes('#')) return { error: 'must not carry a #fragment' };
  if (!isHost(u.hostname)) return { error: 'host is not a lower-case DNS name' };
  // new URL() lower-cases and punycodes; refuse anything it had to rewrite in the host so that
  // what is stored is what is compared.
  if (!text.slice(8).startsWith(u.hostname)) return { error: 'host is not in plain lower-case form' };
  return { host: u.hostname };
}

function isObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

function unknownFields(obj, allowed, where, errors) {
  for (const k of Object.keys(obj)) {
    if (!allowed.includes(k)) errors.push(`${where}: unknown field "${k}"`);
  }
}

function validateRow(row, index, errors) {
  const where = isObject(row) && typeof row.id === 'string' ? `row "${row.id}"` : `row #${index + 1}`;
  const err = (m) => errors.push(`${where}: ${m}`);
  if (!isObject(row)) { err('is not an object'); return; }
  unknownFields(row, ROW_FIELDS, where, errors);
  for (const f of ROW_FIELDS) {
    if (OPTIONAL_ROW_FIELDS.includes(f)) continue;
    if (!(f in row)) err(`missing field "${f}"`);
  }

  if (typeof row.id !== 'string' || !ID_RE.test(row.id) || row.id.length > 40) {
    err('id must be lower-case words joined by hyphens, at most 40 characters');
  }
  for (const f of ['name', 'publisher', 'licence', 'fetch_evidence']) {
    if (typeof row[f] !== 'string' || row[f].trim() === '' || row[f].length > 600) err(`${f} must be a non-empty string of at most 600 characters`);
  }
  if ('status_note' in row && (typeof row.status_note !== 'string' || row.status_note.length > 600)) err('status_note must be a string of at most 600 characters');

  if (!Array.isArray(row.files) || row.files.length > 20 || !row.files.every((f) => typeof f === 'string' && FILE_RE.test(f))) {
    err('files must be an array (at most 20) of plain file names, no folders');
  }
  if (row.min_version !== null && !(typeof row.min_version === 'string' && VERSION_RE.test(row.min_version))) {
    err('min_version must be null or a dotted number such as "3.0"');
  }
  if (!FETCH_ALLOWED.includes(row.fetch_allowed)) err(`fetch_allowed must be one of: ${FETCH_ALLOWED.join(' | ')}`);
  if (!INSTALL_KINDS.includes(row.install_kind)) err(`install_kind must be one of: ${INSTALL_KINDS.join(' | ')}`);
  if (!STATUSES.includes(row.status)) err(`status must be one of: ${STATUSES.join(' | ')}`);
  if (!isDate(row.last_changed)) err('last_changed must be a real date YYYY-MM-DD');
  if (row.last_checked !== null && !isDate(row.last_checked)) err('last_checked must be null or a real date YYYY-MM-DD');

  // Addresses.
  let pageHost = null;
  if (row.page_url !== null) {
    const p = parseAddress(row.page_url);
    if (p.error) err(`page_url ${p.error}`); else pageHost = p.host;
  }
  let dlHost = null;
  if (row.download !== null) {
    if (!isObject(row.download)) {
      err('download must be null or an object');
    } else {
      unknownFields(row.download, DOWNLOAD_FIELDS, `${where} download`, errors);
      const d = parseAddress(row.download.url);
      if (d.error) err(`download.url ${d.error}`); else dlHost = d.host;
      const s = row.download.size_bytes;
      if (s !== null && !(Number.isInteger(s) && s > 0 && s <= 4 * 1024 ** 3)) err('download.size_bytes must be null or a positive whole number of bytes');
      const h = row.download.sha256;
      if (h !== null && !(typeof h === 'string' && SHA256_RE.test(h))) err('download.sha256 must be null or 64 lower-case hex characters');
      if (!('size_bytes' in row.download) || !('sha256' in row.download)) err('download needs url, size_bytes and sha256 (null when unknown)');
    }
  }
  if (!Array.isArray(row.allowed_hosts) || row.allowed_hosts.length > 10 || !row.allowed_hosts.every(isHost)) {
    err('allowed_hosts must be an array (at most 10) of lower-case host names');
  } else {
    const hosts = row.allowed_hosts;
    if (new Set(hosts).size !== hosts.length) err('allowed_hosts lists a host twice');
    if (pageHost && !hosts.includes(pageHost)) err(`page_url host ${pageHost} is not in allowed_hosts`);
    if (dlHost && !hosts.includes(dlHost)) err(`download.url host ${dlHost} is not in allowed_hosts`);
  }

  // The rules that tie the fields together.
  if (row.status === 'unverified') {
    if (row.page_url !== null || row.download !== null) err('status "unverified" means NO address: page_url and download must be null');
    if (Array.isArray(row.allowed_hosts) && row.allowed_hosts.length !== 0) err('status "unverified" means allowed_hosts must be empty');
    if (row.fetch_allowed === 'yes') err('fetch_allowed "yes" needs a verified row');
  } else if (row.page_url === null) {
    err(`status "${row.status}" needs a page_url (only "unverified" rows have none)`);
  }
  if (row.fetch_allowed === 'yes') {
    if (row.download === null) err('fetch_allowed "yes" needs a download address');
    if (row.install_kind === 'helper-program') err('fetch_allowed "yes" is for an installer or a copy-in file, not a helper program');
    if (typeof row.fetch_evidence === 'string' && row.fetch_evidence.trim().length < 20) err('fetch_allowed "yes" needs the licence sentence and address quoted in fetch_evidence');
    if (row.status !== 'ok') err('fetch_allowed "yes" needs status "ok"');
  }
}

// Returns { ok, errors, registry } where `registry` is the parsed object when ok.
export function validateRegistryText(text) {
  if (typeof text !== 'string') return { ok: false, errors: ['registry text is not a string'] };
  if (new TextEncoder().encode(text).length > MAX_REGISTRY_BYTES) return { ok: false, errors: [`registry is larger than ${MAX_REGISTRY_BYTES} bytes`] };
  let reg;
  try { reg = JSON.parse(text); } catch (e) { return { ok: false, errors: [`not valid JSON: ${e.message}`] }; }
  const r = validateRegistry(reg);
  return { ...r, registry: r.ok ? reg : undefined };
}

export function validateRegistry(reg) {
  const errors = [];
  if (!isObject(reg)) return { ok: false, errors: ['registry is not a JSON object'] };
  unknownFields(reg, TOP_FIELDS, 'registry', errors);
  for (const f of TOP_FIELDS) if (!(f in reg)) errors.push(`registry: missing field "${f}"`);
  if (reg.schema_version !== SCHEMA_VERSION) errors.push(`registry: schema_version must be ${SCHEMA_VERSION}`);
  if (!Number.isInteger(reg.registry_version) || reg.registry_version < 1 || reg.registry_version > 1e9) errors.push('registry: registry_version must be a whole number from 1');
  if (!isDate(reg.updated)) errors.push('registry: updated must be a real date YYYY-MM-DD');
  if (!Array.isArray(reg.libraries)) {
    errors.push('registry: libraries must be an array');
  } else {
    if (reg.libraries.length > MAX_ROWS) errors.push(`registry: more than ${MAX_ROWS} rows`);
    const seen = new Set();
    reg.libraries.forEach((row, i) => {
      validateRow(row, i, errors);
      if (isObject(row) && typeof row.id === 'string') {
        if (seen.has(row.id)) errors.push(`row "${row.id}": id appears twice`);
        seen.add(row.id);
      }
    });
  }
  return { ok: errors.length === 0, errors };
}
