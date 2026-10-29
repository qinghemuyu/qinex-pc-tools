"""维护者准备构建依赖：从 Adoptium 官方接口下载、校验并解压 Temurin 17。"""
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

root = Path(__file__).resolve().parents[1] / 'build/android-dependencies'
root.mkdir(parents=True, exist_ok=True)
asset = json.loads((Path(__file__).with_name('android_runtime_versions.json')).read_text())['temurin']
package = asset['binary']['package']
archive = root / package['name']
if not archive.exists():
    print('下载 ' + package['link'], flush=True)
    urllib.request.urlretrieve(package['link'], archive)
if hashlib.sha256(archive.read_bytes()).hexdigest() != package['checksum']:
    raise SystemExit('Temurin 下载摘要不匹配；保留文件供检查。')
with zipfile.ZipFile(archive) as bundle:
    for entry in bundle.infolist():
        destination = (root / entry.filename).resolve()
        if not destination.is_relative_to(root.resolve()):
            raise SystemExit('拒绝包含越界路径的依赖包。')
    bundle.extractall(root)
print('Temurin ' + asset['version']['semver'], flush=True)
print((root / ('jdk-' + asset['version']['semver'])).resolve(), flush=True)
