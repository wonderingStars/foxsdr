import assert from 'node:assert/strict';
import { generateKeyPairSync, randomBytes } from 'node:crypto';
import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { render } from '../tools/build-bundle.mjs';
import { keyIdOf, makeEnvelope, parseEnvelope, PREFIX, privateKeyFromSeed, rawPublicKey, verifyEnvelope } from '../tools/envelope.mjs';
import { seedFromFileBytes } from '../tools/sign-registry.mjs';
import { registryText, registry } from './helpers.mjs';

const here = fileURLToPath(new URL('../', import.meta.url));
const read = (p) => readFileSync(`${here}${p}`, 'utf8');

// Keys here are made IN MEMORY by node:crypto for the test; nothing is written, stored or trusted.
const seed = randomBytes(32);
const key = privateKeyFromSeed(seed);
const pub = rawPublicKey(key);
const payload = Buffer.from(registryText(), 'utf8');

test('the committed bundle is exactly what the build tool writes from the registry now', () => {
  const built = render(read('registry/libraries.json'), existsSync(`${here}registry/libraries.signed.json`) ? read('registry/libraries.signed.json') : null);
  assert.deepEqual(built.problems, []);
  assert.equal(read('src/registry-bundle.js'), built.source);
});

test('the registry rows named from code agree with src/core/radio_setup.cpp (when this tree has it)', () => {
  const cpp = `${here}../src/core/radio_setup.cpp`;
  if (!existsSync(cpp)) return;
  const reg = JSON.parse(read('registry/libraries.json'));
  const pages = new Set(reg.libraries.map((l) => l.page_url));
  const urls = [...readFileSync(cpp, 'utf8').matchAll(/constexpr const char\* k\w+Url = "(https:\/\/[^"]+)"/g)].map((m) => m[1]);
  assert.ok(urls.length >= 6);
  for (const u of urls) assert.ok(pages.has(u), `${u} is in radio_setup.cpp but not in the registry`);
});

test('sign then verify: the envelope has the documented shape and the payload comes back byte for byte', () => {
  const env = makeEnvelope(key, payload);
  const o = JSON.parse(env);
  assert.deepEqual(Object.keys(o), ['format', 'version', 'alg', 'keyId', 'payload', 'sig']);
  assert.equal(o.format, 'foxsdr-link-registry-sig');
  assert.equal(o.keyId, keyIdOf(pub));
  assert.equal(o.keyId.length, 16);
  assert.equal(Buffer.from(o.sig, 'base64').length, 64);
  const v = verifyEnvelope(env, [pub]);
  assert.equal(v.ok, true);
  assert.ok(v.payload.equals(payload));
});

test('a tampered payload, a tampered signature, the wrong key, no key and a rotated key list are handled', () => {
  const env = JSON.parse(makeEnvelope(key, payload));
  const flip = (b64) => { const b = Buffer.from(b64, 'base64'); b[0] ^= 1; return b.toString('base64'); };
  assert.equal(verifyEnvelope(JSON.stringify({ ...env, payload: flip(env.payload) }), [pub]).error, 'bad signature');
  assert.equal(verifyEnvelope(JSON.stringify({ ...env, sig: flip(env.sig) }), [pub]).error, 'bad signature');
  const other = rawPublicKey(generateKeyPairSync('ed25519').privateKey);
  assert.equal(verifyEnvelope(JSON.stringify(env), [other]).error, 'unknown key');
  assert.equal(verifyEnvelope(JSON.stringify(env), []).error, 'no trusted key');
  assert.equal(verifyEnvelope(JSON.stringify(env), [other, pub]).ok, true, 'the program carries a LIST of keys');
});

test('malformed envelopes are refused: format, version, algorithm, key id, encodings, size, non-JSON', () => {
  const good = JSON.parse(makeEnvelope(key, payload));
  const refused = (o) => parseEnvelope(typeof o === 'string' ? o : JSON.stringify(o)).ok === false;
  assert.ok(refused({ ...good, format: 'foxsdr-update-manifest-sig' }));
  assert.ok(refused({ ...good, version: 2 }));
  assert.ok(refused({ ...good, alg: 'rsa' }));
  assert.ok(refused({ ...good, keyId: 'XYZ' }));
  assert.ok(refused({ ...good, payload: 'not base64!' }));
  assert.ok(refused({ ...good, sig: Buffer.alloc(63).toString('base64') }));
  assert.ok(refused({ ...good, sig: 7 }));
  assert.ok(refused('[]'));
  assert.ok(refused('{nope'));
  assert.ok(refused('x'.repeat(500 * 1024)));
});

test('domain separation: a signature made with the update-manifest prefix does not verify as a registry', async () => {
  const { sign } = await import('node:crypto');
  const wrong = sign(null, Buffer.concat([Buffer.from('FoxSDR update manifest v1\n', 'latin1'), payload]), key);
  const env = JSON.stringify({ format: 'foxsdr-link-registry-sig', version: 1, alg: 'ed25519', keyId: keyIdOf(pub), payload: payload.toString('base64'), sig: wrong.toString('base64') });
  assert.equal(verifyEnvelope(env, [pub]).error, 'bad signature');
  assert.equal(PREFIX, 'FoxSDR link registry v1\n');
});

test('the build refuses a registry that does not validate and an envelope that is not of these exact bytes', () => {
  const text = registryText();
  assert.deepEqual(render(text, null).problems, []);
  assert.ok(render('{"x":1}', null).problems.length > 0);
  const edited = registryText(registry(undefined, { registry_version: 4 }));
  const envForOld = makeEnvelope(key, Buffer.from(text, 'utf8'));
  const stale = render(edited, envForOld);
  assert.match(stale.problems.join(), /payload is not exactly/);
  assert.deepEqual(render(text, envForOld).problems, []);
  assert.deepEqual(render(text, envForOld, pub.toString('hex')).problems, []);
  const otherPub = rawPublicKey(generateKeyPairSync('ed25519').privateKey).toString('hex');
  assert.match(render(text, envForOld, otherPub).problems.join(), /unknown key/);
  assert.match(render(text, 'garbage').problems.join(), /signed envelope/);
});

test('the signing tool takes a seed FILE of 32 raw bytes or 64 hex, refuses anything else, and with no arguments exits 2 touching nothing', () => {
  assert.equal(seedFromFileBytes(Buffer.alloc(32, 7)).length, 32);
  assert.equal(seedFromFileBytes(Buffer.from('ab'.repeat(32) + '\n')).length, 32);
  assert.throws(() => seedFromFileBytes(Buffer.from('short')), /32 raw bytes/);
  assert.throws(() => privateKeyFromSeed(Buffer.alloc(31)), /32 bytes/);
  const r = spawnSync(process.execPath, [`${here}tools/sign-registry.mjs`], { encoding: 'utf8', timeout: 20000 });
  assert.equal(r.status, 2);
  assert.match(r.stderr, /usage/);
  const r2 = spawnSync(process.execPath, [`${here}tools/sign-registry.mjs`, '--seed-file', `${here}no-such-seed-file`], { encoding: 'utf8', timeout: 20000 });
  assert.equal(r2.status, 2);
  assert.match(r2.stderr, /not signed/);
  assert.equal(existsSync(`${here}registry/libraries.signed.json`), false, 'nothing was signed in this repository');
});

test('no private key or seed file sits anywhere in links-worker/', () => {
  const walk = (d) => readdirSync(d, { withFileTypes: true }).flatMap((e) => (e.isDirectory() ? (e.name === 'node_modules' || e.name === '.wrangler' ? [] : walk(`${d}${e.name}/`)) : [`${d}${e.name}`]));
  const files = walk(here);
  assert.ok(files.length > 15);
  for (const f of files) {
    assert.doesNotMatch(f, /\.(pem|key|seed|pfx|p12)$/i, f);
    if (/\.(md|toml|json|js|mjs)$/.test(f) && !f.includes('/test/')) {
      assert.doesNotMatch(readFileSync(f, 'utf8'), /BEGIN [A-Z ]*PRIVATE KEY/, f);
    }
  }
});
