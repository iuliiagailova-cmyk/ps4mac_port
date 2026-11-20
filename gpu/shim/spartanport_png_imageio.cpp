// SPDX-License-Identifier: GPL-2.0-or-later
// ps4mac_port: PNG decoding with macOS ImageIO, for the arm64 graphics service (no FFmpeg there).
// Same contract as spartanport_png.cpp: straight RGBA, rows top to bottom.
#include "spartanport_png.h"

#include <algorithm>
#include <cstdio>
#include <ApplicationServices/ApplicationServices.h>

namespace SpartanPng {

namespace {
constexpr int kMaxSide = 4096; // save icons are 228x128; anything far larger is not one
}

bool DecodeRgba(const unsigned char* data, size_t size, Image& out) {
    out = {};
    if (!data || !size || size > 64u << 20) {
        return false;
    }
    CFDataRef bytes = CFDataCreate(nullptr, data, CFIndex(size));
    CGImageSourceRef source = bytes ? CGImageSourceCreateWithData(bytes, nullptr) : nullptr;
    CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    bool ok = false;
    if (image) {
        const int width = int(CGImageGetWidth(image)), height = int(CGImageGetHeight(image));
        if (width > 0 && height > 0 && width <= kMaxSide && height <= kMaxSide) {
            out.width = width;
            out.height = height;
            out.rgba.assign(size_t(width) * height * 4, 0);
            // Premultiplied is the only RGBA layout CGBitmapContext draws into; undone below.
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef context = CGBitmapContextCreate(
                out.rgba.data(), width, height, 8, size_t(width) * 4, space,
                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            if (context) {
                CGContextSetBlendMode(context, kCGBlendModeCopy);
                CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
                CGContextRelease(context);
                for (size_t i = 0; i < out.rgba.size(); i += 4) {
                    const unsigned a = out.rgba[i + 3];
                    if (a && a != 255) {
                        for (int c = 0; c < 3; ++c) {
                            out.rgba[i + c] = (unsigned char)std::min(255u, (out.rgba[i + c] * 255u + a / 2) / a);
                        }
                    }
                }
                ok = true;
            }
            CGColorSpaceRelease(space);
        }
        CGImageRelease(image);
    }
    if (source) CFRelease(source);
    if (bytes) CFRelease(bytes);
    if (!ok) {
        out = {};
    }
    return ok;
}

bool LoadRgba(const char* path, Image& out) {
    out = {};
    FILE* file = path ? std::fopen(path, "rb") : nullptr;
    if (!file) {
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0 && bytes.size() <= (64u << 20)) {
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    std::fclose(file);
    return DecodeRgba(bytes.data(), bytes.size(), out);
}

} // namespace SpartanPng
