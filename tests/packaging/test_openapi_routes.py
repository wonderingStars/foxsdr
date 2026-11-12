"""Keeps docs/api/openapi.yaml honest against the web server's source, so the file cannot rot.

    python -I tests/packaging/test_openapi_routes.py

No YAML library is installed on the build machines and nothing may be downloaded, so this file carries a small, STRICT
parser for the YAML subset openapi.yaml is written in (block mappings and sequences, double-quoted or plain scalars,
`[a, b]` lists of plain scalars, `{}`, full-line comments only). Anything outside the subset is an error, so the file stays
inside the subset that is also plain YAML 1.2.

It checks that
  * every svr.Get/Post/Put/Delete/Patch route in src/net/web_server.cpp is documented with its method, and every
    documented operation is a real route (operations marked x-to-add-at-merge are the other way round: they must NOT be
    in the source yet, and the test says so when one lands);
  * each operation states the sign-in rule (security) the same way the handler does (authorised(req) or not);
  * every status code a handler can answer (deny(..., N), res.status = N) is a documented response;
  * path parameters are declared, $refs resolve, operationIds are unique;
  * ControlRequest has exactly the keys web_control.cpp accepts, with the ranges web_control.hpp declares;
  * Status has the keys statusJson() writes; the small answers (session, spectrum, login, control, tile) have the keys
    their handlers write.

Options (used by the break-it script to test the test against mutated copies):
    --yaml PATH  --source PATH  --control-cpp PATH  --control-hpp PATH
Prints "N checks, M failed"; exit code 1 if any failed.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


# ---------------------------------------------------------------------------------------------------------------------
# the YAML subset
# ---------------------------------------------------------------------------------------------------------------------
class YamlError(Exception):
    pass


def _scalar(text, lineno):
    t = text.strip()
    if t == '':
        raise YamlError('line %d: empty value' % lineno)
    if t[0] == '"':
        if len(t) < 2 or t[-1] != '"':
            raise YamlError('line %d: unterminated double-quoted string' % lineno)
        body = t[1:-1]
        out = []
        i = 0
        while i < len(body):
            c = body[i]
            if c == '\\':
                i += 1
                if i >= len(body) or body[i] not in '"\\':
                    raise YamlError('line %d: only \\" and \\\\ escapes are in the subset' % lineno)
                out.append(body[i])
            elif c == '"':
                raise YamlError('line %d: unescaped quote inside a quoted string' % lineno)
            else:
                out.append(c)
            i += 1
        return ''.join(out)
    if t[0] in "'|>&*!%@`":
        raise YamlError('line %d: %r is outside the subset' % (lineno, t[0]))
    if t == '{}':
        return {}
    if t == '[]':
        return []
    if t[0] == '[':
        if t[-1] != ']':
            raise YamlError('line %d: unterminated flow list' % lineno)
        items = [x.strip() for x in t[1:-1].split(',')]
        return [_plain(x, lineno) for x in items]
    if t[0] == '{':
        raise YamlError('line %d: only {} is allowed as a flow map' % lineno)
    return _plain(t, lineno)


def _plain(t, lineno):
    if t == '':
        raise YamlError('line %d: empty list item' % lineno)
    if ': ' in t or t.endswith(':') or ' #' in t:
        raise YamlError('line %d: plain scalar %r contains ": " or " #" (quote it)' % (lineno, t))
    if t == 'true':
        return True
    if t == 'false':
        return False
    if t == 'null':
        return None
    if re.fullmatch(r'-?\d+', t):
        return int(t)
    if re.fullmatch(r'-?\d+\.\d+', t):
        return float(t)
    return t


def _split_key(text, lineno):
    """'key: value' / 'key:' / '"200": value' -> (key, rest or None); None if the text is not a mapping entry."""
    if text.startswith('"'):
        m = re.match(r'"((?:[^"\\]|\\.)*)":(?:\s+(.*))?$', text)
        if not m:
            return None
        return m.group(1), m.group(2)
    m = re.match(r'([A-Za-z0-9_$/{}.\-]+):(?:\s+(.*))?$', text)
    if not m:
        return None
    return m.group(1), m.group(2)


def parse_yaml(text):
    lines = []
    for n, raw in enumerate(text.split('\n'), 1):
        if raw.endswith('\r'):
            raw = raw[:-1]
        if '\t' in raw:
            raise YamlError('line %d: tab character' % n)
        if raw.strip() == '' or raw.lstrip().startswith('#'):
            continue
        indent = len(raw) - len(raw.lstrip(' '))
        lines.append((n, indent, raw.strip()))
    if not lines:
        raise YamlError('empty document')
    pos = [0]

    def block(indent):
        n, ind, txt = lines[pos[0]]
        if ind != indent:
            raise YamlError('line %d: indentation %d, expected %d' % (n, ind, indent))
        if txt.startswith('- ') or txt == '-':
            return seq(indent)
        return mapping(indent)

    def mapping(indent):
        out = {}
        while pos[0] < len(lines):
            n, ind, txt = lines[pos[0]]
            if ind < indent:
                break
            if ind > indent:
                raise YamlError('line %d: unexpected indentation' % n)
            if txt.startswith('- '):
                break
            kv = _split_key(txt, n)
            if kv is None:
                raise YamlError('line %d: not a "key: value" entry: %r' % (n, txt))
            key, rest = kv
            if key in out:
                raise YamlError('line %d: duplicate key %r' % (n, key))
            pos[0] += 1
            if rest is None or rest == '':
                if pos[0] < len(lines) and lines[pos[0]][1] > indent:
                    out[key] = block(lines[pos[0]][1])
                elif pos[0] < len(lines) and lines[pos[0]][1] == indent and lines[pos[0]][2].startswith('- '):
                    out[key] = seq(indent)   # a list at the same indent as its key
                else:
                    raise YamlError('line %d: key %r has no value' % (n, key))
            else:
                out[key] = _scalar(rest, n)
        return out

    def seq(indent):
        out = []
        while pos[0] < len(lines):
            n, ind, txt = lines[pos[0]]
            if ind != indent or not (txt.startswith('- ') or txt == '-'):
                break
            body = txt[2:].strip() if txt != '-' else ''
            if body == '':
                raise YamlError('line %d: an empty list item' % n)
            kv = _split_key(body, n)
            if kv is not None:
                # a mapping inside the list: its first key sits on the dash line; rewrite that line as a mapping line
                key, rest = kv
                inner = indent + 2
                lines[pos[0]] = (n, inner, body)
                out.append(mapping(inner))
            else:
                pos[0] += 1
                out.append(_scalar(body, n))
        return out

    root = block(lines[0][1])
    if pos[0] != len(lines):
        raise YamlError('line %d: content after the document' % lines[pos[0]][0])
    return root


# ---------------------------------------------------------------------------------------------------------------------
class Suite:
    def __init__(self):
        self.checks = 0
        self.failed = 0

    def check(self, name, ok, detail=''):
        self.checks += 1
        if not ok:
            self.failed += 1
            print('FAIL  ' + name + (('  ' + detail) if detail else ''))


def norm_path(p):
    return re.sub(r'\{[^}]*\}', '{}', p)


def route_blocks(src):
    """[(method, normalised path, body text up to the next route)] for every svr.<Method>( in the file."""
    pat = re.compile(r'svr\.(Get|Post|Put|Delete|Patch)\(\s*(?:R"\((?P<raw>[^"]*?)\)"|"(?P<lit>[^"]*)")', re.S)
    ms = list(pat.finditer(src))
    out = []
    for i, m in enumerate(ms):
        end = ms[i + 1].start() if i + 1 < len(ms) else src.index('\nbool WebServer::Impl::start(', m.end())
        path = m.group('raw') if m.group('raw') is not None else m.group('lit')
        out.append((m.group(1).lower(), path, src[m.start():end]))
    return out


def split_call_args(text, start):
    """text[start] is just after the '(' of a call; returns the list of top-level argument strings (string literals honoured)."""
    args, depth, cur, i, in_str = [], 0, [], start, False
    while i < len(text):
        c = text[i]
        if in_str:
            cur.append(c)
            if c == chr(92):
                i += 1
                cur.append(text[i])
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
            cur.append(c)
        elif c in '([{':
            depth += 1
            cur.append(c)
        elif c in ')]}':
            if depth == 0:
                args.append(''.join(cur).strip())
                return args
            depth -= 1
            cur.append(c)
        elif c == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    raise ValueError('unbalanced call')


def handler_statuses(body):
    """Every HTTP status the handler text can answer: deny(res, msg[, N]), res.status = N, and an implicit 200 for a body
    that is set without a status assigned in the three lines before it."""
    out = set()
    for m in re.finditer(r'\bdeny\(', body):
        args = split_call_args(body, m.end())
        if args and args[0] == 'res':
            if len(args) == 2:
                out.add('401')           # deny()'s default
            elif len(args) == 3 and re.fullmatch(r'\d{3}', args[2]):
                out.add(args[2])
            else:
                out.add('?' + ' '.join(args))
    for m in re.finditer(r'res\.status\s*=\s*(\d{3})', body):
        out.add(m.group(1))
    lines = body.split(chr(10))
    for i, ln in enumerate(lines):
        if 'res.set_content(' in ln or 'serveFont(' in ln or 'set_chunked_content_provider' in ln:
            before = chr(10).join(lines[max(0, i - 3):i])
            # a status assigned inside an if-block that has already been closed (and returned) before this line does not
            # colour the answer below it
            tail = before[before.rfind('res.status'):] if 'res.status' in before else ''
            if 'res.status' not in before or '}' in tail:
                out.add('200')
    if re.search(r'if\s*\(!authorised\(req\)\)', body):
        out.add('401')
    return out


def const_value(text, name):
    m = re.search(r'constexpr\s+double\s+' + name + r'\s*=\s*(-?[0-9.]+(?:e[+-]?\d+)?)\s*;', text)
    return float(m.group(1)) if m else None


def run(args):
    s = Suite()
    yaml_text = open(args.yaml, encoding='utf-8').read()
    src = open(args.source, encoding='utf-8').read()
    ccpp = open(args.control_cpp, encoding='utf-8').read()
    chpp = open(args.control_hpp, encoding='utf-8').read()

    try:
        doc = parse_yaml(yaml_text)
        s.check('openapi.yaml parses inside the YAML subset', True)
    except YamlError as e:
        s.check('openapi.yaml parses inside the YAML subset', False, str(e))
        return s
    s.check('the file declares OpenAPI 3.0.3', doc.get('openapi') == '3.0.3')
    s.check('info has title, version and description', all(k in doc.get('info', {}) for k in ('title', 'version', 'description')))
    s.check('a server and a global security requirement are declared', bool(doc.get('servers')) and doc.get('security') == [{'cookieAuth': []}])
    paths = doc.get('paths', {})
    s.check('paths is not empty', len(paths) > 0)
    comps = doc.get('components', {})
    schemas = comps.get('schemas', {})
    sec_schemes = comps.get('securitySchemes', {})
    s.check('the cookie scheme is foxsdr_session in a cookie', sec_schemes.get('cookieAuth', {}).get('in') == 'cookie' and sec_schemes.get('cookieAuth', {}).get('name') == 'foxsdr_session')
    s.check('source names the cookie foxsdr_session', 'kCookieName[] = "foxsdr_session"' in src)

    # ---- the routes of the source --------------------------------------------------------------------------------
    blocks = route_blocks(src)
    s.check('the scan found the routes (at least 15)', len(blocks) >= 15, str(len(blocks)))
    src_ops = {}
    for method, path, body in blocks:
        p = path.replace('(\\d+)', '{}')
        s.check('route %s %s uses only literal text and (\\d+) groups' % (method.upper(), path), re.fullmatch(r'/[A-Za-z0-9_./{}-]*', p) is not None, p)
        src_ops[(method, p)] = body

    doc_ops = {}
    for p, item in paths.items():
        for m, op in item.items():
            if m in ('get', 'post', 'put', 'delete', 'patch'):
                doc_ops[(m, norm_path(p))] = (p, op)
            else:
                s.check('path %s has only HTTP methods as keys' % p, False, m)
    for key, body in src_ops.items():
        s.check('route %s %s is documented' % (key[0].upper(), key[1]), key in doc_ops)
    for key, (p, op) in doc_ops.items():
        pending = op.get('x-to-add-at-merge') is True
        if pending:
            s.check('%s %s is marked "to add at merge" and is not in the source yet (remove the marker when it lands)' % (key[0].upper(), p), key not in src_ops)
        else:
            s.check('documented operation %s %s is a real route' % (key[0].upper(), p), key in src_ops)

    # ---- each operation -------------------------------------------------------------------------------------------
    seen_ids = set()
    refs = []

    def collect_refs(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k == '$ref':
                    refs.append(v)
                else:
                    collect_refs(v)
        elif isinstance(node, list):
            for v in node:
                collect_refs(v)
    collect_refs(doc)

    for key, (p, op) in doc_ops.items():
        label = '%s %s' % (key[0].upper(), p)
        s.check(label + ': summary, operationId, tags and responses are present', all(k in op for k in ('summary', 'operationId', 'tags', 'responses')))
        oid = op.get('operationId')
        s.check(label + ': operationId is unique', oid not in seen_ids)
        seen_ids.add(oid)
        s.check(label + ': states its sign-in rule (security) explicitly', 'security' in op)
        s.check(label + ': description is not empty', bool(op.get('description')))
        # path parameters
        in_path = re.findall(r'\{([^}]*)\}', p)
        declared = [x['name'] for x in op.get('parameters', []) if x.get('in') == 'path']
        s.check(label + ': every {name} in the path is a declared path parameter and vice versa', sorted(in_path) == sorted(declared), '%s vs %s' % (in_path, declared))
        for x in op.get('parameters', []):
            if x.get('in') == 'path':
                s.check(label + ': path parameter %s is required' % x['name'], x.get('required') is True)
        codes = set(op.get('responses', {}).keys())
        s.check(label + ': at least one 2xx response', any(c.startswith('2') for c in codes))
        if key in src_ops:
            body = src_ops[key]
            needs_auth = (re.search(r'if\s*\(!authorised\(req\)\)', body) is not None
                          or re.search(r'if\s*\(who\.role == Role::None\)', body) is not None)   # the room routes ask identify() for who is there
            sec = op.get('security')
            s.check(label + ': security matches the handler (%s)' % ('the handler refuses an unauthorised request' if needs_auth else 'the handler never refuses one'),
                    (sec == [{'cookieAuth': []}] or (sec is None and False)) if needs_auth else sec == [])
            # status codes the handler can answer
            answered = set(handler_statuses(body))
            for c in sorted(answered):
                s.check(label + ': status %s that the handler can answer is documented' % c, c in codes, 'documented: ' + ','.join(sorted(codes)))
            for c in sorted(codes):
                if c in ('413',):
                    continue   # produced by the HTTP library under set_payload_max_length, not by the handler
                s.check(label + ': documented status %s can really be answered' % c, c in answered, 'handler answers: ' + ','.join(sorted(answered)))
    for r in refs:
        parts = r.lstrip('#/').split('/')
        node = doc
        ok = True
        for part in parts:
            if isinstance(node, dict) and part in node:
                node = node[part]
            else:
                ok = False
                break
        s.check('$ref %s resolves' % r, ok)

    # ---- /api/control -----------------------------------------------------------------------------------------------
    known = re.search(r'kKnownKeys\[\]\s*=\s*\{(.*?)\};', ccpp, re.S)
    keys = re.findall(r'"([A-Za-z0-9]+)"', known.group(1)) if known else []
    s.check('web_control.cpp key list found', len(keys) >= 40, str(len(keys)))
    cr = schemas.get('ControlRequest', {})
    props = cr.get('properties', {})
    pending = {k for k, v in props.items() if v.get('x-to-add-at-merge') is True}
    for k in keys:
        s.check('ControlRequest documents the key %s' % k, k in props)
    for k in props:
        if k in pending:
            s.check('ControlRequest key %s is marked "to add at merge" and is not in web_control.cpp yet' % k, k not in keys)
        else:
            s.check('ControlRequest key %s is one web_control.cpp accepts' % k, k in keys)
    s.check('ControlRequest refuses unknown keys and an empty object', cr.get('additionalProperties') is False and cr.get('minProperties') == 1)
    ranges = {
        'centerHz': ('kMinCenterHz', 'kMaxCenterHz'),
        'bandwidthHz': ('kMinBandwidthHz', 'kMaxBandwidthHz'),
        'squelchDb': ('kMinSquelchDb', 'kMaxSquelchDb'),
        'dbMin': ('kMinDisplayDb', 'kMaxDisplayDb'),
        'dbMax': ('kMinDisplayDb', 'kMaxDisplayDb'),
        'notchFreqHz': ('kMinNotchHz', 'kMaxNotchHz'),
        'notchQ': ('kMinNotchQ', 'kMaxNotchQ'),
        'scanStartHz': ('kMinCenterHz', 'kMaxCenterHz'),
        'scanStopHz': ('kMinCenterHz', 'kMaxCenterHz'),
    }
    for field, (lo, hi) in ranges.items():
        p = props.get(field, {})
        s.check('%s minimum is %s from web_control.hpp' % (field, lo), const_value(chpp, lo) is not None and p.get('minimum') == const_value(chpp, lo), '%r vs %r' % (p.get('minimum'), const_value(chpp, lo)))
        s.check('%s maximum is %s from web_control.hpp' % (field, hi), const_value(chpp, hi) is not None and p.get('maximum') == const_value(chpp, hi), '%r vs %r' % (p.get('maximum'), const_value(chpp, hi)))
    vmax = const_value(chpp, 'kMaxVfoOffsetHz')
    s.check('vfoOffsetHz is +-kMaxVfoOffsetHz', vmax is not None and props.get('vfoOffsetHz', {}).get('minimum') == -vmax and props.get('vfoOffsetHz', {}).get('maximum') == vmax)
    s.check('volume and nrStrength are 0..1 (readNumber calls in web_control.cpp)',
            'readNumber(j, "volume", 0.0, 1.0' in ccpp and 'readNumber(j, "nrStrength", 0.0, 1.0' in ccpp
            and all(props[k].get('minimum') == 0 and props[k].get('maximum') == 1 for k in ('volume', 'nrStrength')))
    m = re.search(r'readNumber\(j, "sampleRateHz", ([0-9.e+]+), ([0-9.e+]+)', ccpp)
    s.check('sampleRateHz range', bool(m) and props['sampleRateHz'].get('minimum') == float(m.group(1)) and props['sampleRateHz'].get('maximum') == float(m.group(2)))
    m = re.search(r'readNumber\(j, "gainDb", (-?[0-9.e+]+), ([0-9.e+]+)', ccpp)
    s.check('gainDb range', bool(m) and props['gainDb'].get('minimum') == float(m.group(1)) and props['gainDb'].get('maximum') == float(m.group(2)))
    m = re.search(r'readNumber\(j, "scanStepHz", ([0-9.e+]+), ([0-9.e+]+)', ccpp)
    s.check('scanStepHz range', bool(m) and props['scanStepHz'].get('minimum') == float(m.group(1)) and props['scanStepHz'].get('maximum') == float(m.group(2)))
    kinds = re.findall(r'\*req\.sourceKind != "([a-z0-9]+)"', ccpp)
    s.check('every source kind web_control.cpp accepts is named in the sourceKind description',
            len(kinds) >= 10 and all(re.search(r'\b' + k + r'\b', props['sourceKind'].get('description', '')) for k in kinds), ','.join(kinds))
    for refused in re.findall(r'\{"(transmitLatch[a-zA-Z]*)",', ccpp):
        s.check('the refused-by-name key %s is mentioned under transmitPtt' % refused, refused in props['transmitPtt'].get('description', ''))
    body_limit = re.search(r'kMaxPayloadBytes\s*=\s*(\d+)\s*\*\s*(\d+)', src)
    s.check('the 8 KiB body limit named in the file is the source\'s kMaxPayloadBytes', bool(body_limit) and int(body_limit.group(1)) * int(body_limit.group(2)) == 8192 and '8 KiB' in yaml_text)
    s.check('the audio listener cap named in the file (4) is the header\'s kMaxAudioListeners', 'kMaxAudioListeners = 4' in open(os.path.join(os.path.dirname(args.source), 'web_server.hpp'), encoding='utf-8').read() and 'At most 4 listeners' in yaml_text)
    s.check('the session cap named in the file (32) is web_auth.hpp\'s kMaxSessions', 'kMaxSessions = 32' in open(os.path.join(os.path.dirname(args.source), 'web_auth.hpp'), encoding='utf-8').read() and 'At most 32 sessions' in yaml_text)
    s.check('the default port named in the file (8073) is kDefaultWebPort', 'kDefaultWebPort = 8073' in open(os.path.join(os.path.dirname(args.source), '..', 'core', 'server_ports.hpp'), encoding='utf-8').read() and 'http://127.0.0.1:8073' in yaml_text)

    # ---- /api/status ----------------------------------------------------------------------------------------------
    sj = src[src.index('std::string statusJson('):]
    sj = sj[:sj.index('\n}\n')]
    status_keys = set(re.findall(r'\bj\["([A-Za-z0-9]+)"\]', sj))
    sprops = schemas.get('Status', {}).get('properties', {})
    s.check('statusJson was scanned (more than 60 keys)', len(status_keys) > 60, str(len(status_keys)))
    for k in sorted(status_keys):
        s.check('Status documents the field %s' % k, k in sprops)
    for k, v in sprops.items():
        if v.get('x-to-add-at-merge') is True:
            s.check('Status field %s is marked "to add at merge" and is not in statusJson yet' % k, k not in status_keys)
        else:
            s.check('Status field %s is one statusJson writes' % k, k in status_keys)
    s.check('scannerState enum is the one RadioStatus names', set(schemas['Status']['properties']['scannerState'].get('enum', [])) == {'idle', 'scanning', 'paused', 'holding'}
            and '"idle" | "scanning" | "paused" | "holding"' in open(os.path.join(os.path.dirname(args.source), 'web_server.hpp'), encoding='utf-8').read())

    # ---- the small answers ------------------------------------------------------------------------------------------
    small = [('get', '/api/session', 'SessionState'), ('get', '/api/spectrum', 'Spectrum'), ('post', '/api/login', 'LoginOk'),
             ('post', '/api/control', 'Accepted'), ('get', '/api/tile/{}/{}/{}', 'Pending')]
    for method, p, schema in small:
        body = src_ops.get((method, p), '')
        written = set(re.findall(r'\bj\["([A-Za-z0-9]+)"\]', body)) - {'error'}
        listed = set(schemas.get(schema, {}).get('properties', {}).keys())
        s.check('%s %s: the schema %s has exactly the keys the handler writes' % (method.upper(), p, schema), written == listed and len(listed) > 0, '%s vs %s' % (sorted(written), sorted(listed)))
    s.check('the Error schema is {"error": ...} as deny() writes it', set(schemas.get('Error', {}).get('properties', {}).keys()) == {'error'} and 'j["error"] = message;' in src)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--yaml', default=os.path.join(REPO, 'docs', 'api', 'openapi.yaml'))
    ap.add_argument('--source', default=os.path.join(REPO, 'src', 'net', 'web_server.cpp'))
    ap.add_argument('--control-cpp', default=os.path.join(REPO, 'src', 'net', 'web_control.cpp'))
    ap.add_argument('--control-hpp', default=os.path.join(REPO, 'src', 'net', 'web_control.hpp'))
    args = ap.parse_args()
    s = run(args)
    print('%d checks, %d failed' % (s.checks, s.failed))
    return 1 if s.failed else 0


if __name__ == '__main__':
    sys.exit(main())
