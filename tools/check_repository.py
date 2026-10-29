"""检查待跟踪文件中的产物、私密文件名、私钥标记和失效的本地 Markdown 链接。"""
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote

root = Path(__file__).resolve().parents[1]
raw = subprocess.check_output(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=root)
files = sorted(set(p for p in raw.decode('utf-8').split('\0') if p))
errors = []
binary_suffixes = {'.exe', '.dll', '.apk', '.dex', '.jar', '.zip', '.obj', '.pdb'}
private_suffixes = {'.pem', '.key', '.jks', '.keystore'}
for name in files:
    path = root / name
    if path.suffix.lower() in binary_suffixes | private_suffixes or path.name in {'.env', 'keystore.properties', 'local.properties', 'link.json', 'pointer.ini'}:
        errors.append(f'不应跟踪的文件：{name}')
    if path.suffix.lower() == '.ico':
        continue
    try:
        content = path.read_text(encoding='utf-8-sig')
    except UnicodeDecodeError:
        errors.append(f'非 UTF-8 文本：{name}')
        continue
    if re.search(r'-----BEGIN (?:[A-Z ]+)?PRIVATE KEY-----', content):
        errors.append(f'发现私钥标记：{name}')
    if path.suffix.lower() != '.md':
        continue
    for match in re.finditer(r'\[[^\]\n]+\]\(([^)\n]+)\)', content):
        target = match.group(1).strip().strip('<>')
        if re.match(r'^[a-zA-Z][a-zA-Z0-9+.-]*:', target) or target.startswith('#'):
            continue
        target = unquote(target.split('#', 1)[0])
        if target and not (path.parent / target).exists():
            errors.append(f'失效本地链接：{name} -> {target}')
if errors:
    print('\n'.join(errors))
    sys.exit(1)
print(f'检查通过：{len(files)} 个源码/文档文件；常见私密文件、产物、私钥标记及本地 Markdown 链接未发现异常。')
