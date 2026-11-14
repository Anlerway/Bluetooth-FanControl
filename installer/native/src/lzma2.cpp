// lzma2.cpp -- self-contained raw LZMA2 decoder (no xz container, no CRC layer).
//
// Implemented from the LZMA2 / LZMA format specification only; no third-party
// code is used.  The whole output is kept in one contiguous std::vector, so a
// match simply copies from an earlier offset of that same buffer -- there is no
// ring buffer and no dictionary window bookkeeping beyond "distance <= bytes
// decoded since the last dictionary reset and distance <= dictionary size".
//
// Reference points of the format (kept here for reviewers):
//   control byte   0x00                end of stream
//                  0x01                uncompressed chunk, resets dictionary
//                  0x02                uncompressed chunk
//                  0x80..0x9F          LZMA chunk, no reset
//                  0xA0..0xBF          LZMA chunk, reset state
//                  0xC0..0xDF          LZMA chunk, reset state, new properties
//                  0xE0..0xFF          LZMA chunk, reset dictionary/state/props
//   LZMA chunk header: control, u16be (uncompressed size - 1), u16be (compressed
//   size - 1), [properties byte], payload.  The five low bits of the control
//   byte are the bits 16..20 of (uncompressed size - 1), i.e. a chunk decodes at
//   most 2 MiB.  A dictionary reset (0x01 or >= 0xE0) also forces the next LZMA
//   chunk to carry new properties.

#include "lzma2.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace fc {
namespace {

// ---------------------------------------------------------------------------
// Constants of the LZMA bit model (see the LZMA specification)
// ---------------------------------------------------------------------------
constexpr uint32_t kNumStates          = 12;
constexpr uint32_t kNumPosStatesMax    = 16;   // 1 << pb, pb <= 4
constexpr uint32_t kNumLenToPosStates  = 4;
constexpr uint32_t kNumPosSlotBits     = 6;
constexpr uint32_t kNumAlignBits       = 4;
constexpr uint32_t kAlignSize          = 1u << kNumAlignBits;
constexpr uint32_t kStartPosModelIndex = 4;
constexpr uint32_t kEndPosModelIndex   = 14;
constexpr uint32_t kNumFullDistances   = 1u << (kEndPosModelIndex >> 1);  // 128
constexpr uint32_t kNumSpecPos         = kNumFullDistances - kEndPosModelIndex;  // 114
constexpr uint32_t kNumLitStates       = 7;
constexpr uint32_t kLenLowSymbols      = 8;
constexpr uint32_t kLenMidSymbols      = 8;
constexpr uint32_t kLenHighSymbols     = 256;
constexpr uint32_t kMatchMinLen        = 2;
constexpr uint32_t kMatchMaxLen        = kMatchMinLen + kLenLowSymbols + kLenMidSymbols + kLenHighSymbols - 1;  // 273
constexpr uint32_t kLiteralCoderSize   = 0x300;
constexpr uint32_t kBitModelTotalBits  = 11;
constexpr uint32_t kBitModelTotal      = 1u << kBitModelTotalBits;  // 2048
constexpr uint32_t kNumMoveBits        = 5;
constexpr uint32_t kTopValue           = 1u << 24;
constexpr uint16_t kProbInit           = static_cast<uint16_t>(kBitModelTotal >> 1);  // 1024

constexpr uint32_t kMaxPropsByte       = (4 * 5 + 4) * 9 + 8;  // 224, largest legal lc/lp/pb byte

// ---------------------------------------------------------------------------
// Range decoder (carry-less binary arithmetic decoder of LZMA)
// ---------------------------------------------------------------------------
struct RangeDecoder
{
    const uint8_t* cur   = nullptr;
    const uint8_t* limit = nullptr;
    uint32_t       range = 0xFFFFFFFFu;
    uint32_t       code  = 0;
    bool           overrun = false;

    // Reads the 5 initialization bytes; the first one must be zero.
    bool Init(const uint8_t* begin, const uint8_t* end)
    {
        cur     = begin;
        limit   = end;
        range   = 0xFFFFFFFFu;
        code    = 0;
        overrun = false;
        if (end - begin < 5 || begin[0] != 0)
            return false;
        for (int i = 1; i < 5; ++i)
            code = (code << 8) | begin[i];
        cur = begin + 5;
        return true;
    }

    inline uint8_t NextByte()
    {
        if (cur < limit)
            return *cur++;
        overrun = true;   // input truncated inside a chunk
        return 0;
    }

    // Decodes one bit; prob is updated in place.
    inline uint32_t Bit(uint16_t& prob)
    {
        const uint32_t bound = (range >> kBitModelTotalBits) * prob;
        uint32_t bit;
        if (code < bound) {
            range = bound;
            prob  = static_cast<uint16_t>(prob + ((kBitModelTotal - prob) >> kNumMoveBits));
            bit   = 0;
        } else {
            code -= bound;
            range -= bound;
            prob  = static_cast<uint16_t>(prob - (prob >> kNumMoveBits));
            bit   = 1;
        }
        if (range < kTopValue) {
            range <<= 8;
            code = (code << 8) | NextByte();
        }
        return bit;
    }

    // Fixed size bit tree, probs indexed 1 .. (1<<numBits)-1.
    inline uint32_t Tree(uint16_t* probs, int numBits)
    {
        uint32_t m = 1;
        for (int i = 0; i < numBits; ++i)
            m = (m << 1) | Bit(probs[m]);
        return m - (1u << numBits);
    }

    // Bit tree decoded LSB first.  "base" shifts the whole block: distSpec is
    // addressed with an offset of dist - posSlot - 1 (which is -1 for slot 4)
    // and the tree itself starts at index 1, so no access goes below base + 1.
    inline uint32_t ReverseTree(uint16_t* probs, int base, int numBits)
    {
        uint32_t m = 1, sym = 0;
        for (int i = 0; i < numBits; ++i) {
            const uint32_t b = Bit(probs[base + static_cast<int>(m)]);
            m   = (m << 1) | b;
            sym |= b << i;
        }
        return sym;
    }

    // Direct bits (no probability model).
    inline uint32_t DirectBits(uint32_t numBits)
    {
        uint32_t res = 0;
        for (uint32_t i = 0; i < numBits; ++i) {
            range >>= 1;
            code -= range;
            const uint32_t t = 0u - (code >> 31);   // 0 when bit is 1, ~0 when 0
            code += range & t;
            if (range < kTopValue) {
                range <<= 8;
                code = (code << 8) | NextByte();
            }
            res = (res << 1) | (t + 1);
        }
        return res;
    }
};

// ---------------------------------------------------------------------------
// Match length decoder (choice / choice2 / low[16][8] / mid[16][8] / high[256])
// ---------------------------------------------------------------------------
struct LenDecoder
{
    uint16_t choice;
    uint16_t choice2;
    uint16_t low[kNumPosStatesMax][kLenLowSymbols];
    uint16_t mid[kNumPosStatesMax][kLenMidSymbols];
    uint16_t high[kLenHighSymbols];
};

template <typename T>
inline void ResetProbTable(T& table)
{
    uint16_t* p  = reinterpret_cast<uint16_t*>(&table);
    const size_t n = sizeof(table) / sizeof(uint16_t);
    for (size_t i = 0; i < n; ++i)
        p[i] = kProbInit;
}

// ---------------------------------------------------------------------------
// LZMA state: probabilities plus the persistent symbol state
// ---------------------------------------------------------------------------
struct LzmaState
{
    std::vector<uint16_t> literal;                               // 0x300 << (lc + lp)

    uint16_t isMatch[kNumStates][kNumPosStatesMax];
    uint16_t isRep[kNumStates];
    uint16_t isRep0[kNumStates];
    uint16_t isRep1[kNumStates];
    uint16_t isRep2[kNumStates];
    uint16_t isRep0Long[kNumStates][kNumPosStatesMax];
    uint16_t distSlot[kNumLenToPosStates][1u << kNumPosSlotBits];
    uint16_t distSpec[kNumSpecPos];
    uint16_t distAlign[kAlignSize];
    LenDecoder matchLen;
    LenDecoder repLen;

    uint32_t lc = 3, lp = 0, pb = 2;
    uint32_t state = 0;
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;

    // "Reset state": probabilities, state and the four rep distances.
    void ResetState()
    {
        const size_t litCount = static_cast<size_t>(kLiteralCoderSize) << (lc + lp);
        if (literal.size() != litCount)
            literal.assign(litCount, kProbInit);
        else
            std::fill(literal.begin(), literal.end(), kProbInit);

        ResetProbTable(isMatch);
        ResetProbTable(isRep);
        ResetProbTable(isRep0);
        ResetProbTable(isRep1);
        ResetProbTable(isRep2);
        ResetProbTable(isRep0Long);
        ResetProbTable(distSlot);
        ResetProbTable(distSpec);
        ResetProbTable(distAlign);
        ResetProbTable(matchLen);
        ResetProbTable(repLen);

        state = 0;
        rep0 = rep1 = rep2 = rep3 = 0;
    }
};

// Length is coded as 2..273.
inline uint32_t DecodeLen(RangeDecoder& rc, LenDecoder& ld, uint32_t posState)
{
    if (rc.Bit(ld.choice) == 0)
        return kMatchMinLen + rc.Tree(ld.low[posState], 3);
    if (rc.Bit(ld.choice2) == 0)
        return kMatchMinLen + kLenLowSymbols + rc.Tree(ld.mid[posState], 3);
    return kMatchMinLen + kLenLowSymbols + kLenMidSymbols + rc.Tree(ld.high, 8);
}

// Decodes one LZMA chunk body (exactly outEnd - outPos bytes) into outBuf.
bool DecodeLzmaChunk(LzmaState& st, RangeDecoder& rc, uint8_t* outBuf, size_t& outPos,
                     size_t outEnd, size_t dictStart, uint64_t dictSize, std::string* error)
{
    const uint32_t pbMask  = (1u << st.pb) - 1;
    const uint32_t lpMask  = (1u << st.lp) - 1;
    const uint32_t lc      = st.lc;
    const uint32_t lcShift = 8 - lc;
    uint16_t* const literal = st.literal.data();

    uint32_t state = st.state;
    uint32_t rep0 = st.rep0, rep1 = st.rep1, rep2 = st.rep2, rep3 = st.rep3;

    // Copies "len" bytes that start "dist + 1" bytes before outPos.
    auto copyMatch = [&](uint32_t dist, uint32_t len) -> bool {
        const uint64_t distance = static_cast<uint64_t>(dist) + 1u;
        const size_t   relPos   = outPos - dictStart;
        if (distance > static_cast<uint64_t>(relPos) || distance > dictSize) {
            if (error)
                *error = "lzma2: match distance out of range at pos " + std::to_string(outPos) +
                         " (distance " + std::to_string(distance) + ", available " +
                         std::to_string(relPos) + ", dictionary " + std::to_string(dictSize) + ")";
            return false;
        }
        if (static_cast<size_t>(len) > outEnd - outPos) {
            if (error)
                *error = "lzma2: match overruns chunk output at pos " + std::to_string(outPos) +
                         " (length " + std::to_string(len) + ", chunk ends at " +
                         std::to_string(outEnd) + ")";
            return false;
        }

        uint8_t* dst = outBuf + outPos;
        if (distance >= len) {
            std::memcpy(dst, dst - distance, len);
        } else if (distance == 1) {
            std::memset(dst, dst[-1], len);
        } else {
            // Overlapping copy: grow the already produced pattern by doubling.
            size_t done = 0;
            size_t step = static_cast<size_t>(distance);
            while (done < len) {
                const size_t n = std::min(step, static_cast<size_t>(len) - done);
                std::memcpy(dst + done, dst + done - distance, n);
                done += n;
                step  = done;
            }
        }
        outPos += len;
        return true;
    };

    while (outPos < outEnd) {
        const size_t   relPos   = outPos - dictStart;
        const uint32_t posState = static_cast<uint32_t>(relPos) & pbMask;

        if (rc.Bit(st.isMatch[state][posState]) == 0) {
            // ---------------- literal ----------------
            const uint32_t prevByte = (relPos > 0) ? outBuf[outPos - 1] : 0u;
            const uint32_t litState =
                ((static_cast<uint32_t>(relPos) & lpMask) << lc) + (prevByte >> lcShift);
            uint16_t* probs  = literal + static_cast<size_t>(litState) * kLiteralCoderSize;
            uint32_t  symbol = 1;

            if (state < kNumLitStates) {
                do {
                    symbol = (symbol << 1) | rc.Bit(probs[symbol]);
                } while (symbol < 0x100);
            } else {
                if (static_cast<uint64_t>(rep0) + 1u > static_cast<uint64_t>(relPos)) {
                    if (error)
                        *error = "lzma2: literal match byte distance out of range at pos " +
                                 std::to_string(outPos) + " (distance " +
                                 std::to_string(static_cast<uint64_t>(rep0) + 1u) + ", available " +
                                 std::to_string(relPos) + ")";
                    return false;
                }
                uint32_t matchByte = outBuf[outPos - rep0 - 1];
                uint32_t offset    = 0x100;
                do {
                    matchByte <<= 1;
                    const uint32_t matchBit = matchByte & offset;
                    const uint32_t bit      = rc.Bit(probs[offset + matchBit + symbol]);
                    symbol = (symbol << 1) | bit;
                    if ((matchBit != 0) != (bit != 0))
                        offset = 0;   // first mismatch: continue with the plain tree
                } while (symbol < 0x100);
            }

            state = (state < 4) ? 0 : (state < 10 ? state - 3 : state - 6);
            outBuf[outPos++] = static_cast<uint8_t>(symbol);
            continue;
        }

        uint32_t len;
        if (rc.Bit(st.isRep[state]) == 0) {
            // ---------------- full match: length, then distance ----------------
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            state = (state < kNumLitStates) ? 7 : 10;

            len = DecodeLen(rc, st.matchLen, posState);

            const uint32_t lenToPosState =
                (len - kMatchMinLen < kNumLenToPosStates) ? (len - kMatchMinLen) : (kNumLenToPosStates - 1);
            const uint32_t posSlot = rc.Tree(st.distSlot[lenToPosState], kNumPosSlotBits);

            uint32_t dist;
            if (posSlot < kStartPosModelIndex) {
                dist = posSlot;
            } else {
                const uint32_t numDirectBits = (posSlot >> 1) - 1;
                dist = (2u | (posSlot & 1u)) << numDirectBits;
                if (posSlot < kEndPosModelIndex) {
                    // Distances 4..127: the low bits are a reverse bit tree
                    // whose probability block starts at dist - posSlot - 1.
                    dist += rc.ReverseTree(st.distSpec, static_cast<int>(dist) - static_cast<int>(posSlot) - 1,
                                           static_cast<int>(numDirectBits));
                } else {
                    dist += rc.DirectBits(numDirectBits - kNumAlignBits) << kNumAlignBits;
                    dist += rc.ReverseTree(st.distAlign, 0, static_cast<int>(kNumAlignBits));
                }
            }
            rep0 = dist;
        } else {
            // ---------------- repeated match ----------------
            if (rc.Bit(st.isRep0[state]) == 0) {
                if (rc.Bit(st.isRep0Long[state][posState]) == 0) {
                    // short rep: a single byte at rep0
                    state = (state < kNumLitStates) ? 9 : 11;
                    if (!copyMatch(rep0, 1))
                        return false;
                    continue;
                }
            } else {
                uint32_t dist;
                if (rc.Bit(st.isRep1[state]) == 0) {
                    dist = rep1;
                } else {
                    if (rc.Bit(st.isRep2[state]) == 0) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            state = (state < kNumLitStates) ? 8 : 11;
            len   = DecodeLen(rc, st.repLen, posState);
        }

        if (!copyMatch(rep0, len))
            return false;
    }

    st.state = state;
    st.rep0  = rep0;
    st.rep1  = rep1;
    st.rep2  = rep2;
    st.rep3  = rep3;
    return true;
}

}  // namespace

bool Lzma2Decode(const uint8_t* src, size_t srcLen, uint8_t dictProp,
                 std::vector<uint8_t>& out, uint64_t expectedSize, std::string* error)
{
    auto fail = [&](std::string msg) -> bool {
        if (error)
            *error = std::move(msg);
        return false;
    };

    if (src == nullptr && srcLen != 0)
        return fail("lzma2: null source pointer with non-zero length");
    if (dictProp > 40)
        return fail("lzma2: invalid dictionary property byte " + std::to_string(static_cast<unsigned>(dictProp)));

    const uint64_t dictSize =
        (dictProp == 40) ? 0xFFFFFFFFull
                         : (static_cast<uint64_t>(2u | (dictProp & 1u)) << (dictProp / 2 + 11));

    const bool knownSize = (expectedSize != 0);
    if (knownSize && expectedSize > static_cast<uint64_t>(static_cast<size_t>(-1)))
        return fail("lzma2: expected size does not fit into size_t");

    out.clear();
    if (knownSize) {
        try {
            out.resize(static_cast<size_t>(expectedSize));
        } catch (const std::bad_alloc&) {
            return fail("lzma2: out of memory reserving " + std::to_string(expectedSize) + " bytes");
        }
    }

    LzmaState st;
    st.lc = 3;
    st.lp = 0;
    st.pb = 2;
    st.ResetState();

    size_t srcPos        = 0;
    size_t outPos        = 0;
    size_t dictStart     = 0;     // output offset of the last dictionary reset
    bool   needProps     = true;  // first LZMA chunk must carry properties
    bool   needDictReset = true;  // stream must start with a dictionary reset

    try {
        while (true) {
            if (srcPos >= srcLen)
                return fail("lzma2: unexpected end of input at offset " + std::to_string(srcPos) +
                            " (missing 0x00 end marker)");

            const uint8_t control = src[srcPos++];

            if (control == 0x00)
                break;   // end of stream

            if (control >= 0xE0 || control == 0x01) {
                needProps     = true;
                needDictReset = true;
            } else if (needDictReset) {
                return fail("lzma2: dictionary reset required before control byte 0x" +
                            std::to_string(static_cast<unsigned>(control)) + " at input offset " +
                            std::to_string(srcPos - 1));
            }

            if (needDictReset && (control >= 0x80 || control == 0x01)) {
                dictStart     = outPos;
                needDictReset = false;
            }

            if (control >= 0x80) {
                // ---------------------------- LZMA chunk ----------------------------
                if (srcLen - srcPos < 4)
                    return fail("lzma2: truncated LZMA chunk header at input offset " +
                                std::to_string(srcPos));

                const uint32_t uncompressedSize =
                    (((static_cast<uint32_t>(control) & 0x1Fu) << 16) |
                     (static_cast<uint32_t>(src[srcPos]) << 8) |
                     static_cast<uint32_t>(src[srcPos + 1])) + 1u;
                const uint32_t compressedSize =
                    ((static_cast<uint32_t>(src[srcPos + 2]) << 8) |
                     static_cast<uint32_t>(src[srcPos + 3])) + 1u;
                srcPos += 4;

                if (control >= 0xC0) {
                    if (srcPos >= srcLen)
                        return fail("lzma2: truncated properties byte at input offset " +
                                    std::to_string(srcPos));
                    const uint8_t props = src[srcPos++];
                    if (props > kMaxPropsByte)
                        return fail("lzma2: invalid properties byte 0x" +
                                    std::to_string(static_cast<unsigned>(props)) + " at input offset " +
                                    std::to_string(srcPos - 1));
                    st.lc = props % 9;
                    st.lp = (props / 9) % 5;
                    st.pb = props / 45;
                    needProps = false;
                    st.ResetState();
                } else if (needProps) {
                    return fail("lzma2: LZMA chunk without properties at input offset " +
                                std::to_string(srcPos - 5) +
                                " (a dictionary reset requires new properties)");
                } else if (control >= 0xA0) {
                    st.ResetState();
                }

                if (knownSize) {
                    if (static_cast<uint64_t>(outPos) + uncompressedSize > out.size())
                        return fail("lzma2: output exceeds the expected size at pos " +
                                    std::to_string(outPos) + " (chunk of " +
                                    std::to_string(uncompressedSize) + " bytes)");
                } else {
                    out.resize(outPos + uncompressedSize);
                }

                if (srcLen - srcPos < compressedSize)
                    return fail("lzma2: truncated LZMA chunk data at input offset " +
                                std::to_string(srcPos) + " (need " +
                                std::to_string(compressedSize) + " bytes, have " +
                                std::to_string(srcLen - srcPos) + ")");

                const uint8_t* chunkBegin = src + srcPos;
                const uint8_t* chunkEnd   = chunkBegin + compressedSize;
                // The range decoder is allowed to peek one byte past the chunk;
                // the next chunk header starts at chunkBegin + compressedSize anyway.
                const uint8_t* rcLimit = (chunkEnd < src + srcLen) ? chunkEnd + 1 : chunkEnd;

                RangeDecoder rc;
                if (!rc.Init(chunkBegin, rcLimit))
                    return fail("lzma2: bad range coder initialization at input offset " +
                                std::to_string(srcPos) + " (first byte must be 0)");

                if (!DecodeLzmaChunk(st, rc, out.data(), outPos, outPos + uncompressedSize,
                                     dictStart, dictSize, error))
                    return false;

                const size_t used = static_cast<size_t>(rc.cur - chunkBegin);
                if (rc.overrun || used > compressedSize)
                    return fail("lzma2: LZMA chunk ran past its compressed size at input offset " +
                                std::to_string(srcPos) + " (used " + std::to_string(used) +
                                " of " + std::to_string(compressedSize) + ")");
                if (used != compressedSize)
                    return fail("lzma2: LZMA chunk compressed size mismatch at input offset " +
                                std::to_string(srcPos) + " (decoder consumed " +
                                std::to_string(used) + " bytes, header declares " +
                                std::to_string(compressedSize) + ")");

                srcPos += compressedSize;
            } else {
                // ------------------------ uncompressed chunk ------------------------
                if (control > 2)
                    return fail("lzma2: unknown control byte 0x" +
                                std::to_string(static_cast<unsigned>(control)) + " at input offset " +
                                std::to_string(srcPos - 1));

                if (srcLen - srcPos < 2)
                    return fail("lzma2: truncated uncompressed chunk header at input offset " +
                                std::to_string(srcPos));

                const uint32_t size = ((static_cast<uint32_t>(src[srcPos]) << 8) |
                                       static_cast<uint32_t>(src[srcPos + 1])) + 1u;
                srcPos += 2;

                if (srcLen - srcPos < size)
                    return fail("lzma2: truncated uncompressed chunk data at input offset " +
                                std::to_string(srcPos) + " (need " + std::to_string(size) +
                                " bytes, have " + std::to_string(srcLen - srcPos) + ")");

                if (knownSize) {
                    if (static_cast<uint64_t>(outPos) + size > out.size())
                        return fail("lzma2: output exceeds the expected size at pos " +
                                    std::to_string(outPos) + " (uncompressed chunk of " +
                                    std::to_string(size) + " bytes)");
                } else {
                    out.resize(outPos + size);
                }

                std::memcpy(out.data() + outPos, src + srcPos, size);
                outPos += size;
                srcPos += size;
            }
        }
    } catch (const std::bad_alloc&) {
        return fail("lzma2: out of memory while growing the output buffer");
    }

    out.resize(outPos);

    if (knownSize && outPos != static_cast<size_t>(expectedSize))
        return fail("lzma2: stream ended after " + std::to_string(outPos) + " bytes, expected " +
                    std::to_string(expectedSize));

    return true;
}

}  // namespace fc
