"""Export the #239 mixed Markdown fixture and check table/content preservation.

Native geometry, hit testing, themes, widths and text scales are covered by
CTest wide_table_layout. Set TINTA_TABLE_RENDER_DIR when running that test to
save its native table PNGs for visual inspection.
"""
from pathlib import Path
from html.parser import HTMLParser
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile

import fitz


class Tables(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tables = []

    def handle_starttag(self, tag, attrs):
        if tag == 'table':
            self.tables.append([])
        elif tag == 'tr':
            self.tables[-1].append(0)
        elif tag in ('td', 'th'):
            self.tables[-1][-1] += 1


repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=repo/'build/Release/scher.exe')
parser.add_argument('--output', type=Path, default=repo/'out/issue-239/exports')
args = parser.parse_args()
output = args.output.resolve()
fixture = repo/'tests/fixtures/wide-table-239.md'
before = hashlib.sha256(fixture.read_bytes()).hexdigest()
ctypes.windll.kernel32.SetErrorMode(3)
startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = 0
results = []
values = ['FX-SGTL-GOODS-001', 'FX-SGTL-GOODS-002', '2026/04/01 00:00', '2030/03/31 23:59', '准备时任一览读回']
for theme, index in [('paper', 0), ('midnight', 5)]:
    folder = output/theme
    folder.mkdir(parents=True, exist_ok=True)
    exe = folder/'scher.exe'
    shutil.copy2(args.binary, exe)
    (folder/'settings.ini').write_text(f'[Settings]\nthemeIndex={index}\nfollowSystemTheme=0\n'
        'hasAskedFileAssociation=1\nlanguage=en\n', encoding='utf-8')
    for option, name in [('printpages', 'pages'), ('exporthtml', 'document.html'),
                         ('exportdocx', 'document.docx'), ('exportpdf', 'document.pdf')]:
        run = subprocess.run([str(exe), str(fixture), '--'+option, str(folder/name)],
            startupinfo=startup, capture_output=True, timeout=60,
            env=dict(os.environ, LOCALAPPDATA=str(folder)))
        assert run.returncode == 0 and (folder/name).exists(), (theme, option, run.returncode)
    html = (folder/'document.html').read_text(encoding='utf-8')
    tables = Tables()
    tables.feed(html)
    assert tables.tables == [[13]*3, [3]*3], tables.tables
    for marker in ['<h1', '<h2', '<blockquote', '<pre', '<strong', '<ul', 'Final heading', *values]:
        assert marker in html, (theme, marker)
    with zipfile.ZipFile(folder/'document.docx') as archive:
        root = ET.fromstring(archive.read('word/document.xml'))
        ns = {'w': 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'}
        table_sizes = [[len(row.findall('w:tc', ns)) for row in table.findall('w:tr', ns)]
                       for table in root.findall('.//w:tbl', ns)]
        assert table_sizes == [[13]*3, [3]*3], table_sizes
        text = ''.join(node.text or '' for node in root.findall('.//w:t', ns))
        for value in [*values, 'Final heading', 'A task stays below the table.']:
            assert value in text, (theme, value)
    with fitz.open(folder/'document.pdf') as doc:
        pdf_text = ''.join(''.join(page.get_text().split()) for page in doc)
        for value in [*values, 'Final heading']:
            assert ''.join(value.split()) in pdf_text, (theme, 'PDF text', value)
        pdf_pages = len(doc)
        for i, page in enumerate(doc):
            page.get_pixmap(matrix=fitz.Matrix(1.5, 1.5)).save(folder/f'pdf-page-{i+1}.png')
    pages = sorted((folder/'pages').glob('*.png'))
    assert pages and all(p.read_bytes().startswith(b'\x89PNG\r\n\x1a\n') for p in pages)
    assert not (folder/'Tinta/crash.dmp').exists()
    results.append(dict(theme=theme, table_dimensions=table_sizes, png_pages=len(pages), pdf_pages=pdf_pages,
                        html_docx_pdf_content_preserved=True))
assert hashlib.sha256(fixture.read_bytes()).hexdigest() == before
(output/'results.json').write_text(json.dumps(results, indent=2)+'\n', encoding='utf-8')
print(json.dumps(results, indent=2))
