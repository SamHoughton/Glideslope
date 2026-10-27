# PlatformIO pre-build step: turn web/index.html into utils/WebPage.h, the
# page the board serves. The brand mark and favicon come from
# utils/BrandMark.h (tools/brand_workbench.py). Only rewrites the header when
# the page has changed, so it doesn't force a rebuild.
#
# Also usable outside PlatformIO:  python scripts/gen_webpage.py
#
# 9.1: stored gzip-compressed (about a quarter of the size: the page had
# grown to 84 KB and took 8 s to reach a phone), with an ETag so a browser
# that has it already gets a 304 and nothing more.
import gzip
import re
import zlib
from pathlib import Path

try:
    Import("env")  # noqa: F821  (SCons)
    ROOT = Path(env["PROJECT_DIR"])  # noqa: F821
except NameError:
    ROOT = Path(__file__).resolve().parent.parent


def page():
    marks = dict(re.findall(r'#define (\w+) "(.*)"', (ROOT / 'utils' / 'BrandMark.h').read_text(encoding='utf-8')))
    html = (ROOT / 'web' / 'index.html').read_text(encoding='utf-8')
    html = html.replace('<!--BRAND_SVG-->', marks['GS_BRAND_SVG']).replace('{{FAVICON}}', marks['GS_FAVICON_URI'])
    # Light minification: drop indentation and blank lines (the page is
    # served from flash on every visit).
    return '\n'.join(line.strip() for line in html.splitlines() if line.strip())


def main():
    raw = page().encode('utf-8')
    gz = gzip.compress(raw, compresslevel=9, mtime=0)   # the same bytes every build
    etag = '"%08x"' % (zlib.crc32(raw) & 0xFFFFFFFF)
    rows = ',\n'.join(','.join('0x%02x' % b for b in gz[i:i + 24]) for i in range(0, len(gz), 24))
    header = ('// Generated from web/index.html by scripts/gen_webpage.py: edit the HTML, not this.\n'
              '// The page gzip-compressed (%d bytes, %d unpacked).\n'
              '#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n'
              'static const uint8_t kHtmlPageGz[] = {\n%s\n};\n'
              'static const size_t kHtmlPageGzLen = sizeof(kHtmlPageGz);\n'
              'static const char kHtmlPageEtag[] = %s;\n' % (len(gz), len(raw), rows, '"' + etag.replace('"', '\\"') + '"'))
    out = ROOT / 'utils' / 'WebPage.h'
    if not out.exists() or out.read_text(encoding='utf-8') != header:
        out.write_text(header, encoding='utf-8')
        print(f'gen_webpage: wrote {out.name} ({len(raw)} bytes of page, {len(gz)} compressed)')


main()
