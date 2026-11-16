// The field tables of the radio journal's kinds (docs/radio-reports-contract.md, section 3): the ONE definition that the
// Worker's validator (radio-reports.js) reads and that the site's Go validator reads too, as the JSON copy
// test-fixtures/radio-journal-spec.json (written from this file by make-radio-fixtures.mjs, and compared with it by a test).
// Both sides interpret this table, so a limit cannot differ between them: the table is the contract's field tables.
//
// A field is { name, type, req?, ... }:
//   enum  { values | list }          a string that is one of the values (`list` names an entry of `lists`)
//   int   { lo, hi }                 a JSON integer in [lo, hi]
//   num   { lo, hi }                 a finite JSON number in [lo, hi]
//   bool                             true or false
//   str   { lo, hi, re? }            a string of lo..hi characters (UTF-16 units) that matches `re` when it is given
//   text  { lo, hi }                 free text of lo..hi characters; the text rules of section 5 apply to it
//   arr   { item, max }              an array of at most `max` items, each checked as `item` (an object with type and limits)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

export const LISTS = {
  drivers: ['rtlsdr', 'hackrf', 'airspy', 'airspyhf', 'sdrplay', 'mirisdr', 'rx888', 'pluto', 'aor', 'hydrasdr', 'rtltcp', 'soapy', 'soundcard', 'other'],
  reasonClasses: ['busy', 'driver', 'bind', 'absent', 'timeout', 'vendor', 'rate', 'control', 'other'],
  triggers: ['startup-restore', 'user-pick', 'saved-retry', 'patch-radio', 'auto-reopen', 'other'],
  channels: ['store', 'installer', 'appimage', 'tarball', 'android', ''],
  updateChannels: ['stable', 'beta', ''],
  // A band NAME, never a frequency: the coarse names the owner's rule allows ("VHF air band").
  bands: ['lf', 'mf', 'hf', 'vhf-low', 'fm-broadcast', 'air-band', 'vhf-high', 'uhf', 'l-band', 'shf', 'other'],
  audioApis: ['mme', 'dsound', 'wasapi', 'wdmks', 'asio', 'alsa', 'jack', 'oss', 'coreaudio', 'none', 'other'],
  audioClasses: ['speakers', 'headphones', 'usb', 'bluetooth', 'hdmi', 'virtual', 'other', 'none'],
  windows: ['new', 'legacy', ''],
};

const D = { name: 'driver', type: 'enum', list: 'drivers', req: true };
const MODEL = { name: 'model', type: 'model', req: true };            // 1..32, unchanged by the Worker's scrubModel
const SERIAL = { name: 'serial', type: 'serial', req: true };         // '' or ^[A-Za-z0-9._-]{1,40}$
const VERSION_RE = '^[A-Za-z0-9._+ -]{1,48}$';
const COMMON = [
  D, MODEL, SERIAL,
  { name: 'v', type: 'str', lo: 1, hi: 48, req: true },
  { name: 'firmware', type: 'str', lo: 0, hi: 48, re: '^(?:[A-Za-z0-9._+ -]{1,48})?$' },
  { name: 'library', type: 'str', lo: 0, hi: 48, re: '^(?:[A-Za-z0-9._+ -]{1,48})?$' },
  { name: 'dropped', type: 'int', lo: 0, hi: 1000000 },
];

export const KIND_SPECS = {
  // One radio seen by a scan, the first-run radio check or a hot-plug, and what the driver binding looks like.
  radio_found: {
    maxBytes: 4096, radio: true, fields: [...COMMON,
      { name: 'binding', type: 'enum', values: ['usable', 'wrong-driver', 'vendor-missing', 'not-visible', 'no-device', 'not-checked'], req: true },
      { name: 'source', type: 'enum', values: ['scan', 'first-run-check', 'hotplug'], req: true },
      { name: 'usbId', type: 'str', lo: 9, hi: 9, re: '^[0-9a-f]{4}:[0-9a-f]{4}$' },
      { name: 'usbHost', type: 'text', lo: 1, hi: 64 },
    ],
  },
  // An open that began (phase start) or succeeded (phase ok). A failed open is radio_fail.
  radio_open: {
    maxBytes: 4096, radio: true, fields: [...COMMON,
      { name: 'phase', type: 'enum', values: ['start', 'ok'], req: true },
      { name: 'trigger', type: 'enum', list: 'triggers', req: true },
      { name: 'attempt', type: 'int', lo: 1, hi: 10000, req: true },
      { name: 'openMs', type: 'int', lo: 0, hi: 3600000 },
      { name: 'sampleRateSps', type: 'int', lo: 0, hi: 1000000000 },
      { name: 'band', type: 'enum', list: 'bands' },
    ],
  },
  // The settings applied to an open radio (or one the radio refused).
  radio_settings: {
    maxBytes: 4096, radio: true, fields: [...COMMON,
      { name: 'why', type: 'enum', values: ['open', 'user', 'auto', 'restore'], req: true },
      { name: 'sampleRateSps', type: 'int', lo: 0, hi: 1000000000 },
      { name: 'bandwidth', type: 'int', lo: 0, hi: 1000000000 },
      { name: 'gainMode', type: 'enum', values: ['auto', 'manual', 'agc', 'other'] },
      { name: 'gainDb', type: 'num', lo: -200, hi: 200 },
      { name: 'ppm', type: 'num', lo: -2000, hi: 2000 },
      { name: 'biasTee', type: 'bool' },
      { name: 'antenna', type: 'text', lo: 1, hi: 24 },
      { name: 'directSampling', type: 'enum', values: ['off', 'i', 'q'] },
      { name: 'decimation', type: 'int', lo: 1, hi: 65536 },
      { name: 'band', type: 'enum', list: 'bands' },
      { name: 'refused', type: 'enum', values: ['rate', 'bandwidth', 'gain', 'ppm', 'bias', 'antenna', 'direct', 'decimation'] },
      { name: 'reason', type: 'text', lo: 1, hi: 200 },
    ],
  },
  // Stream health over a window, and what is attached to the radio.
  radio_stream: {
    maxBytes: 4096, radio: true, fields: [...COMMON,
      { name: 'windowSec', type: 'int', lo: 1, hi: 3600, req: true },
      { name: 'samplesPerSec', type: 'int', lo: 0, hi: 1000000000, req: true },
      { name: 'expectedSps', type: 'int', lo: 0, hi: 1000000000 },
      { name: 'drops', type: 'int', lo: 0, hi: 1000000000, req: true },
      { name: 'ringHighPct', type: 'int', lo: 0, hi: 100 },
      { name: 'usbErrors', type: 'int', lo: 0, hi: 1000000000 },
      { name: 'stalls', type: 'int', lo: 0, hi: 1000000 },
      { name: 'dspLoadPct', type: 'int', lo: 0, hi: 1000 },
      { name: 'openSec', type: 'int', lo: 0, hi: 2592000 },
      { name: 'modes', type: 'arr', max: 8, item: { type: 'str', lo: 1, hi: 12, re: '^[A-Za-z0-9+-]{1,12}$' } },
      { name: 'plugins', type: 'arr', max: 16, item: { type: 'str', lo: 1, hi: 48, re: '^[a-z0-9._-]{1,48}$' } },
      { name: 'pluginFaults', type: 'int', lo: 0, hi: 1000000 },
      { name: 'note', type: 'enum', values: ['', 'sounds-wrong', 'audio-silent', 'audio-clipping', 'nothing-decoded'] },
    ],
  },
  // A recovery: a reopen, a restart of the stream, a vendor-call retry... (`what` is the application's own recovered word).
  radio_recovery: {
    maxBytes: 4096, radio: true, fields: [...COMMON,
      { name: 'phase', type: 'enum', values: ['start', 'ok', 'gave-up'], req: true },
      { name: 'what', type: 'str', lo: 3, hi: 24, re: '^[a-z0-9]{3,24}$', req: true },
      { name: 'attempt', type: 'int', lo: 1, hi: 10000, req: true },
      { name: 'reasonClass', type: 'enum', list: 'reasonClasses' },
      { name: 'reason', type: 'text', lo: 1, hi: 400 },
      { name: 'ms', type: 'int', lo: 0, hi: 3600000 },
    ],
  },
  // A radio closed, and why.
  radio_close: {
    maxBytes: 4096, radio: true, fields: [...COMMON,
      { name: 'cause', type: 'enum', values: ['user', 'switch-radio', 'quit', 'died', 'unplugged', 'fault', 'error', 'other'], req: true },
      { name: 'openSec', type: 'int', lo: 0, hi: 2592000, req: true },
      { name: 'reason', type: 'text', lo: 1, hi: 400 },
    ],
  },
  // Once a session: the machine around the radios. No radio key.
  session_env: {
    maxBytes: 4096, radio: false, fields: [
      { name: 'v', type: 'str', lo: 1, hi: 48, req: true },
      { name: 'os', type: 'str', lo: 1, hi: 40, req: true },
      { name: 'arch', type: 'str', lo: 1, hi: 8, req: true },
      { name: 'ch', type: 'enum', list: 'channels' },
      { name: 'uch', type: 'enum', list: 'updateChannels' },
      { name: 'window', type: 'enum', list: 'windows' },
      { name: 'launches', type: 'int', lo: 1, hi: 1000000, req: true },
      { name: 'cpu', type: 'text', lo: 1, hi: 64, req: true },
      { name: 'cpuThreads', type: 'int', lo: 1, hi: 1024 },
      { name: 'ramMb', type: 'int', lo: 0, hi: 4194304 },
      { name: 'gpu', type: 'text', lo: 1, hi: 64 },
      { name: 'gpuDriver', type: 'str', lo: 1, hi: 48, re: VERSION_RE },
      { name: 'usbHosts', type: 'arr', max: 8, item: { type: 'text', lo: 1, hi: 64 } },
      { name: 'audioApi', type: 'enum', list: 'audioApis', req: true },
      { name: 'audioClass', type: 'enum', list: 'audioClasses' },
      { name: 'displayCount', type: 'int', lo: 0, hi: 16 },
      { name: 'displayScalePct', type: 'arr', max: 8, item: { type: 'int', lo: 25, hi: 1000 } },
      { name: 'diagOn', type: 'bool', req: true },
    ],
  },
};

// The extended running-now beat (section 3.10): at most eight radio keys.
export const BEAT_MAX_RADIOS = 8;
export const BEAT_KEY_RE = '^[a-z0-9-]{0,40}:(?:-|[A-Za-z0-9._-]{1,40})$';

export const SPEC = { schema: 2, lists: LISTS, kinds: KIND_SPECS, beat: { maxRadios: BEAT_MAX_RADIOS, keyRe: BEAT_KEY_RE } };
