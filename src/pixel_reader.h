// PixelReader -- phase 109a: the forward "pixel readers" read their G-buffer / lighting texel at the RASTERISED pixel under the
// phase-23 jitter overscan. Pure DXBC work (no D3D, any thread): the analysis of a game pixel shader and the patched twin.
// inject.cpp creates the twin at CreatePixelShader and binds it for the game's draws of a jittered world / mirror pass that run with
// the overscan viewport (PrOnDraw). Unit test: tests/pixel_reader_test.cpp (checksum + patch on the game's own bytecode; WARP: the
// twin = the game shader bit for bit at the game's W x H viewport, the twin under the overscan = the game at W x H).
//
// WHY (RenderDoc audits 2026-11-14 "water" + "causeway", PHASE109_FINDINGS.md section A): SCS's forward re-draws (road / water
// decals, lane markings, the long wire PS a5aa1c2d, the old SCS water 9a5fca98, ...) do not use SV_Position to find their pixel
// in the lighting / G-buffer targets. Every one of them computes
//     u   = vN.yz / vN.x                      (the interpolated clip position: the pixel's fraction ACROSS THE VIEWPORT)
//     pix = ftoi(u * cbA.xy * (cbB.xy / cbB.zw))   (cbA = (1, 1), cbB = (W, H, 1, 1) = the viewport size: measured in the
//                                                  captures for the 2880x2160 forward pass and the 256 / 512 probe passes)
//     ld t10 / t11 (lighting), t12..t15 (G-buffer) at pix
// With the game's W x H viewport shifted by s (|s| < 0.5) u * W = p + 0.5 - s and pix = p, the rasterised pixel. With the
// phase-23 (W + 1) x (H + 1) viewport u * W = (p + 0.5 - s) * W / (W + 1): beyond x = (0.5 - s) * W the read is the LEFT (upper)
// neighbour's texel -- on about half of the jitter phases mid-screen, always at the right / bottom. Thin lit edges (rails, poles,
// lines) change colour per frame; the old water reads rock normals at the shore.
//
// THE TWIN: the same bytecode with ONE operand changed per index: the source of each such `ftoi` becomes the SV_Position input
// register (v1 in all audited shaders: it is in every input signature, unused), swizzled so the x pixel comes from SV_Position.x
// and the y pixel from .y. SV_Position.xy = p + 0.5 (the render-target pixel centre, whatever the viewport), so ftoi = p: the
// texel the game would read at its own W x H viewport -- bit-exact with the game shader there (the test proves it on WARP), and
// the right texel under the overscan. When the shader never declared SV_Position, a 4-token `dcl_input_ps_siv linear
// noperspective vN.xy, position` is inserted (and the ISGN element's used-mask gets xy); the DXBC checksum is recomputed.
// Nothing else changes (the perspective divide and its multiplies stay as dead code).
//
// PROVEN SHAPE (anything else: no twin, the shader is left alone):
//   - a ps_4_0+ pixel shader whose checksum verifies, an ISGN with an SV_Position element in a register of its own;
//   - P: `div rX, vN.ab, vN.cc` -- numerators .y / .z of the same input register over its .x (axis 0 = .y, axis 1 = .z);
//   - S: P (or S) multiplied by / divided by a CONSTANT value (constant-buffer data, optionally combined with immediates);
//   - I: `ftoi` / `ftou` of S on both axes, no operand modifiers, a temp source; I may be copied with mov;
//   - every use of I is the address of an `ld` / `ld_ms` with address.x = axis 0 and address.y = axis 1;
//   - Census::ScanPs-style pixel loads all accounted for (no ld at an interpolated pixel the walk could not prove), no parity
//     (the phase-97 checkerboard shaders are drawn without the overscan already).
// ASSUMPTION (not checkable from the bytecode): cbA * cbB.xy / cbB.zw = the viewport size, i.e. the game reads the texel of the
// pixel it shades. True in every audited draw; inject.cpp swaps only when the game's viewport is the pass size (the G-buffer's)
// and starts at 0, 0.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "water_ps.h"
#include "census.h"

namespace PixelReader {

// ---- the DXBC checksum (a modified MD5 over bytes 20..end; the D3D runtime rejects a blob whose stored checksum is wrong) -----
namespace md5 {
inline uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
inline void Transform(uint32_t s[4], const uint8_t blk[64]) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14,
                               20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6,
                               10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t M[16];
    memcpy(M, blk, 64);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        const uint32_t t = d;
        d = c; c = b;
        b = b + Rol(a + f + K[i] + M[g], R[i]);
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}
}  // namespace md5

// out = the checksum of the blob (bytes 20..len); the stored one is bytes 4..19
inline void Checksum(const uint8_t* b, size_t len, uint32_t out[4]) {
    uint32_t s[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    const uint8_t* p = b + 20;
    const uint32_t size = (uint32_t)(len - 20), bits = size * 8u;
    const uint32_t full = size & ~63u, last = size - full;
    for (uint32_t i = 0; i < full; i += 64) md5::Transform(s, p + i);
    uint8_t blk[64];
    memset(blk, 0, 64);
    if (last >= 56) {
        memcpy(blk, p + full, last);
        blk[last] = 0x80;
        md5::Transform(s, blk);
        memset(blk, 0, 64);
        memcpy(blk, &bits, 4);
        const uint32_t w = (bits >> 2) | 1u;
        memcpy(blk + 60, &w, 4);
        md5::Transform(s, blk);
    } else {
        memcpy(blk, &bits, 4);
        if (last) memcpy(blk + 4, p + full, last);
        blk[4 + last] = 0x80;
        const uint32_t w = (bits >> 2) | 1u;
        memcpy(blk + 60, &w, 4);
        md5::Transform(s, blk);
    }
    for (int i = 0; i < 4; ++i) out[i] = s[i];
}
inline bool ChecksumOk(const void* code, size_t len) {
    if (!code || len < 32) return false;
    const uint8_t* b = static_cast<const uint8_t*>(code);
    if (WaterPs::Rd(b, 0) != 0x43425844u) return false;
    uint32_t c[4];
    Checksum(b, len, c);
    for (int i = 0; i < 4; ++i) if (c[i] != WaterPs::Rd(b, 4 + 4 * (size_t)i)) return false;
    return true;
}

// ---- the analysis + the twin ---------------------------------------------------------------------------------------------------
enum Why : int {
    kOk = 0,          // a twin was made
    kNone,            // no pixel load at an interpolated pixel: not a pixel reader (the usual case)
    kBadBlob,         // not a DXBC pixel shader / the checksum does not verify / a chunk does not parse
    kNoPosition,      // no SV_Position element in the input signature (or it shares its register)
    kParity,          // the pixel's parity is taken: a phase-97 checkerboard shader (water_ps handles it)
    kShape,           // a pixel load the walk could not prove (another index chain, a modifier, an index used elsewhere)
    kWhyCount
};
inline const char* WhyName(int w) {
    static const char* const k[kWhyCount] = { "twin made", "not a pixel reader", "bad / unparsed blob", "no SV_Position input",
                                              "parity (phase-97 checker shader)", "unproven index shape" };
    return (w >= 0 && w < kWhyCount) ? k[w] : "?";
}

struct Twin {
    int why = kNone;
    uint32_t sum[4] = {};          // the game shader's checksum
    std::vector<uint8_t> code;     // the twin's DXBC (empty unless why == kOk)
    int ftois = 0;                 // index conversions patched
    int lds = 0;                   // pixel loads that read through them
    int posReg = -1;               // SV_Position's input register
    bool dclAdded = false;         // the dcl_input_ps_siv was inserted (else it existed / its mask was widened)
    int cbA = -1, cbB = -1;        // cb0 rows of the index chain u * cbA.xy * cbB.xy / cbB.zw (the tests set the factor there)
    int scanLds = 0;               // Census::ScanPs' count of pixel loads (must equal lds)
    int pReg = -1;                 // the input register of the perspective divide (vN of vN.yz / vN.x)
    bool pRegOnlyDiv = false;      // ... and its value feeds nothing but the index chain (tests: the other interpolants may be held
                                   //     constant -- then the picture depends on the projected position only through the index)
    uint32_t patchTok[8] = {};     // token offsets (in SHEX, before an insertion) of the patched source operands
};

namespace pdetail {
using WaterPs::Rd;
inline void Wr(std::vector<uint8_t>& v, size_t off, uint32_t x) { memcpy(&v[off], &x, 4); }
constexpr uint8_t kP = 1, kS = 2, kI = 4, kK = 8, kImm = 16;   // perspective fraction / x constant / integer pixel / constant / immediate
struct Cell { uint8_t f = 0; int8_t axis = -1; int8_t reg = -1; };   // reg = the input register of the perspective divide
}  // namespace pdetail

// Analyse + patch. Pure function (any thread).
inline Twin Make(const void* code, size_t len) {
    using namespace WaterPs::detail;
    using namespace pdetail;
    Twin t;
    if (!code || len < 32 || len > (16u << 20)) { t.why = kBadBlob; return t; }
    const uint8_t* const b = static_cast<const uint8_t*>(code);
    if (Rd(b, 0) != 0x43425844u) { t.why = kBadBlob; return t; }
    for (int i = 0; i < 4; ++i) t.sum[i] = Rd(b, 4 + 4 * (size_t)i);
    if (Rd(b, 24) != len) { t.why = kBadBlob; return t; }
    const uint32_t nChunks = Rd(b, 28);
    if (nChunks > 64 || 32 + (size_t)nChunks * 4 > len) { t.why = kBadBlob; return t; }
    size_t shexOff = 0, isgnOff = 0;                                   // chunk header offsets
    for (uint32_t c = 0; c < nChunks; ++c) {
        const size_t off = Rd(b, 32 + (size_t)c * 4);
        if (off + 8 > len) { t.why = kBadBlob; return t; }
        const uint32_t fourcc = Rd(b, off), size = Rd(b, off + 4);
        if (off + 8 + size > len) { t.why = kBadBlob; return t; }
        if (fourcc == 0x52444853u || fourcc == 0x58454853u) shexOff = off;   // 'SHDR' / 'SHEX'
        if (fourcc == 0x4E475349u) isgnOff = off;                            // 'ISGN'
    }
    if (!shexOff) { t.why = kBadBlob; return t; }
    const size_t base = shexOff + 8;
    const uint32_t shexSize = Rd(b, shexOff + 4);
    if (shexSize < 8 || (Rd(b, base) >> 16) != 0) { t.why = kBadBlob; return t; }   // program type 0 = pixel shader
    if (((Rd(b, base) >> 4) & 0xFu) < 4u) { t.why = kBadBlob; return t; }           // ps_4_0 and up
    const uint32_t nTok = Rd(b, base + 4);
    if (nTok < 2 || nTok > shexSize / 4) { t.why = kBadBlob; return t; }

    // pass 1: the walk
    constexpr int kTemps = kMaxTemps;
    static thread_local Cell cell[kTemps][4];
    for (auto& r : cell) for (auto& c : r) c = Cell();
    int posDecl = -1;                                                  // token index of an existing dcl_input_ps_siv position
    uint32_t posDeclReg = 0xFFFFFFFFu;
    uint32_t lastInputDclEnd = 0, firstNonInputDcl = 0, firstCode = 0;
    struct Fix { uint32_t srcTok; uint32_t newSwz; uint32_t destMask; };
    Fix fixes[8];
    int nFix = 0;
    int provenLds = 0, anyPixelLd = 0;
    bool bad = false, parity = false;
    uint32_t inputReads = 0;                                           // input registers read outside the perspective divide
    bool pOther = false;                                               // the projected position feeds more than the index chain
    uint32_t divRegs = 0;                                              // input registers of perspective divides
    for (uint32_t i = 2; i < nTok;) {
        const uint32_t op = Rd(b, base + (size_t)i * 4), opc = op & 0x7FFu;
        uint32_t ilen = (op >> 24) & 0x7Fu;
        if (opc == 0x35u) {                                            // customdata: length in the next token
            if (i + 1 >= nTok) { t.why = kBadBlob; return t; }
            ilen = Rd(b, base + (size_t)(i + 1) * 4);
        }
        if (ilen == 0 || ilen > nTok - i) { t.why = kBadBlob; return t; }
        const uint32_t end = i + ilen;
        if (IsDecl(opc)) {
            if (opc >= 95u && opc <= 100u) {                           // dcl_input*
                Opnd o;
                const uint32_t j = ParseOpnd(b, base, i + 1, end, o);
                if (!j) { t.why = kBadBlob; return t; }
                if (opc == 100u && j < end && (Rd(b, base + (size_t)j * 4) & 0xFFFFu) == 1u && o.hasIdx0) {   // position
                    posDecl = (int)i; posDeclReg = o.idx0;
                }
                lastInputDclEnd = end;
            } else if (!firstNonInputDcl && opc >= 101u) {
                firstNonInputDcl = i;
            }
            i = end;
            continue;
        }
        if (!firstCode) firstCode = i;
        uint32_t j = i + 1;
        bool ext = (op >> 31) != 0;
        while (ext) {
            if (j >= end) { t.why = kBadBlob; return t; }
            ext = (Rd(b, base + (size_t)j * 4) >> 31) != 0;
            ++j;
        }
        Opnd ops[8];
        uint32_t opTok[8] = {};
        int no = 0;
        while (j < end && no < 8) {
            opTok[no] = j;
            j = ParseOpnd(b, base, j, end, ops[no]);
            if (!j) { t.why = kBadBlob; return t; }
            ++no;
        }
        if (j != end) { t.why = kBadBlob; return t; }
        const int nDest = NoDest(opc) ? 0 : (TwoDest(opc) ? 2 : 1);
        // the source cells per operand / destination component
        Cell src[8][4];
        bool srcExt[8] = {};
        for (int k = nDest; k < no; ++k) {
            const Opnd& o = ops[k];
            srcExt[k] = (Rd(b, base + (size_t)opTok[k] * 4) >> 31) != 0;
            for (int cc = 0; cc < 4; ++cc) {
                const int sc = o.comp(cc);
                Cell x;
                if (o.type == kOpTemp && o.hasIdx0 && o.idx0 < (uint32_t)kTemps) x = cell[o.idx0][sc];
                else if (o.type == kOpCb) x.f = kK;
                else if (o.type == kOpImm32 || o.type == kOpImm64) x.f = kImm;
                src[k][cc] = x;
            }
        }
        // a use of an integer pixel anywhere but an ld address / a mov = an unproven shape
        const bool isLd = (opc == 45u || opc == 46u) && no >= 3;
        const bool isMov = opc == 54u && no == 2 && !srcExt[1];
        for (int k = nDest; k < no; ++k) {
            if (isLd && k == 1) continue;
            if (isMov) continue;
            for (int cc = 0; cc < 4; ++cc) if (src[k][cc].f & kI) bad = true;
        }
        if (opc == 1u && no == 3) {                                    // and x, I, l(1): the parity (water_ps' domain)
            for (int k = 1; k < 3; ++k) for (int cc = 0; cc < 4; ++cc) if (src[k][cc].f & kI) parity = true;
        }
        if (isLd) {
            const Opnd& a = ops[1];
            const Cell cx = src[1][0], cy = src[1][1];
            const bool anyI = (cx.f | cy.f | src[1][2].f | src[1][3].f) & kI;
            if (anyI) {
                ++anyPixelLd;
                if ((cx.f & kI) && cx.axis == 0 && (cy.f & kI) && cy.axis == 1 && !srcExt[1] && a.type == kOpTemp) ++provenLds;
                else bad = true;
            }
        }
        bool srcPS = false;                                            // a perspective / scaled pixel position is read here
        for (int k = nDest; k < no; ++k) for (int cc = 0; cc < 4; ++cc) srcPS |= (src[k][cc].f & (kP | kS)) != 0;
        if (!nDest) { pOther |= srcPS; i = end; continue; }
        bool divInst = false;
        Cell res[4];
        const Opnd& d0 = ops[0];
        const uint32_t dmask = d0.selMode == 0 ? d0.sel : 0xFu;
        if (IsFetch(opc)) {
            pOther |= srcPS;                                           // e.g. a projective sample at u (not the index chain)
        } else if (opc == 14u && no == 3 && ops[1].type == kOpInput && ops[2].type == kOpInput && ops[1].hasIdx0 && ops[2].hasIdx0 &&
                   ops[1].idx0 == ops[2].idx0 && !srcExt[1] && !srcExt[2] && (int)ops[1].idx0 != (int)posDeclReg) {
            // div rX, vN.ab, vN.cc: a perspective divide (proven only for numerators .y / .z over .x)
            for (int cc = 0; cc < 4; ++cc) {
                const int nc = ops[1].comp(cc), dc = ops[2].comp(cc);
                if (dc == 0 && (nc == 1 || nc == 2) && ops[1].idx0 < 32) {
                    res[cc].f = kP; res[cc].axis = (int8_t)(nc - 1); res[cc].reg = (int8_t)ops[1].idx0;
                    divInst = true;
                }
            }
        } else if ((opc == 56u || opc == 14u) && no == 3) {            // mul / div
            for (int cc = 0; cc < 4; ++cc) {
                const Cell x = src[1][cc], y = src[2][cc];
                const bool xPS = (x.f & (kP | kS)) && !srcExt[1], yPS = (y.f & (kP | kS)) && !srcExt[2];
                const bool xK = (x.f & kK) && !(x.f & (kP | kS | kI)), yK = (y.f & kK) && !(y.f & (kP | kS | kI));
                const bool xKI = (x.f & (kK | kImm)) && !(x.f & (kP | kS | kI)), yKI = (y.f & (kK | kImm)) && !(y.f & (kP | kS | kI));
                if (xPS && yK) { res[cc].f = kS; res[cc].axis = x.axis; res[cc].reg = x.reg; }
                else if (opc == 56u && yPS && xK) { res[cc].f = kS; res[cc].axis = y.axis; res[cc].reg = y.reg; }
                else if (xKI && yKI) res[cc].f = kK;                   // constant arithmetic (1 / cb.zw, cb.xy * that)
                if (((x.f | y.f) & (kP | kS)) && !(res[cc].f & kS)) pOther = true;
            }
        } else if (opc == 54u && no == 2 && !srcExt[1]) {              // mov keeps everything
            for (int cc = 0; cc < 4; ++cc) res[cc] = src[1][cc];
        } else if ((opc == 27u || opc == 28u) && no == 2) {             // ftoi / ftou
            bool allS = true, anyS = false;
            for (int cc = 0; cc < 4; ++cc) {
                if (!(dmask & (1u << cc))) continue;
                const Cell x = src[1][cc];
                if ((x.f & kS) && x.axis >= 0) {
                    anyS = true; res[cc].f = kI; res[cc].axis = x.axis; res[cc].reg = x.reg;
                    if (t.pReg < 0) t.pReg = x.reg;
                    else if (t.pReg != x.reg) bad = true;              // index chains from two perspective registers: not audited
                } else allS = false;
            }
            if (anyS) {
                const Opnd& s = ops[1];
                const uint32_t tok = Rd(b, base + (size_t)opTok[1] * 4);
                const bool simple = allS && !srcExt[1] && s.type == kOpTemp && s.hasIdx0 && ((tok >> 20) & 3u) == 1u &&
                                    ((tok >> 22) & 7u) == 0u && (tok & 3u) == 2u && ((tok >> 2) & 3u) == 1u;
                if (!simple || nFix >= 8) bad = true;
                else {
                    uint32_t swz = 0;
                    for (int cc = 0; cc < 4; ++cc) swz |= (uint32_t)((dmask & (1u << cc)) ? res[cc].axis : 0) << (2 * cc);
                    fixes[nFix++] = { opTok[1], swz, dmask };
                }
            }
        } else {
            // any other op: constants stay constants, the pixel bits are dropped (a P / S / I through add, mad ... is not proven)
            pOther |= srcPS;
            for (int cc = 0; cc < 4; ++cc) {
                bool k = true, anyK = false;
                for (int s = nDest; s < no; ++s)
                    for (int c2 = 0; c2 < 4; ++c2) {
                        if (!IsDot(opc) && c2 != cc) continue;     // a dot product reads every component
                        const uint8_t f = src[s][c2].f;
                        if ((f & ~(kK | kImm)) || !(f & (kK | kImm))) k = false;
                        if (f & kK) anyK = true;
                    }
                if (k && anyK) res[cc].f = kK;
            }
        }
        // the constant-buffer rows of the index chain (the tests set the factor there): cbA = the row a P is multiplied by,
        // cbB = the row whose .zw an immediate is divided by (u * cbA.xy * cbB.xy / cbB.zw in every audited shader)
        if ((opc == 56u || opc == 14u) && no == 3) {
            auto cbRow = [&](int k) -> int {                   // cbN[row] with immediate indices: the row, else -1
                if (ops[k].type != kOpCb) return -1;
                const uint32_t tok = Rd(b, base + (size_t)opTok[k] * 4);
                uint32_t q = opTok[k] + 1;
                bool e = (tok >> 31) != 0;
                while (e) { e = (Rd(b, base + (size_t)q * 4) >> 31) != 0; ++q; }
                if (((tok >> 20) & 3u) != 2u || ((tok >> 22) & 7u) != 0u || ((tok >> 25) & 7u) != 0u) return -1;
                return (int)Rd(b, base + (size_t)(q + 1) * 4);
            };
            if (opc == 56u) {
                for (int k = 1; k < 3; ++k) {
                    const int row = cbRow(k);
                    if (row >= 0 && (src[3 - k][0].f & kP | src[3 - k][1].f & kP | src[3 - k][2].f & kP | src[3 - k][3].f & kP) && t.cbA < 0) t.cbA = row;
                }
            } else if (ops[1].type == kOpImm32) {
                const int row = cbRow(2);
                if (row >= 0 && t.cbB < 0) t.cbB = row;
            }
        }
        for (int d = 0; d < nDest; ++d) {
            const Opnd& o = ops[d];
            if (o.type != kOpTemp || !o.hasIdx0 || o.idx0 >= (uint32_t)kTemps) continue;
            const uint32_t mask = o.selMode == 0 ? o.sel : 0xFu;
            for (int cc = 0; cc < 4; ++cc) if (mask & (1u << cc)) cell[o.idx0][cc] = d == 0 ? res[cc] : Cell();
        }
        for (int k = nDest; k < no; ++k)                               // (the index chain's own divide is not "elsewhere")
            if (ops[k].type == kOpInput && ops[k].hasIdx0 && ops[k].idx0 < 32 && !divInst) inputReads |= 1u << ops[k].idx0;
            else if (divInst && ops[k].type == kOpInput && ops[k].hasIdx0 && ops[k].idx0 < 32) divRegs |= 1u << ops[k].idx0;
        i = end;
    }
    t.pRegOnlyDiv = t.pReg >= 0 && t.pReg < 32 && !(inputReads & (1u << t.pReg)) && !pOther && divRegs == (1u << t.pReg);
    if (!anyPixelLd && !nFix) { t.why = kNone; return t; }
    if (parity) { t.why = kParity; return t; }
    if (bad || !nFix || provenLds != anyPixelLd) { t.why = kShape; return t; }
    t.scanLds = Census::ScanPs(code, len).pixelLds;     // the census' wider taint walk must see exactly these loads
    if (t.scanLds != provenLds) { t.why = kShape; return t; }
    // the input signature: SV_Position's register, alone in it
    if (!isgnOff) { t.why = kNoPosition; return t; }
    const size_t sg = isgnOff + 8;
    const uint32_t sgSize = Rd(b, isgnOff + 4), nEl = Rd(b, sg);
    if (8 + (size_t)nEl * 24 > sgSize) { t.why = kBadBlob; return t; }
    int posEl = -1;
    uint32_t posReg = 0;
    for (uint32_t e = 0; e < nEl; ++e)
        if (Rd(b, sg + 8 + e * 24 + 8) == 1u) { posEl = (int)e; posReg = Rd(b, sg + 8 + e * 24 + 16); }
    if (posEl < 0) { t.why = kNoPosition; return t; }
    for (uint32_t e = 0; e < nEl; ++e)
        if ((int)e != posEl && Rd(b, sg + 8 + e * 24 + 16) == posReg) { t.why = kNoPosition; return t; }
    if (posDecl >= 0 && posDeclReg != posReg) { t.why = kShape; return t; }
    for (int f = 0; f < nFix; ++f) t.patchTok[f] = fixes[f].srcTok;
    if (!ChecksumOk(code, len)) { t.why = kBadBlob; return t; }

    // pass 2: the twin
    std::vector<uint8_t> v(b, b + len);
    for (int f = 0; f < nFix; ++f) {
        const size_t at = base + (size_t)fixes[f].srcTok * 4;
        uint32_t tok = Rd(b, at);
        tok &= ~((0xFFu << 4) | (0xFFu << 12));                        // swizzle, operand type
        tok |= (fixes[f].newSwz & 0xFFu) << 4;
        tok |= (uint32_t)kOpInput << 12;
        Wr(v, at, tok);
        Wr(v, at + 4, posReg);
    }
    const uint32_t kMaskXY = 0x3u;
    if (posDecl >= 0) {                                                // widen its mask to xy when needed
        const size_t at = base + ((size_t)posDecl + 1) * 4;
        const uint32_t tok = Rd(b, at);
        if (((tok >> 2) & 3u) == 0u) Wr(v, at, tok | (kMaskXY << 4));
        t.dclAdded = false;
    } else {                                                           // insert dcl_input_ps_siv linear noperspective vN.xy, position
        uint32_t insAt = 0;                                            // token index: before the first input dcl of a higher register
        for (uint32_t i = 2; i < (firstCode ? firstCode : nTok);) {
            const uint32_t op = Rd(b, base + (size_t)i * 4), opc = op & 0x7FFu;
            uint32_t ilen = (op >> 24) & 0x7Fu;
            if (opc == 0x35u) ilen = Rd(b, base + (size_t)(i + 1) * 4);
            if (opc >= 95u && opc <= 100u) {
                Opnd o;
                ParseOpnd(b, base, i + 1, i + ilen, o);
                if (o.hasIdx0 && o.idx0 > posReg) { insAt = i; break; }
            }
            i += ilen;
        }
        if (!insAt) insAt = lastInputDclEnd ? lastInputDclEnd : (firstNonInputDcl ? firstNonInputDcl : firstCode);
        if (!insAt) { t.why = kShape; return t; }
        const uint32_t dcl[4] = { 100u | (4u << 11) | (4u << 24),                          // dcl_input_ps_siv, linear noperspective
                                  2u | (kMaskXY << 4) | ((uint32_t)kOpInput << 12) | (1u << 20),   // vN.xy (mask mode, 1-D imm index)
                                  posReg, 1u };                                            // D3D_NAME_POSITION
        const size_t pos = base + (size_t)insAt * 4;
        v.insert(v.begin() + (ptrdiff_t)pos, reinterpret_cast<const uint8_t*>(dcl), reinterpret_cast<const uint8_t*>(dcl) + 16);
        Wr(v, base + 4, nTok + 4);                                     // the program's token count
        Wr(v, shexOff + 4, shexSize + 16);                             // the chunk size
        for (uint32_t c = 0; c < nChunks; ++c) {                       // later chunks move by 16 bytes
            const uint32_t off = Rd(b, 32 + (size_t)c * 4);
            if (off > shexOff) Wr(v, 32 + (size_t)c * 4, off + 16);
        }
        Wr(v, 24, (uint32_t)v.size());
        t.dclAdded = true;
    }
    // ISGN (before SHEX in fxc output; an ISGN after SHEX moved by 16): the element's used-mask gains xy
    {
        const size_t sgNow = isgnOff > shexOff && t.dclAdded ? sg + 16 : sg;
        const size_t at = sgNow + 8 + (size_t)posEl * 24 + 21;
        v[at] = (uint8_t)(v[at] | kMaskXY);
    }
    uint32_t c[4];
    Checksum(v.data(), v.size(), c);
    for (int i = 0; i < 4; ++i) Wr(v, 4 + 4 * (size_t)i, c[i]);
    t.code.swap(v);
    t.ftois = nFix;
    t.lds = provenLds;
    t.posReg = (int)posReg;
    t.why = kOk;
    return t;
}

}  // namespace PixelReader
