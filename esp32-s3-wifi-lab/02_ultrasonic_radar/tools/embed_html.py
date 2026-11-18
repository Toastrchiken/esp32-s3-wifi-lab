"""
Embeds web/index.html into src/index_html.h as a C++ raw string.

Runs automatically before every PlatformIO build (see extra_scripts in
platformio.ini), so you only ever edit web/index.html. Can also be run by hand:
    python tools/embed_html.py
"""
import os

try:
    Import("env")  # noqa: F821  (provided by PlatformIO/SCons)
    ROOT = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC = os.path.join(ROOT, "web", "index.html")
DST = os.path.join(ROOT, "src", "index_html.h")
DELIM = "rawliteral"

with open(SRC, encoding="utf-8") as f:
    html = f.read()
if f"){DELIM}\"" in html:
    raise SystemExit(f"index.html contains the raw-string delimiter ){DELIM}\"")

header = (
    "// AUTO-GENERATED from web/index.html by tools/embed_html.py - do not edit.\n"
    "#pragma once\n"
    "#include <Arduino.h>\n"
    f"static const char INDEX_HTML[] PROGMEM = R\"{DELIM}(\n{html}){DELIM}\";\n"
)

old = None
if os.path.exists(DST):
    with open(DST, encoding="utf-8") as f:
        old = f.read()
if old != header:  # only touch the file when the page changed (avoids needless rebuilds)
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"embed_html: wrote {os.path.relpath(DST, ROOT)} ({len(html)} bytes of HTML)")
