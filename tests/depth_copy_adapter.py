"""Generate a test-only seam from the current depth adapter, not a second implementation.

python tests/depth_copy_adapter.py <scratch-dir>
cl /std:c++20 /EHsc /W4 /WX /O2 /I <scratch-dir> tests/nr_depth_copy_thisframe_dx12_gpu.cpp
   OptiScaler/native/DepthFinderCore.cpp OptiScaler/native/DepthResolveDx12.cpp /link user32.lib
"""
from pathlib import Path
import sys

repo = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
source = (repo / "OptiScaler/resource_tracking/GenericDepth_Dx12.cpp").read_text()


def between(start, end):
    return source[source.index(start):source.index(end, source.index(start))]


header = "#pragma once\n"
if "CopyNow CopyThisFrame(" not in source:
    header += "#define DEPTH_COPY_UNAVAILABLE\n"
else:
    header += """#include <format>
#include <mutex>
#include <unordered_map>
#include """ + f'"{repo.as_posix()}/OptiScaler/resource_tracking/GenericDepth_Dx12.h"\n'
    for name in ("DepthFinderCore", "DepthCopyLedger", "DepthResolveDx12", "DepthSnapshotDx12", "NativeDriverRoles"):
        header += f'#include "{repo.as_posix()}/OptiScaler/native/{name}.h"\n'
    header += f'#include "{repo.as_posix()}/OptiScaler/hooks/ListVtables.h"\n'
    header += """inline std::string depthLog;
inline unsigned depthResolves = 0;
template<class... Args> const char* CountDepthResolve(Args... args) {
    const char* failed = native::RecordDepthResolve(args...);
    depthResolves += !failed;
    return failed;
}
#define LOG_INFO(...) (depthLog += std::format(__VA_ARGS__) + "\\n")
#define LOG_DEBUG(...) ((void)std::format(__VA_ARGS__))
struct Config
{
    struct Value { unsigned value; unsigned value_or_default() const { return value; } };
    Value DlssNrNativeMotion { 1 }, DlssNrNativeDepthWarmupFrames { 0 }, ReplaceGameFsr1 { 0 },
          DlssNrNativeDepthResolveInList { 1 };
    static Config* Instance();
};
inline Config depthConfig;
inline Config* Config::Instance() { return &depthConfig; }
namespace {
"""
    header += between("struct DsvInfo", "void STDMETHODCALLTYPE hkCreateDepthStencilView")
    header += between("void TakeSnapshot(", "void OnIndirect(")
    header += between("void OnClose(", "struct DepthTag;")
    header += between("struct DepthTag;", "bool ListThreadHasNoTls()")
    header += "}\nnamespace GenericDepthDx12 {\n"
    header += between("void OnPresent(", "void RecordPreviewCopy(")
    header += "}\n"
    header = header.replace("native::RecordDepthResolve(list,", "CountDepthResolve(list,")
(out / "depth_copy_adapter.h").write_text(header, newline="\r\n")
(out / "pch.h").write_text("#pragma once\n", newline="\r\n")
print(f"generated {out / 'depth_copy_adapter.h'} from {repo}")
