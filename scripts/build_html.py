Import('env')
import gzip, os

html_path = os.path.join(env['PROJECT_DIR'], 'data', 'index.html')
out_path  = os.path.join(env['PROJECT_DIR'], 'src',  'ui_html.h')

with open(html_path, 'rb') as f:
    raw = f.read()

gz = gzip.compress(raw, compresslevel=9)

lines = ['// Auto-generated from data/index.html — do not edit\n',
         '#pragma once\n',
         '#include <Arduino.h>\n',
         f'static const uint32_t UI_HTML_GZ_LEN = {len(gz)}UL;\n',
         'static const uint8_t  UI_HTML_GZ[] PROGMEM = {\n']

for i in range(0, len(gz), 16):
    chunk = gz[i:i+16]
    lines.append('  ' + ','.join(f'0x{b:02x}' for b in chunk) + ',\n')

lines.append('};\n')

with open(out_path, 'w') as f:
    f.writelines(lines)

print(f'[HTML] ui_html.h: {len(gz)} bytes gz ({len(raw)} raw)')
