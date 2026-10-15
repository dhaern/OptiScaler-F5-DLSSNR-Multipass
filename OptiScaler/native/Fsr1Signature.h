#pragma once

// Recognition and binding extraction run only at pipeline creation. Decoders are pure and allocation-free.
// DXIL constants are bit-packed LLVM records, not searchable byte strings; keep the bounded walker independent of DXC.
//
// What gives FSR 1 away does not depend on how an engine names or binds things: ffx_a.h approximates rcp and rsqrt with
// integer tricks, and their magic numbers end up in the code whatever the compiler does around them.
//   EASU  12 gathers per pixel (Gather{Red,Green,Blue} x 4) plus APrxLoRcp and APrxLoRsq:
//         FP32 0x7ef07ebb and 0x5f347d74, half (FSR_EASU_H) 0x7784 and 0x59a3.
//   RCAS  5 loads per pixel and no gather, plus APrxMedRcp (FP32 0x7ef19fff, half 0x778d) and none of the low-precision
//         approximations: FidelityFX CAS also loads and also uses APrxMedRcp, but it has APrxLoRcp next to it.
// The magics alone also mark CAS, LPM and the rest of the FidelityFX family, hence the instruction counts.
//
// DXBC (fxc, SM 5.x): the magics are 32-bit immediates of the token stream, half ones included (SM 5 has no 16-bit
// types, so FSR_EASU_H / FSR_RCAS_H there run on min16 values in 32-bit registers). DXIL (dxc, SM 6.x): they are
// integer constants of the LLVM bitcode, i32 or i16, bit-packed, so the bitcode is walked. Most shaders stop at the
// cheap first step: DXBC, a scan of the raw dwords; DXIL, the constant blocks only (instructions and metadata are
// skipped by their block length). Only shaders that carry the magics get the full walk, which counts texture
// instructions; in DXIL it follows the value numbering of each function to see which dx.op each call is and which
// binding its handle comes from. Bindings come from the SHEX declarations (DXBC: they survive /Qstrip_reflect, RDEF
// does not) or from PSV0 (DXIL: the runtime needs it, so -Qstrip_reflect keeps it); the texture, sampler and constant
// buffer reported are the ones the gathers / loads / constant reads use, so extra bindings an engine adds around FSR do
// not confuse it.
//
// Every read is bounded by the size given and every loop by the bits left, so a malformed or truncated blob ends in
// Pass::None, never in a read past its end. Classify keeps bounded tables on the stack, without heap allocation.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace native::fsr1
{
enum class Pass : uint8_t
{
    None,
    Easu,
    Rcas
};
enum class Precision : uint8_t
{
    Unknown,
    Half,
    Fp32
};
enum class Stage : uint8_t
{
    Other,
    Pixel,
    Compute
};

// Bits of ShaderInfo::magics: which ffx_a.h approximation constants the code carries.
enum : uint32_t
{
    kMagicLoRcpF = 1u << 0,  // APrxLoRcpF1   0x7ef07ebb
    kMagicLoRsqF = 1u << 1,  // APrxLoRsqF1   0x5f347d74
    kMagicMedRcpF = 1u << 2, // APrxMedRcpF1  0x7ef19fff
    kMagicLoSqrtF = 1u << 3, // APrxLoSqrtF1  0x1fbc4639
    kMagicLoRcpH = 1u << 4,  // APrxLoRcpH1   0x7784
    kMagicLoRsqH = 1u << 5,  // APrxLoRsqH1   0x59a3
    kMagicMedRcpH = 1u << 6, // APrxMedRcpH1  0x778d
    kMagicLoSqrtH = 1u << 7, // APrxLoSqrtH1  0x1de2
};

struct ShaderInfo
{
    Pass pass = Pass::None;
    Precision precision = Precision::Unknown;
    // Bindings from PSV0 or DXBC declarations. -1 when not found.
    int textureRegister = -1, textureSpace = 0;   // the input picture (t#)
    int constantRegister = -1, constantSpace = 0; // the FsrEasuCon / FsrRcasCon buffer (b#)
    int samplerRegister = -1, samplerSpace = 0;   // s#, -1 for RCAS (it loads, does not sample)
    const char* why = "";                         // static string: what matched or why not (for the log)

    // Beyond the contract, for the log and for callers that want to be stricter. Bindings and counts are only filled
    // for shaders that get past the cheap first step (the ones that carry the magics).
    Stage stage = Stage::Other;
    bool dxil = false;
    bool uber = false;
    uint32_t numthreads[3] = {};
    int outputRegister = -1, outputSpace = 0; // first UAV (u#): where a compute EASU / RCAS writes its picture
    uint32_t textures = 0, constantBuffers = 0, samplers = 0, uavs = 0; // declared bindings of each kind
    uint32_t gathers = 0, loads = 0, samples = 0;                       // texture instructions in the code
    uint32_t magics = 0;                                                // kMagic* bits found
};

// Bytecode of a pixel shader as given to CreateGraphicsPipelineState (DXBC or DXIL container). Never reads past `size`.
// Compute shaders are recognised as well (the FidelityFX samples run EASU / RCAS as compute); ShaderInfo::stage says
// which one it is. Other stages are Pass::None.
ShaderInfo Classify(const void* bytecode, size_t size);

struct RootSignatureBlob
{
    const void* ptr = nullptr;
    size_t size = 0;
};

// RTS0 payload, not a standalone serialized root-signature container. Borrowed for the bytecode lifetime.
RootSignatureBlob RootSignaturePart(const void* bytecode, size_t size);

struct EasuConstants
{
    uint32_t inputWidth = 0, inputHeight = 0;    // texture size (1/con1.xy)
    float viewportWidth = 0, viewportHeight = 0; // input viewport in pixels (con0.xy * output size)
    uint32_t outputWidth = 0, outputHeight = 0;  // output size, only when the caller gives it (see below)
    float scaleX = 0, scaleY = 0;                // viewport / output (con0.xy)
    float offsetX = 0, offsetY = 0;              // input offset in pixels decoded from con0.zw
};

// `con` = the 16 dwords of FsrEasuCon (con0..con3) exactly as in the constant buffer. True only if every exact bit
// relation of FsrEasuCon holds and the ranges are sane (prototype: B-prototipos/fingerprint.py is_easu).
// outputWidth/Height: pass the render target size if known (then viewport = scale * output and the result is checked
// against it); pass 0,0 to get only what the constants alone give.
bool DecodeEasu(const uint32_t con[16], uint32_t outputWidth, uint32_t outputHeight, EasuConstants* out);

// FsrRcasCon: con0.x = exp2(-sharpness) as float, con0.y = packed half2 of it. True when the 4 dwords look like one.
// *sharpness gets FsrRcasCon's argument back: stops, 0 = sharpest.
bool DecodeRcas(const uint32_t con[4], float* sharpness);

namespace detail
{
inline uint32_t Rd32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline float AsFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline uint32_t AsUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

inline uint32_t MagicBit(uint64_t v)
{
    switch (v)
    {
    case 0x7ef07ebb:
        return kMagicLoRcpF;
    case 0x5f347d74:
        return kMagicLoRsqF;
    case 0x7ef19fff:
        return kMagicMedRcpF;
    case 0x1fbc4639:
        return kMagicLoSqrtF;
    case 0x7784:
        return kMagicLoRcpH;
    case 0x59a3:
        return kMagicLoRsqH;
    case 0x778d:
        return kMagicMedRcpH;
    case 0x1de2:
        return kMagicLoSqrtH;
    default:
        return 0;
    }
}

constexpr uint32_t kMagicLoRcp = kMagicLoRcpF | kMagicLoRcpH;
constexpr uint32_t kMagicLoRsq = kMagicLoRsqF | kMagicLoRsqH;
constexpr uint32_t kMagicLowPrecision = kMagicLoRcp | kMagicLoRsq | kMagicLoSqrtF | kMagicLoSqrtH;

inline bool EasuMagics(uint32_t m) { return (m & kMagicLoRcp) && (m & kMagicLoRsq); }
inline bool RcasMagics(uint32_t m) { return (m & (kMagicMedRcpF | kMagicMedRcpH)) && !(m & kMagicLowPrecision); }

// ---------------------------------------------------------------------------------------------------------------------
// Bindings: what the code declares and what its texture / constant instructions use.

enum Kind : uint8_t
{
    kSrv,
    kSampler,
    kCbv,
    kUav,
    kKindCount
};

struct Binding
{
    uint32_t id = 0; // DXBC SM 5.1 range ID, otherwise the order of the binding within its kind
    uint32_t lower = 0, upper = 0, space = 0;
    bool texture2d = false;
};

// A binding an instruction uses. DXIL createHandle gives the register but not the space; that comes from PSV0 later.
struct Use
{
    bool known = false, spaceKnown = false;
    uint32_t reg = 0, space = 0, range = 0;
};

struct Resources
{
    static constexpr uint32_t kMax = 32;
    Binding list[kKindCount][kMax];
    uint32_t count[kKindCount] = {};
    Use gatherTexture, gatherSampler, loadTexture, sampleTexture, sampleSampler, constants;

    uint32_t Stored(Kind k) const { return count[k] < kMax ? count[k] : kMax; }

    void Add(Kind k, const Binding& b)
    {
        if (count[k] < kMax)
            list[k][count[k]] = b;
        ++count[k];
    }

    const Binding* FindId(Kind k, uint32_t id) const
    {
        for (uint32_t i = 0; i < Stored(k); ++i)
            if (list[k][i].id == id)
                return &list[k][i];
        return nullptr;
    }

    // The binding of kind k whose range holds reg; the range-th one first (DXIL range IDs follow PSV0 order).
    const Binding* FindRegister(Kind k, uint32_t reg, uint32_t range) const
    {
        if (range < Stored(k) && list[k][range].lower <= reg && reg <= list[k][range].upper)
            return &list[k][range];
        for (uint32_t i = 0; i < Stored(k); ++i)
            if (list[k][i].lower <= reg && reg <= list[k][i].upper)
                return &list[k][i];
        return nullptr;
    }
};

inline int Register(uint32_t v) { return v <= 0x7fffffffu ? int(v) : -1; }

inline void PickBinding(const Resources& r, Kind k, const Use& use, bool preferTexture2d, int& reg, int& space)
{
    if (use.known)
    {
        reg = Register(use.reg);
        space = int(use.space);
        if (!use.spaceKnown)
        {
            const Binding* b = r.FindRegister(k, use.reg, use.range);
            space = b ? int(b->space) : 0;
        }
        return;
    }
    // Nothing resolved: the first declared, a Texture2D first for the picture.
    const Binding* pick = nullptr;
    for (uint32_t i = 0; i < r.Stored(k) && !pick; ++i)
        if (!preferTexture2d || r.list[k][i].texture2d)
            pick = &r.list[k][i];
    if (!pick && r.Stored(k) > 0)
        pick = &r.list[k][0];
    if (pick)
    {
        reg = Register(pick->lower);
        space = int(pick->space);
    }
}

inline void SetBindings(ShaderInfo& info, const Resources& r)
{
    info.textures = r.count[kSrv];
    info.samplers = r.count[kSampler];
    info.constantBuffers = r.count[kCbv];
    info.uavs = r.count[kUav];

    // EASU reads its picture with the gathers, RCAS with loads (or samples, if an engine wrote FsrRcasLoad that way).
    // The sampler is the one the picture is read with: none for an RCAS that loads, whatever else the shader samples.
    const bool gathers = info.pass == Pass::Easu || (info.pass == Pass::None && info.gathers > 0);
    const bool loads = !gathers && (r.loadTexture.known || (info.loads > 0 && !r.sampleTexture.known));
    const Use& texture = gathers ? r.gatherTexture : loads ? r.loadTexture : r.sampleTexture;
    const Use& sampler = gathers ? r.gatherSampler : r.sampleSampler;
    PickBinding(r, kSrv, texture, true, info.textureRegister, info.textureSpace);
    if (!loads && info.gathers + info.samples > 0)
        PickBinding(r, kSampler, sampler, false, info.samplerRegister, info.samplerSpace);
    PickBinding(r, kCbv, r.constants, false, info.constantRegister, info.constantSpace);
    PickBinding(r, kUav, Use(), false, info.outputRegister, info.outputSpace);
}

inline void Decide(ShaderInfo& info)
{
    const uint32_t m = info.magics;
    if (EasuMagics(m) && info.gathers >= 12)
    {
        info.pass = Pass::Easu;
        info.uber = (m & (kMagicMedRcpF | kMagicMedRcpH)) != 0;
        info.precision = (m & (kMagicLoRcpH | kMagicLoRsqH)) ? Precision::Half : Precision::Fp32;
        info.why = "EASU: low-precision rcp and rsqrt, 12+ gathers";
    }
    else if (RcasMagics(m) && info.gathers == 0 && info.loads + info.samples >= 5)
    {
        info.pass = Pass::Rcas;
        info.precision = (m & kMagicMedRcpH) ? Precision::Half : Precision::Fp32;
        info.why = "RCAS: medium-precision rcp, 5+ loads, no gather";
    }
    else
        info.why = "FSR 1 constants but not the EASU/RCAS texture pattern";
}

inline Stage StageOf(uint32_t programVersion)
{
    // DXBC version token and DXIL program version share the layout: shader kind in the high 16 bits.
    switch (programVersion >> 16)
    {
    case 0:
        return Stage::Pixel;
    case 5:
        return Stage::Compute;
    default:
        return Stage::Other;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// DXBC container: "DXBC", hash, version, total size, part count, part offsets; each part = fourcc, size, data.

struct Blob
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct Container
{
    Blob dxil, shex, psv0, rts0;
};

inline bool ParseContainer(const uint8_t* p, size_t size, Container& c)
{
    if (!p || size < 32 || Rd32(p) != FourCC('D', 'X', 'B', 'C'))
        return false;
    const uint32_t total = Rd32(p + 24);
    if (total < 32 || total > size)
        return false;
    const uint32_t parts = Rd32(p + 28);
    if (parts > (total - 32) / 4)
        return false;
    for (uint32_t i = 0; i < parts; ++i)
    {
        const uint32_t offset = Rd32(p + 32 + 4 * size_t(i));
        if (offset > total || total - offset < 8)
            continue;
        const uint32_t fourcc = Rd32(p + offset), length = Rd32(p + offset + 4);
        if (length > total - offset - 8)
            continue;
        const Blob part { p + offset + 8, length };
        if (fourcc == FourCC('D', 'X', 'I', 'L') && !c.dxil.data)
            c.dxil = part;
        else if ((fourcc == FourCC('S', 'H', 'E', 'X') || fourcc == FourCC('S', 'H', 'D', 'R')) && !c.shex.data)
            c.shex = part;
        else if (fourcc == FourCC('P', 'S', 'V', '0') && !c.psv0.data)
            c.psv0 = part;
        else if (fourcc == FourCC('R', 'T', 'S', '0') && !c.rts0.data)
            c.rts0 = part;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// DXBC code (SHEX / SHDR): version, length in dwords, then instructions; opcode in bits 0-10, length in bits 24-30.

struct Dwords
{
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint32_t operator[](size_t i) const { return Rd32(p + 4 * i); }
};

struct DxbcOperand
{
    uint32_t type = 0, dims = 0, comps = 0;
    uint32_t index[3] = {};
    bool immediateIndex[3] = {};
    uint32_t value[4] = {};
};

enum : uint32_t
{
    kOperandImmediate32 = 4,
    kOperandImmediate64 = 5,
    kOperandSampler = 6,
    kOperandResource = 7,
    kOperandConstantBuffer = 8,
};

// Returns the position after the operand, 0 when it does not parse (operands never start at dword 0).
inline size_t ReadOperand(const Dwords& d, size_t pos, size_t end, DxbcOperand& o, int depth)
{
    if (depth > 2 || pos >= end)
        return 0;
    const uint32_t t = d[pos++];
    o = DxbcOperand();
    const uint32_t comps = t & 3;
    if (comps == 3) // N-component operands: not used by shader model 4/5 code
        return 0;
    o.comps = comps == 0 ? 0 : comps == 1 ? 1 : 4;
    o.type = (t >> 12) & 0xff;
    o.dims = (t >> 20) & 3;
    for (bool extended = (t >> 31) != 0; extended;)
    {
        if (pos >= end)
            return 0;
        extended = (d[pos++] >> 31) != 0;
    }
    for (uint32_t i = 0; i < o.dims; ++i)
    {
        const uint32_t representation = (t >> (22 + 3 * i)) & 7;
        if (representation > 4)
            return 0;
        if (representation == 0 || representation == 3) // immediate32 (+ relative)
        {
            if (pos >= end)
                return 0;
            o.index[i] = d[pos++];
            o.immediateIndex[i] = representation == 0;
        }
        else if (representation == 1 || representation == 4) // immediate64 (+ relative)
        {
            if (end - pos < 2)
                return 0;
            pos += 2;
        }
        if (representation >= 2) // relative: an operand of its own
        {
            DxbcOperand relative;
            pos = ReadOperand(d, pos, end, relative, depth + 1);
            if (!pos)
                return 0;
        }
    }
    if (o.type == kOperandImmediate32 || o.type == kOperandImmediate64)
    {
        const size_t words = size_t(o.comps) * (o.type == kOperandImmediate64 ? 2 : 1);
        if (end - pos < words)
            return 0;
        if (o.type == kOperandImmediate32)
            for (uint32_t i = 0; i < o.comps; ++i)
                o.value[i] = d[pos + i];
        pos += words;
    }
    return pos;
}

enum TextureOp : uint8_t
{
    kNotTexture,
    kGather,
    kLoad,
    kSample
};

inline TextureOp DxbcTextureOp(uint32_t op)
{
    if (op == 109 || (op >= 126 && op <= 128) || (op >= 219 && op <= 222))
        return kGather;
    if (op == 45 || op == 46 || op == 163 || (op >= 223 && op <= 225))
        return kLoad;
    if ((op >= 69 && op <= 74) || (op >= 228 && op <= 233))
        return kSample;
    return kNotTexture;
}

// Resource / sampler / constant buffer operand of an instruction: SM 5.0 names the register (t3), SM 5.1 the range ID
// and the register (T0[3]); the space comes from the declaration of that range.
inline Use DxbcUse(const Resources& r, Kind k, const DxbcOperand& o, bool sm51)
{
    Use u;
    if (o.dims == 0 || !o.immediateIndex[0])
        return u;
    const Binding* b = r.FindId(k, o.index[0]);
    u.known = true;
    u.spaceKnown = true;
    if (sm51)
    {
        if (o.dims >= 2 && o.immediateIndex[1])
            u.reg = o.index[1];
        else if (b)
            u.reg = b->lower;
        else
            u.known = false;
    }
    else
        u.reg = o.index[0];
    u.space = b ? b->space : 0;
    return u;
}

inline void DxbcInstruction(const Dwords& d, size_t at, size_t end, bool sm51, ShaderInfo& info, Resources& r)
{
    const uint32_t token = d[at];
    const uint32_t op = token & 0x7ff;
    size_t pos = at + 1;
    for (bool extended = (token >> 31) != 0; extended && pos < end;)
        extended = (d[pos++] >> 31) != 0;

    if (op == 155 && info.stage == Stage::Compute && end - pos == 3)
    {
        for (size_t i = 0; i < 3; ++i)
            info.numthreads[i] = d[pos + i];
        return;
    }

    if ((op >= 88 && op <= 106) || (op >= 143 && op <= 162)) // declarations
    {
        Kind kind;
        switch (op)
        {
        case 88:  // dcl_resource
        case 161: // dcl_resource_raw
        case 162: // dcl_resource_structured
            kind = kSrv;
            break;
        case 89: // dcl_constantbuffer
            kind = kCbv;
            break;
        case 90: // dcl_sampler
            kind = kSampler;
            break;
        case 156: // dcl_uav_typed / raw / structured
        case 157:
        case 158:
            kind = kUav;
            break;
        default:
            return;
        }
        DxbcOperand o;
        if (!ReadOperand(d, pos, end, o, 0) || o.dims == 0 || !o.immediateIndex[0])
            return;
        Binding b;
        b.id = o.index[0];
        b.lower = b.upper = o.index[0];
        if (o.dims == 3) // SM 5.1: range ID, lower bound, upper bound; the space is the last dword
        {
            if (!o.immediateIndex[1])
                return;
            b.lower = o.index[1];
            b.upper = o.immediateIndex[2] ? o.index[2] : o.index[1];
            b.space = d[end - 1];
        }
        b.texture2d = op == 88 && ((token >> 11) & 0x1f) == 3;
        r.Add(kind, b);
        return;
    }

    const TextureOp texOp = DxbcTextureOp(op);
    if (texOp == kGather)
        ++info.gathers;
    else if (texOp == kLoad)
        ++info.loads;
    else if (texOp == kSample)
        ++info.samples;

    bool resourceSeen = false, samplerSeen = false;
    while (pos < end)
    {
        DxbcOperand o;
        const size_t next = ReadOperand(d, pos, end, o, 0);
        if (!next)
            break; // not an operand list we can follow: keep the counts, skip the rest
        pos = next;
        if (o.type == kOperandImmediate32)
        {
            for (uint32_t i = 0; i < o.comps; ++i)
                info.magics |= MagicBit(o.value[i]);
        }
        else if (o.type == kOperandResource && texOp != kNotTexture && !resourceSeen)
        {
            resourceSeen = true;
            Use& use = texOp == kGather ? r.gatherTexture : texOp == kLoad ? r.loadTexture : r.sampleTexture;
            if (!use.known)
                use = DxbcUse(r, kSrv, o, sm51);
        }
        else if (o.type == kOperandSampler && (texOp == kGather || texOp == kSample) && !samplerSeen)
        {
            samplerSeen = true;
            Use& use = texOp == kGather ? r.gatherSampler : r.sampleSampler;
            if (!use.known)
                use = DxbcUse(r, kSampler, o, sm51);
        }
        else if (o.type == kOperandConstantBuffer && !r.constants.known)
            r.constants = DxbcUse(r, kCbv, o, sm51);
    }
}

inline void ClassifyDxbc(const Blob& code, ShaderInfo& info)
{
    const Dwords d { code.data, code.size / 4 };
    const size_t length = d.n >= 2 ? d[1] : 0;
    if (length < 2 || length > d.n)
    {
        info.why = "malformed shader code";
        return;
    }
    info.stage = StageOf(d[0]);
    if (info.stage == Stage::Other)
    {
        info.why = "not a pixel or compute shader";
        return;
    }

    // Cheap step: are the magics anywhere in the token stream? Most shaders end here.
    uint32_t raw = 0;
    for (size_t i = 2; i < length; ++i)
        raw |= MagicBit(d[i]);
    info.magics = raw;
    if (!EasuMagics(raw) && !RcasMagics(raw))
    {
        info.why = "no FSR 1 approximation constants";
        return;
    }

    // Full walk: the magics again, now only from immediate operands, the texture instructions and the bindings.
    const uint32_t major = (d[0] >> 4) & 0xf, minor = d[0] & 0xf;
    const bool sm51 = major > 5 || (major == 5 && minor >= 1);
    Resources r;
    info.magics = 0;
    for (size_t i = 2; i < length;)
    {
        const uint32_t op = d[i] & 0x7ff;
        const size_t n = op == 53 ? (i + 1 < length ? d[i + 1] : 0) : (d[i] >> 24) & 0x7f; // 53: customdata
        if (n == 0 || n > length - i)
        {
            info.gathers = info.loads = info.samples = 0;
            info.why = "malformed shader code";
            return;
        }
        if (op != 53)
            DxbcInstruction(d, i, i + n, sm51, info, r);
        i += n;
    }
    if (!EasuMagics(info.magics) && !RcasMagics(info.magics))
    {
        info.why = "no FSR 1 approximation constants";
        return;
    }
    Decide(info);
    SetBindings(info, r);
}

// ---------------------------------------------------------------------------------------------------------------------
// DXIL: PSV0 bindings and the LLVM 3.7 bitstream of the program.

inline void ParsePsv0(const Blob& psv, Resources& r, ShaderInfo& info)
{
    // Runtime info size, runtime info, resource count, bind info size, bind infos {type, space, lower, upper[, kind,
    // flags]}.
    const uint8_t* p = psv.data;
    const size_t size = psv.size;
    if (!p || size < 8)
        return;
    const uint32_t infoSize = Rd32(p);
    if (infoSize > size - 8)
        return;
    if (info.stage == Stage::Compute && infoSize >= 48)
        for (size_t i = 0; i < 3; ++i)
            info.numthreads[i] = Rd32(p + 4 + 36 + 4 * i);
    size_t at = 4 + size_t(infoSize);
    const uint32_t count = Rd32(p + at);
    at += 4;
    if (count == 0 || size - at < 4)
        return;
    const uint32_t stride = Rd32(p + at);
    at += 4;
    if (stride < 16 || count > (size - at) / stride)
        return;
    uint32_t perKind[kKindCount] = {};
    for (uint32_t i = 0; i < count; ++i, at += stride)
    {
        const uint32_t type = Rd32(p + at);
        if (type < 1 || type > 9)
            continue;
        const Kind kind = type == 1 ? kSampler : type == 2 ? kCbv : type <= 5 ? kSrv : kUav;
        Binding b;
        b.id = perKind[kind]++;
        b.space = Rd32(p + at + 4);
        b.lower = Rd32(p + at + 8);
        b.upper = Rd32(p + at + 12);
        b.texture2d = stride >= 20 && Rd32(p + at + 16) == 2; // PSV1 resource kind: Texture2D
        r.Add(kind, b);
    }
}

class DxilWalker
{
  public:
    DxilWalker(const uint8_t* data, size_t bytes, bool full, ShaderInfo& info, Resources& r)
        : data_(data), bits_(bytes * 8), full_(full), info_(info), r_(r)
    {
    }

    // False when the bitcode is malformed or the walk could not follow it.
    bool Run()
    {
        if (bits_ < 64 || Read(32) != 0xdec04342u) // 'B' 'C' 0xC0DE
            return false;
        while (!bad_ && !failed_ && bits_ - pos_ >= 64)
        {
            if (Read(2) != 1) // top level holds only blocks; anything else is padding
                break;
            uint32_t id;
            unsigned width;
            size_t end;
            if (!EnterBlock(id, width, end))
                break;
            if (id == kModuleBlock)
            {
                ParseBlock(id, width, end, 0);
                pos_ = end;
                break;
            }
            pos_ = end;
        }
        if (full_ && info_.stage == Stage::Compute && info_.numthreads[0] == 0)
            ReadNumthreads();
        return !bad_ && !failed_;
    }

  private:
    enum : uint32_t
    {
        kBlockInfoBlock = 0,
        kModuleBlock = 8,
        kConstantsBlock = 11,
        kFunctionBlock = 12,
        kTypeBlock = 17,
        kMetadataBlock = 15,
        kNone = 0xffffffffu,
    };

    // Abbreviation operand: kind in the low 3 bits, width or literal above.
    enum : uint32_t
    {
        kLiteral,
        kFixed,
        kVbr,
        kArray,
        kChar6,
        kBlob
    };

    struct Abbrevs
    {
        static constexpr int kMaxOps = 384, kMax = 96;
        uint32_t ops[kMaxOps];
        uint16_t first[kMax], size[kMax];
        uint32_t block[kMax];
        int nOps = 0, n = 0;
    };

    struct Record
    {
        static constexpr size_t kMaxOps = 32;
        uint64_t code = 0;
        size_t count = 0; // every operand, also the ones past kMaxOps that are not kept
        uint64_t ops[kMaxOps];
        bool Has(size_t i) const { return i < count && i < kMaxOps; }
        uint64_t Op(size_t i) const { return Has(i) ? ops[i] : 0; }
    };

    struct Type
    {
        uint8_t code = 0;
        uint16_t params = 0;
        uint32_t ref = 0; // function: return type; pointer: pointee; integer: width
    };

    struct Function
    {
        uint32_t value = 0, type = 0;
        bool prototype = true;
    };

    struct Constant
    {
        uint32_t id, value;
    };

    struct Aggregate
    {
        uint32_t id;
        uint32_t member[4];
        bool zero;
    };

    struct Handle
    {
        uint32_t id = 0, source = 0;
        uint8_t kind = 0; // 0 register without space (createHandle), 1 with space, 2 alias of source, 3 unknown
        uint32_t reg = 0, space = 0, range = 0;
    };

    struct Metadata
    {
        uint64_t code;
        uint32_t count, ops[8];
    };

    // ---- bitstream

    uint64_t Read(unsigned width)
    {
        if (bad_ || width > 64 || width > bits_ - pos_)
        {
            bad_ = true;
            return 0;
        }
        uint64_t v = 0;
        for (unsigned got = 0; got < width;)
        {
            const unsigned shift = unsigned(pos_ & 7);
            unsigned take = 8 - shift;
            if (take > width - got)
                take = width - got;
            v |= uint64_t((data_[pos_ >> 3] >> shift) & ((1u << take) - 1)) << got;
            got += take;
            pos_ += take;
        }
        return v;
    }

    uint64_t Vbr(unsigned width)
    {
        const uint64_t high = uint64_t(1) << (width - 1);
        uint64_t v = 0;
        for (unsigned shift = 0;; shift += width - 1)
        {
            const uint64_t piece = Read(width);
            if (bad_ || shift > 63)
            {
                bad_ = true;
                return 0;
            }
            v |= (piece & (high - 1)) << shift;
            if (!(piece & high))
                return v;
        }
    }

    void Align32()
    {
        const size_t aligned = (pos_ + 31) & ~size_t(31);
        if (aligned > bits_)
        {
            bad_ = true;
            pos_ = bits_;
        }
        else
            pos_ = aligned;
    }

    bool EnterBlock(uint32_t& id, unsigned& width, size_t& end)
    {
        id = uint32_t(Vbr(8));
        width = unsigned(Vbr(4));
        Align32();
        const uint64_t words = Read(32);
        if (bad_ || width < 2 || width > 32 || words > (bits_ - pos_) / 32)
        {
            bad_ = true;
            return false;
        }
        end = pos_ + size_t(words) * 32;
        return true;
    }

    void DefineAbbrev(Abbrevs& s, uint32_t block)
    {
        const uint64_t n = Vbr(5);
        if (bad_ || n == 0 || n > 64 || s.n >= Abbrevs::kMax || uint64_t(s.nOps) + n > Abbrevs::kMaxOps ||
            block == kNone)
        {
            bad_ = true;
            return;
        }
        const int first = s.nOps;
        for (uint64_t i = 0; i < n && !bad_; ++i)
        {
            uint32_t op = 0;
            if (Read(1))
            {
                const uint64_t literal = Vbr(8);
                if (literal >> 28)
                    bad_ = true;
                op = uint32_t(literal) << 3 | kLiteral;
            }
            else
            {
                const uint64_t encoding = Read(3);
                if (encoding == kFixed || encoding == kVbr)
                {
                    const uint64_t width = Vbr(5);
                    if (width == 0) // fixed(0) / vbr(0) read nothing: a literal 0
                        op = kLiteral;
                    else if (width > (encoding == kFixed ? 64u : 32u) || (encoding == kVbr && width < 2))
                        bad_ = true;
                    else
                        op = uint32_t(width) << 3 | uint32_t(encoding);
                }
                else if (encoding == kArray || encoding == kChar6 || encoding == kBlob)
                    op = uint32_t(encoding);
                else
                    bad_ = true;
            }
            s.ops[s.nOps++] = op;
        }
        s.first[s.n] = uint16_t(first);
        s.size[s.n] = uint16_t(n);
        s.block[s.n] = block;
        ++s.n;
    }

    uint64_t Scalar(uint32_t op)
    {
        switch (op & 7)
        {
        case kLiteral:
            return op >> 3;
        case kFixed:
            return Read(op >> 3);
        case kVbr:
            return Vbr(op >> 3);
        case kChar6:
            return Read(6);
        default:
            bad_ = true;
            return 0;
        }
    }

    void Push(uint64_t v, bool& haveCode)
    {
        if (!haveCode)
        {
            rec_.code = v;
            haveCode = true;
            return;
        }
        if (rec_.count < Record::kMaxOps)
            rec_.ops[rec_.count] = v;
        ++rec_.count;
    }

    bool ReadRecord(uint64_t abbrev, const uint8_t* inherited, int nInherited, int firstLocal)
    {
        rec_.code = 0;
        rec_.count = 0;
        bool haveCode = false;
        if (abbrev == 3) // UNABBREV_RECORD: code, count, operands, all vbr6
        {
            Push(Vbr(6), haveCode);
            const uint64_t n = Vbr(6);
            if (bad_ || n > (bits_ - pos_) / 6)
            {
                bad_ = true;
                return false;
            }
            for (uint64_t i = 0; i < n && !bad_; ++i)
                Push(Vbr(6), haveCode);
            return !bad_;
        }
        const uint64_t k = abbrev - 4;
        const Abbrevs* s;
        int a;
        if (k < uint64_t(nInherited))
        {
            s = &blockInfo_;
            a = inherited[k];
        }
        else if (k - uint64_t(nInherited) < uint64_t(local_.n - firstLocal))
        {
            s = &local_;
            a = firstLocal + int(k - uint64_t(nInherited));
        }
        else
        {
            bad_ = true;
            return false;
        }
        const uint32_t* ops = s->ops + s->first[a];
        const int n = s->size[a];
        for (int i = 0; i < n && !bad_; ++i)
        {
            const uint32_t kind = ops[i] & 7;
            if (kind == kArray)
            {
                if (i + 1 >= n)
                {
                    bad_ = true;
                    break;
                }
                const uint32_t element = ops[++i];
                const uint32_t elementKind = element & 7;
                const uint64_t count = Vbr(6);
                // Every element takes at least one bit, so the count is bounded by what is left.
                if (bad_ || count > bits_ - pos_ ||
                    (elementKind != kFixed && elementKind != kVbr && elementKind != kChar6))
                {
                    bad_ = true;
                    break;
                }
                for (uint64_t j = 0; j < count && !bad_; ++j)
                    Push(Scalar(element), haveCode);
            }
            else if (kind == kBlob)
            {
                const uint64_t length = Vbr(6);
                Align32();
                if (bad_ || length > (bits_ - pos_) / 8)
                {
                    bad_ = true;
                    break;
                }
                pos_ += size_t(length) * 8;
                Align32();
            }
            else
                Push(Scalar(ops[i]), haveCode);
        }
        if (!haveCode)
            bad_ = true;
        return !bad_;
    }

    // Which blocks are read; the rest (metadata, symbol tables, attributes) are skipped by their length.
    bool Wanted(uint32_t parent, uint32_t id) const
    {
        if (parent == kModuleBlock)
            return id == kBlockInfoBlock || id == kTypeBlock || id == kConstantsBlock || id == kFunctionBlock ||
                   (full_ && info_.stage == Stage::Compute && !info_.numthreads[0] && id == kMetadataBlock);
        if (parent == kFunctionBlock)
            return id == kConstantsBlock;
        return false;
    }

    void ParseBlock(uint32_t id, unsigned width, size_t end, int depth)
    {
        const int savedOps = local_.nOps, savedAbbrevs = local_.n;
        const int firstLocal = local_.n;
        uint8_t inherited[Abbrevs::kMax];
        int nInherited = 0;
        for (int a = 0; a < blockInfo_.n; ++a)
            if (blockInfo_.block[a] == id)
                inherited[nInherited++] = uint8_t(a);
        uint32_t blockInfoTarget = kNone;

        if (id == kFunctionBlock)
            BeginFunction();
        else if (id == kConstantsBlock)
            constantType_ = kNone;

        while (!bad_ && !failed_)
        {
            if (pos_ >= end)
            {
                bad_ = true;
                break;
            }
            const uint64_t abbrev = Read(width);
            if (abbrev == 0) // END_BLOCK
            {
                Align32();
                break;
            }
            if (abbrev == 1) // ENTER_SUBBLOCK
            {
                uint32_t sub;
                unsigned subWidth;
                size_t subEnd;
                if (!EnterBlock(sub, subWidth, subEnd) || subEnd > end)
                {
                    bad_ = true;
                    break;
                }
                if (depth < 3 && Wanted(id, sub))
                    ParseBlock(sub, subWidth, subEnd, depth + 1);
                pos_ = subEnd;
                if (id == kFunctionBlock && sub == kConstantsBlock && !full_)
                    break; // cheap step: the constants were all it wanted from this function
                continue;
            }
            if (abbrev == 2) // DEFINE_ABBREV
            {
                if (id == kBlockInfoBlock)
                    DefineAbbrev(blockInfo_, blockInfoTarget);
                else
                    DefineAbbrev(local_, id);
                continue;
            }
            if (!ReadRecord(abbrev, inherited, nInherited, firstLocal))
                break;
            if (id == kBlockInfoBlock)
            {
                if (rec_.code == 1 && rec_.Has(0)) // SETBID
                    blockInfoTarget = uint32_t(rec_.ops[0]);
                continue;
            }
            if (id == kFunctionBlock && !full_ && rec_.code != 1)
                break; // cheap step: past DECLAREBLOCKS come the instructions
            OnRecord(id);
        }
        local_.nOps = savedOps;
        local_.n = savedAbbrevs;
    }

    // ---- records

    const Type* TypeAt(uint64_t id) const { return id < nTypes_ && id < kMaxTypes ? &types_[id] : nullptr; }

    void OnRecord(uint32_t block)
    {
        if (block == kModuleBlock)
            ModuleRecord();
        else if (block == kTypeBlock)
            TypeRecord();
        else if (block == kConstantsBlock)
            ConstantRecord();
        else if (block == kFunctionBlock)
            Instruction();
        else if (block == kMetadataBlock)
            MetadataRecord();
    }

    void MetadataRecord()
    {
        if (rec_.code == 4) // NAME; the following NAMED_NODE holds absolute metadata IDs
        {
            constexpr char name[] = "dx.entryPoints";
            entryName_ = rec_.count == sizeof(name) - 1;
            for (size_t i = 0; entryName_ && i < sizeof(name) - 1; ++i)
                entryName_ = rec_.Op(i) == uint8_t(name[i]);
            return;
        }
        if (rec_.code == 10)
        {
            if (entryName_ && rec_.Has(0))
                entryPoint_ = uint32_t(rec_.Op(0));
            entryName_ = false;
            return;
        }
        if (rec_.code != 1 && rec_.code != 2 && rec_.code != 3 && rec_.code != 5)
            return;
        if (nMetadata_ < kMaxMetadata)
        {
            Metadata& m = metadata_[nMetadata_];
            m.code = rec_.code;
            m.count = uint32_t(rec_.count);
            for (size_t i = 0; i < 8; ++i)
                m.ops[i] = uint32_t(rec_.Op(i));
        }
        ++nMetadata_;
    }

    const Metadata* MetadataAt(uint32_t id) const
    {
        return id < nMetadata_ && id < kMaxMetadata ? &metadata_[id] : nullptr;
    }

    const Metadata* MetadataRef(const Metadata* m, size_t i) const
    {
        return m && (m->code == 3 || m->code == 5) && i < m->count && i < 8 && m->ops[i] ? MetadataAt(m->ops[i] - 1)
                                                                                         : nullptr;
    }

    bool MetadataValue(const Metadata* m, uint32_t& value) const
    {
        return m && m->code == 2 && m->count == 2 && ConstantAt(m->ops[1], value);
    }

    void ReadNumthreads()
    {
        const Metadata* properties = MetadataRef(MetadataAt(entryPoint_), 4);
        for (size_t i = 0; properties && i + 1 < properties->count && i + 1 < 8; i += 2)
        {
            uint32_t tag;
            if (!MetadataValue(MetadataRef(properties, i), tag) || tag != 4)
                continue;
            const Metadata* threads = MetadataRef(properties, i + 1);
            uint32_t xyz[3];
            if (threads && threads->count == 3 && MetadataValue(MetadataRef(threads, 0), xyz[0]) &&
                MetadataValue(MetadataRef(threads, 1), xyz[1]) && MetadataValue(MetadataRef(threads, 2), xyz[2]))
                std::memcpy(info_.numthreads, xyz, sizeof(xyz));
            return;
        }
    }

    void ModuleRecord()
    {
        switch (rec_.code)
        {
        case 1: // VERSION: 1 = operands are relative value IDs
            relativeIds_ = rec_.Op(0) >= 1;
            break;
        case 8: // FUNCTION: [type, callingconv, isproto, ...]
            if (nFunctions_ < kMaxFunctions)
            {
                Function& f = functions_[nFunctions_];
                f.value = nextValue_;
                f.type = uint32_t(rec_.Op(0));
                const Type* t = TypeAt(f.type);
                if (t && t->code == 8) // a pointer to the function type (older writers)
                    f.type = t->ref;
                f.prototype = rec_.Op(2) != 0;
            }
            ++nFunctions_;
            ++nextValue_;
            break;
        case 7:  // GLOBALVAR
        case 9:  // ALIAS_OLD
        case 14: // ALIAS
            ++nextValue_;
            break;
        default:
            break;
        }
    }

    void TypeRecord()
    {
        const uint64_t code = rec_.code;
        if (code == 1 || code == 19) // NUMENTRY, STRUCT_NAME: no type of their own
            return;
        if (nTypes_ < kMaxTypes)
        {
            Type& t = types_[nTypes_];
            t = Type();
            t.code = uint8_t(code);
            if (code == 21 || code == 9)
            {
                const size_t firstParam = code == 21 ? 2 : 3;
                t.ref = uint32_t(rec_.Op(firstParam - 1));
                t.params = uint16_t(
                    rec_.count >= firstParam && rec_.count - firstParam < 0xffff ? rec_.count - firstParam : 0xffff);
            }
            else if (code == 7 || code == 8) // INTEGER [width], POINTER [pointee]
                t.ref = uint32_t(rec_.Op(0));
        }
        ++nTypes_;
    }

    void ConstantRecord()
    {
        if (rec_.code == 1) // SETTYPE
        {
            constantType_ = uint32_t(rec_.Op(0));
            return;
        }
        const uint32_t id = nextValue_++;
        const Type* type = TypeAt(constantType_);
        switch (rec_.code)
        {
        case 2: // NULL: integer 0 or a zeroinitializer
            if (type && type->code == 7)
                AddConstant(id, 0);
            else if (type && (type->code == 18 || type->code == 20))
                AddAggregate(id, true);
            break;
        case 4: // INTEGER: sign-rotated vbr
        {
            const uint64_t v = rec_.Op(0);
            if (!(v & 1))
            {
                info_.magics |= MagicBit(v >> 1);
                if ((v >> 1) < (1u << 20))
                    AddConstant(id, uint32_t(v >> 1));
            }
            break;
        }
        case 7: // AGGREGATE: absolute value IDs of the members
            if (rec_.count == 4)
                AddAggregate(id, false);
            break;
        case 22: // DATA: raw elements of an integer array / vector
            for (size_t i = 0; rec_.Has(i); ++i)
                info_.magics |= MagicBit(rec_.ops[i]);
            break;
        default:
            break;
        }
    }

    void AddConstant(uint32_t id, uint32_t value)
    {
        if (nConstants_ < kMaxConstants)
            constants_[nConstants_++] = Constant { id, value };
    }

    void AddAggregate(uint32_t id, bool zero)
    {
        if (nAggregates_ >= kMaxAggregates)
            return;
        Aggregate& a = aggregates_[nAggregates_++];
        a.id = id;
        a.zero = zero;
        for (int i = 0; i < 4; ++i)
            a.member[i] = zero ? 0 : uint32_t(rec_.Op(size_t(i)));
    }

    // Small integer constant by absolute value ID; IDs are added in increasing order, so a binary search.
    bool ConstantAt(uint32_t id, uint32_t& value) const
    {
        uint32_t lo = 0, hi = nConstants_;
        while (lo < hi)
        {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (constants_[mid].id < id)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < nConstants_ && constants_[lo].id == id)
        {
            value = constants_[lo].value;
            return true;
        }
        return false;
    }

    void BeginFunction()
    {
        if (moduleConstants_ == kNone) // first function: what came before belongs to the module
        {
            moduleConstants_ = nConstants_;
            moduleAggregates_ = nAggregates_;
            moduleValues_ = nextValue_;
        }
        nConstants_ = moduleConstants_;
        nAggregates_ = moduleAggregates_;
        nHandles_ = 0;
        nextValue_ = moduleValues_;

        // Function blocks come in the order of the function records that have a body. Its arguments come first.
        uint32_t defined = 0;
        const Function* f = nullptr;
        for (uint32_t i = 0; i < nFunctions_ && i < kMaxFunctions; ++i)
            if (!functions_[i].prototype && defined++ == functionBlocks_)
            {
                f = &functions_[i];
                break;
            }
        ++functionBlocks_;
        const Type* t = f ? TypeAt(f->type) : nullptr;
        if (full_ && (!t || (t->code != 21 && t->code != 9)))
        {
            failed_ = true;
            return;
        }
        if (t)
            nextValue_ += t->params;
    }

    // Operands are relative to the ID of the instruction that holds them (the next value number before it).
    uint32_t Absolute(uint64_t operand) const
    {
        return relativeIds_ ? uint32_t(instruction_ - uint32_t(operand)) : uint32_t(operand);
    }

    bool ArgConstant(size_t i, uint32_t& value) const
    {
        return rec_.Has(i) && ConstantAt(Absolute(rec_.ops[i]), value);
    }

    void Instruction()
    {
        instruction_ = nextValue_;
        const uint64_t op = rec_.code;
        if (op == 34)
            Call();
        else if ((op >= 2 && op <= 9) || op == 16 || op == 19 || op == 20 || op == 23 || (op >= 26 && op <= 30) ||
                 (op >= 37 && op <= 38) || (op >= 40 && op <= 41) || op == 43 || (op >= 46 && op <= 47))
            ++nextValue_;
        else if (op != 1 && (op < 10 || op > 12) && op != 15 && op != 24 && op != 31 && op != 33 && op != 35 &&
                 op != 36 && op != 39 && op != 42 && op != 44 && op != 45)
            failed_ = true;
    }

    void Call()
    {
        // [paramattrs, cc, (fmf), (fnty), callee, args...]
        const uint64_t cc = rec_.Op(1);
        size_t k = 2;
        if ((cc >> 17) & 1)
            ++k;
        uint64_t fnType = kNone;
        if ((cc >> 15) & 1)
            fnType = rec_.Op(k++);
        const uint32_t callee = Absolute(rec_.Op(k++));
        if (fnType == kNone)
            for (uint32_t i = 0; i < nFunctions_ && i < kMaxFunctions; ++i)
                if (functions_[i].value == callee)
                    fnType = functions_[i].type;
        const Type* ft = TypeAt(fnType);
        const Type* rt = ft ? TypeAt(ft->ref) : nullptr;
        if (!rt || (ft->code != 21 && ft->code != 9))
        {
            failed_ = true;
            return;
        }
        const uint32_t id = nextValue_;
        if (rt->code != 2) // non-void: the call numbers a value
            ++nextValue_;

        uint32_t dxop = 0;
        if (!ArgConstant(k, dxop)) // dx.op calls lead with their opcode as a constant
            return;
        Handle h;
        h.id = id;
        switch (dxop)
        {
        case 57: // createHandle(class, rangeID, index, nonUniform): index is the register
        {
            uint32_t cls, range, reg;
            h.kind = 3;
            if (ArgConstant(k + 1, cls) && ArgConstant(k + 2, range) && ArgConstant(k + 3, reg))
            {
                h.kind = 0;
                h.range = range;
                h.reg = reg;
            }
            AddHandle(h);
            break;
        }
        case 217: // createHandleFromBinding({lower, upper, space, class}, index, nonUniform) (SM 6.6)
        {
            h.kind = 3;
            const uint32_t bind = Absolute(rec_.Op(k + 1));
            for (uint32_t i = 0; i < nAggregates_; ++i)
            {
                const Aggregate& a = aggregates_[i];
                if (a.id != bind)
                    continue;
                uint32_t space = 0, reg = 0;
                if ((a.zero || ConstantAt(a.member[2], space)) && ArgConstant(k + 2, reg))
                {
                    h.kind = 1;
                    h.space = space;
                    h.reg = reg;
                }
                break;
            }
            AddHandle(h);
            break;
        }
        case 216: // annotateHandle(handle, properties) (SM 6.6)
            h.kind = 2;
            h.source = Absolute(rec_.Op(k + 1));
            AddHandle(h);
            break;
        case 218: // createHandleFromHeap: bindless, no register
            h.kind = 3;
            AddHandle(h);
            break;
        case 73:  // textureGather
        case 74:  // textureGatherCmp
        case 223: // textureGatherRaw (SM 6.7)
            ++info_.gathers;
            UseHandle(k + 1, r_.gatherTexture);
            UseHandle(k + 2, r_.gatherSampler);
            break;
        case 66: // textureLoad
            ++info_.loads;
            UseHandle(k + 1, r_.loadTexture);
            break;
        case 60: // sample, sampleBias, sampleLevel, sampleGrad, sampleCmp, sampleCmpLevelZero
        case 61:
        case 62:
        case 63:
        case 64:
        case 65:
        case 224: // sampleCmpLevel (SM 6.7)
            ++info_.samples;
            UseHandle(k + 1, r_.sampleTexture);
            UseHandle(k + 2, r_.sampleSampler);
            break;
        case 58: // cbufferLoad, cbufferLoadLegacy
        case 59:
            UseHandle(k + 1, r_.constants);
            break;
        default:
            break;
        }
    }

    void AddHandle(const Handle& h)
    {
        if (nHandles_ < kMaxHandles)
            handles_[nHandles_++] = h;
    }

    void UseHandle(size_t arg, Use& use) const
    {
        if (use.known || !rec_.Has(arg))
            return;
        uint32_t id = Absolute(rec_.ops[arg]);
        for (int depth = 0; depth < 4; ++depth)
        {
            const Handle* h = nullptr;
            for (uint32_t i = 0; i < nHandles_ && !h; ++i)
                if (handles_[i].id == id)
                    h = &handles_[i];
            if (!h || h->kind == 3)
                return;
            if (h->kind == 2)
            {
                id = h->source;
                continue;
            }
            use.known = true;
            use.reg = h->reg;
            use.range = h->range;
            use.space = h->space;
            use.spaceKnown = h->kind == 1;
            return;
        }
    }

    static constexpr uint32_t kMaxTypes = 512, kMaxFunctions = 256, kMaxConstants = 1024, kMaxAggregates = 32,
                              kMaxHandles = 64;

    const uint8_t* data_;
    size_t bits_, pos_ = 0;
    bool bad_ = false, failed_ = false;
    const bool full_;
    ShaderInfo& info_;
    Resources& r_;

    Abbrevs blockInfo_, local_;
    Record rec_;
    Type types_[kMaxTypes];
    uint32_t nTypes_ = 0;
    Function functions_[kMaxFunctions];
    uint32_t nFunctions_ = 0, functionBlocks_ = 0;
    Constant constants_[kMaxConstants];
    uint32_t nConstants_ = 0, moduleConstants_ = kNone;
    Aggregate aggregates_[kMaxAggregates];
    uint32_t nAggregates_ = 0, moduleAggregates_ = 0;
    Handle handles_[kMaxHandles];
    uint32_t nHandles_ = 0;
    uint32_t nextValue_ = 0, moduleValues_ = 0, constantType_ = kNone, instruction_ = 0;
    static constexpr uint32_t kMaxMetadata = 256;
    Metadata metadata_[kMaxMetadata];
    uint32_t nMetadata_ = 0, entryPoint_ = kNone;
    bool entryName_ = false;
    bool relativeIds_ = false;
};

inline void ClassifyDxil(const Container& c, ShaderInfo& info)
{
    // DXIL part: program version, size in dwords, 'DXIL', DXIL version, bitcode offset (from 'DXIL'), bitcode size.
    info.dxil = true;
    const Blob& part = c.dxil;
    if (part.size < 24 || Rd32(part.data + 8) != FourCC('D', 'X', 'I', 'L'))
    {
        info.why = "malformed shader code";
        return;
    }
    const uint64_t offset = 8 + uint64_t(Rd32(part.data + 16)), length = Rd32(part.data + 20);
    if (offset > part.size || length > part.size - offset)
    {
        info.why = "malformed shader code";
        return;
    }
    info.stage = StageOf(Rd32(part.data));
    if (info.stage == Stage::Other)
    {
        info.why = "not a pixel or compute shader";
        return;
    }
    const uint8_t* bitcode = part.data + offset;

    // Cheap step: the constant blocks only.
    {
        Resources unused;
        DxilWalker cheap(bitcode, size_t(length), false, info, unused);
        if (!cheap.Run())
        {
            info.why = "malformed shader code";
            return;
        }
    }
    if (!EasuMagics(info.magics) && !RcasMagics(info.magics))
    {
        info.why = "no FSR 1 approximation constants";
        return;
    }

    Resources r;
    ParsePsv0(c.psv0, r, info);
    info.magics = 0;
    DxilWalker walker(bitcode, size_t(length), true, info, r);
    if (!walker.Run())
    {
        info.gathers = info.loads = info.samples = 0;
        info.why = "could not follow the DXIL code";
        return;
    }
    Decide(info);
    SetBindings(info, r);
}

inline uint32_t HalfBits(float f) // round to nearest even, normal range only (callers check)
{
    const uint32_t u = AsUint(f);
    const uint32_t exponent = ((u >> 23) & 0xff) - 127 + 15, mantissa = u & 0x7fffff;
    uint32_t h = exponent << 10 | mantissa >> 13;
    const uint32_t rest = mantissa & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1)))
        ++h;
    return h;
}

// 1/size as FsrEasuCon computes it, give or take one ulp for engines that compute it another way.
inline bool SizeFromReciprocal(uint32_t bits, uint32_t& size)
{
    const float r = AsFloat(bits);
    if (!(r >= 1.0f / 16384.0f && r <= 1.0f / 16.0f))
        return false;
    size = uint32_t(std::floor(1.0 / double(r) + 0.5));
    const uint32_t back = AsUint(1.0f / float(size));
    return back + 1 >= bits && back <= bits + 1;
}
} // namespace detail

inline ShaderInfo Classify(const void* bytecode, size_t size)
{
    ShaderInfo info;
    detail::Container c;
    if (!detail::ParseContainer(static_cast<const uint8_t*>(bytecode), size, c))
        info.why = "not a DXBC/DXIL container";
    else if (c.dxil.data)
        detail::ClassifyDxil(c, info);
    else if (c.shex.data)
        detail::ClassifyDxbc(c.shex, info);
    else
        info.why = "no DXIL or SHEX/SHDR part";
    return info;
}

inline RootSignatureBlob RootSignaturePart(const void* bytecode, size_t size)
{
    detail::Container c;
    if (!detail::ParseContainer(static_cast<const uint8_t*>(bytecode), size, c))
        return {};
    return { c.rts0.data, c.rts0.size };
}

inline bool DecodeEasu(const uint32_t con[16], uint32_t outputWidth, uint32_t outputHeight, EasuConstants* out)
{
    using namespace detail;
    if (!con)
        return false;
    const uint32_t* c0 = con;
    const uint32_t* c1 = con + 4;
    const uint32_t* c2 = con + 8;
    const uint32_t* c3 = con + 12;
    constexpr uint32_t kSign = 0x80000000u, kExponentOne = 1u << 23;

    // con1..con3 are +-1, 2 and 4 times 1/size: exact in float, so exact in the bits (sign flips, exponent steps).
    if (c1[2] != c1[0] || c2[2] != c1[0] || c2[0] != (c1[0] ^ kSign) || c1[3] != (c1[1] ^ kSign))
        return false;
    if (c2[1] != c1[1] + kExponentOne || c2[3] != c2[1] || c3[1] != c1[1] + 2 * kExponentOne)
        return false;
    if (c3[0] != 0 && c3[0] != kSign) // 0 * 1/width; con3.zw are padding and not looked at
        return false;

    EasuConstants r;
    if (!SizeFromReciprocal(c1[0], r.inputWidth) || !SizeFromReciprocal(c1[1], r.inputHeight))
        return false;
    r.scaleX = AsFloat(c0[0]);
    r.scaleY = AsFloat(c0[1]);
    if (!(r.scaleX >= 0.2f && r.scaleX <= 1.0001f && r.scaleY >= 0.2f && r.scaleY <= 1.0001f)) // 1x to 5x
        return false;
    // con0.zw = 0.5 * scale - 0.5 + offset (FsrEasuConOffset; plain FsrEasuCon has no offset).
    r.offsetX = AsFloat(c0[2]) - (0.5f * r.scaleX - 0.5f);
    r.offsetY = AsFloat(c0[3]) - (0.5f * r.scaleY - 0.5f);
    if (!(std::fabs(r.offsetX) < float(r.inputWidth) && std::fabs(r.offsetY) < float(r.inputHeight)))
        return false;
    if (outputWidth && outputHeight)
    {
        r.outputWidth = outputWidth;
        r.outputHeight = outputHeight;
        r.viewportWidth = r.scaleX * float(outputWidth);
        r.viewportHeight = r.scaleY * float(outputHeight);
        // The viewport has to fit in the input texture: an output size that does not belong to these constants
        // (a render target the pass does not draw to) gives a viewport larger than the texture.
        if (!(r.viewportWidth >= 1.0f && r.viewportHeight >= 1.0f &&
              r.offsetX + r.viewportWidth <= float(r.inputWidth) + 0.5f &&
              r.offsetY + r.viewportHeight <= float(r.inputHeight) + 0.5f))
            return false;
    }
    if (out)
        *out = r;
    return true;
}

inline bool DecodeRcas(const uint32_t con[4], float* sharpness)
{
    using namespace detail;
    if (!con || con[2] != 0 || con[3] != 0)
        return false;
    const float linear = AsFloat(con[0]);
    if (!(linear >= 1.0f / 16384.0f && linear <= 1.0f)) // 0 to 14 stops, normal halfs
        return false;
    const uint32_t low = con[1] & 0xffff, high = con[1] >> 16;
    const uint32_t half = HalfBits(linear);
    // ffx_a.h's CPU float->half truncates, DirectXMath rounds: one apart at most.
    if (low != high || low + 1 < half || low > half + 1)
        return false;
    if (sharpness)
        *sharpness = -std::log2(linear);
    return true;
}
} // namespace native::fsr1
