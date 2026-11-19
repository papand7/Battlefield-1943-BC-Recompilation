"""build_server.bat helper: writes <out> = the recompiler config <in> with this
PC's path to default.xex (what launcher/setup.js does).
Usage: server_config.py <in.toml> <out.toml> <default.xex>"""
import re
import sys

src, dst, xex = sys.argv[1:4]
with open(src, encoding='utf-8') as f:
    text = f.read()
text = re.sub(r'(?m)^file_path = ".*"$', lambda m: 'file_path = "%s"' % xex.replace('\\', '/'), text)
with open(dst, 'w', encoding='utf-8', newline='') as f:
    f.write(text)
