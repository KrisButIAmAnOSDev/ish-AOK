#ifndef EMU_AVX_H
#define EMU_AVX_H

// Arch-independent AVX/AVX2/AVX-512 instruction semantics (GH #525).
//
// These operate purely on flat byte buffers holding vector values -- they know
// nothing about CPU state, the TLB, modrm, or which guest ABI is executing.
// That keeps exactly one implementation of each instruction's meaning, shared
// by every front-end that decodes VEX/EVEX:
//
//   amd64  emu/amd64_interp.c -- decodes and executes in the interpreter,
//          which the JIT already bails to for any opcode it can't translate
//   i386   emu/decode.h / jit/gen.c -- decodes at codegen time and calls
//          these through the vec-helper gadgets, since i386 is JIT-only and
//          has no interpreter to fall back on
//
// Buffers are always at least `vlen / 8` bytes. A `vlen` of 128/256/512 is
// the operation width; helpers that are lane-local (shuffles, packs, unpacks)
// iterate 128-bit lanes internally, and the ones that deliberately cross lanes
// (the permutes) say so at their definition.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "misc.h"
#include "emu/cpu.h"

// ---- lane helpers ----
//
// A "lane" here is one element (1/2/4/8 bytes). Values are moved through
// uint64_t regardless of width; avx_lane_mask/avx_lane_sext put them back
// into the right width with the right signedness.

static inline uint64_t avx_lane_get(const uint8_t *p, unsigned bytes) {
    uint64_t v = 0;
    memcpy(&v, p, bytes);
    return v;
}

static inline void avx_lane_put(uint8_t *p, unsigned bytes, uint64_t v) {
    memcpy(p, &v, bytes);
}

static inline uint64_t avx_lane_mask(unsigned bytes) {
    return bytes >= 8 ? ~UINT64_C(0) : (UINT64_C(1) << (bytes * 8)) - 1;
}

static inline int64_t avx_lane_sext(uint64_t v, unsigned bytes) {
    unsigned sh = 64 - bytes * 8;
    return ((int64_t) (v << sh)) >> sh;
}


enum avx_op {
    AVX_XOR, AVX_AND, AVX_ANDN, AVX_OR,
    AVX_ADD, AVX_SUB,
    AVX_ADDS, AVX_ADDUS, AVX_SUBS, AVX_SUBUS,
    AVX_CMPEQ, AVX_CMPGT,
    AVX_MINS, AVX_MINU, AVX_MAXS, AVX_MAXU,
    AVX_AVG, AVX_MULLO, AVX_MULHI, AVX_MULHIU,
};

enum avx_shift_kind { AVX_SHL, AVX_SHR, AVX_SAR };

enum avx_fp_op { AVX_FADD, AVX_FSUB, AVX_FMUL, AVX_FDIV, AVX_FMIN, AVX_FMAX };

void avx_binop(enum avx_op op, unsigned lb, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_shift(enum avx_shift_kind kind, unsigned lb, unsigned vlen, const uint8_t *s, uint64_t count, uint8_t *d);
void avx_shift_var(enum avx_shift_kind kind, unsigned lb, unsigned vlen, const uint8_t *s, const uint8_t *counts, uint8_t *d);
void avx_byte_shift(bool left, unsigned vlen, unsigned count, const uint8_t *s, uint8_t *d);
void avx_pshufb(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pshufd(unsigned vlen, byte_t imm, const uint8_t *s, uint8_t *d);
void avx_pshufw_half(unsigned vlen, byte_t imm, bool high, const uint8_t *s, uint8_t *d);
void avx_unpack(bool high, unsigned lb, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pack(bool to_signed, unsigned src_lb, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_widen(bool sign, unsigned src_lb, unsigned dst_lb, unsigned vlen, const uint8_t *s, uint8_t *d);
void avx_broadcast(unsigned lb, unsigned vlen, const uint8_t *s, uint8_t *d);
void avx_palignr(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pmaddwd(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pmaddubsw(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pmuludq(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_psadbw(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_abs(unsigned lb, unsigned vlen, const uint8_t *s, uint8_t *d);
double avx_fp_apply(enum avx_fp_op op, double a, double b);
// x86-exact SQRT: a negative operand gives the real indefinite (sign set),
// which arm64's sqrt does not produce.
float avx_sqrt_f32(float x);
double avx_sqrt_f64(double x);
void avx_fp_binop(enum avx_fp_op op, bool is_double, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
bool avx_fp_compare(unsigned pred, double a, double b);
void avx_fp_cmp(unsigned pred, bool is_double, unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_fma(unsigned form, bool is_double, bool negate_mul, bool subtract, unsigned vlen, const uint8_t *dst_in, const uint8_t *s2, const uint8_t *s3, uint8_t *d);
void avx_aes_round(bool decrypt, bool last, const uint8_t *state, const uint8_t *round_key, uint8_t *out);
void avx_pclmulqdq(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_gf2p8affine(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_vpdpbusd(unsigned vlen, const uint8_t *acc, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_vpdpwssd(unsigned vlen, const uint8_t *acc, const uint8_t *s1, const uint8_t *s2, uint8_t *d);
void avx_pternlog(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, const uint8_t *s3, uint8_t *d);

// ---- register-file access (shared by every front-end) ----
//
// A vector register's bits live in three arrays: xmm[i] holds 0-127,
// ymm_hi[i] holds 128-255, zmm_hi[i] holds 256-511. avx_reg_write always
// zeroes everything above `vlen`, which is what VEX/EVEX require of any
// write (a VEX.128 write to xmm3 clears ymm3's and zmm3's upper bits).
// Bits 0-127 of vector register `idx`. Registers 0-15 live in cpu->xmm (where
// every legacy SSE gadget expects them); 16-31 are EVEX-only and live in
// cpu->xmm_ext, so the two ranges must never be indexed as one array.
static inline union xmm_reg *avx_xmm(struct cpu_state *cpu, unsigned idx) {
    return idx < 16 ? &cpu->xmm[idx] : &cpu->xmm_ext[idx - 16];
}

void avx_reg_read(struct cpu_state *cpu, unsigned idx, unsigned vlen, uint8_t *out);
void avx_reg_write(struct cpu_state *cpu, unsigned idx, unsigned vlen, const uint8_t *in);

// ---- i386 front-end ----
//
// i386 is JIT-only: there is no interpreter to fall back to, so VEX is
// decoded at codegen time (emu/decode.h -> gen_vex32 in jit/gen.c) and the
// decoded instruction is handed to vec_avx32 at runtime through the existing
// vec-helper gadgets. Everything the runtime needs is packed into the 32-bit
// `desc` word that the gadgets already carry as their immediate:
//
//   bits  0-1   opcode map (1 = 0F, 2 = 0F38, 3 = 0F3A)
//   bits  2-3   pp (0 = none, 1 = 66, 2 = F3, 3 = F2)
//   bits  4-11  opcode
//   bit    12   L (0 = 128-bit, 1 = 256-bit)
//   bit    13   W
//   bits 14-16  vvvv        (32-bit mode limits every operand to xmm0-7)
//   bits 17-19  reg operand
//   bit    20   r/m operand is memory
//   bits 21-23  r/m register (when bit 20 is clear)
//   bits 24-31  imm8
#define AVX32_DESC(map, pp, op, l, w, vvvv, reg, rm_mem, rm, imm) ( \
    ((uint32_t) (map) & 3) | (((uint32_t) (pp) & 3) << 2) | \
    (((uint32_t) (op) & 0xff) << 4) | (((uint32_t) (l) & 1) << 12) | \
    (((uint32_t) (w) & 1) << 13) | (((uint32_t) (vvvv) & 7) << 14) | \
    (((uint32_t) (reg) & 7) << 17) | (((uint32_t) (rm_mem) & 1) << 20) | \
    (((uint32_t) (rm) & 7) << 21) | (((uint32_t) (imm) & 0xff) << 24))

// True when the r/m operand is the DESTINATION rather than a source (the
// stores: VMOVUPS/VMOVDQU to memory, VMOVD/VMOVQ to r/m, VEXTRACTI128,
// VPEXTRx). Shared so codegen picks the write gadget for exactly the cases
// the runtime treats as a store.
bool avx32_rm_is_dst(unsigned map, unsigned op, unsigned pp);

// Size in BITS of the memory operand, which is not always the operation
// width: VPBROADCASTD reads 32 bits into a 256-bit result, VINSERTI128 reads
// 128, the shift-by-xmm forms read 128 regardless of destination width.
// Returns 0 for an instruction this front-end does not implement, which
// makes codegen emit SIGILL rather than a wrong-sized access.
unsigned avx32_mem_bits(unsigned map, unsigned op, unsigned pp, unsigned w, unsigned vlen);

// True if the instruction has a ModRM byte at all. VZEROUPPER/VZEROALL
// (0F 77) do not -- consuming one for them eats the next instruction's first
// byte and desynchronizes the decoder, which is far worse than a SIGILL.
bool avx32_has_modrm(unsigned map, unsigned op, unsigned pp);

// True if the instruction takes a trailing imm8 byte (needed at codegen time
// to compute the instruction length correctly).
bool avx32_has_imm8(unsigned map, unsigned op, unsigned pp);

// Runtime entry point, called through the vec-helper gadgets with the
// gadget's usual (cpu, src, dst) plus the descriptor as the immediate.
void vec_avx32(struct cpu_state *cpu, const void *src, void *dst, uint32_t desc);

#endif
