"""Strict host contract for the production descriptor/root/list helpers (vcvars64 first)."""
from pathlib import Path
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
(OUT / 'pch.h').write_text('#pragma once\n#include <windows.h>\n#include <d3d12.h>\n#include <algorithm>\n#include <cstdint>\n', encoding='utf-8')
command = ['cl', '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/FC', f'/I{OUT}',
           str(ROOT / 'tests/nr_list_tracking_smoke.cpp'), str(ROOT / 'OptiScaler/native/DescriptorTrackerDx12.cpp'),
           f'/Fo{OUT}\\', f'/Fe{OUT / "tracking.exe"}', 'd3d12.lib']
result = subprocess.run(command, cwd=OUT, capture_output=True, text=True)
(OUT / 'compile.log').write_text(result.stdout + result.stderr, encoding='utf-8')
receipt = {'command': command, 'compile_exit': result.returncode}
if result.returncode == 0:
    result = subprocess.run([str(OUT / 'tracking.exe')], cwd=OUT, capture_output=True, text=True, timeout=60)
    (OUT / 'run.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    receipt['run_exit'] = result.returncode
(OUT / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
print(json.dumps(receipt))
sys.exit(result.returncode)
