"""兼容入口：调用仓库统一的桌面打包脚本，包含许可文件并先运行测试。"""
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
shell = shutil.which('pwsh') or shutil.which('powershell')
if not shell:
    raise SystemExit('找不到 PowerShell，请从仓库根目录运行 package.ps1。')
subprocess.run([shell, '-NoProfile', '-File', str(root / 'package.ps1')], check=True, cwd=root)
