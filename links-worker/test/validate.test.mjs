import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { validateRegistry, validateRegistryText, parseAddress, MAX_REGISTRY_BYTES } from '../src/validate.js';
import { registry, row } from './helpers.mjs';

const errorsOf = (reg) => validateRegistry(reg).errors.join(' | ');

test('the shipped registry validates and seeds nothing invented', () => {
  const text = readFileSync(new URL('../registry/libraries.json', import.meta.url), 'utf8');
  const r = validateRegistryText(text);
  assert.equal(r.ok, true, r.errors?.join('\n'));
  for (const lib of r.registry.libraries) {
    if (lib.status === 'unverified') {
      assert.equal(lib.page_url, null, lib.id);
      assert.equal(lib.download, null, lib.id);
      assert.deepEqual(lib.allowed_hosts, [], lib.id);
    }
    // nothing is offered for one-click download by the seed
    assert.notEqual(lib.fetch_allowed, 'yes', lib.id);
  }
});

test('a good minimal registry validates', () => {
  assert.deepEqual(validateRegistry(registry()), { ok: true, errors: [] });
});

test('malformed registries are rejected', () => {
  const cases = {
    'not JSON': () => validateRegistryText('{nope'),
    'array at top': () => validateRegistry([]),
    'null': () => validateRegistry(null),
    'empty text': () => validateRegistryText(''),
    'too large': () => validateRegistryText('x'.repeat(MAX_REGISTRY_BYTES + 1)),
  };
  for (const [name, f] of Object.entries(cases)) assert.equal(f().ok, false, name);
});

test('every required field, every enum, every shape is enforced', () => {
  const bad = (over, expect) => {
    const e = errorsOf(registry([row(over)]));
    assert.match(e, expect, JSON.stringify(over));
  };
  bad({ typo_field: 1 }, /unknown field "typo_field"/);
  bad({ id: 'Bad_Id' }, /id must be/);
  bad({ id: '' }, /id must be/);
  bad({ name: '' }, /name must be/);
  bad({ files: ['a/b.dll'] }, /files must be/);
  bad({ files: 'a.dll' }, /files must be/);
  bad({ min_version: '3.x' }, /min_version/);
  bad({ fetch_allowed: 'maybe' }, /fetch_allowed must be/);
  bad({ install_kind: 'magic' }, /install_kind must be/);
  bad({ status: 'fine' }, /status must be/);
  bad({ last_changed: '2026-02-30' }, /last_changed/);
  bad({ last_checked: '12/11/2026' }, /last_checked/);
  bad({ allowed_hosts: ['Demo.Example'] }, /allowed_hosts/);
  bad({ allowed_hosts: ['demo.example', 'demo.example'] }, /twice/);
  bad({ licence: 7 }, /licence must be/);
  const missing = row();
  delete missing.publisher;
  assert.match(errorsOf(registry([missing])), /missing field "publisher"/);
  assert.match(errorsOf(registry([row(), row()])), /appears twice/);
  assert.match(errorsOf({ ...registry(), registry_version: 0 }), /registry_version/);
  assert.match(errorsOf({ ...registry(), schema_version: 2 }), /schema_version/);
  assert.match(errorsOf({ ...registry(), extra: 1 }), /unknown field "extra"/);
});

test('addresses: https only, no user-info, port, fragment, upper case, spaces; host must be allowed', () => {
  const urls = [
    'http://demo.example/page', 'ftp://demo.example/', 'https://user:pw@demo.example/', 'https://demo.example:8443/',
    'https://demo.example/page#top', 'https://DEMO.example/page', 'https://demo.example/a b', 'javascript:alert(1)', 'https://localhost/', '//demo.example/',
  ];
  for (const u of urls) {
    assert.notEqual(parseAddress(u).error, undefined, u);
    assert.equal(validateRegistry(registry([row({ page_url: u })])).ok, false, u);
  }
  assert.equal(parseAddress('https://demo.example/page?a=1').host, 'demo.example');
  assert.match(errorsOf(registry([row({ page_url: 'https://other.example/' })])), /not in allowed_hosts/);
  assert.match(errorsOf(registry([row({ download: { url: 'https://other.example/f.exe', size_bytes: null, sha256: null } })])), /download.url host/);
});

test('download object shape and checksum', () => {
  const dl = (d) => registry([row({ download: d })]);
  assert.equal(validateRegistry(dl({ url: 'https://demo.example/f.exe', size_bytes: 1234, sha256: 'a'.repeat(64) })).ok, true);
  assert.equal(validateRegistry(dl({ url: 'https://demo.example/f.exe', size_bytes: null, sha256: null })).ok, true);
  assert.match(errorsOf(dl({ url: 'https://demo.example/f.exe', size_bytes: 0, sha256: null })), /size_bytes/);
  assert.match(errorsOf(dl({ url: 'https://demo.example/f.exe', size_bytes: 1.5, sha256: null })), /size_bytes/);
  assert.match(errorsOf(dl({ url: 'https://demo.example/f.exe', size_bytes: null, sha256: 'A'.repeat(64) })), /sha256/);
  assert.match(errorsOf(dl({ url: 'https://demo.example/f.exe' })), /needs url, size_bytes and sha256/);
  assert.match(errorsOf(dl({ url: 'https://demo.example/f.exe', size_bytes: null, sha256: null, mirror: 'x' })), /unknown field "mirror"/);
  assert.match(errorsOf(dl('https://demo.example/f.exe')), /download/);
});

test('unverified means NO address (the owner rule: invent none)', () => {
  const unverified = row({ status: 'unverified', page_url: null, download: null, allowed_hosts: [] });
  assert.equal(validateRegistry(registry([unverified])).ok, true);
  assert.match(errorsOf(registry([row({ status: 'unverified' })])), /NO address/);
  assert.match(errorsOf(registry([{ ...unverified, allowed_hosts: ['demo.example'] }])), /allowed_hosts must be empty/);
  assert.match(errorsOf(registry([row({ page_url: null, allowed_hosts: [] })])), /needs a page_url/);
});

test('fetch_allowed "yes" needs a verified, installable, evidenced download', () => {
  const dl = { url: 'https://demo.example/setup.exe', size_bytes: 10, sha256: null };
  const yes = (over) => registry([row({ fetch_allowed: 'yes', download: dl, fetch_evidence: 'MIT licence, "anyone may download": https://demo.example/licence', ...over })]);
  assert.equal(validateRegistry(yes({})).ok, true);
  assert.match(errorsOf(yes({ download: null })), /needs a download/);
  assert.match(errorsOf(yes({ install_kind: 'helper-program' })), /helper program/);
  assert.match(errorsOf(yes({ fetch_evidence: 'ok' })), /quoted in fetch_evidence/);
  assert.match(errorsOf(yes({ status: 'moved' })), /status "ok"/);
  assert.match(errorsOf(yes({ status: 'unverified', page_url: null, download: null, allowed_hosts: [] })), /verified row|NO address/);
});
