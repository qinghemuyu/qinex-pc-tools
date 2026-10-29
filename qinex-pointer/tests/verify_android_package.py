"""使用真实内嵌工具检查模块 ZIP、签名 APK、mdpi 尺寸和边界热点。"""
import argparse
import io
import json
from pathlib import Path
import struct
import subprocess
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--directory', type=Path, required=True, help='pointer_android_tests --package 的输出目录')
parser.add_argument('--runtime-manifest', type=Path, default=root.parent / 'build/pointer-android/qinex-pointer/android-runtime/runtime.json')
options = parser.parse_args()
directory = options.directory.resolve()
package = directory / '光标模块.zip'
with zipfile.ZipFile(package) as module:
    assert module.testzip() is None
    assert b'id=qinex_stylus_cursor' in module.read('module.prop')
    service = module.read('service.sh')
    assert b'cmd overlay enable --user 0 com.qinex.pointer.stylus' in service
    apk_data = module.read('system/product/overlay/QInEXStylus/QInEXStylus.apk')
with zipfile.ZipFile(io.BytesIO(apk_data)) as apk:
    assert apk.testzip() is None
    png = apk.read(next(n for n in apk.namelist() if n.endswith('/qinex_cursor.png')))
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    width, height = struct.unpack_from('>II', png, 16)
    assert (width, height) == (32, 16), (width, height)
    assert not any(n.endswith('.dex') for n in apk.namelist())
manifest = json.loads(options.runtime_manifest.read_text(encoding='utf-8'))
runtime = directory / 'runtime' / __import__('hashlib').sha256(
    options.runtime_manifest.read_bytes()).hexdigest()
apk_path = directory / 'verified.apk'
apk_path.write_bytes(apk_data)
# 原生工具仍采用 ASCII 参数与宽字符 cwd，验证中文父目录。
tree = subprocess.check_output([str(runtime / 'tools/aapt2.exe'), 'dump', 'xmltree',
    '--file', 'res/drawable/pointer_crosshair_icon.xml', 'verified.apk'], cwd=directory).decode('utf-8')
assert '31.000000dp' in tree or '31dp' in tree or '31.0dp' in tree, tree
assert '15.000000dp' in tree or '15dp' in tree or '15.0dp' in tree, tree
subprocess.run([str(runtime / 'java/bin/java.exe'), '-jar', str(runtime / 'tools/lib/apksigner.jar'),
    'verify', '--verbose', str(apk_path)], check=True, stdout=subprocess.DEVNULL)
print('ZIP CRC、APK 签名、32×16 mdpi 图片与 31/15 dp 边界热点验证通过。')
