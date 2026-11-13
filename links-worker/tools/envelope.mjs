// The signature envelope for the registry: the SAME scheme as src/core/sys_manifest_sig.hpp (the
// update-manifest envelope), with a different format word and a different domain-separation
// prefix so a signature made for one purpose can never be replayed as the other.
//
//   {"format":"foxsdr-link-registry-sig","version":1,"alg":"ed25519","keyId":"<16 hex>",
//    "payload":"<base64 of the registry bytes>","sig":"<base64, 64 bytes>"}
//
// The signature is over  "FoxSDR link registry v1\n" + payload bytes. keyId is the first 8 bytes of
// SHA-256 of the 32-byte public key, so keys can be rotated (the program carries a LIST).
//
// NO KEY IS GENERATED OR STORED HERE. makeEnvelope takes a private key the caller holds;
// tools/sign-registry.mjs (never run in this repository) reads a seed FILE the owner keeps offline.
// Node-only (node:crypto): the Worker never signs and never verifies - it serves the envelope as a file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import { createHash, createPrivateKey, createPublicKey, sign, verify } from 'node:crypto';

export const FORMAT = 'foxsdr-link-registry-sig';
export const VERSION = 1;
export const PREFIX = 'FoxSDR link registry v1\n';
export const MAX_ENVELOPE_BYTES = 400 * 1024;

const PKCS8_ED25519_PREFIX = Buffer.from('302e020100300506032b657004220420', 'hex');

export function privateKeyFromSeed(seed32) {
  if (!Buffer.isBuffer(seed32) || seed32.length !== 32) throw new Error('an Ed25519 seed is exactly 32 bytes');
  return createPrivateKey({ key: Buffer.concat([PKCS8_ED25519_PREFIX, seed32]), format: 'der', type: 'pkcs8' });
}

export function publicKeyFromRaw(raw32) {
  if (!Buffer.isBuffer(raw32) || raw32.length !== 32) throw new Error('an Ed25519 public key is exactly 32 bytes');
  return createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: raw32.toString('base64url') }, format: 'jwk' });
}

export function rawPublicKey(keyObject) {
  const pub = keyObject.type === 'private' ? createPublicKey(keyObject) : keyObject;
  return Buffer.from(pub.export({ format: 'jwk' }).x, 'base64url');
}

export function keyIdOf(raw32) {
  return createHash('sha256').update(raw32).digest().subarray(0, 8).toString('hex');
}

export function makeEnvelope(privateKey, payloadBytes) {
  const payload = Buffer.from(payloadBytes);
  const sig = sign(null, Buffer.concat([Buffer.from(PREFIX, 'latin1'), payload]), privateKey);
  return JSON.stringify({
    format: FORMAT,
    version: VERSION,
    alg: 'ed25519',
    keyId: keyIdOf(rawPublicKey(privateKey)),
    payload: payload.toString('base64'),
    sig: sig.toString('base64'),
  });
}

const B64_RE = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;

// Structure only (no key needed): { ok, error?, payload? }. The payload is returned as a Buffer.
export function parseEnvelope(text) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > MAX_ENVELOPE_BYTES) return { ok: false, error: 'too large or not text' };
  let e;
  try { e = JSON.parse(text); } catch { return { ok: false, error: 'not JSON' }; }
  if (e === null || typeof e !== 'object' || Array.isArray(e)) return { ok: false, error: 'not an object' };
  if (e.format !== FORMAT) return { ok: false, error: 'unknown format' };
  if (e.version !== VERSION) return { ok: false, error: 'unknown version' };
  if (e.alg !== 'ed25519') return { ok: false, error: 'unknown algorithm' };
  if (typeof e.keyId !== 'string' || !/^[0-9a-f]{16}$/.test(e.keyId)) return { ok: false, error: 'bad keyId' };
  if (typeof e.payload !== 'string' || !B64_RE.test(e.payload)) return { ok: false, error: 'bad payload encoding' };
  if (typeof e.sig !== 'string' || !B64_RE.test(e.sig) || Buffer.from(e.sig, 'base64').length !== 64) return { ok: false, error: 'bad signature encoding' };
  return { ok: true, keyId: e.keyId, payload: Buffer.from(e.payload, 'base64'), sig: Buffer.from(e.sig, 'base64') };
}

// Verifies against a LIST of trusted raw public keys (Buffers). { ok, error?, payload? }.
export function verifyEnvelope(text, trustedRawKeys) {
  const p = parseEnvelope(text);
  if (!p.ok) return p;
  if (!trustedRawKeys || trustedRawKeys.length === 0) return { ok: false, error: 'no trusted key' };
  const raw = trustedRawKeys.find((k) => keyIdOf(k) === p.keyId);
  if (!raw) return { ok: false, error: 'unknown key' };
  const good = verify(null, Buffer.concat([Buffer.from(PREFIX, 'latin1'), p.payload]), publicKeyFromRaw(raw), p.sig);
  return good ? { ok: true, payload: p.payload } : { ok: false, error: 'bad signature' };
}
