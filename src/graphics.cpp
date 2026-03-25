#include "mil/graphics.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "font8x8_basic.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace mil::gfx {

namespace {

constexpr const char* kPrimaryFontPath = "romfs:/fonts/Atkinson-Hyperlegible-Regular.ttf";
constexpr const char* kSymbolsFontPath = "romfs:/fonts/Labo Symbols.ttf";

enum class AccentMark {
    None,
    Acute,
    Grave,
    Circumflex,
    Tilde,
    Cedilla,
};

struct GlyphInfo {
    char base = '?';
    AccentMark accent = AccentMark::None;
    bool shiftDown = false;
};

struct CachedTtfGlyph {
    int width = 0;
    int height = 0;
    int xOffset = 0;
    int yOffset = 0;
    int advance = 0;
    std::vector<unsigned char> bitmap;
};

struct TtfRenderer {
    bool loadAttempted = false;
    bool ready = false;
    stbtt_fontinfo info{};
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    std::vector<unsigned char> fontData;
    std::map<std::uint64_t, CachedTtfGlyph> glyphCache;
};

struct CachedImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<unsigned char> pixels;
};

struct CachedScaledImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

TtfRenderer g_primaryTtfRenderer;
TtfRenderer g_symbolsTtfRenderer;
std::map<std::string, CachedImage> g_imageCache;
std::map<std::string, CachedScaledImage> g_scaledImageCache;

TtfRenderer& RendererForFace(FontFace face) {
    return face == FontFace::Symbols ? g_symbolsTtfRenderer : g_primaryTtfRenderer;
}

const char* FontPathForFace(FontFace face) {
    return face == FontFace::Symbols ? kSymbolsFontPath : kPrimaryFontPath;
}

void PutPixel(Canvas& canvas, int x, int y, std::uint32_t color) {
    if (!canvas.pixels || x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) {
        return;
    }
    canvas.pixels[y * canvas.stridePixels + x] = color;
}

std::uint8_t Channel(std::uint32_t color, int shift) {
    return static_cast<std::uint8_t>((color >> shift) & 0xFF);
}

std::uint32_t LerpColor(std::uint32_t a, std::uint32_t b, float t) {
    const auto lerp = [t](std::uint8_t lhs, std::uint8_t rhs) -> std::uint8_t {
        return static_cast<std::uint8_t>(lhs + static_cast<int>((rhs - lhs) * t));
    };

    return Rgba(lerp(Channel(a, 0), Channel(b, 0)),
                lerp(Channel(a, 8), Channel(b, 8)),
                lerp(Channel(a, 16), Channel(b, 16)),
                lerp(Channel(a, 24), Channel(b, 24)));
}

void FillScaledPixel(Canvas& canvas, int x, int y, std::uint32_t color, int scale) {
    for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
            PutPixel(canvas, x + dx, y + dy, color);
        }
    }
}

void BlendPixel(Canvas& canvas, int x, int y, std::uint32_t color, unsigned char coverage) {
    if (!canvas.pixels || x < 0 || y < 0 || x >= canvas.width || y >= canvas.height || coverage == 0) {
        return;
    }

    const std::uint32_t dst = canvas.pixels[y * canvas.stridePixels + x];
    const unsigned int srcAlpha = (static_cast<unsigned int>(Channel(color, 24)) * coverage) / 255u;
    const unsigned int invAlpha = 255u - srcAlpha;

    const auto blendChannel = [&](int shift) -> std::uint8_t {
        const unsigned int src = Channel(color, shift);
        const unsigned int dstValue = Channel(dst, shift);
        return static_cast<std::uint8_t>((src * srcAlpha + dstValue * invAlpha) / 255u);
    };

    canvas.pixels[y * canvas.stridePixels + x] =
        Rgba(blendChannel(0), blendChannel(8), blendChannel(16), 0xFF);
}

void BlendImagePixel(Canvas& canvas, int x, int y, const unsigned char* rgba) {
    if (!canvas.pixels || x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) {
        return;
    }

    const std::uint32_t src = Rgba(rgba[0], rgba[1], rgba[2], rgba[3]);
    BlendPixel(canvas, x, y, src, rgba[3]);
}

void BlendImagePixel(Canvas& canvas,
                     int x,
                     int y,
                     std::uint8_t red,
                     std::uint8_t green,
                     std::uint8_t blue,
                     std::uint8_t alpha) {
    if (!canvas.pixels || x < 0 || y < 0 || x >= canvas.width || y >= canvas.height) {
        return;
    }

    const std::uint32_t src = Rgba(red, green, blue, alpha);
    BlendPixel(canvas, x, y, src, alpha);
}

bool DecodeUtf8(const std::string& text, std::size_t& index, std::uint32_t& codepoint) {
    if (index >= text.size()) {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(text[index]);
    if ((first & 0x80u) == 0) {
        codepoint = first;
        ++index;
        return true;
    }

    if ((first & 0xE0u) == 0xC0u && index + 1 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[index + 1]);
        if ((second & 0xC0u) == 0x80u) {
            codepoint = ((first & 0x1Fu) << 6) | (second & 0x3Fu);
            index += 2;
            return true;
        }
    } else if ((first & 0xF0u) == 0xE0u && index + 2 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[index + 1]);
        const unsigned char third = static_cast<unsigned char>(text[index + 2]);
        if ((second & 0xC0u) == 0x80u && (third & 0xC0u) == 0x80u) {
            codepoint = ((first & 0x0Fu) << 12) | ((second & 0x3Fu) << 6) | (third & 0x3Fu);
            index += 3;
            return true;
        }
    } else if ((first & 0xF8u) == 0xF0u && index + 3 < text.size()) {
        const unsigned char second = static_cast<unsigned char>(text[index + 1]);
        const unsigned char third = static_cast<unsigned char>(text[index + 2]);
        const unsigned char fourth = static_cast<unsigned char>(text[index + 3]);
        if ((second & 0xC0u) == 0x80u && (third & 0xC0u) == 0x80u && (fourth & 0xC0u) == 0x80u) {
            codepoint = ((first & 0x07u) << 18) | ((second & 0x3Fu) << 12) | ((third & 0x3Fu) << 6) |
                        (fourth & 0x3Fu);
            index += 4;
            return true;
        }
    }

    codepoint = '?';
    ++index;
    return true;
}

std::vector<std::uint32_t> DecodeCodepoints(const std::string& text) {
    std::vector<std::uint32_t> codepoints;
    std::size_t index = 0;
    std::uint32_t codepoint = 0;
    while (DecodeUtf8(text, index, codepoint)) {
        codepoints.push_back(codepoint);
    }
    return codepoints;
}

std::size_t Utf8GlyphCount(const std::string& text) {
    std::size_t count = 0;
    std::size_t index = 0;
    std::uint32_t codepoint = 0;
    while (DecodeUtf8(text, index, codepoint)) {
        ++count;
    }
    return count;
}

std::size_t Utf8ByteOffsetForGlyph(const std::string& text, std::size_t glyphIndex) {
    std::size_t index = 0;
    std::size_t currentGlyph = 0;
    std::uint32_t codepoint = 0;
    while (currentGlyph < glyphIndex && DecodeUtf8(text, index, codepoint)) {
        ++currentGlyph;
    }
    return index;
}

std::string Utf8SliceGlyphs(const std::string& text, std::size_t startGlyph, std::size_t glyphCount) {
    const std::size_t startByte = Utf8ByteOffsetForGlyph(text, startGlyph);
    const std::size_t endByte = Utf8ByteOffsetForGlyph(text, startGlyph + glyphCount);
    return text.substr(startByte, endByte - startByte);
}

GlyphInfo LookupGlyphInfo(std::uint32_t codepoint) {
    if (codepoint < 128) {
        return {static_cast<char>(codepoint), AccentMark::None, false};
    }

    switch (codepoint) {
        case 0x00E1:
            return {'a', AccentMark::Acute, true};
        case 0x00C1:
            return {'A', AccentMark::Acute, true};
        case 0x00E0:
            return {'a', AccentMark::Grave, true};
        case 0x00C0:
            return {'A', AccentMark::Grave, true};
        case 0x00E2:
            return {'a', AccentMark::Circumflex, true};
        case 0x00C2:
            return {'A', AccentMark::Circumflex, true};
        case 0x00E3:
            return {'a', AccentMark::Tilde, true};
        case 0x00C3:
            return {'A', AccentMark::Tilde, true};
        case 0x00E9:
            return {'e', AccentMark::Acute, true};
        case 0x00C9:
            return {'E', AccentMark::Acute, true};
        case 0x00EA:
            return {'e', AccentMark::Circumflex, true};
        case 0x00CA:
            return {'E', AccentMark::Circumflex, true};
        case 0x00ED:
            return {'i', AccentMark::Acute, true};
        case 0x00CD:
            return {'I', AccentMark::Acute, true};
        case 0x00F3:
            return {'o', AccentMark::Acute, true};
        case 0x00D3:
            return {'O', AccentMark::Acute, true};
        case 0x00F4:
            return {'o', AccentMark::Circumflex, true};
        case 0x00D4:
            return {'O', AccentMark::Circumflex, true};
        case 0x00F5:
            return {'o', AccentMark::Tilde, true};
        case 0x00D5:
            return {'O', AccentMark::Tilde, true};
        case 0x00FA:
            return {'u', AccentMark::Acute, true};
        case 0x00DA:
            return {'U', AccentMark::Acute, true};
        case 0x00E7:
            return {'c', AccentMark::Cedilla, false};
        case 0x00C7:
            return {'C', AccentMark::Cedilla, false};
        default:
            return {'?', AccentMark::None, false};
    }
}

void DrawAccent(Canvas& canvas, int x, int y, AccentMark accent, std::uint32_t color, int scale) {
    switch (accent) {
        case AccentMark::Acute:
            FillScaledPixel(canvas, x + 5 * scale, y + 1 * scale, color, scale);
            FillScaledPixel(canvas, x + 4 * scale, y + 2 * scale, color, scale);
            FillScaledPixel(canvas, x + 3 * scale, y + 3 * scale, color, scale);
            break;
        case AccentMark::Grave:
            FillScaledPixel(canvas, x + 2 * scale, y + 1 * scale, color, scale);
            FillScaledPixel(canvas, x + 3 * scale, y + 2 * scale, color, scale);
            FillScaledPixel(canvas, x + 4 * scale, y + 3 * scale, color, scale);
            break;
        case AccentMark::Circumflex:
            FillScaledPixel(canvas, x + 2 * scale, y + 3 * scale, color, scale);
            FillScaledPixel(canvas, x + 3 * scale, y + 2 * scale, color, scale);
            FillScaledPixel(canvas, x + 4 * scale, y + 1 * scale, color, scale);
            FillScaledPixel(canvas, x + 5 * scale, y + 2 * scale, color, scale);
            FillScaledPixel(canvas, x + 6 * scale, y + 3 * scale, color, scale);
            break;
        case AccentMark::Tilde:
            FillScaledPixel(canvas, x + 1 * scale, y + 2 * scale, color, scale);
            FillScaledPixel(canvas, x + 2 * scale, y + 1 * scale, color, scale);
            FillScaledPixel(canvas, x + 3 * scale, y + 1 * scale, color, scale);
            FillScaledPixel(canvas, x + 4 * scale, y + 2 * scale, color, scale);
            FillScaledPixel(canvas, x + 5 * scale, y + 2 * scale, color, scale);
            FillScaledPixel(canvas, x + 6 * scale, y + 1 * scale, color, scale);
            break;
        case AccentMark::Cedilla:
            FillScaledPixel(canvas, x + 3 * scale, y + 8 * scale, color, scale);
            FillScaledPixel(canvas, x + 4 * scale, y + 9 * scale, color, scale);
            FillScaledPixel(canvas, x + 3 * scale, y + 10 * scale, color, scale);
            break;
        case AccentMark::None:
        default:
            break;
    }
}

int PixelHeightForScale(int scale) {
    const int clampedScale = std::max(1, scale);
    return 16 + (clampedScale - 1) * 8;
}

bool ReadBinaryFile(const char* path, std::vector<unsigned char>& data) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return false;
    }

    data.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !data.empty();
}

const CachedImage* LoadCachedImage(const std::string& path) {
    auto it = g_imageCache.find(path);
    if (it != g_imageCache.end()) {
        return &it->second;
    }

    std::vector<unsigned char> fileData;
    if (!ReadBinaryFile(path.c_str(), fileData)) {
        return nullptr;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(fileData.data(),
                                                   static_cast<int>(fileData.size()),
                                                   &width,
                                                   &height,
                                                   &channels,
                                                   STBI_rgb_alpha);
    if (!decoded) {
        return nullptr;
    }

    CachedImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.pixels.assign(decoded, decoded + static_cast<std::size_t>(width * height * 4));
    stbi_image_free(decoded);

    auto [inserted, _] = g_imageCache.emplace(path, std::move(image));
    return &inserted->second;
}

std::string MakeScaledImageKey(const std::string& path, int width, int height) {
    return path + "#" + std::to_string(width) + "x" + std::to_string(height);
}

const CachedScaledImage* LoadScaledImage(const std::string& path, int width, int height) {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    const std::string key = MakeScaledImageKey(path, width, height);
    auto it = g_scaledImageCache.find(key);
    if (it != g_scaledImageCache.end()) {
        return &it->second;
    }

    const CachedImage* image = LoadCachedImage(path);
    if (!image || image->width <= 0 || image->height <= 0) {
        return nullptr;
    }

    const float scale = std::min(static_cast<float>(width) / static_cast<float>(image->width),
                                 static_cast<float>(height) / static_cast<float>(image->height));
    const int drawWidth = std::max(1, static_cast<int>(std::round(static_cast<float>(image->width) * scale)));
    const int drawHeight = std::max(1, static_cast<int>(std::round(static_cast<float>(image->height) * scale)));

    CachedScaledImage scaled;
    scaled.width = drawWidth;
    scaled.height = drawHeight;
    scaled.pixels.resize(static_cast<std::size_t>(drawWidth * drawHeight * 4));

    for (int yy = 0; yy < drawHeight; ++yy) {
        const float srcY = ((static_cast<float>(yy) + 0.5f) / static_cast<float>(drawHeight)) * image->height - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(srcY)), 0, image->height - 1);
        const int y1 = std::min(image->height - 1, y0 + 1);
        const float fy = std::clamp(srcY - static_cast<float>(y0), 0.0f, 1.0f);
        for (int xx = 0; xx < drawWidth; ++xx) {
            const float srcX = ((static_cast<float>(xx) + 0.5f) / static_cast<float>(drawWidth)) * image->width - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(srcX)), 0, image->width - 1);
            const int x1 = std::min(image->width - 1, x0 + 1);
            const float fx = std::clamp(srcX - static_cast<float>(x0), 0.0f, 1.0f);

            const unsigned char* p00 = &image->pixels[static_cast<std::size_t>((y0 * image->width + x0) * 4)];
            const unsigned char* p10 = &image->pixels[static_cast<std::size_t>((y0 * image->width + x1) * 4)];
            const unsigned char* p01 = &image->pixels[static_cast<std::size_t>((y1 * image->width + x0) * 4)];
            const unsigned char* p11 = &image->pixels[static_cast<std::size_t>((y1 * image->width + x1) * 4)];

            const auto bilerp = [fx, fy](unsigned char a, unsigned char b, unsigned char c, unsigned char d) -> std::uint8_t {
                const float top = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * fx;
                const float bottom = static_cast<float>(c) + (static_cast<float>(d) - static_cast<float>(c)) * fx;
                return static_cast<std::uint8_t>(std::clamp(top + (bottom - top) * fy, 0.0f, 255.0f));
            };

            unsigned char* dst = &scaled.pixels[static_cast<std::size_t>((yy * drawWidth + xx) * 4)];
            dst[0] = bilerp(p00[0], p10[0], p01[0], p11[0]);
            dst[1] = bilerp(p00[1], p10[1], p01[1], p11[1]);
            dst[2] = bilerp(p00[2], p10[2], p01[2], p11[2]);
            dst[3] = bilerp(p00[3], p10[3], p01[3], p11[3]);
        }
    }

    auto [inserted, _] = g_scaledImageCache.emplace(key, std::move(scaled));
    return &inserted->second;
}

bool EnsureTtfRenderer(FontFace face) {
    TtfRenderer& renderer = RendererForFace(face);
    if (renderer.loadAttempted) {
        return renderer.ready;
    }

    renderer.loadAttempted = true;
    if (!ReadBinaryFile(FontPathForFace(face), renderer.fontData)) {
        return false;
    }

    const int offset = stbtt_GetFontOffsetForIndex(renderer.fontData.data(), 0);
    if (offset < 0) {
        renderer.fontData.clear();
        return false;
    }

    if (!stbtt_InitFont(&renderer.info, renderer.fontData.data(), offset)) {
        renderer.fontData.clear();
        return false;
    }

    stbtt_GetFontVMetrics(&renderer.info, &renderer.ascent, &renderer.descent, &renderer.lineGap);
    renderer.ready = true;
    return true;
}

std::uint64_t GlyphCacheKey(std::uint32_t codepoint, int pixelHeight) {
    return (static_cast<std::uint64_t>(codepoint) << 32) | static_cast<std::uint32_t>(pixelHeight);
}

const CachedTtfGlyph& GetTtfGlyph(TtfRenderer& renderer, std::uint32_t codepoint, int pixelHeight) {
    const std::uint64_t key = GlyphCacheKey(codepoint, pixelHeight);
    auto it = renderer.glyphCache.find(key);
    if (it != renderer.glyphCache.end()) {
        return it->second;
    }

    CachedTtfGlyph glyph;
    const float fontScale = stbtt_ScaleForPixelHeight(&renderer.info, static_cast<float>(pixelHeight));

    int advanceWidth = 0;
    int leftBearing = 0;
    stbtt_GetCodepointHMetrics(&renderer.info, static_cast<int>(codepoint), &advanceWidth, &leftBearing);
    glyph.advance = std::max(1, static_cast<int>(std::lround(static_cast<float>(advanceWidth) * fontScale)));

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBox(&renderer.info, static_cast<int>(codepoint), fontScale, fontScale, &x0, &y0, &x1, &y1);
    glyph.xOffset = x0;
    glyph.yOffset = y0;
    glyph.width = std::max(0, x1 - x0);
    glyph.height = std::max(0, y1 - y0);

    if (glyph.width > 0 && glyph.height > 0) {
        glyph.bitmap.resize(static_cast<std::size_t>(glyph.width * glyph.height));
        stbtt_MakeCodepointBitmap(&renderer.info,
                                  glyph.bitmap.data(),
                                  glyph.width,
                                  glyph.height,
                                  glyph.width,
                                  fontScale,
                                  fontScale,
                                  static_cast<int>(codepoint));
    }

    auto [inserted, _] = renderer.glyphCache.emplace(key, std::move(glyph));
    return inserted->second;
}

int LineHeightBitmap(int scale) {
    return 12 * scale;
}

int LineHeightTtf(const TtfRenderer& renderer, int pixelHeight) {
    const float fontScale = stbtt_ScaleForPixelHeight(&renderer.info, static_cast<float>(pixelHeight));
    const float rawHeight =
        static_cast<float>(renderer.ascent - renderer.descent + renderer.lineGap) * fontScale;
    return std::max(9, static_cast<int>(std::ceil(rawHeight)));
}

void DrawGlyphBitmap(Canvas& canvas, int x, int y, std::uint32_t codepoint, std::uint32_t color, int scale) {
    const GlyphInfo info = LookupGlyphInfo(codepoint);
    const unsigned char* glyph = reinterpret_cast<const unsigned char*>(font8x8_basic[static_cast<unsigned char>(info.base)]);
    const int glyphY = y + (info.shiftDown ? scale : 0);
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            if ((glyph[row] & (1 << col)) == 0) {
                continue;
            }
            FillScaledPixel(canvas, x + col * scale, glyphY + row * scale, color, scale);
        }
    }
    DrawAccent(canvas, x, y, info.accent, color, scale);
}

void DrawTextBitmap(Canvas& canvas, int x, int y, const std::string& text, std::uint32_t color, int scale) {
    int cursorX = x;
    int cursorY = y;
    std::size_t index = 0;
    std::uint32_t codepoint = 0;
    while (DecodeUtf8(text, index, codepoint)) {
        if (codepoint == '\n') {
            cursorX = x;
            cursorY += LineHeightBitmap(scale);
            continue;
        }
        DrawGlyphBitmap(canvas, cursorX, cursorY, codepoint, color, scale);
        cursorX += 8 * scale;
    }
}

int MeasureTextWidthBitmap(const std::string& text, int scale) {
    int width = 0;
    int lineWidth = 0;
    std::size_t index = 0;
    std::uint32_t codepoint = 0;
    while (DecodeUtf8(text, index, codepoint)) {
        if (codepoint == '\n') {
            width = std::max(width, lineWidth);
            lineWidth = 0;
            continue;
        }
        lineWidth += 8 * scale;
    }
    return std::max(width, lineWidth);
}

std::vector<std::string> WrapLinesPx(const std::string& text, int maxWidth, int pixelHeight, int maxLines) {
    std::vector<std::string> lines;
    std::stringstream paragraphStream(text);
    std::string paragraph;

    while (std::getline(paragraphStream, paragraph, '\n')) {
        std::stringstream wordStream(paragraph);
        std::string word;
        std::string line;

        while (wordStream >> word) {
            std::string candidate = line.empty() ? word : line + ' ' + word;
            if (gfx::MeasureTextWidthPx(candidate, pixelHeight) <= maxWidth) {
                line = std::move(candidate);
                continue;
            }

            if (!line.empty()) {
                lines.push_back(line);
                if (maxLines > 0 && static_cast<int>(lines.size()) >= maxLines) {
                    return lines;
                }
            }

            if (gfx::MeasureTextWidthPx(word, pixelHeight) <= maxWidth) {
                line = word;
                continue;
            }

            line.clear();
            const std::size_t glyphCount = Utf8GlyphCount(word);
            std::size_t offset = 0;
            while (offset < glyphCount) {
                std::size_t chunkGlyphs = 1;
                std::string bestChunk = Utf8SliceGlyphs(word, offset, chunkGlyphs);
                while (offset + chunkGlyphs < glyphCount) {
                    const std::string nextChunk = Utf8SliceGlyphs(word, offset, chunkGlyphs + 1);
                    if (gfx::MeasureTextWidthPx(nextChunk, pixelHeight) > maxWidth) {
                        break;
                    }
                    bestChunk = nextChunk;
                    ++chunkGlyphs;
                }

                lines.push_back(bestChunk);
                if (maxLines > 0 && static_cast<int>(lines.size()) >= maxLines) {
                    return lines;
                }
                offset += chunkGlyphs;
            }
        }

        if (!line.empty()) {
            lines.push_back(line);
            if (maxLines > 0 && static_cast<int>(lines.size()) >= maxLines) {
                return lines;
            }
        } else if (paragraph.empty()) {
            lines.emplace_back();
            if (maxLines > 0 && static_cast<int>(lines.size()) >= maxLines) {
                return lines;
            }
        }
    }

    return lines;
}

std::vector<std::string> WrapLines(const std::string& text, int maxWidth, int scale, int maxLines) {
    return WrapLinesPx(text, maxWidth, PixelHeightForScale(scale), maxLines);
}

void DrawTextTtf(Canvas& canvas,
                 TtfRenderer& renderer,
                 int x,
                 int y,
                 const std::string& text,
                 std::uint32_t color,
                 int pixelHeight) {
    const std::vector<std::uint32_t> codepoints = DecodeCodepoints(text);
    const float fontScale = stbtt_ScaleForPixelHeight(&renderer.info, static_cast<float>(pixelHeight));
    const int baseline = std::max(0, static_cast<int>(std::ceil(static_cast<float>(renderer.ascent) * fontScale)));

    int cursorX = x;
    int cursorY = y;
    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        const std::uint32_t codepoint = codepoints[index];
        if (codepoint == '\n') {
            cursorX = x;
            cursorY += LineHeightTtf(renderer, pixelHeight);
            continue;
        }

        const CachedTtfGlyph& glyph = GetTtfGlyph(renderer, codepoint, pixelHeight);
        const int drawX = cursorX + glyph.xOffset;
        const int drawY = cursorY + baseline + glyph.yOffset;

        for (int row = 0; row < glyph.height; ++row) {
            for (int col = 0; col < glyph.width; ++col) {
                const unsigned char coverage = glyph.bitmap[static_cast<std::size_t>(row * glyph.width + col)];
                BlendPixel(canvas, drawX + col, drawY + row, color, coverage);
            }
        }

        int advance = glyph.advance;
        if (index + 1 < codepoints.size() && codepoints[index + 1] != '\n') {
            const int kern = stbtt_GetCodepointKernAdvance(&renderer.info,
                                                           static_cast<int>(codepoint),
                                                           static_cast<int>(codepoints[index + 1]));
            advance += static_cast<int>(std::lround(static_cast<float>(kern) * fontScale));
        }
        cursorX += advance;
    }
}

int MeasureTextWidthTtf(TtfRenderer& renderer, const std::string& text, int pixelHeight) {
    const std::vector<std::uint32_t> codepoints = DecodeCodepoints(text);
    const float fontScale = stbtt_ScaleForPixelHeight(&renderer.info, static_cast<float>(pixelHeight));

    int width = 0;
    int lineWidth = 0;
    for (std::size_t index = 0; index < codepoints.size(); ++index) {
        const std::uint32_t codepoint = codepoints[index];
        if (codepoint == '\n') {
            width = std::max(width, lineWidth);
            lineWidth = 0;
            continue;
        }

        const CachedTtfGlyph& glyph = GetTtfGlyph(renderer, codepoint, pixelHeight);
        lineWidth += glyph.advance;
        if (index + 1 < codepoints.size() && codepoints[index + 1] != '\n') {
            const int kern = stbtt_GetCodepointKernAdvance(&renderer.info,
                                                           static_cast<int>(codepoint),
                                                           static_cast<int>(codepoints[index + 1]));
            lineWidth += static_cast<int>(std::lround(static_cast<float>(kern) * fontScale));
        }
    }

    return std::max(width, lineWidth);
}

}  // namespace

Canvas BeginFrame(Framebuffer& framebuffer) {
    Canvas canvas;
    if (!framebuffer.has_init) {
        return canvas;
    }

    u32 stride = 0;
    canvas.pixels = static_cast<std::uint32_t*>(framebufferBegin(&framebuffer, &stride));
    canvas.width = 1280;
    canvas.height = 720;
    canvas.stridePixels = static_cast<int>(stride / sizeof(std::uint32_t));
    return canvas;
}

void EndFrame(Framebuffer& framebuffer) {
    framebufferEnd(&framebuffer);
}

std::uint32_t Rgba(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha) {
    return RGBA8(red, green, blue, alpha);
}

void Clear(Canvas& canvas, std::uint32_t color) {
    for (int y = 0; y < canvas.height; ++y) {
        for (int x = 0; x < canvas.width; ++x) {
            canvas.pixels[y * canvas.stridePixels + x] = color;
        }
    }
}

void ClearVerticalGradient(Canvas& canvas, std::uint32_t topColor, std::uint32_t bottomColor) {
    for (int y = 0; y < canvas.height; ++y) {
        const float t = canvas.height <= 1 ? 0.0f : static_cast<float>(y) / static_cast<float>(canvas.height - 1);
        const std::uint32_t rowColor = LerpColor(topColor, bottomColor, t);
        for (int x = 0; x < canvas.width; ++x) {
            canvas.pixels[y * canvas.stridePixels + x] = rowColor;
        }
    }
}

void FillRect(Canvas& canvas, int x, int y, int width, int height, std::uint32_t color) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(canvas.width, x + width);
    const int y1 = std::min(canvas.height, y + height);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) {
            canvas.pixels[yy * canvas.stridePixels + xx] = color;
        }
    }
}

void DrawRect(Canvas& canvas, int x, int y, int width, int height, std::uint32_t color) {
    FillRect(canvas, x, y, width, 1, color);
    FillRect(canvas, x, y + height - 1, width, 1, color);
    FillRect(canvas, x, y, 1, height, color);
    FillRect(canvas, x + width - 1, y, 1, height, color);
}

void FillCircle(Canvas& canvas, int centerX, int centerY, int radius, std::uint32_t color) {
    for (int y = -radius; y <= radius; ++y) {
        const int span = static_cast<int>(std::sqrt(static_cast<double>(radius * radius - y * y)));
        FillRect(canvas, centerX - span, centerY + y, span * 2 + 1, 1, color);
    }
}

void DrawCircle(Canvas& canvas, int centerX, int centerY, int radius, std::uint32_t color) {
    int x = radius;
    int y = 0;
    int error = 1 - x;

    while (x >= y) {
        PutPixel(canvas, centerX + x, centerY + y, color);
        PutPixel(canvas, centerX + y, centerY + x, color);
        PutPixel(canvas, centerX - y, centerY + x, color);
        PutPixel(canvas, centerX - x, centerY + y, color);
        PutPixel(canvas, centerX - x, centerY - y, color);
        PutPixel(canvas, centerX - y, centerY - x, color);
        PutPixel(canvas, centerX + y, centerY - x, color);
        PutPixel(canvas, centerX + x, centerY - y, color);

        ++y;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x + 1);
        }
    }
}

void DrawText(Canvas& canvas, int x, int y, const std::string& text, std::uint32_t color, int scale) {
    DrawTextFont(canvas, x, y, text, color, FontFace::Primary, scale);
}

void DrawTextPx(Canvas& canvas, int x, int y, const std::string& text, std::uint32_t color, int pixelHeight) {
    if (EnsureTtfRenderer(FontFace::Primary)) {
        DrawTextTtf(canvas, RendererForFace(FontFace::Primary), x, y, text, color, pixelHeight);
        return;
    }

    const int fallbackScale = pixelHeight >= 20 ? 2 : 1;
    DrawText(canvas, x, y, text, color, fallbackScale);
}

void DrawTextFont(Canvas& canvas,
                  int x,
                  int y,
                  const std::string& text,
                  std::uint32_t color,
                  FontFace face,
                  int scale) {
    if (EnsureTtfRenderer(face)) {
        DrawTextTtf(canvas, RendererForFace(face), x, y, text, color, PixelHeightForScale(scale));
        return;
    }

    if (face == FontFace::Primary) {
        DrawTextBitmap(canvas, x, y, text, color, scale);
    }
}

bool DrawImageFile(Canvas& canvas, int x, int y, int width, int height, const std::string& path) {
    const CachedScaledImage* image = LoadScaledImage(path, width, height);
    if (!image || image->width <= 0 || image->height <= 0 || width <= 0 || height <= 0) {
        return false;
    }

    const int offsetX = x + (width - image->width) / 2;
    const int offsetY = y + (height - image->height) / 2;

    for (int yy = 0; yy < image->height; ++yy) {
        for (int xx = 0; xx < image->width; ++xx) {
            const unsigned char* pixel = &image->pixels[static_cast<std::size_t>((yy * image->width + xx) * 4)];
            BlendImagePixel(canvas, offsetX + xx, offsetY + yy, pixel);
        }
    }

    return true;
}

int DrawTextWrapped(Canvas& canvas,
                    int x,
                    int y,
                    int maxWidth,
                    const std::string& text,
                    std::uint32_t color,
                    int scale,
                    int maxLines,
                    int lineSpacingAdjust) {
    const auto lines = WrapLines(text, maxWidth, scale, maxLines);
    const int lineAdvance = std::max(1, LineHeight(scale) + lineSpacingAdjust);
    int drawnLines = 0;
    for (const std::string& line : lines) {
        DrawText(canvas, x, y + drawnLines * lineAdvance, line, color, scale);
        ++drawnLines;
    }
    return drawnLines * lineAdvance;
}

int DrawTextWrappedPx(Canvas& canvas,
                      int x,
                      int y,
                      int maxWidth,
                      const std::string& text,
                      std::uint32_t color,
                      int pixelHeight,
                      int maxLines,
                      int lineSpacingAdjust) {
    const auto lines = WrapLinesPx(text, maxWidth, pixelHeight, maxLines);
    const int lineAdvance = std::max(1, LineHeightPx(pixelHeight) + lineSpacingAdjust);
    int drawnLines = 0;
    for (const std::string& line : lines) {
        DrawTextPx(canvas, x, y + drawnLines * lineAdvance, line, color, pixelHeight);
        ++drawnLines;
    }
    return drawnLines * lineAdvance;
}

int MeasureWrappedTextHeight(const std::string& text, int maxWidth, int scale, int maxLines, int lineSpacingAdjust) {
    const auto lines = WrapLines(text, maxWidth, scale, maxLines);
    const int lineAdvance = std::max(1, LineHeight(scale) + lineSpacingAdjust);
    return static_cast<int>(lines.size()) * lineAdvance;
}

int MeasureWrappedTextHeightPx(const std::string& text, int maxWidth, int pixelHeight, int maxLines, int lineSpacingAdjust) {
    const auto lines = WrapLinesPx(text, maxWidth, pixelHeight, maxLines);
    const int lineAdvance = std::max(1, LineHeightPx(pixelHeight) + lineSpacingAdjust);
    return static_cast<int>(lines.size()) * lineAdvance;
}

int MeasureTextWidth(const std::string& text, int scale) {
    return MeasureTextWidthFont(text, FontFace::Primary, scale);
}

int MeasureTextWidthPx(const std::string& text, int pixelHeight) {
    if (EnsureTtfRenderer(FontFace::Primary)) {
        return MeasureTextWidthTtf(RendererForFace(FontFace::Primary), text, pixelHeight);
    }
    return MeasureTextWidth(text, pixelHeight >= 20 ? 2 : 1);
}

int MeasureTextWidthFont(const std::string& text, FontFace face, int scale) {
    if (EnsureTtfRenderer(face)) {
        return MeasureTextWidthTtf(RendererForFace(face), text, PixelHeightForScale(scale));
    }
    return face == FontFace::Primary ? MeasureTextWidthBitmap(text, scale) : 0;
}

int LineHeight(int scale) {
    return LineHeightFont(FontFace::Primary, scale);
}

int LineHeightPx(int pixelHeight) {
    if (EnsureTtfRenderer(FontFace::Primary)) {
        return LineHeightTtf(RendererForFace(FontFace::Primary), pixelHeight);
    }
    return LineHeight(pixelHeight >= 20 ? 2 : 1);
}

int LineHeightFont(FontFace face, int scale) {
    if (EnsureTtfRenderer(face)) {
        return LineHeightTtf(RendererForFace(face), PixelHeightForScale(scale));
    }
    return face == FontFace::Primary ? LineHeightBitmap(scale) : 0;
}

}  // namespace mil::gfx
