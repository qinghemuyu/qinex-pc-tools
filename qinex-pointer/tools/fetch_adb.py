"""从 Google 官方地址取得固定版本 ADB，下载文件与解压内容均留在忽略目录。"""
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

project = Path(__file__).resolve().parents[1]
package = json.loads((project / 'tools/android_runtime_versions.json').read_text(encoding='utf-8'))['adb_package']
root = project.parent / 'dependencies'
root.mkdir(parents=True, exist_ok=True)
archive = root / package['name']
if not archive.exists():
    pending = archive.with_suffix('.download')
    urllib.request.urlretrieve(package['link'], pending)
    if hashlib.sha256(pending.read_bytes()).hexdigest() != package['checksum']:
        raise SystemExit('ADB 下载摘要不匹配，停止解压。')
    pending.replace(archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != package['checksum']:
    raise SystemExit('ADB 缓存摘要不匹配，停止解压。')
with zipfile.ZipFile(archive) as bundle:
    for entry in bundle.infolist():
        destination = (root / entry.filename).resolve()
        if not destination.is_relative_to((root / 'platform-tools').resolve()):
            raise SystemExit('依赖包包含非预期路径，停止解压。')
    bundle.extractall(root)
print(root / 'platform-tools')
