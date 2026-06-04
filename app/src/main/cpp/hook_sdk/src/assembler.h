#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "arm64_constants.h"

namespace arm64 {

// Growable buffer for emitting machine code.
struct CodeBuffer {
    uint8_t *data = nullptr;
    uint32_t size = 0;
    uint32_t capacity = 0;

    CodeBuffer();
    ~CodeBuffer();

    CodeBuffer(const CodeBuffer &) = delete;
    CodeBuffer &operator=(const CodeBuffer &) = delete;

    CodeBuffer(CodeBuffer &&other) noexcept;
    CodeBuffer &operator=(CodeBuffer &&other) noexcept;

    void EnsureCapacity(uint32_t needed);
    void EmitU32(uint32_t val);
    void EmitU64(uint64_t val);
    void EmitBytes(const void *src, uint32_t len);

    uint32_t CurrentOffset() const { return size; }
};

// Represents a data label: an 8-byte absolute address that will be placed
// after all code, with a back-reference to a LDR literal instruction that
// needs its imm19 patched.
struct DataLabel {
    uint32_t code_offset;    // offset of the LDR instruction referencing this label
    uint32_t data_offset;    // offset where the 8-byte data will be placed (filled by Finalize)
    uint64_t value;          // the absolute address to embed
};

// Minimal ARM64 instruction emitter.
class Assembler {
public:
    Assembler();
    ~Assembler() = default;

    // --- Instruction emitters ---

    // B offset26
    void EmitB(int64_t offset);

    // BR Xn
    void EmitBR(int rn);

    // BLR Xn
    void EmitBLR(int rn);

    // LDR X17, [PC, #forward]  — placeholder, patched by PlaceDataLabel
    // Returns the code offset of this instruction.
    uint32_t EmitLdrX17Placeholder();

    // LDR Xt, [X17]
    void EmitLdrReg(int rt, int rn);

    // NOP
    void EmitNop();

    // Emit a raw 32-bit instruction
    void EmitInst(uint32_t inst);

    // MOVZ Xd, #imm16, LSL #shift
    void EmitMovZ(int rd, uint16_t imm, int shift);

    // MOVK Xd, #imm16, LSL #shift
    void EmitMovK(int rd, uint16_t imm, int shift);

    // Emit a 4-instruction MOV sequence to load a 64-bit immediate into Xn
    void EmitMovImm64(int rd, uint64_t val);

    // ADRP Xd, page_offset
    void EmitAdrp(int rd, int64_t offset);

    // ADD Xd, Xn, #imm12
    void EmitAddImm(int rd, int rn, uint32_t imm);

    // --- Data label management ---

    // Place a data label: the LDR at ldr_code_offset will reference
    // this 8-byte value placed at the end of the buffer.
    void PlaceDataLabel(uint64_t value, uint32_t ldr_code_offset);

    // Finalize: append all data labels at the end and patch LDR instructions.
    void Finalize();

    // Get the final buffer (only valid after Finalize).
    CodeBuffer &Buffer() { return buf_; }

    // Reset for reuse.
    void Reset();

    // Build a LDR X17 + literal + BR X17 sequence with a data label.
    // Returns total bytes emitted (code only, before finalize).
    void EmitLdrX17Branch(uint64_t target_addr);

    // Build a LDR LR + literal + BLR LR sequence with a data label.
    void EmitLdrLRBranch(uint64_t target_addr);

private:
    CodeBuffer buf_;
    std::vector<DataLabel> labels_;
};

}  // namespace arm64
