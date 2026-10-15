#pragma once

// From the repo root after vcvars64:
// cl /nologo /std:c++20 /EHsc /W4 /O2 /FC /TP /DD3D12_HARNESS_SELFTEST tests\d3d12_harness.h /Fe:harness.exe
// harness.exe [warp] [gbv]

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

namespace d3d12_test
{
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

inline bool Ok(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr))
        return true;
    std::printf("FAIL %s: 0x%08X\n", what, static_cast<unsigned>(hr));
    return false;
}

struct Checks
{
    unsigned failures = 0, skipped = 0;
    bool Check(bool ok, const char* what)
    {
        std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
        return ok;
    }
    void Skip(const char* what)
    {
        ++skipped;
        std::printf("SKIP %s\n", what);
    }
    int Summary() const
    {
        std::printf("%s (%u failures, %u skipped)\n", failures ? "FAIL" : "PASS", failures, skipped);
        return failures ? 1 : 0;
    }
};

inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after,
                                         UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, subresource, before, after };
    return b;
}

inline ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    // Upload and readback heaps have mandatory, fixed resource states.
    if (type == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (type == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    ComPtr<ID3D12Resource> result;
    Ok(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)),
       "buffer");
    return result;
}

inline ComPtr<ID3D12Resource> Texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear = nullptr)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> result;
    Ok(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&result)),
       "texture");
    return result;
}

inline ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                      D3D12_RESOURCE_STATES state,
                                      D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                      const D3D12_CLEAR_VALUE* clear = nullptr)
{
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return Texture(device, desc, state, clear);
}

inline ComPtr<ID3D12DescriptorHeap> DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count,
                                                   bool visible = false)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc {};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> result;
    Ok(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&result)), "descriptor heap");
    return result;
}

inline ComPtr<ID3D12RootSignature> RootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc)
{
    ComPtr<ID3DBlob> blob, errors;
    ComPtr<ID3D12RootSignature> result;
    if (Ok(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors),
           "serialize root signature"))
        Ok(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&result)),
           "root signature");
    else if (errors)
        std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    return result;
}

enum class Adapter
{
    Hardware,
    Warp
};

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    Gpu() = default;
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;
    ~Gpu()
    {
        if (event)
            CloseHandle(event);
    }

    bool CreateList(ComPtr<ID3D12CommandAllocator>& alloc, ComPtr<ID3D12GraphicsCommandList>& commands,
                    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) const
    {
        return Ok(device->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc)), "allocator") &&
               Ok(device->CreateCommandList(0, type, alloc.Get(), nullptr, IID_PPV_ARGS(&commands)), "command list");
    }

    bool Create(Adapter adapter = Adapter::Hardware, bool gbv = false, bool debugLayer = true)
    {
        if (device)
            return false;
        if (debugLayer || gbv)
        {
            ComPtr<ID3D12Debug> debug;
            if (!Ok(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "debug layer"))
                return false;
            debug->EnableDebugLayer();
            if (gbv)
            {
                ComPtr<ID3D12Debug1> debug1;
                if (!Ok(debug.As(&debug1), "GPU-based validation"))
                    return false;
                debug1->SetEnableGPUBasedValidation(TRUE);
            }
        }
        ComPtr<IDXGIFactory4> factory;
        if (!Ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory"))
            return false;
        ComPtr<IDXGIAdapter1> chosen;
        if (adapter == Adapter::Warp)
        {
            if (!Ok(factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen)), "WARP adapter"))
                return false;
        }
        else
        {
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> candidate;
                const HRESULT hr = factory->EnumAdapters1(i, &candidate);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (!Ok(hr, "hardware adapter"))
                    return false;
                DXGI_ADAPTER_DESC1 desc {};
                if (SUCCEEDED(candidate->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(
                        D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
                {
                    chosen = candidate;
                    break;
                }
            }
            if (!chosen)
                return false;
        }
        DXGI_ADAPTER_DESC1 desc {};
        chosen->GetDesc1(&desc);
        std::printf("adapter: %ls; debug %d, gbv %d\n", desc.Description, debugLayer || gbv, gbv);
        if (!Ok(D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device"))
            return false;
        if ((debugLayer || gbv) && !Ok(device.As(&info), "info queue"))
            return false;
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (!Ok(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue") || !CreateList(allocator, list) ||
            !Ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence"))
            return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }

    bool WaitIdle(DWORD timeoutMs = 30000)
    {
        const UINT64 value = ++fenceValue;
        if (!Ok(queue->Signal(fence.Get(), value), "signal"))
            return false;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (fence->GetCompletedValue() < value)
        {
            if (!Ok(fence->SetEventOnCompletion(value, event), "fence event"))
                return false;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(event, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            {
                std::printf("FAIL GPU wait timed out or failed\n");
                return false;
            }
        }
        return Ok(device->GetDeviceRemovedReason(), "device after wait");
    }

    // Starts with an open list, returns with it open and empty. No allocator reset before the fence completes.
    bool ExecuteAndWait(DWORD timeoutMs = 30000)
    {
        if (!Ok(list->Close(), "close"))
            return false;
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        return WaitIdle(timeoutMs) && Ok(allocator->Reset(), "allocator reset") &&
               Ok(list->Reset(allocator.Get(), nullptr), "list reset");
    }

    UINT64 Messages(D3D12_MESSAGE_SEVERITY threshold = D3D12_MESSAGE_SEVERITY_WARNING, bool clear = true) const
    {
        if (!info)
            return 0;
        UINT64 count = info->GetNumMessagesDiscardedByMessageCountLimit();
        for (UINT64 i = 0, n = info->GetNumStoredMessagesAllowedByRetrievalFilter(); i < n; ++i)
        {
            SIZE_T size = 0;
            if (!Ok(info->GetMessage(i, nullptr, &size), "debug message size"))
            {
                ++count;
                continue;
            }
            Bytes bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            if (!Ok(info->GetMessage(i, message, &size), "debug message"))
            {
                ++count;
                continue;
            }
            if (message->Severity <= threshold)
            {
                ++count;
                std::printf("debug layer: %s\n", message->pDescription);
            }
        }
        if (clear)
            info->ClearStoredMessages();
        return count;
    }

    // Single-sample Texture2D subresource, tightly packed rows (also block rows); restores its supplied state.
    // Submits all work on list and resets it, so any previous bindings must be rebound afterwards.
    bool Readback(ID3D12Resource* texture, D3D12_RESOURCE_STATES state, Bytes& out, UINT subresource = 0)
    {
        out.clear();
        if (!texture)
            return false;
        const auto desc = texture->GetDesc();
        D3D12_FEATURE_DATA_FORMAT_INFO format { desc.Format, 0 };
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            !Ok(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format, sizeof(format)), "format planes") ||
            subresource >= UINT(desc.MipLevels) * desc.DepthOrArraySize * format.PlaneCount)
            return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT rows = 0;
        UINT64 rowSize = 0, total = 0;
        device->GetCopyableFootprints(&desc, subresource, 1, 0, &fp, &rows, &rowSize, &total);
        auto buffer = Buffer(device.Get(), total, D3D12_HEAP_TYPE_READBACK);
        if (!buffer)
            return false;
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            const auto b = Transition(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE, subresource);
            list->ResourceBarrier(1, &b);
        }
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = subresource;
        dst.pResource = buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            const auto b = Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state, subresource);
            list->ResourceBarrier(1, &b);
        }
        if (!ExecuteAndWait())
            return false;
        void* mapped = nullptr;
        const D3D12_RANGE read { 0, static_cast<SIZE_T>(total) }, written { 0, 0 };
        if (!Ok(buffer->Map(0, &read, &mapped), "readback map"))
            return false;
        out.resize(static_cast<size_t>(rowSize) * rows);
        for (UINT y = 0; y < rows; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y * rowSize),
                        static_cast<const uint8_t*>(mapped) + fp.Offset +
                            static_cast<size_t>(y) * fp.Footprint.RowPitch,
                        static_cast<size_t>(rowSize));
        buffer->Unmap(0, &written);
        return true;
    }
};

enum class CompileResult
{
    Success,
    Skipped,
    Failed
};

inline CompileResult CompileHlsl(std::string_view source, const char* entry, const char* target, Bytes& out,
                                 std::initializer_list<const wchar_t*> extraDxcArgs = {})
{
    out.clear();
    if (std::string_view(target).find("_6_") == std::string_view::npos)
    {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), "harness", nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr))
        {
            if (errors)
                std::printf("fxc: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            Ok(hr, "FXC compile");
            return CompileResult::Failed;
        }
        const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        out.assign(p, p + code->GetBufferSize());
        return CompileResult::Success;
    }
    std::error_code ec;
    wchar_t programFiles[32768] {};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", programFiles, static_cast<DWORD>(std::size(programFiles))))
    {
        std::printf("SKIP DXIL: Windows SDK directory unavailable\n");
        return CompileResult::Skipped;
    }
    const fs::path kits = fs::path(programFiles) / "Windows Kits/10/bin";
    std::vector<fs::path> versions;
    for (fs::directory_iterator it(kits, ec), end; !ec && it != end; it.increment(ec))
        if (fs::exists(it->path() / "x64/dxcompiler.dll", ec) && fs::exists(it->path() / "x64/dxil.dll", ec))
            versions.push_back(it->path() / "x64");
    std::sort(versions.rbegin(), versions.rend());
    for (const auto& dir : versions)
    {
        HMODULE validator = LoadLibraryExW((dir / "dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE module = validator
                             ? LoadLibraryExW((dir / "dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)
                             : nullptr;
        auto create = module ? reinterpret_cast<DxcCreateInstanceProc>(
                                   reinterpret_cast<void*>(GetProcAddress(module, "DxcCreateInstance")))
                             : nullptr;
        CompileResult outcome = CompileResult::Skipped;
        {
            ComPtr<IDxcCompiler3> compiler;
            if (create && SUCCEEDED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
            {
                const std::wstring wentry(entry, entry + std::strlen(entry)),
                    wtarget(target, target + std::strlen(target));
                // The FSR1 headers use vector conditions unsupported by HLSL 2021's conditional operator.
                std::vector<const wchar_t*> args { L"-E",  wentry.c_str(), L"-T",  wtarget.c_str(),
                                                   L"-O3", L"-HV",         L"2018" };
                args.insert(args.end(), extraDxcArgs.begin(), extraDxcArgs.end());
                const DxcBuffer buffer { source.data(), source.size(), DXC_CP_UTF8 };
                ComPtr<IDxcResult> result;
                outcome = CompileResult::Failed;
                if (Ok(compiler->Compile(&buffer, args.data(), static_cast<UINT32>(args.size()), nullptr,
                                         IID_PPV_ARGS(&result)),
                       "DXC compile"))
                {
                    HRESULT status = E_FAIL;
                    result->GetStatus(&status);
                    ComPtr<IDxcBlob> code;
                    if (SUCCEEDED(status) && Ok(result->GetResult(&code), "DXIL result"))
                    {
                        const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
                        out.assign(p, p + code->GetBufferSize());
                        outcome = CompileResult::Success;
                    }
                    else
                    {
                        ComPtr<IDxcBlobEncoding> errors;
                        result->GetErrorBuffer(&errors);
                        if (errors)
                            std::printf("dxc: %.*s\n", static_cast<int>(errors->GetBufferSize()),
                                        static_cast<const char*>(errors->GetBufferPointer()));
                    }
                }
            }
        }
        if (module)
            FreeLibrary(module);
        if (validator)
            FreeLibrary(validator);
        if (outcome != CompileResult::Skipped)
            return outcome;
    }
    std::printf("SKIP DXIL: Windows SDK dxcompiler.dll and dxil.dll unavailable\n");
    return CompileResult::Skipped;
}
inline std::string ReadText(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

inline bool SetIni(const fs::path& path, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    std::error_code ec;
    const auto absolute = fs::absolute(path, ec);
    return !ec && WritePrivateProfileStringW(section, key, value, absolute.c_str()) != FALSE;
}

struct ChildResult
{
    DWORD exitCode = MAXDWORD;
    bool started = false, timedOut = false;
    std::string output;
};

inline ChildResult Launch(const fs::path& executable, const std::wstring& arguments, const fs::path& directory,
                          const fs::path& outputFile, DWORD timeoutMs = 300000)
{
    ChildResult run;
    SECURITY_ATTRIBUTES sa { sizeof(sa), nullptr, TRUE };
    HANDLE output = CreateFileW(outputFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE)
    {
        STARTUPINFOW si {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = output;
        si.hStdInput = input;
        PROCESS_INFORMATION pi {};
        std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
        run.started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                     directory.c_str(), &si, &pi) != FALSE;
        if (run.started)
        {
            // Capture to a file: a blocked pipe read must not defeat the child's timeout.
            const DWORD waited = WaitForSingleObject(pi.hProcess, timeoutMs);
            run.timedOut = waited == WAIT_TIMEOUT;
            if (waited != WAIT_OBJECT_0)
            {
                TerminateProcess(pi.hProcess, 124);
                WaitForSingleObject(pi.hProcess, 10000);
            }
            GetExitCodeProcess(pi.hProcess, &run.exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    if (input != INVALID_HANDLE_VALUE)
        CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE)
        CloseHandle(output);
    if (run.started)
        run.output = ReadText(outputFile);
    return run;
}

enum class FsrPass
{
    Easu,
    Rcas,
    Uber
};

inline std::string FsrSource(const fs::path& repo, FsrPass pass, bool compute = false)
{
    const auto dir = repo / "OptiScaler/shaders/output_scaling/fsr1";
    const auto a = ReadText(dir / "ffx_a.h"), fsr = ReadText(dir / "ffx_fsr1.h");
    if (a.empty() || fsr.empty() || (pass == FsrPass::Uber && !compute))
        return {};
    std::string source = "#define A_GPU 1\n#define A_HLSL 1\n";
    if (pass != FsrPass::Rcas)
        source += "#define FSR_EASU_F 1\n";
    if (pass != FsrPass::Easu)
        source += "#define FSR_RCAS_F 1\n";
    source += a + "\nTexture2D<float4> fsrInput : register(t0);\nSamplerState fsrSampler : register(s0);\n";
    if (pass == FsrPass::Uber)
        source += "cbuffer C : register(b0) { float2 resolution; float2 upscaled; float sharpness; uint pass; };\n";
    else
        source += "cbuffer C : register(b0) { uint4 con0; uint4 con1; uint4 con2; uint4 con3; };\n";
    if (pass != FsrPass::Rcas)
        source += "AF4 FsrEasuRF(AF2 p) { return fsrInput.GatherRed(fsrSampler,p); }\n"
                  "AF4 FsrEasuGF(AF2 p) { return fsrInput.GatherGreen(fsrSampler,p); }\n"
                  "AF4 FsrEasuBF(AF2 p) { return fsrInput.GatherBlue(fsrSampler,p); }\n";
    if (pass != FsrPass::Easu)
        source += "AF4 FsrRcasLoadF(ASU2 p) { return fsrInput.Load(int3(p,0)); }\n"
                  "void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b) {}\n";
    source += fsr + "\n";
    if (compute)
        source += "RWTexture2D<float4> fsrOutput : register(u0);\n"
                  "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {\n"
                  "uint w,h; fsrOutput.GetDimensions(w,h); if (id.x>=w || id.y>=h) return; uint2 p=id.xy; float3 c;\n";
    else
        source += "float4 main(float4 position : SV_Position) : SV_Target { uint2 p=uint2(position.xy); float3 c;\n";
    if (pass == FsrPass::Uber)
        source += "if (pass==0) { uint4 c0,c1,c2,c3; FsrEasuCon(c0,c1,c2,c3,resolution.x,resolution.y,"
                  "resolution.x,resolution.y,upscaled.x,upscaled.y); FsrEasuF(c,p,c0,c1,c2,c3); }\n"
                  "else { uint4 c0; FsrRcasCon(c0,sharpness); FsrRcasF(c.r,c.g,c.b,p,c0); }\n";
    else if (pass == FsrPass::Easu)
        source += "FsrEasuF(c,p,con0,con1,con2,con3);\n";
    else
        source += "FsrRcasF(c.r,c.g,c.b,p,con0);\n";
    return source + (compute ? "fsrOutput[p]=float4(c,1); }\n" : "return float4(c,1); }\n");
}

// Tables deliberately live for the process: other patchers may retain these forwarding entries.
struct PrivateListVtable
{
    void** runtimeTable;
    void* rtti;
    void* entries[86];
    void* previous[86];
};

struct RuntimeListIdentity
{
    ID3D12GraphicsCommandList* list;
    void** saved;
    RuntimeListIdentity(ID3D12GraphicsCommandList* commands, void** runtime) : list(commands)
    {
        saved = *reinterpret_cast<void***>(list);
        *reinterpret_cast<void***>(list) = runtime;
    }
    ~RuntimeListIdentity() { *reinterpret_cast<void***>(list) = saved; }
};

template <size_t Slot, typename Function> struct PrivateForward;
template <size_t Slot, typename Result, typename... Args>
struct PrivateForward<Slot, Result(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, Args...)>
{
    static Result STDMETHODCALLTYPE Call(ID3D12GraphicsCommandList* list, Args... args)
    {
        auto* table = reinterpret_cast<PrivateListVtable*>(
            reinterpret_cast<uint8_t*>(*reinterpret_cast<void***>(list)) - offsetof(PrivateListVtable, entries));
        // The debug runtime checks its class identity during internal forwarding, notably Reset.
        const RuntimeListIdentity runtime(list, table->runtimeTable);
        return reinterpret_cast<Result(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, Args...)>(table->previous[Slot])(
            list, args...);
    }
};

inline PrivateListVtable* MakePrivateVtable(ID3D12GraphicsCommandList* list,
                                            std::initializer_list<UINT> slots = { 9, 10, 12, 13, 14, 21, 25, 26, 38 })
{
    if (!list)
        return nullptr;
    auto* table = new (std::nothrow) PrivateListVtable {};
    if (!table)
        return nullptr;
    const IID versions[] = { __uuidof(ID3D12GraphicsCommandList10), __uuidof(ID3D12GraphicsCommandList9),
                             __uuidof(ID3D12GraphicsCommandList8),  __uuidof(ID3D12GraphicsCommandList7),
                             __uuidof(ID3D12GraphicsCommandList6),  __uuidof(ID3D12GraphicsCommandList5),
                             __uuidof(ID3D12GraphicsCommandList4),  __uuidof(ID3D12GraphicsCommandList3),
                             __uuidof(ID3D12GraphicsCommandList2),  __uuidof(ID3D12GraphicsCommandList1) };
    const size_t sizes[] = { 86, 84, 82, 81, 80, 79, 77, 68, 67, 66 };
    size_t count = 60;
    for (size_t i = 0; i < std::size(versions); ++i)
    {
        ComPtr<IUnknown> version;
        if (SUCCEEDED(list->QueryInterface(versions[i], &version)) && version.Get() == list)
        {
            count = sizes[i];
            break;
        }
    }
    auto** original = *reinterpret_cast<void***>(list);
    table->runtimeTable = original;
    table->rtti = original[-1];
    std::memcpy(table->entries, original, count * sizeof(void*));
    std::memcpy(table->previous, table->entries, sizeof(table->previous));
    using List = ID3D12GraphicsCommandList;
    for (UINT slot : slots)
    {
        void* entry = nullptr;
        switch (slot)
        {
        case 9:
            entry = reinterpret_cast<void*>(PrivateForward<9, HRESULT(STDMETHODCALLTYPE*)(List*)>::Call);
            break;
        case 10:
            entry =
                reinterpret_cast<void*>(PrivateForward<10, HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*,
                                                                                       ID3D12PipelineState*)>::Call);
            break;
        case 12:
            entry = reinterpret_cast<void*>(
                PrivateForward<12, void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, UINT)>::Call);
            break;
        case 13:
            entry = reinterpret_cast<void*>(
                PrivateForward<13, void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT)>::Call);
            break;
        case 14:
            entry =
                reinterpret_cast<void*>(PrivateForward<14, void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT)>::Call);
            break;
        case 21:
            entry = reinterpret_cast<void*>(
                PrivateForward<21, void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_VIEWPORT*)>::Call);
            break;
        case 25:
            entry = reinterpret_cast<void*>(
                PrivateForward<25, void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*)>::Call);
            break;
        case 26:
            entry = reinterpret_cast<void*>(
                PrivateForward<26, void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RESOURCE_BARRIER*)>::Call);
            break;
        case 38:
            entry = reinterpret_cast<void*>(
                PrivateForward<38, void(STDMETHODCALLTYPE*)(List*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)>::Call);
            break;
        default:
            delete table;
            return nullptr;
        }
        table->entries[slot] = entry;
    }
    *reinterpret_cast<void***>(list) = table->entries;
    return table;
}
} // namespace d3d12_test

#ifdef D3D12_HARNESS_SELFTEST
int main(int argc, char** argv)
{
    using namespace d3d12_test;
    Checks checks;
    if (argc == 2 && std::strcmp(argv[1], "child") == 0)
    {
        std::printf("child stdout\n");
        std::fprintf(stderr, "child stderr\n");
        return 7;
    }
    if (argc == 2 && std::strcmp(argv[1], "child-hang") == 0)
    {
        Sleep(10000);
        return 0;
    }
    bool warp = false, gbv = false;
    for (int i = 1; i < argc; ++i)
    {
        warp |= std::strcmp(argv[i], "warp") == 0;
        gbv |= std::strcmp(argv[i], "gbv") == 0;
    }
    Gpu gpu;
    if (!checks.Check(gpu.Create(warp ? Adapter::Warp : Adapter::Hardware, gbv), "create GPU with debug layer"))
        return checks.Summary();
    const auto device = gpu.device.Get();
    auto upload = Buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD);
    auto local = Buffer(device, 256);
    auto read = Buffer(device, 256, D3D12_HEAP_TYPE_READBACK);
    if (!checks.Check(upload && local && read, "default/upload/readback buffers"))
        return checks.Summary();
    void* mapped = nullptr;
    const D3D12_RANGE empty { 0, 0 }, full { 0, 256 };
    if (!checks.Check(Ok(upload->Map(0, &empty, &mapped), "upload map"), "map upload"))
        return checks.Summary();
    for (UINT i = 0; i < 256; ++i)
        static_cast<uint8_t*>(mapped)[i] = static_cast<uint8_t>(i);
    upload->Unmap(0, &full);
    gpu.list->CopyBufferRegion(local.Get(), 0, upload.Get(), 0, 256);
    const auto barrier = Transition(local.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->ResourceBarrier(1, &barrier);
    gpu.list->CopyBufferRegion(read.Get(), 0, local.Get(), 0, 256);
    if (!checks.Check(gpu.ExecuteAndWait(), "execute, fence wait, allocator/list reset"))
        return checks.Summary();
    if (!checks.Check(Ok(read->Map(0, &full, &mapped), "buffer map"), "map readback"))
        return checks.Summary();
    bool equal = true;
    for (UINT i = 0; i < 256; ++i)
        equal &= static_cast<uint8_t*>(mapped)[i] == static_cast<uint8_t>(i);
    read->Unmap(0, &empty);
    checks.Check(equal, "GPU buffer copy bytes");
    D3D12_CLEAR_VALUE clear {};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    clear.Color[0] = clear.Color[3] = 1.0f;
    auto texture = Texture(device, 7, 3, clear.Format, D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &clear);
    auto heap = DescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
    if (!checks.Check(texture && heap, "texture and RTV heap"))
        return checks.Summary();
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(texture.Get(), nullptr, handle);
    gpu.list->ClearRenderTargetView(handle, clear.Color, 0, nullptr);
    Bytes pixels;
    checks.Check(gpu.Readback(texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, pixels), "texture readback");
    equal = pixels.size() == 7 * 3 * 4;
    for (size_t i = 0; i < pixels.size(); ++i)
        equal &= pixels[i] == (i % 4 == 0 || i % 4 == 3 ? 255 : 0);
    checks.Check(equal, "readback strips row padding");
    gpu.list->ClearRenderTargetView(handle, clear.Color, 0, nullptr);
    checks.Check(gpu.ExecuteAndWait(), "readback restored the texture state");
    D3D12_ROOT_PARAMETER parameter {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;
    auto root = RootSignature(device, rootDesc);
    auto output = Buffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto visible = DescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true);
    if (!checks.Check(root && output && visible, "root signature, UAV buffer and shader-visible heap"))
        return checks.Summary();
    ComPtr<ID3D12CommandAllocator> extraAllocator;
    ComPtr<ID3D12GraphicsCommandList> extraList;
    if (checks.Check(gpu.CreateList(extraAllocator, extraList), "additional allocator/list pair"))
        checks.Check(Ok(extraList->Close(), "extra close"), "close additional list");
    auto* privateTable = MakePrivateVtable(gpu.list.Get());
    if (!checks.Check(privateTable != nullptr, "private command-list vtable"))
        return checks.Summary();
    Bytes shader;
    const char* source = "RWByteAddressBuffer dst : register(u0); [numthreads(1,1,1)] void main() { dst.Store(0,42); }";
    for (const char* target : { "cs_5_1", "cs_6_0" })
    {
        const auto compiled = CompileHlsl(source, "main", target, shader);
        if (compiled == CompileResult::Skipped)
        {
            checks.Skip(target);
            continue;
        }
        if (!checks.Check(compiled == CompileResult::Success && !shader.empty(), target))
            continue;
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc {};
        psoDesc.pRootSignature = root.Get();
        psoDesc.CS = { shader.data(), shader.size() };
        ComPtr<ID3D12PipelineState> pso;
        if (!checks.Check(Ok(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "compute PSO"),
                          "compiled shader accepted by D3D12"))
            continue;
        const auto toUav = Transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.list->ResourceBarrier(1, &toUav);
        gpu.list->SetComputeRootSignature(root.Get());
        gpu.list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
        gpu.list->SetPipelineState(pso.Get());
        gpu.list->Dispatch(1, 1, 1);
        const auto toCopy =
            Transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(1, &toCopy);
        gpu.list->CopyBufferRegion(read.Get(), 0, output.Get(), 0, 4);
        if (!checks.Check(gpu.ExecuteAndWait(), "execute shader through private vtable; Close/Reset forward"))
            break;
        if (checks.Check(Ok(read->Map(0, &full, &mapped), "shader result map"), "map shader result"))
        {
            uint32_t value = 0;
            std::memcpy(&value, mapped, sizeof(value));
            read->Unmap(0, &empty);
            checks.Check(value == 42, "compiled shader executed: value 42");
        }
        checks.Check(*reinterpret_cast<void***>(gpu.list.Get()) == privateTable->entries,
                     "runtime forwarding retained the private vtable");
    }
    *reinterpret_cast<void***>(gpu.list.Get()) = privateTable->runtimeTable;
    const fs::path repo = fs::path(__FILE__).parent_path().parent_path();
    for (const auto pass : { FsrPass::Easu, FsrPass::Rcas })
    {
        const auto fsr = FsrSource(repo, pass);
        checks.Check(!fsr.empty() && CompileHlsl(fsr, "main", "ps_5_1", shader) == CompileResult::Success,
                     pass == FsrPass::Easu ? "real FSR1 EASU PS (DXBC)" : "real FSR1 RCAS PS (DXBC)");
    }
    const auto easuCs = FsrSource(repo, FsrPass::Easu, true);
    checks.Check(!easuCs.empty() && CompileHlsl(easuCs, "main", "cs_5_1", shader) == CompileResult::Success,
                 "real FSR1 EASU CS (DXBC)");
    const auto uberCs = FsrSource(repo, FsrPass::Uber, true);
    const auto uberResult = CompileHlsl(uberCs, "main", "cs_6_0", shader);
    if (uberResult == CompileResult::Skipped)
        checks.Skip("FSR1 uber DXIL compiler unavailable");
    else
        checks.Check(!uberCs.empty() && uberResult == CompileResult::Success, "real FSR1 uber CS (DXIL)");
    wchar_t module[32768] {};
    GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    const fs::path exe(module), folder = exe.parent_path();
    const auto ini = folder / ("harness-selftest-" + std::to_string(GetCurrentProcessId()) + ".ini");
    const auto childOutput = fs::path(ini.wstring() + L".txt");
    checks.Check(SetIni(ini, L"Upscalers", L"ReplaceGameFsr1", L"true") &&
                     SetIni(ini, L"Upscalers", L"ReplaceGameFsr1", L"false") &&
                     ReadText(ini).find("ReplaceGameFsr1=false") != std::string::npos,
                 "write and update INI");
    const auto child = Launch(exe, L"child", folder, childOutput);
    checks.Check(child.started && !child.timedOut && child.exitCode == 7 &&
                     child.output.find("child stdout") != std::string::npos &&
                     child.output.find("child stderr") != std::string::npos,
                 "child exit code and stdout/stderr capture");
    const auto timeout = Launch(exe, L"child-hang", folder, childOutput, 100);
    checks.Check(timeout.started && timeout.timedOut && timeout.exitCode == 124, "child timeout terminates and reaps");
    std::error_code ec;
    checks.Check(fs::remove(ini, ec) && !ec && fs::remove(childOutput, ec) && !ec, "self-test files removed");
    checks.Check(Ok(gpu.info->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, "harness counter probe"),
                    "add debug probe") &&
                     gpu.Messages() == 1 && gpu.Messages() == 0,
                 "info-queue warning counting and clearing");
    checks.Check(gpu.Messages() == 0, "debug layer: zero errors or warnings");
    return checks.Summary();
}
#endif
