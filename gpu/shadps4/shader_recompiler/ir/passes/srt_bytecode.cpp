// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: the portable SRT walker interpreter (see srt_bytecode.h).
#include "shader_recompiler/ir/passes/srt_bytecode.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "common/arch.h"

extern "C" int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);

namespace Shader::SrtBytecode {
namespace {

/// Reads `size` bytes of guest memory, or returns false when they are not mapped and readable:
/// the x86 walker turns such loads into zero (its fault handler patches the instruction).
bool ReadGuest(u64 address, void* out, size_t size) {
    thread_local uintptr_t cached_start = 0, cached_end = 0;
    if (address < cached_start || address + size > cached_end) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(address, &start, &end, &mapped) || !mapped || address < start ||
            address + size > end) {
            return false;
        }
        cached_start = start;
        cached_end = end;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
}

u32 Eval(const u32* rpn, u32 len, const u32* flat) {
    u32 stack[32];
    u32 sp = 0;
    for (u32 i = 0; i < len;) {
        const u32 op = rpn[i++];
        switch (op) {
        case Flat:
            stack[sp++] = flat[rpn[i++]];
            break;
        case Imm:
            stack[sp++] = rpn[i++];
            break;
        case Not:
            stack[sp - 1] = ~stack[sp - 1];
            break;
        case BitFieldUExtract: {
            const u32 count = stack[--sp], offset = stack[--sp], base = stack[sp - 1];
            stack[sp - 1] = (base >> (offset & 31)) & ((1u << (count & 31)) - 1u);
            break;
        }
        default: {
            const u32 b = stack[--sp], a = stack[sp - 1];
            u32 r = 0;
            switch (op) {
            case Add: r = a + b; break;
            case Sub: r = a - b; break;
            case Mul: r = a * b; break;
            case Shl: r = a << (b & 31); break;
            case Shr: r = a >> (b & 31); break;
            case And: r = a & b; break;
            case Or: r = a | b; break;
            case Xor: r = a ^ b; break;
            case UMin: r = a < b ? a : b; break;
            case UMax: r = a > b ? a : b; break;
            default: break;
            }
            stack[sp - 1] = r;
        }
        }
    }
    return sp ? stack[sp - 1] : 0;
}

} // namespace

bool IsBytecode(const u8* blob, size_t size) {
    u32 magic = 0;
    if (!blob || size < sizeof(magic)) {
        return false;
    }
    std::memcpy(&magic, blob, sizeof(magic));
    return magic == Magic;
}

bool Enabled() {
#ifdef ARCH_X86_64
    static const bool enabled = [] {
        const char* env = std::getenv("SPARTAN_SRT_INTERPRETER");
        return env && (env[0] == '1' || env[0] == 'v');
    }();
    return enabled;
#else
    return true;
#endif
}

bool Verify() {
#ifdef ARCH_X86_64
    static const bool verify = [] {
        const char* env = std::getenv("SPARTAN_SRT_INTERPRETER");
        return env && env[0] == 'v';
    }();
    return verify;
#else
    return false;
#endif
}

namespace {
std::mutex reference_mutex;
std::unordered_map<const u8*, void*> references;
} // namespace

void SetReference(const u8* blob, void* x86_walker) {
    std::scoped_lock lock{reference_mutex};
    references[blob] = x86_walker;
}

void* Reference(const u8* blob) {
    std::scoped_lock lock{reference_mutex};
    const auto it = references.find(blob);
    return it == references.end() ? nullptr : it->second;
}

void Run(const u8* blob, size_t size, const u32* user_data, u32* flat) {
    const u32* code = reinterpret_cast<const u32*>(blob) + 1;
    const u32* code_end = reinterpret_cast<const u32*>(blob + size);
    u64 ptr_stack[64];
    u32 depth = 0;
    u64 ptr = reinterpret_cast<u64>(user_data);
    while (code < code_end) {
        switch (*code++) {
        case End:
            return;
        case PushPtrImm:
        case PushPtrExpr: {
            u32 off_dw;
            if (code[-1] == PushPtrImm) {
                off_dw = *code++;
            } else {
                const u32 len = *code++;
                off_dw = Eval(code, len, flat);
                code += len;
            }
            u64 next = 0;
            if (depth == 0) {
                // The roots: 64-bit pointers in the user data (host memory, not guest memory).
                std::memcpy(&next, user_data + off_dw, sizeof(next));
            } else {
                ReadGuest(ptr + u64(off_dw) * 4, &next, sizeof(next));
            }
            ptr_stack[depth++] = ptr;
            ptr = next & 0xFFFFFFFFFFFFull;
            break;
        }
        case CopyImm: {
            const u32 src = *code++, dst = *code++;
            u32 value = 0;
            ReadGuest(ptr + u64(src) * 4, &value, sizeof(value));
            flat[dst] = value;
            break;
        }
        case CopyExpr: {
            const u32 dst = *code++, len = *code++;
            const u32 src = Eval(code, len, flat);
            code += len;
            u32 value = 0;
            ReadGuest(ptr + u64(src) * 4, &value, sizeof(value));
            flat[dst] = value;
            break;
        }
        case PopPtr:
            ptr = ptr_stack[--depth];
            break;
        default:
            return;
        }
    }
}

} // namespace Shader::SrtBytecode
