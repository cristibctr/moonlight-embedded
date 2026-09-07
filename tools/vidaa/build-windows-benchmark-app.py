"""Build the temporary Sunshine app command from its readable PowerShell source."""
import base64
import json
from pathlib import Path

root = Path(__file__).resolve().parent
payload = base64.b64encode((root / 'windows-benchmark.ps1').read_text().encode('utf-16-le')).decode()
app = {
    'name': 'Moonlight Benchmark', 'index': 1,
    'cmd': 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -NoProfile -NonInteractive -EncodedCommand ' + payload,
    'working-dir': 'C:\\Windows', 'auto-detach': True, 'wait-all': True,
    'exit-timeout': 5, 'elevated': False, 'image-path': 'desktop.png',
    'prep-cmd': [], 'detached': [],
}
(root / 'windows-benchmark-app-20260906.json').write_text(json.dumps(app, indent=2) + '\n')
print('Built temporary benchmark app JSON (no credentials stored).')
