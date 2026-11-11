"""Break-it check for test_openapi_routes.py: each mutant changes ONE thing in a COPY of openapi.yaml or of the source it is
read against, the suite is run on the copy, and it must go RED. Then it runs on the real files and must be GREEN.

    python -I tests/packaging/breakit_openapi.py

The real files are never modified. Prints a table and "N mutants, M survived"; exit code 1 if any survived.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SUITE = os.path.join(HERE, 'test_openapi_routes.py')

REAL = {
    'yaml': os.path.join(REPO, 'docs', 'api', 'openapi.yaml'),
    'source': os.path.join(REPO, 'src', 'net', 'web_server.cpp'),
    'control_cpp': os.path.join(REPO, 'src', 'net', 'web_control.cpp'),
    'control_hpp': os.path.join(REPO, 'src', 'net', 'web_control.hpp'),
}


def read(p):
    with open(p, encoding='utf-8', newline='') as f:
        return f.read().replace('\r\n', '\n')


def mutate(text, old, new, count=1):
    if old not in text:
        raise SystemExit('mutant anchor not found: %r' % old[:70])
    return text.replace(old, new, count)


def cut_block(text, start_marker, end_marker):
    """Removes from start_marker up to (not including) end_marker."""
    a = text.index(start_marker)
    b = text.index(end_marker, a + len(start_marker))
    return text[:a] + text[b:]


MUTANTS = [
    ('yaml', 'a documented route is removed (GET /api/spectrum)', lambda t: cut_block(t, '  /api/spectrum:\n', '  /api/events:\n')),
    ('yaml', 'a route that is not in the source is added (GET /api/nothing)', lambda t: mutate(t, '  /api/events:\n', '  /api/nothing:\n    get:\n      tags: [receiver]\n      operationId: getNothing\n      summary: Nothing\n      description: "x"\n      security: []\n      responses:\n        "200":\n          description: "x"\n  /api/events:\n')),
    ('yaml', 'a route is documented under the wrong method (POST /api/status)', lambda t: mutate(t, '  /api/status:\n    get:', '  /api/status:\n    post:')),
    ('yaml', 'a status the handler answers is not documented (409 on /api/control)', lambda t: cut_block(t, '        "409":\n          description: "transmitPtt was asked for', '        "413":\n          description: "The body is over the 8 KiB limit (the status is the vendored HTTP library\'s PayloadTooLarge_413)."\n        "415":\n          description: "The Content-Type is not application/json."\n          content:\n            application/json:\n              schema:\n                $ref: "#/components/schemas/Error"\n        "503":\n          description: "The receiver is not available to this server, or its state')),
    ('yaml', 'a status the handler never answers is documented (404 on /api/status)', lambda t: mutate(t, '        "401":\n          $ref: "#/components/responses/SignInFirst"\n  /api/audio:', '        "401":\n          $ref: "#/components/responses/SignInFirst"\n        "404":\n          description: "never"\n  /api/audio:')),
    ('yaml', 'a protected route is documented as public (/api/status security: [])', lambda t: _status_public(t)),
    ('yaml', 'a public route is documented as protected (/api/session)', lambda t: mutate(t, '      operationId: getSession\n      summary: Is a sign-in needed, and is this client signed in\n      description: "Needs no sign-in. Reveals nothing beyond whether a credential is needed and whether this client holds a valid one."\n      security: []', '      operationId: getSession\n      summary: Is a sign-in needed, and is this client signed in\n      description: "Needs no sign-in. Reveals nothing beyond whether a credential is needed and whether this client holds a valid one."\n      security:\n        - cookieAuth: []')),
    ('yaml', 'a path parameter is not declared (/api/image/{index})', lambda t: cut_block(t, '      parameters:\n        - name: index\n', '      responses:\n        "200":\n          description: "The picture as a BMP."')),
    ('yaml', 'a $ref that does not resolve', lambda t: mutate(t, '#/components/schemas/Spectrum', '#/components/schemas/Spectrumm')),
    ('yaml', 'a control key is dropped (pluginFetch)', lambda t: cut_block(t, '        pluginFetch:\n', '        pluginInstall:\n')),
    ('yaml', 'a control key is invented (centreHz)', lambda t: mutate(t, '        running:\n          type: boolean\n          description: "Start or stop the receiver."', '        centreHz:\n          type: number\n        running:\n          type: boolean\n          description: "Start or stop the receiver."')),
    ('yaml', 'a control range is wrong (centerHz maximum)', lambda t: mutate(t, 'maximum: 6000000000\n          description: "Source centre frequency, Hz."', 'maximum: 7000000000\n          description: "Source centre frequency, Hz."')),
    ('yaml', 'a status field is dropped (scannerState)', lambda t: cut_block(t, '        scannerState:\n', '        scanStartHz:\n          type: number\n        scanStopHz:\n          type: number\n        scanStepHz:\n          type: number\n        muted:')),
    ('yaml', 'a status field is invented (rdsStation)', lambda t: mutate(t, '        rdsPs:\n          type: string', '        rdsStation:\n          type: string\n        rdsPs:\n          type: string')),
    ('yaml', 'a to-add-at-merge route loses its marker (GET /api/events)', lambda t: mutate(t, '      x-to-add-at-merge: true\n      description: "TO ADD AT MERGE: this route', '      description: "TO ADD AT MERGE: this route')),
    ('yaml', 'the session schema loses a key (reachableOffMachine)', lambda t: mutate(t, '      required: [authRequired, authenticated, reachableOffMachine]\n      properties:\n        authRequired:\n          type: boolean\n          description: "A sign-in is needed."\n        authenticated:\n          type: boolean\n          description: "This request holds a valid session (always true when no sign-in is needed)."\n        reachableOffMachine:\n          type: boolean\n          description: "The server is bound to an address other machines can reach."', '      required: [authRequired, authenticated]\n      properties:\n        authRequired:\n          type: boolean\n          description: "A sign-in is needed."\n        authenticated:\n          type: boolean\n          description: "This request holds a valid session (always true when no sign-in is needed)."')),
    ('yaml', 'the file leaves the YAML subset (a tab)', lambda t: mutate(t, '  title: FoxSDR web remote', '\ttitle: FoxSDR web remote')),
    ('yaml', 'the file leaves the YAML subset (a duplicate key)', lambda t: mutate(t, 'openapi: 3.0.3\n', 'openapi: 3.0.3\nopenapi: 3.0.3\n')),
    ('source', 'a new route appears in the source (GET /api/newthing)', lambda t: mutate(t, '    svr.Get("/api/spectrum",', '    svr.Get("/api/newthing", [this](const httplib::Request& req, httplib::Response& res) { res.set_content("x", "text/plain"); });\n    svr.Get("/api/spectrum",')),
    ('source', 'a handler answers a new status (418 from /api/audio)', lambda t: mutate(t, 'deny(res, "too many listeners", 503);', 'deny(res, "too many listeners", 418);')),
    ('source', 'a handler stops requiring a sign-in (/api/status)', lambda t: mutate(t, '    svr.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {\n        if (!authorised(req)) {\n            deny(res, "sign in first");\n            return;\n        }', '    svr.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {')),
    ('source', 'statusJson writes a new field', lambda t: mutate(t, '    j["running"] = s.running;\n    j["faulted"] = s.faulted;', '    j["running"] = s.running;\n    j["newField"] = 1;\n    j["faulted"] = s.faulted;')),
    ('source', 'the session answer gains a key', lambda t: mutate(t, '        j["reachableOffMachine"] = d.reachableOffMachine;', '        j["reachableOffMachine"] = d.reachableOffMachine;\n        j["extra"] = 1;')),
    ('source', 'the audio listener cap changes in the header it is read from', None),
    ('control_cpp', 'web_control.cpp accepts a new key', lambda t: mutate(t, '"pluginPresetName", "pluginPresetIndex", "transmitPtt"};', '"pluginPresetName", "pluginPresetIndex", "transmitPtt", "muted"};')),
    ('control_hpp', 'web_control.hpp widens the centre-frequency range', lambda t: mutate(t, 'kMaxCenterHz = 6.0e9;', 'kMaxCenterHz = 7.0e9;')),
]


def _status_public(t):
    i = t.index('      operationId: getStatus\n')
    j = t.index('      security:\n        - cookieAuth: []', i)
    return t[:j] + '      security: []' + t[j + len('      security:\n        - cookieAuth: []'):]


def run_suite(paths):
    p = subprocess.run([sys.executable, '-I', SUITE, '--yaml', paths['yaml'], '--source', paths['source'],
                        '--control-cpp', paths['control_cpp'], '--control-hpp', paths['control_hpp']],
                       capture_output=True, text=True)
    lines = p.stdout.strip().split('\n')
    fails = [l[6:] for l in lines if l.startswith('FAIL')]
    err = p.stderr.strip().split('\n')[-1] if p.returncode not in (0, 1) else ''
    return p.returncode, fails, (lines[-1] if lines else ''), err


def main():
    survived = 0
    rows = []
    work = tempfile.mkdtemp(prefix='openapi-breakit-')
    # the source tree's headers sit beside web_server.cpp: copy the three the test reads so a mutant can change them
    for n, (kind, name, fn) in enumerate(MUTANTS):
        d = os.path.join(work, 'm%02d' % n)
        os.makedirs(os.path.join(d, 'net'), exist_ok=True)
        os.makedirs(os.path.join(d, 'core'), exist_ok=True)
        paths = {
            'yaml': os.path.join(d, 'openapi.yaml'),
            'source': os.path.join(d, 'net', 'web_server.cpp'),
            'control_cpp': os.path.join(d, 'net', 'web_control.cpp'),
            'control_hpp': os.path.join(d, 'net', 'web_control.hpp'),
        }
        texts = {k: read(v) for k, v in REAL.items()}
        # files the suite reads next to web_server.cpp
        for extra in ('web_server.hpp', 'web_auth.hpp'):
            with open(os.path.join(d, 'net', extra), 'w', encoding='utf-8', newline='') as f:
                f.write(read(os.path.join(REPO, 'src', 'net', extra)))
        with open(os.path.join(d, 'core', 'server_ports.hpp'), 'w', encoding='utf-8', newline='') as f:
            f.write(read(os.path.join(REPO, 'src', 'core', 'server_ports.hpp')))
        if fn is None:
            # the cap named in the YAML is compared with web_server.hpp's: change the header copy
            hp = os.path.join(d, 'net', 'web_server.hpp')
            with open(hp, 'w', encoding='utf-8', newline='') as f:
                f.write(mutate(read(os.path.join(REPO, 'src', 'net', 'web_server.hpp')), 'kMaxAudioListeners = 4', 'kMaxAudioListeners = 8'))
        else:
            texts[kind] = fn(texts[kind])
        for k, v in texts.items():
            with open(paths[k], 'w', encoding='utf-8', newline='') as f:
                f.write(v)
        code, fails, summary, err = run_suite(paths)
        red = code != 0
        if not red:
            survived += 1
        rows.append((name, 'RED' if red else 'SURVIVED', (fails[0] if fails else (err or summary))[:110]))
    w = max(len(r[0]) for r in rows)
    for name, res, first in rows:
        print('%-*s  %-8s  %s' % (w, name, res, first))
    code, fails, summary, err = run_suite(REAL)
    print('unmodified files: %s (exit %d)' % (summary, code))
    print('%d mutants, %d survived' % (len(MUTANTS), survived))
    return 1 if survived or code != 0 else 0


if __name__ == '__main__':
    sys.exit(main())
