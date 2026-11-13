// node tools/sign-registry.mjs --seed-file <file> [--in registry/libraries.json] [--out registry/libraries.signed.json]
//
// DORMANT: written, tested only through makeEnvelope with a key made in memory, NEVER RUN in this
// repository. It reads the owner's 32-byte Ed25519 seed from a FILE the owner keeps offline (raw
// bytes, or 64 hex characters), and writes the signature envelope. It generates no key, stores no
// key and prints no key. The equivalent for the update manifest is tools/manifest_sign.cpp.
// Where the seed lives and who holds it is an OWNER DECISION (DEPLOY.md, "Owner decisions").
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { keyIdOf, makeEnvelope, privateKeyFromSeed, rawPublicKey } from './envelope.mjs';

const here = fileURLToPath(new URL('.', import.meta.url));

function arg(name) {
  const i = process.argv.indexOf(name);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

export function seedFromFileBytes(bytes) {
  if (bytes.length === 32) return Buffer.from(bytes);
  const s = bytes.toString('utf8').trim();
  if (/^[0-9a-fA-F]{64}$/.test(s)) return Buffer.from(s, 'hex');
  throw new Error('the seed file must hold exactly 32 raw bytes or 64 hex characters');
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const seedFile = arg('--seed-file');
  if (!seedFile) {
    console.error('usage: node tools/sign-registry.mjs --seed-file <file> [--in <registry>] [--out <envelope>]');
    process.exit(2);
  }
  try {
    const inPath = arg('--in') || `${here}../registry/libraries.json`;
    const outPath = arg('--out') || `${here}../registry/libraries.signed.json`;
    const key = privateKeyFromSeed(seedFromFileBytes(readFileSync(seedFile)));
    const payload = readFileSync(inPath);
    if (payload.length === 0) throw new Error('the registry file is empty');
    writeFileSync(outPath, makeEnvelope(key, payload), 'utf8');
    console.log(`signed ${inPath} -> ${outPath} (keyId ${keyIdOf(rawPublicKey(key))})`);
  } catch (e) {
    console.error(`not signed: ${e.message}`);
    process.exit(2);
  }
}
