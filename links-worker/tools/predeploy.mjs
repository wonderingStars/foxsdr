// node tools/predeploy.mjs   - the gate in front of `wrangler deploy`. Reads files, writes none,
// contacts nothing. Exit 0 only when every check passes.
//   1. registry/libraries.json validates
//   2. src/registry-bundle.js is exactly what tools/build-bundle.mjs would write now
//   3. wrangler.toml has a real KV namespace id (not the placeholder)
//   4. wrangler.toml names a route (the hostname is an owner decision) or says so
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { BUNDLE_PATH, REGISTRY_PATH, SIGNED_PATH, render } from './build-bundle.mjs';

const here = fileURLToPath(new URL('.', import.meta.url));
const problems = [];

const registryText = readFileSync(REGISTRY_PATH, 'utf8');
const signedText = existsSync(SIGNED_PATH) ? readFileSync(SIGNED_PATH, 'utf8') : null;
const built = render(registryText, signedText, process.env.LINKS_PUBKEY_HEX || null);
problems.push(...built.problems);
const bundle = existsSync(BUNDLE_PATH) ? readFileSync(BUNDLE_PATH, 'utf8') : '';
if (bundle !== built.source) problems.push('src/registry-bundle.js is out of date: run  node tools/build-bundle.mjs');

const toml = readFileSync(`${here}../wrangler.toml`, 'utf8');
const active = toml.split('\n').filter((l) => !l.trim().startsWith('#')).join('\n');
if (/REPLACE_WITH_KV_NAMESPACE_ID/.test(active)) problems.push('wrangler.toml still has the KV namespace placeholder: run  npx wrangler kv namespace create LINKS_STATE  and paste its id (DEPLOY.md step 2)');
if (!/^\s*\[\[routes\]\]/m.test(active)) problems.push('wrangler.toml has no [[routes]] block: decide the hostname and uncomment it (DEPLOY.md step 3)');

if (problems.length) {
  console.error('NOT READY TO DEPLOY:');
  for (const p of problems) console.error(`  - ${p}`);
  process.exit(1);
}
console.log('predeploy checks passed');
