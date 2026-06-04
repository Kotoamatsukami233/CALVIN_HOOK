#include "assembler.h"
#include "logging.h"
#include <cstdlib>

namespace arm64 {

// ============================================================
// CodeBuffer
// ============================================================

CodeBuffer::CodeBuffer() {
    capacity = 256;
    data = static_cast<uint8_t *>(malloc(capacity));
    size = 0;
}

CodeBuffer::~CodeBuffer() {
    free(data);
}

CodeBuffer::CodeBuffer(CodeBuffer &&other) noexcept
    : data(other.data), size(other.size), capacity(other.capacity) {
    other.data = nullptr;
    other.size = 0;
    other.capacity = 0;
}

CodeBuffer &CodeBuffer::operator=(CodeBuffer &&other) noexcept {
    if (this != &other) {
        free(data);
        data = other.data;
        size = other.size;
        capacity = other.capacity;
        other.data = nullptr;
        other.size = 0;
        other.capacity = 0;
    }
    return *this;
}

void CodeBuffer::EnsureCapacity(uint32_t needed) {
    if (size + needed <= capacity) return;
    while (size + needed > capacity) {
        capacity *= 2;
    }
    data = static_cast<uint8_t *>(realloc(data, capacity));
}

void CodeBuffer::EmitU32(uint32_t val) {
    EnsureCapacity(4);
    memcpy(data + size, &val, 4);
    size += 4;
}

void CodeBuffer::EmitU64(uint64_t val) {
    EnsureCapacity(8);
    memcpy(data + size, &val, 8);
    size += 8;
}

void CodeBuffer::EmitBytes(const void *src, uint32_t len) {
    EnsureCapacity(len);
    memcpy(data + size, src, len);
    size += len;
}

// ============================================================
// Assembler
// ============================================================

Assembler::Assembler() = default;

void Assembler::EmitB(int64_t offset) {
    uint32_t inst = kOpB | EncodeImm26(offset);
    buf_.EmitU32(inst);
}

void Assembler::EmitBR(int rn) {
    uint32_t inst = kOpBR | (rn << 5);
    buf_.EmitU32(inst);
}

void Assembler::EmitBLR(int rn) {
    uint32_t inst = kOpBLR | (rn << 5);
    buf_.EmitU32(inst);
}

uint32_t Assembler::EmitLdrX17Placeholder() {
    uint32_t offset = buf_.CurrentOffset();
    // LDR X17, [PC, #0] — placeholder with imm19=0
    // Encoding: opc=01 (X reg), V=0, imm19=0, Rt=17
    // 0x58000000 | (0 << 5) | 17 = 0x58000011
    uint32_t inst = 0x58000011;
    buf_.EmitU32(inst);
    return offset;
}

void Assembler::EmitLdrReg(int rt, int rn) {
    // LDR Xt, [Xn, #0] — unsigned offset, size=11 (64-bit), opc=01
    // Encoding: 0xF9400000 | (0 << 10) | (rn << 5) | rt
    uint32_t inst = 0xF9400200 | (rn << 5) | rt;
    buf_.EmitU32(inst);
}

void Assembler::EmitNop() {
    buf_.EmitU32(kOpNOP);
}

void Assembler::EmitInst(uint32_t inst) {
    buf_.EmitU32(inst);
}

void Assembler::EmitMovZ(int rd, uint16_t imm, int shift) {
    uint32_t hw = (shift / 16) & 0x3;
    uint32_t inst = kOpMovZ | (hw << 21) | (static_cast<uint32_t>(imm) << 5) | rd;
    buf_.EmitU32(inst);
}

void Assembler::EmitMovK(int rd, uint16_t imm, int shift) {
    uint32_t hw = (shift / 16) & 0x3;
    uint32_t inst = kOpMovK | (hw << 21) | (static_cast<uint32_t>(imm) << 5) | rd;
    buf_.EmitU32(inst);
}

void Assembler::EmitMovImm64(int rd, uint64_t val) {
    EmitMovZ(rd, static_cast<uint16_t>(val & 0xFFFF), 0);
    EmitMovK(rd, static_cast<uint16_t>((val >> 16) & 0xFFFF), 16);
    EmitMovK(rd, static_cast<uint16_t>((val >> 32) & 0xFFFF), 32);
    EmitMovK(rd, static_cast<uint16_t>((val >> 48) & 0xFFFF), 48);
}

void Assembler::EmitAdrp(int rd, int64_t offset) {
    uint32_t inst = kOpADRP | EncodeAdrpImm(offset) | rd;
    buf_.EmitU32(inst);
}

void Assembler::EmitAddImm(int rd, int rn, uint32_t imm) {
    // ADD Xd, Xn, #imm12  (shift=0)
    uint32_t inst = 0x91000000 | (imm << 10) | (rn << 5) | rd;
    buf_.EmitU32(inst);
}

void Assembler::PlaceDataLabel(uint64_t value, uint32_t ldr_code_offset) {
    DataLabel label;
    label.code_offset = ldr_code_offset;
    label.data_offset = 0;
    label.value = value;
    labels_.push_back(label);
}

void Assembler::Finalize() {
    uint32_t code_size = buf_.CurrentOffset();

    // Align data to 8 bytes
    uint32_t aligned_size = (code_size + 7) & ~7u;
    while (buf_.CurrentOffset() < aligned_size) {
        EmitNop();
    }

    // Place each data label and patch its LDR instruction
    for (auto &label : labels_) {
        label.data_offset = buf_.CurrentOffset();
        buf_.EmitU64(label.value);

        // Patch the LDR instruction: imm19 = (data_offset - code_offset) / 4
        int64_t disp = static_cast<int64_t>(label.data_offset) - static_cast<int64_t>(label.code_offset);
        int32_t imm19 = static_cast<int32_t>(disp / 4);

        uint32_t *inst_ptr = reinterpret_cast<uint32_t *>(buf_.data + label.code_offset);
        uint32_t imm19_bits = (imm19 & 0x7FFFF) << 5;
        *inst_ptr = (*inst_ptr & ~0x00FFFFE0) | imm19_bits;
    }

    labels_.clear();
}

void Assembler::Reset() {
    buf_.size = 0;
    labels_.clear();
}

void Assembler::EmitLdrX17Branch(uint64_t target_addr) {
    uint32_t ldr_offset = EmitLdrX17Placeholder();
    EmitBR(kRegX17);
    PlaceDataLabel(target_addr, ldr_offset);
}

void Assembler::EmitLdrLRBranch(uint64_t target_addr) {
    uint32_t ldr_offset = EmitLdrX17Placeholder();

    // MOV LR, X17  — we loaded into X17, now move to LR
    uint32_t mov_inst = 0xAA0003E0 | (kRegX17 << 16) | kRegLR;
    buf_.EmitU32(mov_inst);

    // BLR LR
    EmitBLR(kRegLR);
    PlaceDataLabel(target_addr, ldr_offset);
}

}  // namespace arm64
