// WaterPs -- phase 97: the game's "new SCS water" (Iceland DLC and later) is drawn WITHOUT the phase-23 jitter overscan.
// Pure DXBC classification of a pixel shader at CreatePixelShader time (any thread, no D3D calls); inject.cpp keeps the table of
// the classified shader objects and switches the viewport per draw (WaterOnDrawBegin / WaterOnDrawEnd).
// Unit test: tests/water_ps_test.cpp (fxc-compiled look-alikes of the game's shaders + a WARP check of the checker maths).
//
// WHY (captures/ets2_pond_water_audit.md, RenderDoc ETS2 2026-11-11 frames 2564 / 3700): the water keeps the bed's G-buffer and the
// water surface on a CHECKERBOARD of the same G-buffer. Each water shader works out its checker cell from the INTERPOLATED clip
// position, not from SV_Position:
//     G-buffer writer (PS c369f136-...):  pix = v5.yz / v5.x * cb0[17].xy;  discard_z((ftou(pix.x) + ftou(pix.y) + ftou(cb0[20].x)) & 1)
//     forward reader  (PS 185d5b7a-...):  the same parity from v7.yz / v7.x * cb0[23..24] -> ld of the water cell and the bed cell
// v5.yz / v5.x is the pixel's position in [0, 1] across the VIEWPORT; cb0[17] is the scene size W x H. With the game's W x H
// viewport shifted by s, pixel p samples u * W = p + 0.5 - s, and floor() = p for every |s| < 0.5: the computed cell is the
// rasterised pixel. With the phase-23 (W + 1) x (H + 1) viewport u * W = (p + 0.5 - s) * W / (W + 1): beyond x = (0.5 - s) * W the
// cell is p - 1 in that axis, the parity flips in every rectangle where exactly ONE axis is off, and the forward pass reads the bed
// cell as the water cell -> the water vanishes in big rectangular blocks that move with the jitter phase (issue #30).
//
// RULES (one classification per created shader; the first that holds wins):
//   kRuleCheckerWrite  the structural checker WRITER: an interpolant divided by a component of the SAME input register (a
//                      perspective divide: vN.yz / vN.x), multiplied by a constant-buffer value (the scene size), converted to
//                      an integer (ftou / ftoi), reduced to its parity (and ..., l(1)), and a discard on that parity
//   kRuleCheckerRead   the structural checker READER: the same parity chain, and an `ld` whose address carries both the integer
//                      pixel and the parity (the reader picks the water / bed cell by the parity)
//   kRuleFlag          the water's material-FLAG G-buffer draw (PS 4b5eb7ed-...): no inputs, no resources, >= 4 outputs, nothing
//                      but `mov oN, l(...)` + ret, at least one integer immediate (a uint material id, 1 .. 2^24-1, not a float
//                      bit pattern). It writes the depth (GREATER_EQUAL) the checker writer then tests GREATER_EQUAL against at the
//                      same pixels, so it must rasterise with the same mapping, or the writer's depth test fails in half the
//                      picture (the plane's depth sampled ~x/W px apart)
//   kRuleHash          none of the above, but the bytecode checksum is one of the three known 2026-11 shaders (the fallback)
// The checksum is compared for every class: `known` = it is one of the three (the log says "known hash" / "NEW shader").
// False positives are harmless by construction: a shader of another draw that matches only loses the overscan for that draw (its
// picture lands up to x / W px off the overscanned geometry around it -- sub-pixel over most of the screen -- and its last column /
// row on a negative shift, which inject.cpp re-draws). The structural rules need a perspective divide of an interpolant by its
// own component AND a parity of the resulting pixel, a pattern that only screen-space checkerboards have.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace WaterPs {

enum Rule : int { kRuleNone = 0, kRuleCheckerWrite = 1, kRuleCheckerRead = 2, kRuleFlag = 3, kRuleHash = 4 };

inline const char* RuleName(int r) {
    switch (r) {
        case kRuleCheckerWrite: return "checker writer (interpolant / w * cbuffer size -> ftou -> & 1 -> discard)";
        case kRuleCheckerRead:  return "checker reader (interpolant / w * cbuffer size -> ftou -> & 1 -> ld at that cell)";
        case kRuleFlag:         return "material flag (no inputs, constant MRT outputs with a uint id)";
        case kRuleHash:         return "known hash only (the structural rules did not match)";
        default:                return "none";
    }
}

// The three shaders of the RenderDoc audit (ETS2 1.5x, 2026-11-11): DXBC checksum = bytes 4..19 as 4 little-endian dwords
// (RenderDoc's "Shader hash"). Index 0 writer, 1 reader, 2 flag.
constexpr uint32_t kKnown[3][4] = {
    { 0xc369f136u, 0x109f6918u, 0x1886484du, 0x8c1e6edeu },   // G-buffer checker writer (EID 6611)
    { 0x185d5b7au, 0xe6cc2ba8u, 0x6bef5c09u, 0x5f5bbf0fu },   // forward water reader (EID 6876)
    { 0x4b5eb7edu, 0x0a911f92u, 0x17a9c63fu, 0xe632a5b8u },   // G-buffer material-flag draw, RT3 = (0, 0, 3, 8064) (EID 6571)
};

struct Result {
    int rule = kRuleNone;     // Rule
    int known = -1;           // index into kKnown, -1 = not one of them
    uint32_t sum[4] = {};     // the checksum (for the log)
    bool parsed = false;      // a pixel shader whose SHDR / SHEX chunk parsed to the end
};

inline uint32_t Rd(const uint8_t* b, size_t off) { uint32_t v; memcpy(&v, b + off, 4); return v; }

namespace detail {

// taint bits per temp component
constexpr uint8_t kP = 1;     // a perspective-divided interpolant (vN.? / vN.?)
constexpr uint8_t kS = 2;     // ... times a constant-buffer value (a screen position in pixels)
constexpr uint8_t kI = 4;     // ... as an integer (ftou / ftoi)
constexpr uint8_t kPar = 8;   // ... its parity (and with l(1)), or derived from it
constexpr uint8_t kC = 16;    // derived from a constant buffer

constexpr uint32_t kOpTemp = 0, kOpInput = 1, kOpOutput = 2, kOpImm32 = 4, kOpImm64 = 5, kOpCb = 8;
constexpr int kMaxTemps = 256;

struct Opnd {
    uint32_t type = 0xFFFFFFFFu;
    uint32_t idx0 = 0;
    bool     hasIdx0 = false;
    uint32_t selMode = 0;     // 0 mask, 1 swizzle, 2 select1
    uint32_t sel = 0;         // mask bits / swizzle byte / select1 component
    uint32_t nComp = 0;       // 0 / 1 / 4
    uint32_t imm[4] = {};
    uint32_t nImm = 0;
    int comp(int c) const {   // the source component read for destination component c
        if (selMode == 1) return (int)((sel >> (2 * c)) & 3u);
        if (selMode == 2) return (int)(sel & 3u);
        return c;
    }
};

// Parses one operand at token i (< end). Returns the next token index, or 0 on a malformed operand.
inline uint32_t ParseOpnd(const uint8_t* b, size_t base, uint32_t i, uint32_t end, Opnd& o, int depth = 0) {
    if (i >= end || depth > 4) return 0;
    const uint32_t t = Rd(b, base + (size_t)i * 4);
    ++i;
    const uint32_t nc = t & 3u;
    o.nComp = nc == 0 ? 0 : (nc == 1 ? 1 : 4);
    o.selMode = (t >> 2) & 3u;
    o.sel = o.selMode == 0 ? ((t >> 4) & 0xFu) : (o.selMode == 1 ? ((t >> 4) & 0xFFu) : ((t >> 4) & 3u));
    if (nc != 2) { o.selMode = 2; o.sel = 0; }       // 1-component operands read .x
    o.type = (t >> 12) & 0xFFu;
    const uint32_t dims = (t >> 20) & 3u;
    bool ext = (t >> 31) != 0;
    while (ext) {                                     // extended operand tokens (modifiers: neg / abs, precision)
        if (i >= end) return 0;
        ext = (Rd(b, base + (size_t)i * 4) >> 31) != 0;
        ++i;
    }
    if (o.type == kOpImm32 || o.type == kOpImm64) {
        const uint32_t n = (o.nComp == 4 ? 4u : 1u) * (o.type == kOpImm64 ? 2u : 1u);
        if (i + n > end) return 0;
        if (o.type == kOpImm32) {
            o.nImm = o.nComp == 4 ? 4u : 1u;
            for (uint32_t k = 0; k < o.nImm; ++k) o.imm[k] = Rd(b, base + (size_t)(i + k) * 4);
        }
        return i + n;
    }
    for (uint32_t d = 0; d < dims; ++d) {
        const uint32_t rep = (t >> (22 + 3 * d)) & 7u;
        if (rep == 0 || rep == 3) {                   // imm32 (+ relative)
            if (i >= end) return 0;
            if (d == 0) { o.idx0 = Rd(b, base + (size_t)i * 4); o.hasIdx0 = true; }
            ++i;
        } else if (rep == 1 || rep == 4) {            // imm64 (+ relative)
            if (i + 2 > end) return 0;
            i += 2;
        }
        if (rep == 2 || rep == 3 || rep == 4) {       // relative: a nested operand
            Opnd rel;
            i = ParseOpnd(b, base, i, end, rel, depth + 1);
            if (!i) return 0;
        }
        if (rep > 4) return 0;
    }
    return i;
}

inline bool IsDecl(uint32_t opc) {
    return (opc >= 88u && opc <= 106u) || opc == 53u || (opc >= 143u && opc <= 162u);   // D3D10 / D3D11 declarations, customdata
}
// opcodes that fetch from a resource: the result is new data, not derived from the address (no taint)
inline bool IsFetch(uint32_t opc) {
    return opc == 45u || opc == 46u || opc == 61u || (opc >= 69u && opc <= 74u) || (opc >= 108u && opc <= 111u) ||
           (opc >= 126u && opc <= 128u) || opc == 163u || opc == 165u || opc == 167u;
}
// opcodes without a destination operand
inline bool NoDest(uint32_t opc) {
    switch (opc) {
        case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 13: case 18: case 19: case 20: case 21:
        case 22: case 23: case 31: case 44: case 48: case 58: case 62: case 63: case 76:
            return true;
        default: return false;
    }
}
// opcodes with two destination operands
inline bool TwoDest(uint32_t opc) { return opc == 38u || opc == 77u || opc == 78u || opc == 81u || opc == 132u || opc == 133u || opc == 142u; }
// dot products: every destination component reads every source component
inline bool IsDot(uint32_t opc) { return opc == 15u || opc == 16u || opc == 17u; }

}  // namespace detail

// Classifies a DXBC blob. Pure function (any thread).
inline Result ClassifyImpl(const void* code, size_t len) {
    using namespace detail;
    Result r;
    if (!code || len < 32) return r;
    const uint8_t* const b = static_cast<const uint8_t*>(code);
    if (Rd(b, 0) != 0x43425844u) return r;                       // 'DXBC'
    for (int i = 0; i < 4; ++i) r.sum[i] = Rd(b, 4 + 4 * (size_t)i);
    for (int k = 0; k < 3; ++k)
        if (!memcmp(r.sum, kKnown[k], sizeof(r.sum))) r.known = k;
    const uint32_t nChunks = Rd(b, 28);
    if (nChunks > 64 || 32 + (size_t)nChunks * 4 > len) return r;
    size_t base = 0;
    uint32_t n = 0;
    for (uint32_t c = 0; c < nChunks && !n; ++c) {
        const size_t off = Rd(b, 32 + (size_t)c * 4);
        if (off + 8 > len) return r;
        const uint32_t fourcc = Rd(b, off), size = Rd(b, off + 4);
        if (fourcc != 0x52444853u && fourcc != 0x58454853u) continue;   // 'SHDR' / 'SHEX'
        if (size < 8 || off + 8 + size > len) return r;
        base = off + 8;
        if ((Rd(b, base) >> 16) != 0) return r;                  // program type 0 = pixel shader
        const uint32_t nTok = size / 4, declared = Rd(b, base + 4);
        n = declared < nTok ? declared : nTok;
    }
    if (!n) return r;

    static_assert(kMaxTemps * 4 <= 1024, "taint table size");
    uint8_t taint[kMaxTemps][4];
    memset(taint, 0, sizeof(taint));
    int nInputs = 0, nOutputs = 0, nRes = 0;
    bool onlyImmMovs = true, intImm = false, anyCode = false;
    bool parity = false, discardPar = false, ldPar = false;
    for (uint32_t i = 2; i < n;) {
        const uint32_t op = Rd(b, base + (size_t)i * 4), opc = op & 0x7FFu;
        uint32_t ilen = (op >> 24) & 0x7Fu;
        if (opc == 0x35u) {                                      // customdata: the next token is the length
            if (i + 1 >= n) return r;
            ilen = Rd(b, base + (size_t)(i + 1) * 4);
        }
        if (ilen == 0 || ilen > n - i) return r;                 // (98c: no 32-bit wrap on a huge customdata length)
        const uint32_t end = i + ilen;
        if (IsDecl(opc)) {
            if (opc >= 98u && opc <= 100u) ++nInputs;            // dcl_input_ps / _siv / _sgv
            else if (opc >= 95u && opc <= 97u) ++nInputs;        // dcl_input (/ _sgv / _siv): pixel-shader system inputs
            else if (opc >= 101u && opc <= 103u) ++nOutputs;     // dcl_output (/ _sgv / _siv)
            else if (opc == 88u || opc == 89u || opc == 90u || (opc >= 156u && opc <= 158u) || opc == 161u || opc == 162u)
                ++nRes;                                          // resource / cbuffer / sampler / UAVs / raw / structured
            i = end;
            continue;
        }
        anyCode = true;
        uint32_t j = i + 1;
        bool ext = (op >> 31) != 0;
        while (ext) {                                            // extended opcode tokens (sample offsets, resource dims)
            if (j >= end) return r;
            ext = (Rd(b, base + (size_t)j * 4) >> 31) != 0;
            ++j;
        }
        Opnd ops[8];
        int no = 0;
        while (j < end && no < 8) {
            j = ParseOpnd(b, base, j, end, ops[no]);
            if (!j) return r;
            ++no;
        }
        if (j != end) return r;                                  // more than 8 operands / trailing tokens: not understood
        // the flag rule: nothing but mov oN, l(...) and ret
        if (opc == 62u) {
        } else if (opc == 54u && no == 2 && ops[0].type == kOpOutput && ops[1].type == kOpImm32) {
            for (uint32_t k = 0; k < ops[1].nImm; ++k)
                if (ops[1].imm[k] >= 1u && ops[1].imm[k] < (1u << 24)) intImm = true;
        } else {
            onlyImmMovs = false;
        }
        // the taint of every source operand, per destination component
        const int nDest = NoDest(opc) ? 0 : (TwoDest(opc) ? 2 : 1);
        uint8_t srcT[8][4] = {};
        bool srcCb[8] = {}, srcOne[8][4] = {};
        for (int k = nDest; k < no; ++k) {
            const Opnd& o = ops[k];
            for (int cc = 0; cc < 4; ++cc) {
                const int sc = o.comp(cc);
                if (o.type == kOpTemp && o.hasIdx0 && o.idx0 < (uint32_t)kMaxTemps) srcT[k][cc] = taint[o.idx0][sc];
                else if (o.type == kOpCb) { srcT[k][cc] = kC; srcCb[k] = true; }
                else if (o.type == kOpImm32) srcOne[k][cc] = o.imm[o.nImm == 4 ? cc : 0] == 1u;
            }
        }
        if (opc == 13u && no >= 1) {                             // discard_z / discard_nz on the parity
            for (int cc = 0; cc < 4; ++cc) if (srcT[0][cc] & kPar) discardPar = true;
        }
        if ((opc == 45u || opc == 46u) && no >= 2) {             // ld / ld_ms: operand 1 = the address
            uint8_t u = 0;
            for (int cc = 0; cc < 4; ++cc) u |= srcT[1][cc];
            if ((u & kI) && (u & kPar)) ldPar = true;
        }
        if (!nDest) { i = end; continue; }
        uint8_t res[4] = {};
        if (!IsFetch(opc)) {
            for (int cc = 0; cc < 4; ++cc) {
                uint8_t u = 0;
                for (int k = nDest; k < no; ++k) {
                    if (IsDot(opc)) { for (int c2 = 0; c2 < 4; ++c2) u |= srcT[k][c2]; }
                    else u |= srcT[k][cc];
                }
                res[cc] = u;
            }
            if (opc == 14u && no == 3 && ops[1].type == kOpInput && ops[2].type == kOpInput && ops[1].hasIdx0 && ops[2].hasIdx0 &&
                ops[1].idx0 == ops[2].idx0) {                    // div rX, vN.?, vN.? = a perspective divide of an interpolant
                for (int cc = 0; cc < 4; ++cc) res[cc] |= kP;
            }
            if (opc == 56u || opc == 50u) {                      // mul / mad: P x (cbuffer value) -> a pixel position
                for (int cc = 0; cc < 4; ++cc) {
                    bool p = false, cb = false;
                    for (int k = 1; k < no && k < 3; ++k) {      // the two factors (mad's addend is operand 3)
                        if (srcT[k][cc] & (kP | kS)) p = true;
                        if (srcCb[k] || (srcT[k][cc] & kC)) cb = true;
                    }
                    if (p && cb) res[cc] |= kS;
                }
            }
            if (opc == 27u || opc == 28u) {                      // ftoi / ftou of a pixel position
                for (int cc = 0; cc < 4; ++cc) if (res[cc] & kS) res[cc] |= kI;
            }
            if (opc == 1u && no == 3) {                          // and x, (integer pixel), l(1) -> the parity
                for (int cc = 0; cc < 4; ++cc) {
                    if (((srcT[1][cc] & kI) && srcOne[2][cc]) || ((srcT[2][cc] & kI) && srcOne[1][cc])) {
                        res[cc] |= kPar;
                        parity = true;
                    }
                }
            }
        }
        for (int d = 0; d < nDest; ++d) {
            const Opnd& o = ops[d];
            if (o.type != kOpTemp || !o.hasIdx0 || o.idx0 >= (uint32_t)kMaxTemps) continue;
            const uint32_t mask = o.selMode == 0 ? o.sel : 0xFu;
            for (int cc = 0; cc < 4; ++cc) if (mask & (1u << cc)) taint[o.idx0][cc] = res[cc];
        }
        i = end;
    }
    r.parsed = true;
    if (parity && discardPar) r.rule = kRuleCheckerWrite;
    else if (parity && ldPar) r.rule = kRuleCheckerRead;
    else if (anyCode && onlyImmMovs && intImm && nInputs == 0 && nRes == 0 && nOutputs >= 4) r.rule = kRuleFlag;
    else if (r.known >= 0) r.rule = kRuleHash;
    return r;
}

// "c369f136-109f6918-1886484d-8c1e6ede" (RenderDoc's form); buf >= 36 chars
inline void SumText(const uint32_t s[4], char* buf, size_t n) {
    static const char* const hx = "0123456789abcdef";
    size_t p = 0;
    for (int i = 0; i < 4 && p + 9 < n; ++i) {
        for (int k = 7; k >= 0; --k) buf[p++] = hx[(s[i] >> (4 * k)) & 0xFu];
        if (i < 3) buf[p++] = '-';
    }
    if (p < n) buf[p] = 0; else if (n) buf[n - 1] = 0;
}

// phase 98 (Fable audit): an audited checksum is honoured even when the parse gives up early (ClassifyImpl's early returns
// left kRuleNone although `known` was set).
inline Result Classify(const void* code, size_t len) {
    Result r = ClassifyImpl(code, len);
    if (r.rule == kRuleNone && r.known >= 0) r.rule = kRuleHash;
    return r;
}

}  // namespace WaterPs
