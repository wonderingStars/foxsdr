import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import * as v from '../src/validate.js';

const schema = JSON.parse(readFileSync(new URL('../registry/schema.json', import.meta.url), 'utf8'));
const rowSchema = schema.$defs.row;

test('registry/schema.json agrees with src/validate.js on fields, enums and required lists', () => {
  assert.deepEqual(Object.keys(schema.properties).sort(), [...v.TOP_FIELDS].sort());
  assert.deepEqual([...schema.required].sort(), [...v.TOP_FIELDS].sort());
  assert.deepEqual(Object.keys(rowSchema.properties).sort(), [...v.ROW_FIELDS].sort());
  assert.deepEqual([...rowSchema.required].sort(), v.ROW_FIELDS.filter((f) => !v.OPTIONAL_ROW_FIELDS.includes(f)).sort());
  assert.deepEqual(rowSchema.properties.status.enum, v.STATUSES);
  assert.deepEqual(rowSchema.properties.fetch_allowed.enum, v.FETCH_ALLOWED);
  assert.deepEqual(rowSchema.properties.install_kind.enum, v.INSTALL_KINDS);
  const dl = rowSchema.properties.download.oneOf.find((x) => x.type === 'object');
  assert.deepEqual(Object.keys(dl.properties).sort(), [...v.DOWNLOAD_FIELDS].sort());
  assert.equal(schema.properties.schema_version.const, v.SCHEMA_VERSION);
  assert.equal(schema.properties.libraries.maxItems, v.MAX_ROWS);
  assert.equal(schema.additionalProperties, false);
  assert.equal(rowSchema.additionalProperties, false);
});
