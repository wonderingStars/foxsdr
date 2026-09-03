/*
 * Fox & Schirmer Radar Unit - the ADS-B interface, driven by FoxSDR.
 *
 * This is a SEPARATE INTERFACE, not a view inside the application. FoxSDR
 * hides its own window when this page is opened and shows it again when the
 * POWER button is switched to standby - that is what the power button means
 * here, and it is the only route back, exactly as asked.
 *
 * Everything on the panel comes from the receiver through /api/status. The
 * page invents nothing: a value the radio has not reported is drawn as a
 * hatched "NO REPORT" rather than a plausible number, because a panel whose
 * unknowns look like readings is worse than one with gaps in it.
 */
'use strict';

// The range ladder, in nautical miles, as the design's knob specifies it.
var RANGES = [20, 40, 80, 160, 200];

var BOOT_LINES = [
  'FOX & SCHIRMER  V2.4',
  'FOXSDR LINK .......OK',
  'ADS-B 1090MHZ .....OK',
  'DECODER ARMED',
];

var M_PER_NM = 1852.0;
var M_PER_KM = 1000.0;
var SCOPE_R = 200;      // the scope is 400 px across
var GLYPH_INSET = 22;   // keep the outermost contacts inside the bezel

// A reading older than this is not a reading. The handoff is explicit that a
// frozen figure must never read as a healthy one: past STALE_MS it takes the
// warning colour and says STALE, past UNKNOWN_MS it becomes a hatched unknown.
var STALE_MS = 2000;
var UNKNOWN_MS = 10000;

// THE ALTITUDE PALETTE, entry for entry the one the desktop map uses
// (src/gui/track_metrics.hpp altBandStyle). Two displays of the same air that
// disagreed about what green means would be worse than one of them having no
// colour at all, so this is copied deliberately rather than re-chosen.
//
// Warm to cool, and no red anywhere: red is the emergency colour and the
// unknown-altitude colour, and both of those have to win.
var ALT_BANDS = [
  { ft: 1000, rgb: [255, 122, 20], label: '< 1 kft' },
  { ft: 5000, rgb: [255, 190, 30], label: '1-5 kft' },
  { ft: 10000, rgb: [156, 220, 40], label: '5-10 kft' },
  { ft: 20000, rgb: [0, 205, 130], label: '10-20 kft' },
  { ft: 30000, rgb: [40, 190, 240], label: '20-30 kft' },
  { ft: Infinity, rgb: [130, 140, 255], label: '> 30 kft' }
];

function altBand(altM) {
  if (!isNum(altM)) { return -1; }
  var ft = altM * 3.28084;
  for (var i = 0; i < ALT_BANDS.length; ++i) {
    if (ft < ALT_BANDS[i].ft) { return i; }
  }
  return ALT_BANDS.length - 1;
}

function bandColour(band, alpha) {
  var a = (alpha === undefined) ? 1 : alpha;
  if (band < 0) { return 'rgba(134,214,74,' + a + ')'; }
  var c = ALT_BANDS[band].rgb;
  return 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + a + ')';
}

function altColour(altM, alpha) { return bandColour(altBand(altM), alpha); }

// WHERE EACH CONTACT HAS BEEN, and how high it was at each point.
//
// Accumulated here from what the panel already polls rather than asked of the
// receiver: this page sees a position a second, and building the trail
// locally means it cannot disagree with the marker drawn at the end of it.
// The cost is honest and worth stating - a trail begins when the panel opens,
// not when the aircraft was first heard.
var TRAIL_MAX_POINTS = 256;
var TRAIL_MAX_TRACKS = 256;
var TRAIL_MIN_MOVE_M = 50;
var trails = new Map();

function recordTrail(list) {
  for (var i = 0; i < list.length; ++i) {
    var c = list[i];
    var t = trails.get(c.id);
    if (!t) {
      if (trails.size >= TRAIL_MAX_TRACKS) { continue; }
      t = [];
      trails.set(c.id, t);
    }
    var last = t.length > 0 ? t[t.length - 1] : null;
    if (last !== null) {
      // A stationary contact must not fill its trail with 256 copies of one
      // point - the same 50 m gate the desktop's altitude store applies.
      var dLat = (c.lat - last.lat) * 111320;
      var dLon = (c.lon - last.lon) * 111320 * Math.cos(toRad(c.lat));
      if (dLat * dLat + dLon * dLon < TRAIL_MIN_MOVE_M * TRAIL_MIN_MOVE_M) { continue; }
    }
    t.push({ lat: c.lat, lon: c.lon, altM: c.altM });
    if (t.length > TRAIL_MAX_POINTS) { t.shift(); }
  }
}

/* ------------------------------------------------------------------ *
 * Basemap tiles.
 *
 * The receiver serves them at api/tile/z/x/y, already fetched by whichever
 * basemap plugin is installed - this page never talks to a tile server
 * itself. Three rules are inherited from the browser map, each learned the
 * hard way there: at most three requests in flight, because a page that
 * floods its own origin starves its own status poll and the whole panel
 * freezes; a 202 means "ask again later" and is retried on a TIMER, never on
 * the next frame; and a 404 is remembered so nothing ever asks twice.
 * ------------------------------------------------------------------ */

function mercYn(lat) {
  var l = Math.max(-85.05112878, Math.min(85.05112878, lat));
  var sn = Math.sin(toRad(l));
  return 0.5 - Math.log((1 + sn) / (1 - sn)) / (4 * Math.PI);
}

var tileCache = new Map();
var tileFrame = 0;
var tileInFlight = 0;

function tileFor(z, x, y) {
  var key = z + '/' + x + '/' + y;
  var e = tileCache.get(key);
  if (e) {
    e.used = tileFrame;
    if (e.state === 'ready') { return e.bmp; }
    if (e.state !== 'pending' || performance.now() < e.retryAt) { return null; }
  } else {
    e = { state: 'pending', bmp: null, used: tileFrame, retryAt: 0 };
    tileCache.set(key, e);
  }
  if (tileInFlight >= 3) { return null; }
  e.state = 'loading';
  tileInFlight += 1;
  fetch('api/tile/' + z + '/' + x + '/' + y, { credentials: 'same-origin' })
    .then(function (r) {
      if (r.status === 200) {
        return r.blob()
          .then(function (b) { return createImageBitmap(b); })
          .then(function (bmp) { e.bmp = bmp; e.state = 'ready'; });
      }
      if (r.status === 404) { e.state = 'missing'; return null; }
      e.state = 'pending';
      e.retryAt = performance.now() + 500;
      return null;
    })
    .catch(function () {
      e.state = 'pending';
      e.retryAt = performance.now() + 2500;
    })
    .then(function () { tileInFlight = Math.max(0, tileInFlight - 1); });
  return null;
}

function pruneTiles() {
  tileFrame += 1;
  if (tileCache.size <= 384) { return; }
  var dead = [];
  tileCache.forEach(function (e, k) { if (e.used < tileFrame - 4) { dead.push(k); } });
  for (var i = 0; i < dead.length; ++i) {
    var e = tileCache.get(dead[i]);
    if (e && e.bmp && e.bmp.close) { e.bmp.close(); }
    tileCache.delete(dead[i]);
  }
}

// The scope's projection: Web Mercator about the receiver, scaled so the
// outermost ring is exactly the selected range. Null when the receiver has no
// position, which is the one case where a range display cannot mean anything.
function scopeView() {
  if (!state.rx) { return null; }
  var rangeM = RANGES[state.rangeIdx] * M_PER_NM;
  var ringPx = SCOPE_R - 12;
  var cosLat = Math.cos(toRad(state.rx.lat));
  return {
    cx: (state.rx.lon + 180) / 360,
    cy: mercYn(state.rx.lat),
    pixPerWorld: 40075016.686 * cosLat * ringPx / rangeM
  };
}

function projX(view, lon) {
  return (((lon + 180) / 360) - view.cx) * view.pixPerWorld + SCOPE_R;
}
function projY(view, lat) {
  return (mercYn(lat) - view.cy) * view.pixPerWorld + SCOPE_R;
}

function drawScopeMap(vis, sel) {
  var cv = $('scopeMap');
  var ctx = cv.getContext('2d');
  var dpr = window.devicePixelRatio || 1;
  var want = Math.round(2 * SCOPE_R * dpr);
  if (cv.width !== want) { cv.width = want; cv.height = want; }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, 2 * SCOPE_R, 2 * SCOPE_R);
  $('attrib').hidden = true;
  if (state.power !== 'on') { return; }

  var view = scopeView();
  if (view === null) { return; }
  pruneTiles();

  // Everything inside the tube and nothing outside it: the scope is a circle,
  // and a square tile edge showing past the bezel would give the whole
  // instrument away.
  ctx.save();
  ctx.beginPath();
  ctx.arc(SCOPE_R, SCOPE_R, SCOPE_R, 0, Math.PI * 2);
  ctx.clip();

  var bm = state.basemap;
  var drew = false;
  if (bm && bm.active) {
    var ts = bm.tileSize || 256;
    var z = Math.round(Math.log2(view.pixPerWorld / ts));
    z = Math.max(bm.minZoom || 0, Math.min(bm.maxZoom || 19, z));
    var n = Math.pow(2, z);
    var half = SCOPE_R / view.pixPerWorld;
    var tx0 = Math.floor((view.cx - half) * n);
    var tx1 = Math.floor((view.cx + half) * n);
    var ty0 = Math.max(0, Math.floor((view.cy - half) * n));
    var ty1 = Math.min(n - 1, Math.floor((view.cy + half) * n));
    if (tx1 - tx0 > 32) { tx1 = tx0 + 32; }
    if (ty1 - ty0 > 32) { ty1 = ty0 + 32; }
    for (var ty = ty0; ty <= ty1; ++ty) {
      for (var tr = tx0; tr <= tx1; ++tr) {
        var tx = ((tr % n) + n) % n;   // wrap across the antimeridian
        var bmp = tileFor(z, tx, ty);
        if (!bmp) { continue; }
        // Placed from the tile's own edges and rounded, so neighbours share a
        // pixel rather than leaving a hairline gap between them.
        var ax = Math.round((tr / n - view.cx) * view.pixPerWorld + SCOPE_R);
        var bx = Math.round(((tr + 1) / n - view.cx) * view.pixPerWorld + SCOPE_R);
        var ay = Math.round((ty / n - view.cy) * view.pixPerWorld + SCOPE_R);
        var by = Math.round(((ty + 1) / n - view.cy) * view.pixPerWorld + SCOPE_R);
        ctx.drawImage(bmp, ax, ay, bx - ax, by - ay);
        drew = true;
      }
    }
  }
  if (drew && bm && bm.attribution) {
    $('attrib').hidden = false;
    $('attrib').textContent = bm.attribution;
  }

  drawTrails(ctx, view, vis, sel);
  ctx.restore();
}

// The flight trails, coloured by the altitude the aircraft actually had along
// each leg - the same rule the desktop map's banded trail follows, including
// taking a segment's colour from the MEAN of its two ends rather than from
// one of them, so a climb changes colour halfway up rather than at a step.
function drawTrails(ctx, view, vis, sel) {
  if (!state.opts.trails) { return; }
  for (var i = 0; i < vis.length; ++i) {
    var c = vis[i];
    var pts = trails.get(c.id);
    if (!pts || pts.length < 2) { continue; }
    var isSel = sel !== null && c.id === sel.id;
    ctx.lineCap = 'round';
    for (var j = 1; j < pts.length; ++j) {
      var a = pts[j - 1];
      var b = pts[j];
      var mid = (isNum(a.altM) && isNum(b.altM)) ? 0.5 * (a.altM + b.altM)
              : (isNum(a.altM) ? a.altM : b.altM);
      // Older legs fade, so the direction of travel reads without an
      // arrowhead: the bright end is where the aircraft is now.
      var fade = 0.25 + 0.55 * (j / pts.length);
      ctx.strokeStyle = altColour(mid, isSel ? Math.min(1, fade + 0.3) : fade);
      ctx.lineWidth = isSel ? 2.0 : 1.2;
      ctx.beginPath();
      ctx.moveTo(projX(view, a.lon), projY(view, a.lat));
      ctx.lineTo(projX(view, b.lon), projY(view, b.lat));
      ctx.stroke();
    }
  }
}

// The altitude key, on the surface that uses the coding rather than behind a
// menu - the rule the desktop map follows too. It lists only the bands
// actually on screen, so it never explains a colour nothing is drawn in.
function renderAltKey(vis) {
  var el = $('altKey');
  if (state.power !== 'on' || vis.length === 0) { el.hidden = true; return; }
  var seen = [];
  for (var i = 0; i < vis.length; ++i) {
    var b = altBand(vis[i].altM);
    if (b >= 0 && seen.indexOf(b) < 0) { seen.push(b); }
  }
  if (seen.length === 0) { el.hidden = true; return; }
  seen.sort(function (a, b) { return a - b; });
  el.hidden = false;
  el.textContent = '';
  for (var k = 0; k < seen.length; ++k) {
    var row = document.createElement('div');
    var sw = document.createElement('i');
    sw.style.background = bandColour(seen[k], 1);
    row.appendChild(sw);
    row.appendChild(document.createTextNode(ALT_BANDS[seen[k]].label));
    el.appendChild(row);
  }
}

var state = {
  power: 'on',
  boot: -1,
  bootTimer: 0,
  screen: 'flight',
  rangeIdx: 4,
  selId: '',
  opts: { sweep: true, labels: true, trails: true, units: 'NM',
          filter: 'ALL', audio: 'OFF' },
  link: 'connecting',   // connecting | live | lost | demo
  tracks: [],
  signalDb: null,
  rx: null,             // {lat, lon} or null when the receiver has no position
  basemap: null,        // what the installed basemap plugin can serve
  running: false,
};

function $(id) { return document.getElementById(id); }

/* ------------------------------------------------------------------ *
 * Geometry. Range and bearing from the receiver to a contact.
 * ------------------------------------------------------------------ */

function toRad(d) { return d * Math.PI / 180.0; }
function toDeg(r) { return r * 180.0 / Math.PI; }

// Great-circle distance in metres. The scope's rings are true distances, so
// a flat approximation would put a 200 NM contact visibly off its ring at
// this latitude - cheap to do properly, so it is done properly.
function haversineM(lat1, lon1, lat2, lon2) {
  var R = 6371008.8;
  var dLat = toRad(lat2 - lat1);
  var dLon = toRad(lon2 - lon1);
  var a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
          Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) *
          Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return 2 * R * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

// Initial bearing, degrees clockwise from true north.
function bearingDeg(lat1, lon1, lat2, lon2) {
  var p1 = toRad(lat1), p2 = toRad(lat2), dl = toRad(lon2 - lon1);
  var y = Math.sin(dl) * Math.cos(p2);
  var x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  var b = toDeg(Math.atan2(y, x));
  return (b + 360) % 360;
}

/* ------------------------------------------------------------------ *
 * Formatting. Every one of these can be handed nothing and says so.
 * ------------------------------------------------------------------ */

function isNum(v) { return typeof v === 'number' && isFinite(v); }

function fmtAlt(altM) {
  if (!isNum(altM)) { return null; }
  return String(Math.round(altM * 3.28084 / 100) * 100) + ' FT';
}
function fmtSpeed(mps) {
  if (!isNum(mps)) { return null; }
  return String(Math.round(mps * 1.943844)) + ' KT';
}
function fmtCourse(deg) {
  if (!isNum(deg)) { return null; }
  var d = Math.round(deg) % 360;
  if (d < 0) { d += 360; }
  return ('00' + d).slice(-3) + ' DEG';
}
function fmtRange(m) {
  if (!isNum(m)) { return null; }
  if (state.opts.units === 'KM') { return (m / M_PER_KM).toFixed(1) + ' KM'; }
  return (m / M_PER_NM).toFixed(1) + ' NM';
}
function fmtPos(lat, lon) {
  var ns = lat >= 0 ? 'N' : 'S';
  var ew = lon >= 0 ? 'E' : 'W';
  return Math.abs(lat).toFixed(1) + ns + ' ' + ('00' + Math.abs(lon).toFixed(1)).slice(-5) + ew;
}

/* ------------------------------------------------------------------ *
 * Contacts. One place turns a status track into what the panel draws.
 * ------------------------------------------------------------------ */

function contacts() {
  if (state.power !== 'on') { return []; }
  var rx = state.rx;
  var out = [];
  for (var i = 0; i < state.tracks.length; ++i) {
    var t = state.tracks[i];
    if (!isNum(t.latDeg) || !isNum(t.lonDeg)) { continue; }
    // A track with no position is a real thing in ADS-B - an aircraft heard
    // by its identity messages alone - but it cannot be placed on a scope,
    // and a scope is what this interface is. It is counted, not drawn.
    var c = {
      id: t.id,
      label: (t.label || t.id || '').toUpperCase(),
      lat: t.latDeg,
      lon: t.lonDeg,
      altM: isNum(t.altM) ? t.altM : null,
      courseDeg: isNum(t.courseDeg) ? t.courseDeg : null,
      speedMps: isNum(t.speedMps) ? t.speedMps : null,
      ageMs: isNum(t.ageMs) ? t.ageMs : 0,
      emergency: ((t.flags || 0) & 1) !== 0,
      reg: t.reg || '',
      acType: t.acType || '',
      operator: t.acOperator || '',
      country: t.acCountry || '',
      rangeM: null,
      bearing: null,
    };
    if (rx) {
      c.rangeM = haversineM(rx.lat, rx.lon, c.lat, c.lon);
      c.bearing = bearingDeg(rx.lat, rx.lon, c.lat, c.lon);
    }
    out.push(c);
  }
  out.sort(function (a, b) {
    if (a.rangeM === null || b.rangeM === null) { return a.label < b.label ? -1 : 1; }
    return a.rangeM - b.rangeM;
  });
  return out;
}

function inRange(list) {
  var maxM = RANGES[state.rangeIdx] * M_PER_NM;
  var f = state.opts.filter;
  var out = [];
  for (var i = 0; i < list.length; ++i) {
    var c = list[i];
    if (c.rangeM === null || c.rangeM > maxM) { continue; }
    if (f === 'ALERT' && !c.emergency) { continue; }
    if (f === 'NAMED' && !c.label) { continue; }
    out.push(c);
  }
  return out;
}

function selected(vis) {
  for (var i = 0; i < vis.length; ++i) {
    if (vis[i].id === state.selId) { return vis[i]; }
  }
  return vis.length > 0 ? vis[0] : null;
}

/* ------------------------------------------------------------------ *
 * Rendering
 * ------------------------------------------------------------------ */

function buildBars(el, n) {
  el.textContent = '';
  for (var i = 0; i < n; ++i) {
    var d = document.createElement('div');
    d.className = 'bar';
    el.appendChild(d);
  }
}

function setBars(el, activeCount) {
  var kids = el.children;
  for (var i = 0; i < kids.length; ++i) {
    var on = state.power === 'on' && i < activeCount;
    kids[i].className = on ? 'bar on' : 'bar';
  }
}

function buildDrum(el, len) {
  el.textContent = '';
  for (var i = 0; i < len; ++i) {
    var cell = document.createElement('div');
    cell.className = 'drum';
    var strip = document.createElement('div');
    strip.className = 'drum-strip';
    for (var n = 0; n < 10; ++n) {
      var dg = document.createElement('div');
      dg.className = 'drum-digit';
      dg.textContent = String(n);
      strip.appendChild(dg);
    }
    cell.appendChild(strip);
    el.appendChild(cell);
  }
}

function setDrum(el, value, len) {
  var s = ('000000' + String(Math.max(0, Math.min(999, value)))).slice(-len);
  for (var i = 0; i < len; ++i) {
    var strip = el.children[i].firstChild;
    strip.style.transform = 'translateY(' + (-Number(s.charAt(i)) * 38) + 'px)';
  }
}

function buildRings() {
  var host = $('rings');
  host.textContent = '';
  var fracs = [0.25, 0.5, 0.75, 1];
  for (var i = 0; i < fracs.length; ++i) {
    var f = fracs[i];
    var d = document.createElement('div');
    d.className = 'ring' + (i === 3 ? ' outer' : '');
    var size = f * 2 * (SCOPE_R - 12);
    d.style.width = size + 'px';
    d.style.height = size + 'px';
    d.style.marginLeft = (-f * (SCOPE_R - 12)) + 'px';
    d.style.marginTop = (-f * (SCOPE_R - 12)) + 'px';
    host.appendChild(d);
  }
}

// Ring labels carry the ladder's own numbers, so a user never has to work out
// what a ring is worth. Placed inside the ring on the vertical, the one place
// no contact bearing crowds them badly.
function setRingLabels() {
  var host = $('rings');
  var old = host.querySelectorAll('.ring-label');
  for (var i = 0; i < old.length; ++i) { host.removeChild(old[i]); }
  if (state.power !== 'on') { return; }
  var max = RANGES[state.rangeIdx];
  var fracs = [0.5, 1];
  for (var j = 0; j < fracs.length; ++j) {
    var f = fracs[j];
    var lab = document.createElement('div');
    lab.className = 'ring-label';
    lab.textContent = Math.round(max * f) + (state.opts.units === 'KM' ? 'km' : '');
    lab.style.left = '50%';
    lab.style.top = (SCOPE_R - f * (SCOPE_R - 12) + 9) + 'px';
    host.appendChild(lab);
  }
}

function renderTargets(vis, sel) {
  var host = $('targets');
  host.textContent = '';
  if (state.power !== 'on' || !state.rx) { return; }
  var maxM = RANGES[state.rangeIdx] * M_PER_NM;
  for (var i = 0; i < vis.length; ++i) {
    var c = vis[i];
    var isSel = sel !== null && c.id === sel.id;
    var rad = toRad(c.bearing - 90);
    var d = (c.rangeM / maxM) * (SCOPE_R - GLYPH_INSET);
    var x = SCOPE_R + Math.cos(rad) * d;
    var y = SCOPE_R + Math.sin(rad) * d;

    var wrap = document.createElement('div');
    wrap.className = 'tgt' + (isSel ? ' sel' : '');
    wrap.style.left = x + 'px';
    wrap.style.top = y + 'px';
    // Age fades a contact, so one that stopped reporting visibly recedes
    // instead of sitting there looking as current as the rest.
    wrap.style.opacity = String(Math.max(0.25, 1 - c.ageMs / 30000));
    wrap.setAttribute('data-id', c.id);

    var g = document.createElement('div');
    // No altitude reported: a hollow ring, not a different colour. Every
    // state on this panel is carried twice.
    g.className = 'glyph' + (isSel ? ' sel' : '') + (c.altM === null ? ' noalt' : '');
    if (c.altM !== null && c.courseDeg !== null) {
      g.style.transform = 'rotate(' + c.courseDeg + 'deg)';
    }
    // ALTITUDE BAND, the same coding the trail beneath it uses and the same
    // one the desktop map uses. Selection and distress both override it, in
    // that order, because a contact can be both and the emergency has to be
    // the one that shows.
    if (c.altM !== null && !isSel) {
      g.style.borderBottomColor = altColour(c.altM, 1);
      g.style.filter = 'drop-shadow(0 0 4px ' + altColour(c.altM, 0.8) + ')';
    }
    if (c.emergency) {
      g.style.borderBottomColor = '#ff5a3c';
      g.style.filter = 'drop-shadow(0 0 8px #ff5a3c)';
    }
    wrap.appendChild(g);

    if (state.opts.labels) {
      var lab = document.createElement('div');
      lab.className = 'tgt-label';
      lab.textContent = (c.emergency ? 'DISTRESS ' : '') + (c.label || c.id);
      wrap.appendChild(lab);
    }
    host.appendChild(wrap);
  }
}

function ageClass(ageMs) {
  if (ageMs >= UNKNOWN_MS) { return 'unknown'; }
  if (ageMs >= STALE_MS) { return 'stale'; }
  return '';
}

function addRow(host, key, value, ageMs) {
  var row = document.createElement('div');
  row.className = 'row';
  var k = document.createElement('span');
  k.className = 'k';
  k.textContent = key;
  var v = document.createElement('span');
  var cls = 'v';
  if (value === null || value === '') {
    v.textContent = 'NO REPORT';
    cls += ' unknown';
  } else {
    var ac = ageMs === undefined ? '' : ageClass(ageMs);
    if (ac === 'unknown') {
      v.textContent = 'NO REPORT';
      cls += ' unknown';
    } else {
      v.textContent = ac === 'stale' ? value + '  STALE' : value;
      if (ac) { cls += ' ' + ac; }
    }
  }
  v.className = cls;
  row.appendChild(k);
  row.appendChild(v);
  host.appendChild(row);
}

function renderFlight(sel) {
  var host = $('detailRows');
  host.textContent = '';
  if (sel === null) {
    $('carrierBar').hidden = true;
    $('selSub').textContent = state.rx
      ? 'NO CONTACT IN RANGE'
      : 'RECEIVER POSITION NOT SET';
    if (!state.rx) {
      addRow(host, 'POSITION', null);
      var note = document.createElement('div');
      note.className = 'carrier-sub';
      note.textContent = 'SET THE RECEIVER POSITION IN FOXSDR';
      host.appendChild(note);
    }
    return;
  }
  $('carrierBar').hidden = false;
  $('selCarrier').textContent = sel.operator || sel.label || sel.id;
  $('selSub').textContent = sel.label || sel.id;

  addRow(host, 'MODE S', sel.id.toUpperCase());
  addRow(host, 'TYPE', sel.acType || null);
  addRow(host, 'REG', sel.reg || null);
  addRow(host, 'ALT', fmtAlt(sel.altM), sel.ageMs);
  addRow(host, 'SPD', fmtSpeed(sel.speedMps), sel.ageMs);
  addRow(host, 'HDG', fmtCourse(sel.courseDeg), sel.ageMs);
  addRow(host, 'RANGE', fmtRange(sel.rangeM), sel.ageMs);
  addRow(host, 'BRG', fmtCourse(sel.bearing), sel.ageMs);
  // Squawk and route are NOT in the receiver's track data. The design has a
  // place for them, so the place stays and says plainly that nothing was
  // reported - inventing them would be the one thing this panel must not do.
  addRow(host, 'SQUAWK', null);
  addRow(host, 'ROUTE', null);
}

function renderHome(all, vis) {
  $('homeCount').textContent = state.power === 'on' ? String(vis.length) : '--';
  var host = $('homeStats');
  host.textContent = '';
  var noPos = 0, withAlt = 0, alert = 0, nearest = null;
  for (var i = 0; i < all.length; ++i) {
    if (all[i].rangeM === null) { noPos++; }
    if (all[i].altM !== null) { withAlt++; }
    if (all[i].emergency) { alert++; }
  }
  for (var j = 0; j < vis.length; ++j) {
    if (nearest === null || vis[j].rangeM < nearest.rangeM) { nearest = vis[j]; }
  }
  var stats = [
    ['TRACKED', String(all.length)],
    ['IN RANGE', String(vis.length)],
    ['WITH ALT', String(withAlt)],
    ['NO POSITION', String(noPos)],
    ['NEAREST', nearest ? fmtRange(nearest.rangeM) : null],
    ['ALERTS', String(alert)],
  ];
  for (var k = 0; k < stats.length; ++k) {
    var cell = document.createElement('div');
    var kk = document.createElement('div');
    kk.className = 'k';
    kk.textContent = stats[k][0];
    var vv = document.createElement('div');
    vv.className = 'v';
    vv.textContent = stats[k][1] === null ? 'NO REPORT' : stats[k][1];
    cell.appendChild(kk);
    cell.appendChild(vv);
    host.appendChild(cell);
  }
}

var OPTIONS = [
  ['SWEEP', function () { return state.opts.sweep ? 'ON' : 'OFF'; },
   function () { state.opts.sweep = !state.opts.sweep; }],
  ['LABELS', function () { return state.opts.labels ? 'ON' : 'OFF'; },
   function () { state.opts.labels = !state.opts.labels; }],
  ['TRAILS', function () { return state.opts.trails ? 'ON' : 'OFF'; },
   function () { state.opts.trails = !state.opts.trails; }],
  ['UNITS', function () { return state.opts.units; },
   function () { state.opts.units = state.opts.units === 'NM' ? 'KM' : 'NM'; }],
  // ALL / ALERT / NAMED, not the design's ALL / MIL / CIV: the receiver
  // reports no military-or-civil discriminator, and a filter that silently
  // did nothing would be worse than one that filters on what is actually
  // known. ALERT is the emergency squawk flag; NAMED is a contact whose
  // callsign has been heard.
  ['FILTER', function () { return state.opts.filter; },
   function () {
     state.opts.filter = state.opts.filter === 'ALL' ? 'ALERT'
                       : state.opts.filter === 'ALERT' ? 'NAMED' : 'ALL';
   }],
  ['ALERT TONE', function () { return state.opts.audio; },
   function () { state.opts.audio = state.opts.audio === 'OFF' ? 'ON' : 'OFF'; }],
];

function renderSys() {
  var host = $('options');
  host.textContent = '';
  for (var i = 0; i < OPTIONS.length; ++i) {
    (function (opt) {
      var row = document.createElement('div');
      row.className = 'opt';
      var k = document.createElement('span');
      k.className = 'k';
      k.textContent = opt[0];
      var pill = document.createElement('span');
      var val = opt[1]();
      pill.className = 'pill' + (val === 'OFF' ? ' off' : '');
      pill.textContent = val;
      row.appendChild(k);
      row.appendChild(pill);
      row.addEventListener('click', function () { opt[2](); render(); });
      host.appendChild(row);
    })(OPTIONS[i]);
  }
}

function renderKnob() {
  var host = $('knobTicks');
  host.textContent = '';
  for (var i = 0; i < RANGES.length; ++i) {
    var a = toRad(-135 + i * (270 / (RANGES.length - 1)));
    var t = document.createElement('div');
    t.className = 'tick' + (i === state.rangeIdx ? ' on' : '');
    t.textContent = String(RANGES[i]);
    t.style.left = (75 + Math.sin(a) * 62 - 14) + 'px';
    t.style.top = (52 - Math.cos(a) * 56 - 8) + 'px';
    host.appendChild(t);
  }
  $('knobDial').style.transform =
    'rotate(' + (-135 + state.rangeIdx * (270 / (RANGES.length - 1))) + 'deg)';
}

function renderBoot() {
  var host = $('screenBoot');
  host.textContent = '';
  var n = Math.min(state.boot, BOOT_LINES.length);
  for (var i = 0; i < n; ++i) {
    var d = document.createElement('div');
    d.textContent = BOOT_LINES[i];
    host.appendChild(d);
  }
}

function render() {
  var on = state.power === 'on';
  var booting = state.power === 'boot';

  var all = contacts();
  var vis = inRange(all);
  var sel = selected(vis);
  if (sel !== null) { state.selId = sel.id; }

  $('scopeLive').hidden = !on;
  $('scopeDark').hidden = on;
  $('scopeOffLabel').textContent = booting ? 'SELF TEST' : 'STANDBY';
  $('sweep').hidden = !(on && state.opts.sweep);
  $('lcdOff').hidden = on || booting;

  $('trackCount').textContent = on ? String(vis.length) : '--';
  $('rangeNm').textContent = on ? String(RANGES[state.rangeIdx]) : '--';
  $('modeLabel').textContent = 'MODE ' + state.opts.filter;
  $('homePos').textContent = state.rx ? fmtPos(state.rx.lat, state.rx.lon)
                                      : 'POSITION NOT SET';

  setRingLabels();
  // The map goes down first, then the trails on top of it, then the contact
  // markers as their own elements above both - the markers stay in the DOM
  // rather than on the canvas because they are what the user clicks.
  drawScopeMap(vis, sel);
  renderTargets(vis, sel);
  renderAltKey(vis);

  // The signal bargraph is the receiver's own figure, so it is the one that
  // must never freeze while looking alive: no link, no bars. The
  // demonstration is allowed to drive it because the whole panel is captioned
  // as demonstration data while it runs - a lost LIVE link is the case this
  // guard exists for.
  var sigOk = on && (state.link === 'live' || state.link === 'demo') &&
              isNum(state.signalDb);
  var sigFrac = sigOk ? (state.signalDb + 100) / 70 : 0;
  setBars($('sigBars'), Math.round(Math.max(0, Math.min(1, sigFrac)) * 18));
  $('sigLabel').textContent = sigOk ? String(Math.round(state.signalDb)) : '--';

  var altTop = null;
  for (var i = 0; i < vis.length; ++i) {
    if (vis[i].altM !== null && (altTop === null || vis[i].altM > altTop)) { altTop = vis[i].altM; }
  }
  setBars($('altBars'), altTop === null ? 0 : Math.round(Math.min(1, altTop / 13000) * 18));
  $('altLabel').textContent =
    (on && altTop !== null) ? 'FL' + ('000' + Math.round(altTop * 3.28084 / 100)).slice(-3) : '--';

  setDrum($('rangeDrum'), on ? RANGES[state.rangeIdx] : 0, 3);
  setDrum($('trackDrum'), on ? vis.length : 0, 3);

  $('tabHome').className = 'tab' + (on && state.screen === 'home' ? ' on' : '');
  $('tabFlight').className = 'tab' + (on && state.screen === 'flight' ? ' on' : '');
  $('tabSys').className = 'tab' + (on && state.screen === 'sys' ? ' on' : '');

  $('screenBoot').hidden = !booting;
  $('screenFlight').hidden = booting || state.screen !== 'flight';
  $('screenHome').hidden = booting || state.screen !== 'home';
  $('screenSys').hidden = booting || state.screen !== 'sys';

  if (booting) { renderBoot(); }
  if (on && state.screen === 'flight') { renderFlight(sel); }
  if (on && state.screen === 'home') { renderHome(all, vis); }
  if (on && state.screen === 'sys') { renderSys(); }

  $('powerLamp').className = 'power-lamp' + (on ? '' : ' off');
  $('powerLabel').textContent = on ? 'ON LINE' : booting ? 'BOOTING' : 'STANDBY';

  $('prevTrack').disabled = !on || vis.length === 0;
  $('nextTrack').disabled = !on || vis.length === 0;

  renderKnob();

  var link = $('linkState');
  if (state.link === 'live' || !on) {
    link.hidden = true;
  } else if (state.link === 'demo') {
    link.hidden = false;
    link.className = 'link-state demo';
    link.textContent = 'DEMONSTRATION DATA - NOT CONNECTED TO A RECEIVER';
  } else if (state.link === 'lost') {
    link.hidden = false;
    link.className = 'link-state';
    link.textContent = 'LINK TO FOXSDR LOST - FIGURES BELOW ARE NOT LIVE';
  } else {
    link.hidden = false;
    link.className = 'link-state';
    link.textContent = 'CONNECTING TO FOXSDR...';
  }
}

/* ------------------------------------------------------------------ *
 * Controls
 * ------------------------------------------------------------------ */

function step(dir) {
  var vis = inRange(contacts());
  if (vis.length === 0) { return; }
  var pos = -1;
  for (var i = 0; i < vis.length; ++i) {
    if (vis[i].id === state.selId) { pos = i; break; }
  }
  pos = (pos + dir + vis.length) % vis.length;
  state.selId = vis[pos].id;
  state.screen = 'flight';
  render();
}

// THE POWER BUTTON IS THE WAY BACK. FoxSDR hid its window when this page
// opened; switching to standby is what brings it back, and switching on
// again hides it once more. Nothing else on this panel touches the radio.
function togglePower() {
  clearInterval(state.bootTimer);
  if (state.power === 'on') {
    state.power = 'off';
    state.boot = -1;
    render();
    postRadar(false);
    return;
  }
  state.power = 'boot';
  state.boot = 0;
  state.screen = 'flight';
  render();
  postRadar(true);
  state.bootTimer = setInterval(function () {
    if (state.boot >= BOOT_LINES.length) {
      clearInterval(state.bootTimer);
      state.power = 'on';
      state.boot = -1;
    } else {
      state.boot += 1;
    }
    render();
  }, 320);
}

function knobDown(e) {
  var box = e.currentTarget.getBoundingClientRect();
  var cx = box.left + box.width / 2;
  var cy = box.top + box.height / 2;
  function move(ev) {
    var a = toDeg(Math.atan2(ev.clientX - cx, cy - ev.clientY));
    a = Math.max(-135, Math.min(135, a));
    var idx = Math.round((a + 135) / (270 / (RANGES.length - 1)));
    if (idx !== state.rangeIdx) { state.rangeIdx = idx; render(); }
  }
  function up() {
    window.removeEventListener('pointermove', move);
    window.removeEventListener('pointerup', up);
  }
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', up);
  move(e);
}

/* ------------------------------------------------------------------ *
 * The link to FoxSDR
 * ------------------------------------------------------------------ */

function postRadar(active) {
  if (state.link === 'demo') { return; }
  try {
    fetch('api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ radarActive: active }),
    }).catch(function () { /* the panel keeps working without the host */ });
  } catch (err) { /* same */ }
}

var missedPolls = 0;

function poll() {
  fetch('api/status', { cache: 'no-store' })
    .then(function (r) {
      if (!r.ok) { throw new Error('status ' + r.status); }
      return r.json();
    })
    .then(function (j) {
      missedPolls = 0;
      state.link = 'live';
      state.tracks = Array.isArray(j.tracks) ? j.tracks : [];
      state.signalDb = isNum(j.signalDb) ? j.signalDb : null;
      state.running = !!j.running;
      state.rx = (isNum(j.rxLatDeg) && isNum(j.rxLonDeg) && j.rxPositionSet)
        ? { lat: j.rxLatDeg, lon: j.rxLonDeg } : null;
      state.basemap = j.basemap || null;
      recordTrail(contacts());
      render();
    })
    .catch(function () {
      missedPolls += 1;
      // One missed poll is a hiccup; three in a row is a lost link, and the
      // panel says so rather than leaving the last good figures on screen
      // looking current.
      if (missedPolls >= 3 && state.link !== 'demo') {
        state.link = 'lost';
        state.tracks = [];
        state.signalDb = null;
        render();
      }
    });
}

/* ------------------------------------------------------------------ *
 * Demonstration data, for looking at the panel with no receiver running.
 * Reached only by opening the page with ?demo - never by a failed poll,
 * because a panel that invents contacts when its link drops is the exact
 * failure this design exists to prevent.
 * ------------------------------------------------------------------ */

function startDemo() {
  state.link = 'demo';
  state.rx = { lat: 51.5, lon: -2.1 };
  state.signalDb = -42;
  var seed = [
    ['4CA1FB', 'THY99D', 'TURKISH AIRLINES', 'B77W', 'TC-LJC', 10668, 250, 4, 144, 52],
    ['400939', 'BAW117', 'BRITISH AIRWAYS', 'A35K', 'G-XWBA', 11582, 258, 271, 38, 310],
    ['4CA2D1', 'EZY44RK', 'EASYJET', 'A320', 'G-EZTK', 6400, 207, 188, 24, 200],
    ['4CA8B2', 'RYR8GT', 'RYANAIR', 'B738', 'EI-DWF', 10058, 234, 95, 66, 130],
    ['394C1A', 'AFR1381', 'AIR FRANCE', 'A220', 'F-HZUB', 8534, 223, 148, 52, 88],
    ['3C6444', 'DLH9RT', 'LUFTHANSA', 'A333', 'D-AIKO', 10972, 242, 312, 97, 24],
    ['484AA1', 'KLM43W', 'KLM', 'E195', 'PH-EXV', 5943, 200, 61, 31, 355],
    ['8961BF', 'UAE7EX', 'EMIRATES', 'A388', 'A6-EOA', 12192, 263, 118, 178, 290],
    ['4008F2', 'VIR12N', 'VIRGIN ATLANTIC', 'B789', 'G-VBEL', 11278, 254, 264, 120, 236],
    ['342073', 'IBE3271', 'IBERIA', 'A20N', 'EC-NDN', 9144, 227, 201, 73, 172],
    ['4B1802', 'SWR86K', 'SWISS', 'BCS3', 'HB-JCA', 7925, 215, 76, 158, 64],
    ['471F5C', 'WZZ4TL', 'WIZZ AIR', 'A321', 'HA-LVK', 7010, 209, 335, 44, 120],
  ];
  state.tracks = seed.map(function (s, i) {
    // Place each one at its design range and bearing from the demo receiver.
    var d = s[8] * M_PER_NM / 6371008.8;
    var th = toRad(s[9]);
    var p1 = toRad(state.rx.lat), l1 = toRad(state.rx.lon);
    var p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(th));
    var l2 = l1 + Math.atan2(Math.sin(th) * Math.sin(d) * Math.cos(p1),
                             Math.cos(d) - Math.sin(p1) * Math.sin(p2));
    return {
      id: s[0], label: s[1], plugin: 'ADS-B',
      latDeg: toDeg(p2), lonDeg: toDeg(l2),
      // One contact deliberately reports no altitude, so the hollow glyph and
      // the hatched "NO REPORT" are visible in the demonstration too.
      altM: i === 6 ? null : s[5],
      speedMps: s[6], courseDeg: s[7],
      ageMs: i === 3 ? 4200 : 300,
      kind: 0, flags: i === 11 ? 1 : 0,
      reg: s[4], acType: s[3], acOperator: s[2], acCountry: '',
    };
  });
  render();
}

// Fly the demonstration contacts along their own headings at their own
// speeds, so the trails, the altitude banding and the range rings can all be
// seen doing what they do without a receiver attached.
function advanceDemo() {
  var dt = 1.0;
  for (var i = 0; i < state.tracks.length; ++i) {
    var t = state.tracks[i];
    if (!isNum(t.speedMps) || !isNum(t.courseDeg)) { continue; }
    var d = t.speedMps * dt / 6371008.8;
    var th = toRad(t.courseDeg);
    var p1 = toRad(t.latDeg), l1 = toRad(t.lonDeg);
    var p2 = Math.asin(Math.sin(p1) * Math.cos(d) + Math.cos(p1) * Math.sin(d) * Math.cos(th));
    var l2 = l1 + Math.atan2(Math.sin(th) * Math.sin(d) * Math.cos(p1),
                             Math.cos(d) - Math.sin(p1) * Math.sin(p2));
    t.latDeg = toDeg(p2);
    t.lonDeg = toDeg(l2);
    // One of them climbs, so a trail changes band along its length.
    if (i === 2 && isNum(t.altM)) { t.altM = Math.min(11000, t.altM + 60); }
  }
}

/* ------------------------------------------------------------------ *
 * Start-up
 * ------------------------------------------------------------------ */

function init() {
  buildBars($('sigBars'), 18);
  buildBars($('altBars'), 18);
  buildDrum($('rangeDrum'), 3);
  buildDrum($('trackDrum'), 3);
  buildRings();

  $('power').addEventListener('click', togglePower);
  $('knob').addEventListener('pointerdown', knobDown);
  $('prevTrack').addEventListener('click', function () { step(-1); });
  $('nextTrack').addEventListener('click', function () { step(1); });

  var tabs = document.querySelectorAll('.tab');
  for (var i = 0; i < tabs.length; ++i) {
    (function (tab) {
      tab.addEventListener('click', function () {
        if (state.power !== 'on') { return; }
        state.screen = tab.getAttribute('data-screen');
        render();
      });
    })(tabs[i]);
  }

  $('targets').addEventListener('click', function (ev) {
    var node = ev.target;
    while (node && node !== this && !node.getAttribute('data-id')) { node = node.parentNode; }
    if (node && node.getAttribute) {
      var id = node.getAttribute('data-id');
      if (id) { state.selId = id; state.screen = 'flight'; render(); }
    }
  });

  // The knob also answers the arrow keys, because a range control that only
  // works by dragging is unusable to anyone who cannot drag.
  window.addEventListener('keydown', function (ev) {
    if (ev.key === 'ArrowUp' || ev.key === 'ArrowRight') {
      state.rangeIdx = Math.min(RANGES.length - 1, state.rangeIdx + 1); render();
    } else if (ev.key === 'ArrowDown' || ev.key === 'ArrowLeft') {
      state.rangeIdx = Math.max(0, state.rangeIdx - 1); render();
    } else if (ev.key === 'n') { step(1); }
    else if (ev.key === 'p') { step(-1); }
  });

  if (window.location.search.indexOf('demo') >= 0) {
    startDemo();
    // The demonstration MOVES: a static one shows no trail at all, and the
    // trail is half of what this panel is being judged on.
    setInterval(function () { advanceDemo(); recordTrail(contacts()); render(); }, 1000);
    return;
  }

  render();
  postRadar(true);
  poll();
  setInterval(poll, 1000);
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', init);
} else {
  init();
}
