"""Strict host contract for the production descriptor/root/list helpers (vcvars64 first)."""
from pathlib import Path
import json
import subprocess
import sys
import hashlib

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(sys.argv[1]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
(OUT / 'pch.h').write_text('#pragma once\n#include <windows.h>\n#include <d3d12.h>\n#include <algorithm>\n#include <cstdint>\n', encoding='utf-8')
source = (ROOT / 'OptiScaler/native/DescriptorTrackerDx12.cpp').read_text(encoding='utf-8-sig')
identity = hashlib.sha256(source.encode()).hexdigest()
mutate = len(sys.argv) > 3 and sys.argv[3] == 'hot-allocate'
if mutate:
    source = source.replace('    device->heapAttempts.fetch_add(1, std::memory_order_relaxed);',
                            '    (void)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);\n'
                            '    device->heapAttempts.fetch_add(1, std::memory_order_relaxed);', 1)
(OUT / 'descriptor-source.cpp').write_text(source, encoding='utf-8')
(OUT / 'observed.cpp').write_text('''#include "pch.h"
bool trackingContractHot = false, trackingContractFailAllocation = false;
unsigned trackingContractHotAllocations = 0, trackingContractHotLocks = 0;
void* ContractAllocate(void* address, SIZE_T bytes, DWORD type, DWORD protection)
{
    if (trackingContractHot) { ++trackingContractHotAllocations; return nullptr; }
    return trackingContractFailAllocation ? nullptr : VirtualAlloc(address, bytes, type, protection);
}
void ContractLock(PSRWLOCK lock)
{
    if (trackingContractHot) ++trackingContractHotLocks;
    AcquireSRWLockExclusive(lock);
}
#define VirtualAlloc ContractAllocate
#define AcquireSRWLockExclusive ContractLock
#include "descriptor-source.cpp"
''', encoding='utf-8')
command = ['cl', '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/FC', f'/I{OUT}',
           f'/I{ROOT / "OptiScaler/native"}',
           str(ROOT / ('tests/' + (sys.argv[2] if len(sys.argv) > 2 else 'nr_list_tracking_smoke.cpp'))), str(OUT / 'observed.cpp'),
           f'/Fo{OUT}\\', f'/Fe{OUT / "tracking.exe"}', 'd3d12.lib']
result = subprocess.run(command, cwd=OUT, capture_output=True, text=True)
(OUT / 'compile.log').write_text(result.stdout + result.stderr, encoding='utf-8')
receipt = {'command': command, 'compile_exit': result.returncode, 'production_source_sha256': identity, 'hot_allocation_mutation': mutate}
if result.returncode == 0:
    result = subprocess.run([str(OUT / 'tracking.exe')], cwd=OUT, capture_output=True, text=True, timeout=60)
    (OUT / 'run.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    receipt['run_exit'] = result.returncode
(OUT / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
print(json.dumps(receipt))
sys.exit(result.returncode)
