// Copyright (C) 2026 アケネＪ / Akenejie
// SPDX-License-Identifier: AGPL-3.0-only
//
// This file is part of Overte Headless-Server (overte-hs), an unofficial
// stripped-down, headless-only derivative of Overte. It is licensed under
// the GNU Affero General Public License v3.0 (see LICENSE-AGPL-3.0.txt and
// NOTICE in the repository root).

#include "Image.h"
#include "ImageLogging.h"
#include "TextureProcessing.h"

#if !defined(OVERTE_HEADLESS)
#include <nvtt/nvtt.h>
#endif

using namespace image;

Image::Image(int width, int height, Format format) : 
    _dims(width, height), 
    _format(format) {
    if (_format == Format_RGBAF) {
        _floatData.resize(width*height);
    } else {
        _packedData = QImage(width, height, (QImage::Format)format);
    }
}

size_t Image::getByteCount() const {
    if (_format == Format_RGBAF) {
        return sizeof(FloatPixels::value_type) * _floatData.size();
    } else {
        return _packedData.sizeInBytes();
    }
}

size_t Image::getBytesPerLineCount() const { 
    if (_format == Format_RGBAF) {
        return sizeof(FloatPixels::value_type) * _dims.x;
    } else {
        return _packedData.bytesPerLine();
    }
}

glm::uint8* Image::editScanLine(int y) {
    if (_format == Format_RGBAF) {
        return reinterpret_cast<glm::uint8*>(_floatData.data() + y * _dims.x);
    } else {
        return _packedData.scanLine(y);
    }
}

const glm::uint8* Image::getScanLine(int y) const { 
    if (_format == Format_RGBAF) {
        return reinterpret_cast<const glm::uint8*>(_floatData.data() + y * _dims.x);
    } else {
        return _packedData.scanLine(y);
    }
}

glm::uint8* Image::editBits() {
    if (_format == Format_RGBAF) {
        return reinterpret_cast<glm::uint8*>(_floatData.data());
    } else {
        return _packedData.bits();
    }
}

const glm::uint8* Image::getBits() const {
    if (_format == Format_RGBAF) {
        return reinterpret_cast<const glm::uint8*>(_floatData.data());
    } else {
        return _packedData.bits();
    }
}

Image Image::getScaled(glm::uvec2 dstSize, AspectRatioMode ratioMode, TransformationMode transformMode) const {
#if defined(OVERTE_HEADLESS)
    if (_format != Format_PACKED_FLOAT && _format != Format_RGBAF) {
        return _packedData.scaled(fromGlm(dstSize), ratioMode, transformMode);
    }

    glm::uvec2 target = dstSize;
    if (ratioMode != Qt::IgnoreAspectRatio && _dims.y > 0) {
        const float srcRatio = static_cast<float>(_dims.x) / static_cast<float>(_dims.y);
        const float dstRatio = static_cast<float>(dstSize.x) / static_cast<float>(dstSize.y);
        if ((ratioMode == Qt::KeepAspectRatio && dstRatio > srcRatio) ||
            (ratioMode == Qt::KeepAspectRatioByExpanding && dstRatio < srcRatio)) {
            target.y = static_cast<glm::uint32>(dstSize.x / srcRatio);
        } else {
            target.x = static_cast<glm::uint32>(dstSize.y * srcRatio);
        }
        target = glm::max(target, glm::uvec2(1, 1));
    }

    Image output(static_cast<int>(target.x), static_cast<int>(target.y), _format);
    const int srcMaxX = _dims.x - 1;
    const int srcMaxY = _dims.y - 1;

    if (_format == Format_RGBAF) {
        for (glm::uint32 y = 0; y < target.y; y++) {
            for (glm::uint32 x = 0; x < target.x; x++) {
                const float sx = (static_cast<float>(x) + 0.5f) * _dims.x / target.x - 0.5f;
                const float sy = (static_cast<float>(y) + 0.5f) * _dims.y / target.y - 0.5f;
                const int x0 = glm::clamp(static_cast<int>(glm::floor(sx)), 0, srcMaxX);
                const int y0 = glm::clamp(static_cast<int>(glm::floor(sy)), 0, srcMaxY);
                const int x1 = glm::min(x0 + 1, srcMaxX);
                const int y1 = glm::min(y0 + 1, srcMaxY);
                const float fx = glm::clamp(sx - x0, 0.0f, 1.0f);
                const float fy = glm::clamp(sy - y0, 0.0f, 1.0f);
                const auto p00 = getFloatPixel(x0, y0);
                const auto p10 = getFloatPixel(x1, y0);
                const auto p01 = getFloatPixel(x0, y1);
                const auto p11 = getFloatPixel(x1, y1);
                output.setFloatPixel(x, y, glm::mix(glm::mix(p00, p10, fx), glm::mix(p01, p11, fx), fy));
            }
        }
    } else {
        auto unpackFunc = getHDRUnpackingFunction();
        auto packFunc = getHDRPackingFunction();
        for (glm::uint32 y = 0; y < target.y; y++) {
            glm::uint32* dstLine = reinterpret_cast<glm::uint32*>(output.editScanLine(static_cast<int>(y)));
            for (glm::uint32 x = 0; x < target.x; x++) {
                const float sx = (static_cast<float>(x) + 0.5f) * _dims.x / target.x - 0.5f;
                const float sy = (static_cast<float>(y) + 0.5f) * _dims.y / target.y - 0.5f;
                const int x0 = glm::clamp(static_cast<int>(glm::floor(sx)), 0, srcMaxX);
                const int y0 = glm::clamp(static_cast<int>(glm::floor(sy)), 0, srcMaxY);
                const int x1 = glm::min(x0 + 1, srcMaxX);
                const int y1 = glm::min(y0 + 1, srcMaxY);
                const float fx = glm::clamp(sx - x0, 0.0f, 1.0f);
                const float fy = glm::clamp(sy - y0, 0.0f, 1.0f);
                auto readPacked = [&](int px, int py) {
                    return glm::vec4(unpackFunc(reinterpret_cast<const glm::uint32*>(getScanLine(py))[px]), 1.0f);
                };
                const auto p00 = readPacked(x0, y0);
                const auto p10 = readPacked(x1, y0);
                const auto p01 = readPacked(x0, y1);
                const auto p11 = readPacked(x1, y1);
                const glm::vec4 color = glm::mix(glm::mix(p00, p10, fx), glm::mix(p01, p11, fx), fy);
                dstLine[x] = packFunc(glm::vec3(color));
            }
        }
    }
    return output;
#else
    if (_format == Format_PACKED_FLOAT || _format == Format_RGBAF) {
        nvtt::Surface surface;

        if (_format == Format_RGBAF) {
            surface.setImage(nvtt::InputFormat_RGBA_32F, getWidth(), getHeight(), 1, _floatData.data());
        } else {
            // Start by converting to full float
            glm::vec4* floatPixels = new glm::vec4[getWidth()*getHeight()];
            auto unpackFunc = getHDRUnpackingFunction();
            auto floatDataIt = floatPixels;
            for (glm::uint32 lineNb = 0; lineNb < getHeight(); lineNb++) {
                const glm::uint32* srcPixelIt = reinterpret_cast<const glm::uint32*>(getScanLine((int)lineNb));
                const glm::uint32* srcPixelEnd = srcPixelIt + getWidth();

                while (srcPixelIt < srcPixelEnd) {
                    *floatDataIt = glm::vec4(unpackFunc(*srcPixelIt), 1.0f);
                    ++srcPixelIt;
                    ++floatDataIt;
                }
            }

            // Perform filtered resize with NVTT
            static_assert(sizeof(glm::vec4) == 4 * sizeof(float), "Assuming glm::vec4 holds 4 floats");
            surface.setImage(nvtt::InputFormat_RGBA_32F, getWidth(), getHeight(), 1, floatPixels);
            delete[] floatPixels;
        }

        nvtt::ResizeFilter filter = nvtt::ResizeFilter_Kaiser;
        if (transformMode == Qt::TransformationMode::FastTransformation) {
            filter = nvtt::ResizeFilter_Box;
        }
        surface.resize(dstSize.x, dstSize.y, 1, filter);

        auto srcRedIt = reinterpret_cast<const float*>(surface.channel(0));
        auto srcGreenIt = reinterpret_cast<const float*>(surface.channel(1));
        auto srcBlueIt = reinterpret_cast<const float*>(surface.channel(2));
        auto srcAlphaIt = reinterpret_cast<const float*>(surface.channel(3));

        if (_format == Format_RGBAF) {
            Image output(_dims.x, _dims.y, _format);
            auto dstPixelIt = output._floatData.begin();
            auto dstPixelEnd = output._floatData.end();

            while (dstPixelIt < dstPixelEnd) {
                *dstPixelIt = glm::vec4(*srcRedIt, *srcGreenIt, *srcBlueIt, *srcAlphaIt);
                ++srcRedIt;
                ++srcGreenIt;
                ++srcBlueIt;
                ++srcAlphaIt;

                ++dstPixelIt;
            }

            return output;
        } else {
            // And convert back to original format
            QImage resizedImage((int)dstSize.x, (int)dstSize.y, (QImage::Format)Image::Format_PACKED_FLOAT);

            auto packFunc = getHDRPackingFunction();
            for (glm::uint32 lineNb = 0; lineNb < dstSize.y; lineNb++) {
                glm::uint32* dstPixelIt = reinterpret_cast<glm::uint32*>(resizedImage.scanLine((int)lineNb));
                glm::uint32* dstPixelEnd = dstPixelIt + dstSize.x;

                while (dstPixelIt < dstPixelEnd) {
                    *dstPixelIt = packFunc(glm::vec3(*srcRedIt, *srcGreenIt, *srcBlueIt));
                    ++srcRedIt;
                    ++srcGreenIt;
                    ++srcBlueIt;
                    ++dstPixelIt;
                }
            }
            return resizedImage;
        }
    } else {
        return _packedData.scaled(fromGlm(dstSize), ratioMode, transformMode);
    }
#endif
}

Image Image::getConvertedToFormat(Format newFormat) const {
    const float MAX_COLOR_VALUE = 255.0f;

    if (newFormat == _format) {
        return *this;
    } else if ((_format != Format_R11G11B10F && _format != Format_RGBAF) && (newFormat != Format_R11G11B10F && newFormat != Format_RGBAF)) {
        return _packedData.convertToFormat((QImage::Format)newFormat);
    } else if (_format == Format_PACKED_FLOAT) {
        Image newImage(_dims.x, _dims.y, newFormat);

        switch (newFormat) {
            case Format_RGBAF:
                convertToFloatFromPacked(getBits(), _dims.x, _dims.y, getBytesPerLineCount(), gpu::Element::COLOR_R11G11B10, newImage._floatData.data(), _dims.x);
                break;

            default:
            {
                auto unpackFunc = getHDRUnpackingFunction();
                const glm::uint32* srcIt = reinterpret_cast<const glm::uint32*>(getBits());

                for (int y = 0; y < _dims.y; y++) {
                    for (int x = 0; x < _dims.x; x++) {
                        auto color = glm::clamp(unpackFunc(*srcIt) * MAX_COLOR_VALUE, 0.0f, 255.0f);
                        newImage.setPackedPixel(x, y, qRgb(color.r, color.g, color.b));
                        srcIt++;
                    }
                }
                break;
            }
        }
        return newImage;
    } else if (_format == Format_RGBAF) {
        Image newImage(_dims.x, _dims.y, newFormat);

        switch (newFormat) {
            case Format_R11G11B10F:
                convertToPackedFromFloat(newImage.editBits(), _dims.x, _dims.y, getBytesPerLineCount(), gpu::Element::COLOR_R11G11B10, _floatData.data(), _dims.x);
                break;

            default:
            {
                FloatPixels::const_iterator srcIt = _floatData.begin();

                for (int y = 0; y < _dims.y; y++) {
                    for (int x = 0; x < _dims.x; x++) {
                        auto color = glm::clamp((*srcIt) * MAX_COLOR_VALUE, 0.0f, 255.0f);
                        newImage.setPackedPixel(x, y, qRgba(color.r, color.g, color.b, color.a));
                        srcIt++;
                    }
                }
                break;
            }
        }
        return newImage;
    } else {
        Image newImage(_dims.x, _dims.y, newFormat);
        assert(newImage.hasFloatFormat());

        if (newFormat == Format_RGBAF) {
            FloatPixels::iterator dstIt = newImage._floatData.begin();

            for (int y = 0; y < _dims.y; y++) {
                auto line = (const QRgb*)getScanLine(y);
                for (int x = 0; x < _dims.x; x++) {
                    QRgb pixel = line[x];
                    *dstIt = glm::vec4(qRed(pixel), qGreen(pixel), qBlue(pixel), qAlpha(pixel)) / MAX_COLOR_VALUE;
                    dstIt++;
                }
            }
        } else {
            auto packFunc = getHDRPackingFunction();
            glm::uint32* dstIt = reinterpret_cast<glm::uint32*>( newImage.editBits() );

            for (int y = 0; y < _dims.y; y++) {
                auto line = (const QRgb*)getScanLine(y);
                for (int x = 0; x < _dims.x; x++) {
                    QRgb pixel = line[x];
                    *dstIt = packFunc(glm::vec3(qRed(pixel), qGreen(pixel), qBlue(pixel)) / MAX_COLOR_VALUE);
                    dstIt++;
                }
            }
        }
        return newImage;
    }
}

void Image::invertPixels() {
    assert(_format != Format_PACKED_FLOAT && _format != Format_RGBAF);
    _packedData.invertPixels(QImage::InvertRgba);
}

Image Image::getSubImage(QRect rect) const {
    assert(_format != Format_RGBAF);
    return _packedData.copy(rect);
}

Image Image::getMirrored(bool horizontal, bool vertical) const {
    assert(_format != Format_RGBAF);
    return _packedData.mirrored(horizontal, vertical);
}
