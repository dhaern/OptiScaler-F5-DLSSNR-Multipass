#include "pch.h"
// cl /std:c++20 /EHsc /W4 /O2 /I <host-pch-directory> tests/nr_fsr1_signature_smoke.cpp d3dcompiler.lib
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include "../OptiScaler/native/Fsr1Signature.h"
#pragma warning(push, 0)
#define A_CPU 1
#include "../OptiScaler/shaders/output_scaling/fsr1/ffx_fsr1.h"
#pragma warning(pop)
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
using namespace native::fsr1;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
static int fails = 0;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                                                \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)
static bool ReadFile(const fs::path& path, Bytes& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static std::string ReadText(const fs::path& path)
{
    Bytes b;
    return ReadFile(path, b) ? std::string(b.begin(), b.end()) : std::string();
}
using D3DStripShaderFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, ID3DBlob**);

struct Compilers
{
    pD3DCompile d3dCompile = nullptr;
    D3DStripShaderFn d3dStrip = nullptr;
    IDxcCompiler3* dxc = nullptr;
    std::string dxcFrom;

    ~Compilers()
    {
        if (dxc)
            dxc->Release();
    }

    void Load(const fs::path& repo)
    {
        if (HMODULE d3d = LoadLibraryW(L"d3dcompiler_47.dll"))
        {
            d3dCompile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(d3d, "D3DCompile")));
            d3dStrip =
                reinterpret_cast<D3DStripShaderFn>(reinterpret_cast<void*>(GetProcAddress(d3d, "D3DStripShader")));
        }

        std::vector<fs::path> candidates { repo / "OptiScaler/shaders/shader_tools/dxcompiler.dll", "dxcompiler.dll" };
        std::error_code ec;
        const fs::path kits = "C:/Program Files (x86)/Windows Kits/10/bin";
        std::vector<fs::path> versions;
        for (const auto& e : fs::directory_iterator(kits, ec))
            if (fs::exists(e.path() / "x64/dxcompiler.dll", ec))
                versions.push_back(e.path() / "x64/dxcompiler.dll");
        std::sort(versions.rbegin(), versions.rend());
        candidates.insert(candidates.end(), versions.begin(), versions.end());
        for (const fs::path& c : candidates)
        {
            HMODULE m = LoadLibraryExW(c.c_str(), nullptr, c.has_parent_path() ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
            auto create = m ? reinterpret_cast<DxcCreateInstanceProc>(
                                  reinterpret_cast<void*>(GetProcAddress(m, "DxcCreateInstance")))
                            : nullptr;
            if (create && SUCCEEDED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc))))
            {
                dxcFrom = c.string();
                break;
            }
        }
    }

    bool Dxbc(const std::string& source, const char* target, bool strip, Bytes& out) const
    {
        ID3DBlob *code = nullptr, *errors = nullptr;
        const HRESULT hr = d3dCompile(source.data(), source.size(), "fsr1_test", nullptr, nullptr, "main", target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (errors)
        {
            if (FAILED(hr))
                printf("  fxc %s: %.*s\n", target, int(errors->GetBufferSize()),
                       static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        if (FAILED(hr) || !code)
            return false;
        if (strip && d3dStrip)
        {
            ID3DBlob* stripped = nullptr;
            if (SUCCEEDED(d3dStrip(code->GetBufferPointer(), code->GetBufferSize(),
                                   D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO, &stripped)))
            {
                code->Release();
                code = stripped;
            }
        }
        const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        out.assign(p, p + code->GetBufferSize());
        code->Release();
        return true;
    }

    bool Dxil(const std::string& source, const wchar_t* target, bool sixteenBit, bool strip, Bytes& out,
              bool legacy = false) const
    {
        // ffx_a.h uses vector conditions in ?:, which HLSL 2021 (the default of newer dxc builds) rejects.
        std::vector<const wchar_t*> args { L"-E", L"main", L"-T", target, L"-O3", L"-HV", L"2018" };
        if (sixteenBit)
            args.push_back(L"-enable-16bit-types");
        if (strip)
            args.push_back(L"-Qstrip_reflect");
        if (legacy)
            args.insert(args.end(), { L"-validator-version", L"1.0" });
        const DxcBuffer buffer { source.data(), source.size(), DXC_CP_UTF8 };
        IDxcResult* result = nullptr;
        if (FAILED(dxc->Compile(&buffer, args.data(), UINT32(args.size()), nullptr, IID_PPV_ARGS(&result))))
            return false;
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlob* code = nullptr;
        if (SUCCEEDED(status))
            result->GetResult(&code);
        else
        {
            IDxcBlobEncoding* errors = nullptr;
            if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors)
            {
                const int shown = int(std::min<size_t>(errors->GetBufferSize(), 1200));
                printf("  dxc %ls: %.*s\n", target, shown, static_cast<const char*>(errors->GetBufferPointer()));
                errors->Release();
            }
        }
        result->Release();
        if (!code)
            return false;
        const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
        out.assign(p, p + code->GetBufferSize());
        code->Release();
        return true;
    }
};
struct Reg
{
    int reg = 0, space = 0;
};

static std::string Register(char kind, Reg r, bool spaces)
{
    char text[48];
    if (spaces)
        snprintf(text, sizeof(text), "register(%c%d, space%d)", kind, r.reg, r.space);
    else
        snprintf(text, sizeof(text), "register(%c%d)", kind, r.reg);
    return text;
}

struct FsrShader
{
    Pass pass = Pass::Easu;
    bool half = false;
    bool compute = false;
    bool extras = false; // engine bindings at lower registers, used after FSR
    Reg texture, sampler, constants, output;
};

static std::string FsrSource(const std::string& ffxA, const std::string& ffxFsr1, const FsrShader& v, bool spaces)
{
    const bool easu = v.pass == Pass::Easu;
    std::string s = "#define A_GPU 1\n#define A_HLSL 1\n";
    if (v.half)
        s += "#define A_HALF 1\n";
    s += easu ? (v.half ? "#define FSR_EASU_H 1\n" : "#define FSR_EASU_F 1\n")
              : (v.half ? "#define FSR_RCAS_H 1\n" : "#define FSR_RCAS_F 1\n");
    s += ffxA + "\n";
    s += "cbuffer FsrConstants : " + Register('b', v.constants, spaces) +
         (easu ? " { uint4 con0; uint4 con1; uint4 con2; uint4 con3; };\n" : " { uint4 con0; };\n");
    s += "Texture2D<float4> fsrInput : " + Register('t', v.texture, spaces) + ";\n";
    if (easu)
        s += "SamplerState fsrSampler : " + Register('s', v.sampler, spaces) + ";\n";
    if (v.compute)
        s += "RWTexture2D<float4> fsrOutput : " + Register('u', v.output, spaces) + ";\n";
    if (v.extras)
        s += "cbuffer EngineConstants : register(b0) { float4 engineTint; };\n"
             "Texture2D<float4> engineNoise : register(t0);\n"
             "SamplerState enginePoint : register(s0);\n";
    if (easu && v.half)
        s += "AH4 FsrEasuRH(AF2 p) { return AH4(fsrInput.GatherRed(fsrSampler, p)); }\n"
             "AH4 FsrEasuGH(AF2 p) { return AH4(fsrInput.GatherGreen(fsrSampler, p)); }\n"
             "AH4 FsrEasuBH(AF2 p) { return AH4(fsrInput.GatherBlue(fsrSampler, p)); }\n";
    else if (easu)
        s += "AF4 FsrEasuRF(AF2 p) { return fsrInput.GatherRed(fsrSampler, p); }\n"
             "AF4 FsrEasuGF(AF2 p) { return fsrInput.GatherGreen(fsrSampler, p); }\n"
             "AF4 FsrEasuBF(AF2 p) { return fsrInput.GatherBlue(fsrSampler, p); }\n";
    else if (v.half)
        s += "AH4 FsrRcasLoadH(ASW2 p) { return AH4(fsrInput.Load(int3(ASU2(p), 0))); }\n"
             "void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}\n";
    else
        s += "AF4 FsrRcasLoadF(ASU2 p) { return fsrInput.Load(int3(p, 0)); }\n"
             "void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}\n";
    s += ffxFsr1 + "\n";

    const char* type = v.half ? "AH3" : "AF3";
    std::string call =
        easu ? (v.half ? "FsrEasuH(c, p, con0, con1, con2, con3);" : "FsrEasuF(c, p, con0, con1, con2, con3);")
             : (v.half ? "FsrRcasH(c.r, c.g, c.b, p, con0);" : "FsrRcasF(c.r, c.g, c.b, p, con0);");
    if (v.compute)
    {
        s +=
            std::string("[numthreads(64, 1, 1)] void main(uint3 lid : SV_GroupThreadID, uint3 gid : SV_GroupID)\n{\n") +
            "    AU2 p = ARmp8x8(lid.x) + AU2(gid.x << 4u, gid.y << 4u);\n    " + type + " c;\n";
        for (int i = 0; i < 4; ++i)
            s += std::string("    ") + call + " fsrOutput[p] = AF4(c, 1);" +
                 (i == 0   ? " p.x += 8u;"
                  : i == 1 ? " p.y += 8u;"
                           : " p.x -= 8u;") +
                 "\n";
        s += "}\n";
    }
    else
    {
        s += std::string(
                 "float4 main(float4 position : SV_Position) : SV_Target\n{\n    AU2 p = AU2(position.xy);\n    ") +
             type + " c;\n    " + call + "\n";
        if (v.extras)
            s +=
                "    return float4(c, 1) * engineTint + engineNoise.Sample(enginePoint, position.xy * engineTint.xy) * "
                "0.01;\n";
        else
            s += "    return float4(c, 1);\n";
        s += "}\n";
    }
    return s;
}
static const char* kBlit = R"(
Texture2D<float4> picture : register(t0);
SamplerState linearClamp : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return picture.Sample(linearClamp, uv); }
)";
class Guarded
{
  public:
    explicit Guarded(size_t capacity)
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page_ = si.dwPageSize;
        bytes_ = (capacity + page_ - 1) / page_ * page_;
        base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes_ + page_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old;
        if (base_ && !VirtualProtect(base_ + bytes_, page_, PAGE_NOACCESS, &old))
        {
            VirtualFree(base_, 0, MEM_RELEASE);
            base_ = nullptr;
        }
    }
    ~Guarded()
    {
        if (base_)
            VirtualFree(base_, 0, MEM_RELEASE);
    }
    Guarded(const Guarded&) = delete;
    Guarded& operator=(const Guarded&) = delete;

    const uint8_t* Place(const uint8_t* data, size_t size)
    {
        if (!base_ || size > bytes_)
            return nullptr;
        uint8_t* p = base_ + bytes_ - size;
        if (size)
            memcpy(p, data, size);
        return p;
    }

  private:
    uint8_t* base_ = nullptr;
    size_t page_ = 4096, bytes_ = 0;
};

// No C++ objects to unwind here, so structured exception handling can catch an access violation.
static bool SafeClassify(const void* p, size_t size, ShaderInfo* out)
{
    __try
    {
        *out = Classify(p, size);
        const auto root = RootSignaturePart(p, size);
        if (root.ptr && (static_cast<const uint8_t*>(root.ptr) < p || root.size > size))
            return false;
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return false;
    }
}

// The compute wrapper follows the public fsr_upscale.glsl pass selection and 8x8 lane remap:
// https://github.com/godotengine/godot/blob/4.3/servers/rendering/renderer_rd/shaders/effects/fsr_upscale.glsl
// Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).
// Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions
// of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
static std::string UberSource(const std::string& a, const std::string& fsr, bool half)
{
    const std::string suffix = half ? "H" : "F", color = half ? "AH3" : "AF3";
    std::string s = "#define A_GPU 1\n#define A_HLSL 1\n";
    if (half)
        s += "#define A_HALF 1\n";
    s += "#define FSR_EASU_" + suffix + " 1\n#define FSR_RCAS_" + suffix + " 1\n" + a + "\n";
    s += R"(
cbuffer Params : register(b0) { float2 resolution; float2 upscaled; float sharpness; uint passIndex; };
Texture2D<float4> sourceImage : register(t0);
SamplerState sourceSampler : register(s0);
RWTexture2D<float4> resultImage : register(u0);
)";
    const std::string vec = half ? "AH4" : "AF4";
    for (const auto& channel : { "R", "G", "B" })
        s += vec + " FsrEasu" + channel + suffix + "(AF2 p) { return " + vec + "(sourceImage.Gather" +
             (channel == std::string("R")   ? "Red"
              : channel == std::string("G") ? "Green"
                                            : "Blue") +
             "(sourceSampler, p)); }\n";
    s += vec + " FsrRcasLoad" + suffix + (half ? "(ASW2 p)" : "(ASU2 p)") + " { return " + vec +
         "(sourceImage.Load(int3(p, 0))); }\n";
    s += "void FsrRcasInput" + suffix + "(inout " + (half ? "AH1" : "AF1") + " r, inout " + (half ? "AH1" : "AF1") +
         " g, inout " + (half ? "AH1" : "AF1") + " b) {}\n" + fsr + "\n";
    s += "void RunPass(uint2 p, uint4 c0, uint4 c1, uint4 c2, uint4 c3) { " + color + " c = 0;\n";
    s += "if (passIndex == 0) FsrEasu" + suffix + "(c, p, c0, c1, c2, c3);\n";
    s += "else if (passIndex == 1) FsrRcas" + suffix + "(c.r, c.g, c.b, p, c0); else return;\n";
    s += "resultImage[p] = float4(c, 1); }\n";
    s += R"hlsl(
[RootSignature("RootConstants(num32BitConstants=6, b0), DescriptorTable(SRV(t0)), DescriptorTable(UAV(u0)), DescriptorTable(Sampler(s0))")]
[numthreads(64, 1, 1)]
void main(uint3 local : SV_GroupThreadID, uint3 group : SV_GroupID) {
    uint4 c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    if (passIndex == 0) FsrEasuCon(c0, c1, c2, c3, resolution.x, resolution.y,
                            resolution.x, resolution.y, upscaled.x, upscaled.y);
    else if (passIndex == 1) FsrRcasCon(c0, sharpness);
    uint2 p = ARmp8x8(local.x) + uint2(group.x << 4u, group.y << 4u);
    RunPass(p, c0, c1, c2, c3); p.x += 8;
    RunPass(p, c0, c1, c2, c3); p.y += 8;
    RunPass(p, c0, c1, c2, c3); p.x -= 8;
    RunPass(p, c0, c1, c2, c3);
}
)hlsl";
    return s;
}

static const char* kGatherOcclusion = R"(
Texture2D<float> depth : register(t0);
SamplerState pointSampler : register(s0);
cbuffer K : register(b0) { float2 invSize; float radius; };
float4 main(float4 position : SV_Position) : SV_Target {
    float sum = 0;
    [unroll] for (int i = 0; i < 12; ++i) {
        float4 z = depth.GatherRed(pointSampler, (position.xy + float2(i - 6, i % 3) * radius) * invSize);
        sum += dot(z, float4(1, 2, 3, 4));
    }
    return sum / 120;
}
)";

static bool Threads(const ShaderInfo& s, uint32_t x, uint32_t y, uint32_t z)
{
    return s.numthreads[0] == x && s.numthreads[1] == y && s.numthreads[2] == z;
}
static bool IsUber(const ShaderInfo& s) { return s.uber; }

static void Constants()
{
    AU1 c0[4], c1[4], c2[4], c3[4];
    FsrEasuConOffset(c0, c1, c2, c3, 1600, 900, 1920, 1080, 2560, 1440, 100, 50);
    uint32_t con[16];
    memcpy(con, c0, 16);
    memcpy(con + 4, c1, 16);
    memcpy(con + 8, c2, 16);
    memcpy(con + 12, c3, 16);
    EasuConstants e;
    CHECK(DecodeEasu(con, 2560, 1440, &e));
    CHECK(e.inputWidth == 1920 && e.inputHeight == 1080 && e.viewportWidth == 1600 && e.viewportHeight == 900);
    CHECK(e.offsetX == 100 && e.offsetY == 50);
    con[4] ^= 1u << 16;
    CHECK(!DecodeEasu(con, 2560, 1440, nullptr));
    CHECK(!DecodeEasu(nullptr, 0, 0, nullptr));
    for (float stops : { 0.0f, 0.2f, 1.0f, 5.0f })
    {
        AU1 c[4];
        FsrRcasCon(c, stops);
        float back = -1;
        CHECK(DecodeRcas(c, &back) && std::fabs(back - stops) < 1e-4f);
        c[1] ^= 1;
        CHECK(!DecodeRcas(c, nullptr));
    }
    CHECK(!DecodeRcas(nullptr, nullptr));
}

int main()
{
    Constants();
    const fs::path repo = fs::current_path();
    const auto dir = repo / "OptiScaler/shaders/output_scaling/fsr1";
    const auto a = ReadText(dir / "ffx_a.h"), fsr = ReadText(dir / "ffx_fsr1.h");
    Compilers cc;
    cc.Load(repo);
    CHECK(cc.d3dCompile && cc.dxc && !a.empty() && !fsr.empty());
    if (!cc.d3dCompile || !cc.dxc || a.empty() || fsr.empty())
        return 1;
    printf("DXC: %s\n", cc.dxcFrom.c_str());
    std::vector<Bytes> corpus;
    auto check = [&](const char* name, const Bytes& b, Pass pass, Precision precision, Stage stage, bool uber = false)
    {
        const auto s = Classify(b.data(), b.size());
        printf("%s: pass=%u precision=%u stage=%u gathers=%u loads=%u\n", name, unsigned(s.pass), unsigned(s.precision),
               unsigned(s.stage), s.gathers, s.loads);
        CHECK(s.pass == pass && s.precision == precision && s.stage == stage);
        CHECK(IsUber(s) == uber);
        const auto root = RootSignaturePart(b.data(), b.size());
        CHECK(!root.ptr && root.size == 0);
        if (pass != Pass::None)
        {
            CHECK(s.textureRegister == 3 && s.textureSpace == 2);
            CHECK(s.constantRegister == 4 && s.constantSpace == 1);
            CHECK(s.samplerRegister == (pass == Pass::Easu ? 2 : -1));
            CHECK(s.samplerSpace == (pass == Pass::Easu ? 3 : 0));
            CHECK(Threads(s, stage == Stage::Compute ? 64 : 0, stage == Stage::Compute ? 1 : 0,
                          stage == Stage::Compute ? 1 : 0));
            if (stage == Stage::Compute)
                CHECK(s.outputRegister == 1 && s.outputSpace == 4);
        }
        corpus.push_back(b);
    };
    for (bool dxil : { false, true })
    {
        for (bool half : { false, true })
        {
            for (Pass pass : { Pass::Easu, Pass::Rcas })
            {
                FsrShader v { pass, half, false, false, { 3, 2 }, { 2, 3 }, { 4, 1 }, { 1, 4 } };
                Bytes b;
                const auto src = FsrSource(a, fsr, v, true);
                CHECK(dxil ? cc.Dxil(src, half ? L"ps_6_6" : L"ps_6_0", half, true, b)
                           : cc.Dxbc(src, "ps_5_1", true, b));
                check(dxil ? "DXIL pixel" : "DXBC pixel", b, pass, half ? Precision::Half : Precision::Fp32,
                      Stage::Pixel);
            }
        }
        for (Pass pass : { Pass::Easu, Pass::Rcas })
        {
            FsrShader v { pass, false, false, true, { 3, 2 }, { 2, 3 }, { 4, 1 }, { 1, 4 } };
            const auto src = FsrSource(a, fsr, v, true);
            Bytes b;
            CHECK(dxil ? cc.Dxil(src, L"ps_6_6", false, true, b) : cc.Dxbc(src, "ps_5_1", true, b));
            check("pixel with extra bindings", b, pass, Precision::Fp32, Stage::Pixel);
        }
        FsrShader v { Pass::Easu, false, true, false, { 3, 2 }, { 2, 3 }, { 4, 1 }, { 1, 4 } };
        Bytes b;
        const auto src = FsrSource(a, fsr, v, true);
        CHECK(dxil ? cc.Dxil(src, L"cs_6_0", false, true, b) : cc.Dxbc(src, "cs_5_1", true, b));
        check(dxil ? "DXIL compute table" : "DXBC compute table", b, Pass::Easu, Precision::Fp32, Stage::Compute);
        for (const char* source : { kBlit, kGatherOcclusion })
        {
            b.clear();
            CHECK(dxil ? cc.Dxil(source, L"ps_6_0", false, true, b) : cc.Dxbc(source, "ps_5_0", true, b));
            check(source == kBlit ? "bilinear" : "gather occlusion", b, Pass::None, Precision::Unknown, Stage::Pixel);
        }
    }
    {
        FsrShader v { Pass::Easu, false, true, false, { 3, 2 }, { 2, 3 }, { 4, 1 }, { 1, 4 } };
        auto src = FsrSource(a, fsr, v, true);
        src.replace(src.find("numthreads(64, 1, 1)"), strlen("numthreads(64, 1, 1)"), "numthreads(8, 4, 2)");
        Bytes b;
        CHECK(cc.Dxil(src, L"cs_6_0", false, true, b, true));
        const auto s = Classify(b.data(), b.size());
        detail::Container container;
        CHECK(detail::ParseContainer(b.data(), b.size(), container));
        CHECK(container.psv0.size >= 4 && detail::Rd32(container.psv0.data) < 48);
        printf("legacy DXIL compute: pass=%u\n", unsigned(s.pass));
        CHECK(s.pass == Pass::Easu && Threads(s, 8, 4, 2));
        corpus.push_back(b);
    }
    for (bool dxil : { false, true })
        for (bool half : { false, true })
        {
            Bytes b;
            const auto src = UberSource(a, fsr, half);
            CHECK(dxil ? cc.Dxil(src, half ? L"cs_6_2" : L"cs_6_0", half, true, b) : cc.Dxbc(src, "cs_5_1", true, b));
            const auto s = Classify(b.data(), b.size());
            printf("%s compute uber %s: pass=%u gathers=%u loads=%u\n", dxil ? "DXIL" : "DXBC", half ? "half" : "fp32",
                   unsigned(s.pass), s.gathers, s.loads);
            CHECK(s.pass == Pass::Easu && IsUber(s) && Threads(s, 64, 1, 1));
            CHECK(s.precision == (half ? Precision::Half : Precision::Fp32));
            CHECK(s.textureRegister == 0 && s.samplerRegister == 0 && s.constantRegister == 0 && s.outputRegister == 0);
            const auto part = RootSignaturePart(b.data(), b.size());
            CHECK(part.ptr && part.size > 0);
            ID3DBlob* expected = nullptr;
            CHECK(SUCCEEDED(D3DGetBlobPart(b.data(), b.size(), D3D_BLOB_ROOT_SIGNATURE, 0, &expected)));
            if (expected)
            {
                const auto data = static_cast<const uint8_t*>(expected->GetBufferPointer());
                uint32_t offset = 0, length = 0;
                CHECK(expected->GetBufferSize() >= 44);
                if (expected->GetBufferSize() >= 44)
                {
                    memcpy(&offset, data + 32, 4);
                    CHECK(offset <= expected->GetBufferSize() - 8);
                    if (offset <= expected->GetBufferSize() - 8)
                    {
                        memcpy(&length, data + offset + 4, 4);
                        CHECK(memcmp(data + offset, "RTS0", 4) == 0);
                        CHECK(part.size == length && length <= expected->GetBufferSize() - offset - 8);
                        if (part.size == length && length <= expected->GetBufferSize() - offset - 8)
                            CHECK(memcmp(part.ptr, data + offset + 8, length) == 0);
                    }
                }
                expected->Release();
            }

            corpus.push_back(b);
        }
    for (const auto& file : { "FSR_EASU_Shader.cso", "FSR_EASU_Shader_Dx11.cso", "FSR_RCAS_Shader.cso" })
    {
        Bytes b;
        CHECK(ReadFile(dir / file, b));
        if (b.empty())
            continue;
        const auto s = Classify(b.data(), b.size());
        const Pass expected = strstr(file, "RCAS") ? Pass::Rcas : Pass::Easu;
        printf("precompiled %s: pass=%u\n", file, unsigned(s.pass));
        CHECK(s.pass == expected && s.stage == Stage::Compute);
        corpus.push_back(b);
    }
    uint64_t calls = 0, oob = 0;
    for (const auto& b : corpus)
    {
        Guarded guard(b.size());
        for (size_t len = 0; len <= b.size(); ++len)
        {
            auto p = const_cast<uint8_t*>(guard.Place(b.data(), len));
            CHECK(p);
            for (int patched = 0; patched < (len >= 32 ? 2 : 1); ++patched)
            {
                if (patched)
                {
                    const uint32_t total = uint32_t(len);
                    memcpy(p + 24, &total, 4);
                }
                ShaderInfo s;
                ++calls;
                if (!SafeClassify(p, len, &s))
                    ++oob;
            }
        }
    }
    CHECK(oob == 0);
    printf("corpus truncation: %llu calls, %llu OOB\n", (unsigned long long) calls, (unsigned long long) oob);
    printf(fails ? "FAIL: %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
