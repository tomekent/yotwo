#!/bin/sh
# Checks the setup page and the player's web page without a player: each script
# parses, every element the script looks up exists, and every field it sends is one the
# firmware reads (and the other way about for the settings it loads).
set -e
cd "$(dirname "$0")/../.."
python3 - <<'PY'
import re, subprocess, shutil, sys, tempfile, os
settings = open('firmware/main/setup/player_settings.c').read()
portal = open('firmware/main/setup/setup_portal.c').read()
web = open('firmware/main/web/web.c').read()
READ = r'(?:str|num)\(req, "(\w+)"|cJSON_GetObjectItem\(req, "(\w+)"\)'
GIVE = r'cJSON_Add\w+ToObject\(o, "(\w+)"'
def keys(rx, text): return {a or b for a, b in re.findall(rx, text)} if '|' in rx else set(re.findall(rx, text))
settings_read, settings_given = keys(READ, settings), keys(GIVE, settings)
total = 0
def check(label, header, var, read, given, save_rx):
    global total
    fails = []
    src = open(header).read()
    html = ''.join(re.findall(r'^"((?:[^"\\]|\\.)*)"', src, re.M)).encode().decode('unicode_escape')
    js = re.search(r'<script>(.*)</script>', html, re.S).group(1)
    ids = set(re.findall(r'\bid=([\w-]+)', html))
    for used in sorted(set(re.findall(r"\$\('([\w-]+)'\)", js))):
        if used not in ids: fails.append(f"script uses element '{used}' that the page does not have")
    body = re.search(save_rx, js, re.S).group(1)
    sent = set(re.findall(r'(?:^|,)(\w+):', body))
    for f in sorted(sent - read): fails.append(f"page sends '{f}' but the firmware does not read it")
    for f in sorted(read - sent): fails.append(f"firmware reads '{f}' but the page does not send it")
    shown = set(re.findall(r'\bc\.(\w+)', js))
    for f in sorted(shown - given): fails.append(f"page shows 'c.{f}' but the firmware does not give it")
    for f in sorted(settings_given - shown): fails.append(f"firmware gives setting '{f}' but the page does not show it")
    if shutil.which('node'):
        with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as t: t.write(js)
        r = subprocess.run(['node', '--check', t.name], capture_output=True, text=True); os.unlink(t.name)
        if r.returncode: fails.append('script does not parse: ' + r.stderr.strip().split('\n')[-1])
    for f in fails: print(f'FAIL ({label}):', f)
    print(f"{label}: {len(html)} bytes, {len(ids)} elements, {len(sent)} fields sent; {len(fails)} failed")
    total += len(fails)
# The setup page: Wi-Fi, server and sign-in (setup_portal.c) plus the shared settings.
portal_cfg = portal.split('static esp_err_t on_scan')[0].split('static esp_err_t on_config')[1]
check('setup page', 'firmware/main/setup/setup_page.h', 'SETUP_PAGE',
      keys(READ, portal) | settings_read, keys(GIVE, portal_cfg) | settings_given, r"const b=\{(.*?)\};")
# The player's web page: the shared settings only.
check('web page', 'firmware/main/web/web_page.h', 'WEB_PAGE', settings_read, settings_given, r"const b=\{(.*?)\};")
if not shutil.which('node'): print('(node not found: script syntax not checked)')
sys.exit(1 if total else 0)
PY
