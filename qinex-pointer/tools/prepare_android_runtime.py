"""将已取得的固定版本依赖编入 EXE；本脚本不在用户运行时下载组件。"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess


def prepare(adb, sdk, jdk, output):
    root = Path(__file__).resolve().parents[1]
    versions = json.loads((root / 'tools/android_runtime_versions.json').read_text(encoding='utf-8'))
    properties = (sdk / 'source.properties').read_text(encoding='utf-8')
    if not re.search(r'Pkg\.Revision\s*=\s*' + re.escape(versions['build_tools']) + r'\s*$', properties, re.MULTILINE):
        raise SystemExit('Android Build Tools 版本与锁定清单不一致。')
    output.mkdir(parents=True, exist_ok=True)
    payload = output / 'payload'
    modules = 'java.base,java.logging,java.xml,jdk.crypto.ec,jdk.crypto.mscapi,jdk.unsupported'
    release = (jdk / 'release').read_bytes()
    if b'Eclipse Adoptium' not in release:
        raise SystemExit('内嵌 Java 必须使用 Eclipse Temurin OpenJDK，不能混入本机其它 JDK。')
    expected_java = versions['temurin']['version']['semver'].split('+')[0]
    if f'JAVA_VERSION="{expected_java}"'.encode() not in release:
        raise SystemExit('Temurin 版本与锁定清单不一致。')
    adb_version = subprocess.check_output([str(adb / 'adb.exe'), 'version']).decode('utf-8')
    if versions['adb'] not in adb_version:
        raise SystemExit('ADB 版本与锁定清单不一致。')
    java = payload / 'java'
    marker = output / 'java.version'
    identity = hashlib.sha256(release + modules.encode()).hexdigest()
    if java.exists() and (not marker.exists() or marker.read_text() != identity):
        raise SystemExit('Java 缓存版本不匹配，请换一个构建目录。')
    if not java.exists():
        java.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(jdk / 'bin/jlink.exe'), '--add-modules', modules,
            '--compress=2', '--no-header-files', '--no-man-pages', '--strip-debug',
            '--output', str(java)], check=True)
        marker.write_text(identity)
    for group, source, files in [
        ('adb', adb, ['adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt']),
        ('tools', sdk, ['aapt2.exe', 'zipalign.exe', 'libwinpthread-1.dll', 'NOTICE.txt', 'lib/apksigner.jar']),
    ]:
        for name in files:
            src, dst = source / name, payload / group / name
            if not src.is_file():
                raise SystemExit(f'缺少 Android 内嵌依赖：{src}')
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    # 版本、许可证和来源随包保留，程序按清单校验所有文件。
    manifest = []
    for index, path in enumerate(sorted(p for p in payload.rglob('*') if p.is_file()), 5000):
        relative = path.relative_to(payload).as_posix()
        manifest.append({'id': index, 'path': relative, 'group': relative.split('/')[0],
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size': path.stat().st_size})
    record = {'adb': versions['adb'], 'build_tools': versions['build_tools'],
        'java_release': release.decode(), 'jlink_modules': modules, 'files': manifest}
    content = json.dumps(record, ensure_ascii=False, indent=2)
    digest = hashlib.sha256(content.encode()).hexdigest()
    (output / 'runtime.json').write_bytes(content.encode('utf-8'))
    header = '#pragma once\nstruct RuntimeFile { int id; const wchar_t* path; const char* group; const char* hash; };\n'
    header += f'inline constexpr wchar_t kRuntimeHash[] = L"{digest}";\n'
    header += 'inline constexpr RuntimeFile kRuntimeFiles[] = {\n'
    rc = '#pragma code_page(65001)\n#include <windows.h>\n'
    for item in manifest:
        header += f'    {{{item["id"]}, L"{item["path"]}", "{item["group"]}", "{item["sha256"]}"}},\n'
        rc += f'{item["id"]} RCDATA "{(payload / item["path"]).as_posix()}"\n'
    header += '};\n'
    # 清单单独嵌入，不把自身摘要递归放进 JSON。
    header = header.rsplit('};\n', 1)[0] + f'    {{4999, L"dependencies.json", "adb", "{digest}"}},\n}};\n'
    rc += f'4999 RCDATA "{(output / "runtime.json").as_posix()}"\n'
    for name, value in [('runtime_manifest.h', header), ('runtime.rc', rc)]:
        destination = output / name
        if not destination.exists() or destination.read_text(encoding='utf-8') != value:
            destination.write_text(value, encoding='utf-8')
    print(f'内嵌依赖：{len(manifest)} 个文件，{sum(x["size"] for x in manifest)/1024/1024:.1f} MiB，SHA256 {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--adb', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.adb.resolve(), args.sdk.resolve(), args.jdk.resolve(), args.output.resolve())
