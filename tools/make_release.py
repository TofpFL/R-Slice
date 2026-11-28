import json, subprocess, sys, urllib.request, os, time
import requests

REPO = 'TofpFL/R-Slice'
TAG = 'v0.8.6-testers'

# pull the github.com credential from git credential manager without printing it
p = subprocess.run(['git', 'credential', 'fill'], input='protocol=https\nhost=github.com\n\n',
                   capture_output=True, text=True)
token = None
for line in p.stdout.splitlines():
    if line.startswith('password='):
        token = line.split('=', 1)[1]
if not token:
    print('NO TOKEN'); sys.exit(1)

def api(method, path, data=None, raw=None, ctype='application/json'):
    req = urllib.request.Request('https://api.github.com' + path, method=method)
    req.add_header('Authorization', 'Bearer ' + token)
    req.add_header('Accept', 'application/vnd.github+json')
    req.add_header('X-GitHub-Api-Version', '2022-11-28')
    if data is not None:
        req.add_header('Content-Type', ctype)
        req.data = json.dumps(data).encode()
    elif raw is not None:
        req.add_header('Content-Type', ctype)
        req.data = raw
    with urllib.request.urlopen(req, timeout=600) as r:
        body = r.read()
        return r.status, (json.loads(body) if body else {})

notes = """# R-Slice v0.8.6 — TESTER BUILD

> **VERSION 0.8.6 — THIS BUILD NEEDS MAJOR ARCHITECTURE FIXES.**
> This is a pre-alpha tester build for development purposes only. Expect
> crashes, missing features, broken mods, and breaking changes. It is NOT
> a stable release and does not represent the final direction of the engine.

## Contents

- **`assets-vslice.zip`** — the full V-Slice asset set (songs, sprites,
  characters, stage, fonts, videos). NOT stored in git. Extract into the
  repository root so you get `assets/` and `art/` next to `project.hxp`.
- **`R-Slice-debug.apk`** — prebuilt Android debug APK (arm64-v8a +
  armeabi-v7a, ~965 MB). Includes the RAID engine and all five `.raid`
  songs. Verified launching + streaming charts on a Moto G Stylus 5G 2024.

## Building from source

- **Windows:** `haxelib run lime build windows` → `export/release/windows/bin/R-Slice.exe`
- **Android:** `tools\\Androidify.bat --install` (provisions JDK 17, Android
  SDK/NDK, builds the RAID engine per-ABI, rebuilds lime ndll from the
  pinned FunkinCrew fork, then builds the APK)

## Known issues (needs major architecture fixes)

- `e404c` — the `.raid` stress chart (~173M expanded events) exceeds the
  streaming engine's throughput; it degrades and can get LMK-killed on both
  PC and Android. Other `.raid` songs (quantum, fraudulent, raidstress,
  watermelon) play fine.
- Android: video playback compiled out (hxvlc has no Android VLC libs).
- First launch of each `.raid` song copies the chart from APK assets to
  app storage (one-time, sub-second).
"""

# create release (idempotent-ish: fail if tag exists, then reuse)
try:
    st, rel = api('POST', '/repos/' + REPO + '/releases', {
        'tag_name': TAG,
        'name': 'R-Slice v0.8.6 — Tester Build (Needs Major Architecture Fixes)',
        'body': notes,
        'draft': False,
        'prerelease': True,
    })
    print('release created:', rel.get('id'), rel.get('html_url'))
except urllib.error.HTTPError as e:
    if e.code == 422:
        st, rels = api('GET', '/repos/' + REPO + '/releases/tags/' + TAG)
        rel = rels[0] if isinstance(rels, list) else rels
        print('release exists:', rel.get('id'))
    else:
        raise

upload_url = rel['upload_url'].split('{')[0]
assets = [
    (r'F:\assets-vslice.zip', 'assets-vslice.zip', 'application/zip'),
    (r'C:\Users\YalishannaZhou\Downloads\R-Slice-debug.apk', 'R-Slice-debug.apk', 'application/vnd.android.package-archive'),
]
for path, name, ctype in assets:
    size = os.path.getsize(path)
    print('uploading', name, round(size/1e9, 2), 'GB ...', flush=True)
    t0 = time.time()
    # stream the file - never load it into memory
    with open(path, 'rb') as f:
        r = requests.post(upload_url + '?name=' + name, data=f,
                          headers={'Authorization': 'Bearer ' + token,
                                   'Accept': 'application/vnd.github+json',
                                   'Content-Type': ctype},
                          timeout=3600)
    try:
        resp = r.json()
    except Exception:
        resp = {'error': r.text[:300]}
    print('  ->', r.status_code, resp.get('browser_download_url', resp), round(time.time()-t0), 's', flush=True)
print('ALL DONE')
