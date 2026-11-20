// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: a portable SRT walker. The flatten pass (flatten_extended_userdata_pass.cpp)
// generates x86-64 code that walks a shader's resource tables in guest memory and copies them into
// a flat buffer; this is the same walk as bytecode run by an interpreter, for arm64 (the native
// graphics service) and, with SPARTAN_SRT_INTERPRETER=1, on x86-64 to check it against the code.
//
// Blob: u32 magic 'SRTB', then u32 words: ops below; expressions are RPN of Expr ops.
#pragma once

#include <cstddef>
#include <vector>
#include "common/types.h"

namespace Shader::SrtBytecode {

constexpr u32 Magic = 0x42545253u; // "SRTB"

enum Op : u32 {
    End = 0,
    PushPtrImm,  // off_dw: ptr = *(u64*)(ptr + off*4) & 2^48-1 (the previous ptr is saved)
    PushPtrExpr, // len, rpn[len]: as PushPtrImm with the offset computed
    CopyImm,     // src_off_dw, dst_off_dw: flat[dst] = *(u32*)(ptr + src*4)
    CopyExpr,    // dst_off_dw, len, rpn[len]
    PopPtr,
};

enum Expr : u32 {
    Flat = 1, // off: push flat[off] (user data and values already copied)
    Imm,      // value
    Add, Sub, Mul, Shl, Shr, And, Or, Xor, Not, UMin, UMax,
    BitFieldUExtract, // pops count, offset, base: (base >> offset) & ((1 << count) - 1)
};

/// True when the blob is bytecode (else it is x86-64 code from the code generator).
bool IsBytecode(const u8* blob, size_t size);

/// Whether the flatten pass emits bytecode: always on arm64, on x86-64 with SPARTAN_SRT_INTERPRETER=1.
bool Enabled();

/// SPARTAN_SRT_INTERPRETER=verify (x86-64): shaders get both walkers; every run of the bytecode
/// also runs the x86-64 code and compares the flat buffers (mismatches are logged).
bool Verify();
void SetReference(const u8* blob, void* x86_walker);
void* Reference(const u8* blob);

/// Runs a walker blob: user_data is the shader's user data, flat_dst the flat buffer (its first
/// dwords already hold the user data).
void Run(const u8* blob, size_t size, const u32* user_data, u32* flat_dst);

} // namespace Shader::SrtBytecode
