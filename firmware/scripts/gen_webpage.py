# PlatformIO pre-build step: turn web/index.html into utils/WebPage.h, the
# page the board serves. The brand mark and favicon come from
# utils/BrandMark.h (tools/brand_workbench.py). Only rewrites the header when
# the page has changed, so it doesn't force a rebuild.
#
# Also usable outside PlatformIO:  python scripts/gen_webpage.py
import re
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
    html = page()
    assert ')GSPAGE"' not in html
    header = ('// Generated from web/index.html by scripts/gen_webpage.py: edit the HTML, not this.\n'
              '#pragma once\n#include <stddef.h>\n\n'
              'static const char kHtmlPage[] = R"GSPAGE(' + html + ')GSPAGE";\n'
              'static const size_t kHtmlPageLen = sizeof(kHtmlPage) - 1;\n')
    out = ROOT / 'utils' / 'WebPage.h'
    if not out.exists() or out.read_text(encoding='utf-8') != header:
        out.write_text(header, encoding='utf-8')
        print(f'gen_webpage: wrote {out.name} ({len(html)} bytes of page)')


main()
