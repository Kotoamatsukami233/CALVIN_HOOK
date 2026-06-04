#pragma once

#include <cstdint>
#include <cstddef>

namespace arm64 {

// --- Bit manipulation helpers ---

inline uint32_t Bits(uint32_t val, int hi, int lo) {
    int width = hi - lo + 1;
    return (val >> lo) & ((1u << width) - 1);
}

inline bool Bit(uint32_t val, int n) {
    return (val >> n) & 1;
}

inline uint32_t SetBits(uint32_t val, int hi, int lo, uint32_t bits) {
    int width = hi - lo + 1;
    uint32_t mask = ((1u << width) - 1) << lo;
    return (val & ~mask) | ((bits & ((1u << width) - 1)) << lo);
}

inline int64_t SignExtend(uint64_t val, int bits) {
    uint64_t sign_bit = 1ull << (bits - 1);
    uint64_t mask = (1ull << bits) - 1;
    val &= mask;
    if (val & sign_bit) {
        val |= ~mask;
    }
    return static_cast<int64_t>(val);
}

// --- Instruction masks ---

// Top-level encoding groups (bits 28:25)
constexpr uint32_t kMaskReserved  = 0x1F000000;  // 00xx
constexpr uint32_t kMaskUncondB   = 0x7C000000;  // 00101xx
constexpr uint32_t kMaskCondB     = 0xFF000010;  // 0101010x cond
constexpr uint32_t kMaskCompB     = 0x7E000000;  // 011xx0x
constexpr uint32_t kMaskTestB     = 0x7E000000;  // 011x1x0
constexpr uint32_t kMaskLdrLit    = 0x3B000000;  // 00011x0
constexpr uint32_t kMaskAdr       = 0x7F000000;  // 0001x00

// Specific opcodes
constexpr uint32_t kOpB           = 0x14000000;
constexpr uint32_t kMaskOpB       = 0xFC000000;  // bits 31:26 = 000101x

constexpr uint32_t kOpBL          = 0x94000000;
constexpr uint32_t kMaskOpBL      = 0xFC000000;

constexpr uint32_t kOpBCond       = 0x54000000;
constexpr uint32_t kMaskOpBCond   = 0xFF000010;

constexpr uint32_t kOpCBZ         = 0x34000000;
constexpr uint32_t kOpCBNZ        = 0x35000000;
constexpr uint32_t kMaskOpCompB   = 0x7E000000;

constexpr uint32_t kOpTBZ         = 0x36000000;
constexpr uint32_t kOpTBNZ        = 0x37000000;
constexpr uint32_t kMaskOpTestB   = 0x7E000000;

constexpr uint32_t kOpLdrLitW     = 0x18000000;  // LDR Wt, label (opc=00)
constexpr uint32_t kOpLdrLitX     = 0x58000000;  // LDR Xt, label (opc=01)
constexpr uint32_t kMaskOpLdrLit  = 0x3B000000;
constexpr uint32_t kValLdrLit    = 0x18000000;

constexpr uint32_t kOpADR        = 0x10000000;
constexpr uint32_t kMaskOpADR    = 0x9F000000;
constexpr uint32_t kValOpADR     = 0x10000000;

constexpr uint32_t kOpADRP       = 0x90000000;
constexpr uint32_t kMaskOpADRP   = 0x9F000000;
constexpr uint32_t kValOpADRP    = 0x90000000;

constexpr uint32_t kOpNOP        = 0xD503201F;
constexpr uint32_t kOpBR         = 0xD61F0000;
constexpr uint32_t kOpBLR        = 0xD63F0000;
constexpr uint32_t kOpRET        = 0xD65F0000;

constexpr uint32_t kOpMovZ       = 0xD2800000;  // MOVZ Xd, #imm16, LSL #shift
constexpr uint32_t kOpMovK       = 0xF2800000;  // MOVK Xd, #imm16, LSL #shift

// LDR (register) offset encoding
constexpr uint32_t kOpLdrXRegOff = 0xF8600800;  // LDR Xt, [Xn, Xm]

// STP/LDP (pre-index) for X regs
constexpr uint32_t kOpSTPXPre    = 0xA9800000;  // STP Xt1, Xt2, [Xn, #imm]!
constexpr uint32_t kOpLDPXPost   = 0xA8C00000;  // LDP Xt1, Xt2, [Xn], #imm

// Register IDs
constexpr int kRegX17 = 17;
constexpr int kRegX16 = 16;
constexpr int kRegLR  = 30;  // X30 = LR
constexpr int kRegSP  = 31;  // in load/store context

// B/BL imm26 range
constexpr int64_t kMaxBRange = (1LL << 27);  // +/-128MB

// ADRP range
constexpr int64_t kMaxAdrpRange = (1LL << 32);  // +/-4GB

// --- Immediate decode/encode helpers ---

// B/BL: 26-bit signed offset, in units of 4 bytes
inline int64_t DecodeImm26(uint32_t inst) {
    uint32_t imm26 = inst & 0x03FFFFFF;
    return SignExtend(imm26, 26) * 4;
}

inline uint32_t EncodeImm26(int64_t offset) {
    uint32_t imm26 = (offset / 4) & 0x03FFFFFF;
    return imm26;
}

// B.cond: 19-bit signed offset, in units of 4 bytes
inline int64_t DecodeImm19(uint32_t inst) {
    uint32_t imm19 = Bits(inst, 23, 5);
    return SignExtend(imm19, 19) * 4;
}

inline uint32_t EncodeImm19(int64_t offset) {
    uint32_t imm19 = ((offset / 4) & 0x7FFFF);
    return imm19 << 5;
}

// CBZ/CBNZ: 19-bit signed offset, in units of 4 bytes (same encoding position as B.cond)
inline int64_t DecodeCompImm19(uint32_t inst) {
    uint32_t imm19 = Bits(inst, 23, 5);
    return SignExtend(imm19, 19) * 4;
}

inline uint32_t EncodeCompImm19(int64_t offset) {
    uint32_t imm19 = ((offset / 4) & 0x7FFFF);
    return imm19 << 5;
}

// TBZ/TBNZ: 14-bit signed offset, in units of 4 bytes
inline int64_t DecodeImm14(uint32_t inst) {
    uint32_t imm14 = Bits(inst, 18, 5);
    return SignExtend(imm14, 14) * 4;
}

inline uint32_t EncodeImm14(int64_t offset) {
    uint32_t imm14 = ((offset / 4) & 0x3FFF);
    return imm14 << 5;
}

// LDR literal: 19-bit signed offset, in units of 4 bytes
inline int64_t DecodeLdrLitImm19(uint32_t inst) {
    uint32_t imm19 = Bits(inst, 23, 5);
    return SignExtend(imm19, 19) * 4;
}

inline uint32_t EncodeLdrLitImm19(int64_t offset) {
    uint32_t imm19 = ((offset / 4) & 0x7FFFF);
    return imm19 << 5;
}

// ADR: 21-bit signed offset (immhi:bits 23:5, immlo:bits 30:29)
inline int64_t DecodeAdrImm(uint32_t inst) {
    uint32_t immhi = Bits(inst, 23, 5);
    uint32_t immlo = Bits(inst, 30, 29);
    uint32_t imm21 = (immhi << 2) | immlo;
    return SignExtend(imm21, 21);
}

inline uint32_t EncodeAdrImm(int64_t offset) {
    uint64_t imm = static_cast<uint64_t>(offset) & 0x1FFFFF;
    uint32_t immlo = imm & 0x3;
    uint32_t immhi = (imm >> 2) & 0x7FFFF;
    return (immlo << 29) | (immhi << 5);
}

// ADRP: 21-bit signed offset, in units of 4KB pages
inline int64_t DecodeAdrpImm(uint32_t inst) {
    uint32_t immhi = Bits(inst, 23, 5);
    uint32_t immlo = Bits(inst, 30, 29);
    uint32_t imm21 = (immhi << 2) | immlo;
    return SignExtend(imm21, 21) * 4096;
}

inline uint32_t EncodeAdrpImm(int64_t offset) {
    int64_t page_offset = offset / 4096;
    uint64_t imm = static_cast<uint64_t>(page_offset) & 0x1FFFFF;
    uint32_t immlo = imm & 0x3;
    uint32_t immhi = (imm >> 2) & 0x7FFFF;
    return (immlo << 29) | (immhi << 5);
}

// --- Instruction classification ---

inline bool IsB(uint32_t inst) {
    return (inst & kMaskOpB) == kOpB;
}

inline bool IsBL(uint32_t inst) {
    return (inst & kMaskOpB) == kOpBL;
}

inline bool IsBCond(uint32_t inst) {
    return (inst & kMaskOpBCond) == kOpBCond;
}

inline bool IsCBZ(uint32_t inst) {
    return (inst & 0x7F000000) == kOpCBZ;
}

inline bool IsCBNZ(uint32_t inst) {
    return (inst & 0x7F000000) == kOpCBNZ;
}

inline bool IsTBZ(uint32_t inst) {
    return (inst & 0x7F000000) == kOpTBZ;
}

inline bool IsTBNZ(uint32_t inst) {
    return (inst & 0x7F000000) == kOpTBNZ;
}

inline bool IsLdrLiteral(uint32_t inst) {
    return (inst & 0x3B000000) == 0x18000000;
}

inline bool IsADR(uint32_t inst) {
    return (inst & 0x9F000000) == 0x10000000;
}

inline bool IsADRP(uint32_t inst) {
    return (inst & 0x9F000000) == 0x90000000;
}

// Get the condition code from B.cond (bits 3:0)
inline uint32_t CondFromBCond(uint32_t inst) {
    return inst & 0xF;
}

// Invert a condition code (flip bit 0)
inline uint32_t InvertCond(uint32_t cond) {
    return cond ^ 1;
}

// Get Rt field (bits 4:0) for load/store/branch instructions
inline uint32_t GetRt(uint32_t inst) {
    return Bits(inst, 4, 0);
}

// Get Rd field (bits 4:0) for data processing instructions
inline uint32_t GetRd(uint32_t inst) {
    return Bits(inst, 4, 0);
}

}  // namespace arm64
