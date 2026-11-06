/*****************************************************************************
 * Copyright (c) 2014-2026 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "DrawingEngineFactory.hpp"

#include <SDL_hints.h>
#include <SDL_render.h>
#include <SDL_version.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <openrct2/Diagnostic.h>
#include <openrct2/Game.h>
#include <openrct2/config/Config.h>
#include <openrct2/core/Guard.hpp>
#include <openrct2/drawing/IDrawingEngine.h>
#include <openrct2/drawing/LightFX.h>
#include <openrct2/drawing/X8DrawingEngine.h>
#include <openrct2/interface/Window.h>
#include <openrct2/paint/Paint.h>
#include <openrct2/ui/UiContext.h>
#include <vector>

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;
using namespace OpenRCT2::Ui;

class HardwareDisplayDrawingEngine final : public X8DrawingEngine
{
private:
    constexpr static uint32_t kDirtyVisualTime = 40;
    constexpr static uint32_t kDirtyRegionAlpha = 100;

    IUiContext& _uiContext;
    SDL_Window* _window = nullptr;
    SDL_Renderer* _sdlRenderer = nullptr;
    SDL_Texture* _screenTexture = nullptr;
    SDL_Texture* _scaledScreenTexture = nullptr;
    SDL_PixelFormat* _screenTextureFormat = nullptr;
    uint32_t _paletteHWMapped[256] = { 0 };
    uint32_t _lightPaletteHWMapped[256] = { 0 };

    bool _useVsync = true;

    std::vector<uint32_t> _dirtyVisualsTime;

    bool smoothNN = false;

    // Direct 8-bit output: when the window surface is 8-bit paletted (a plain framebuffer such as
    // the iMac G3's console), our 8-bit image goes straight into it, block by redrawn block,
    // with no SDL renderer, colour conversion or full-screen copies.
    bool _direct8 = false;
    SDL_Surface* _windowSurface = nullptr;
    std::vector<SDL_Rect> _direct8Rects;

public:
    explicit HardwareDisplayDrawingEngine(IUiContext& uiContext)
        : X8DrawingEngine(uiContext)
        , _uiContext(uiContext)
    {
        _window = static_cast<SDL_Window*>(_uiContext.GetWindow());
    }

    ~HardwareDisplayDrawingEngine() override
    {
        if (_screenTexture != nullptr)
        {
            SDL_DestroyTexture(_screenTexture);
        }
        if (_scaledScreenTexture != nullptr)
        {
            SDL_DestroyTexture(_scaledScreenTexture);
        }
        SDL_FreeFormat(_screenTextureFormat);
        SDL_DestroyRenderer(_sdlRenderer);
    }

    void Initialise() override
    {
        // Decide by the window's pixel format: SDL_GetWindowSurface would bind the window to its
        // surface and stop SDL_CreateRenderer from working on it.
        if (SDL_GetWindowPixelFormat(_window) == SDL_PIXELFORMAT_INDEX8)
            _windowSurface = SDL_GetWindowSurface(_window);
        if (_windowSurface != nullptr && _windowSurface->format->palette != nullptr)
        {
            _direct8 = true;
            LOG_VERBOSE("HWDisplayDrawingEngine: direct 8-bit output to the window surface");
            return;
        }
        _sdlRenderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_ACCELERATED | (_useVsync ? SDL_RENDERER_PRESENTVSYNC : 0));
        // Plain framebuffer video drivers (no GPU) only have the software renderer
        if (_sdlRenderer == nullptr)
            _sdlRenderer = SDL_CreateRenderer(_window, -1, SDL_RENDERER_SOFTWARE);
    }

    void SetVSync(bool vsync) override
    {
        if (_direct8)
            return;
        if (_useVsync != vsync)
        {
            _useVsync = vsync;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_RenderSetVSync(_sdlRenderer, vsync ? 1 : 0);
#else
            SDL_DestroyRenderer(_sdlRenderer);
            _screenTexture = nullptr;
            _scaledScreenTexture = nullptr;
            Initialise();
            Resize(_uiContext->GetWidth(), _uiContext->GetHeight());
#endif
        }
    }

    void Resize(uint32_t width, uint32_t height) override
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        if (_direct8)
        {
            // A new size frees the old window surface: take our image back into our own memory first
            if (_bitsExternal)
                ConfigureBits(_width, _height, _width);
            _windowSurface = SDL_GetWindowSurface(_window);
            _direct8Rects.clear();
            X8DrawingEngine::Resize(width, height);
            // Draw straight into the window surface: showing a frame is then only the driver's
            // copy of the redrawn blocks to the screen
            if (_windowSurface != nullptr && _windowSurface->w >= static_cast<int>(width)
                && _windowSurface->h >= static_cast<int>(height))
                UseExternalBits(static_cast<PaletteIndex*>(_windowSurface->pixels), _windowSurface->pitch);
            // Everything is new: copy the whole screen on the next frame
            _direct8Rects.push_back({ 0, 0, static_cast<int>(width), static_cast<int>(height) });
            return;
        }

        if (_screenTexture != nullptr)
        {
            SDL_DestroyTexture(_screenTexture);
        }
        SDL_FreeFormat(_screenTextureFormat);

        SDL_RendererInfo rendererInfo = {};
        int32_t result = SDL_GetRendererInfo(_sdlRenderer, &rendererInfo);
        if (result < 0)
        {
            LOG_WARNING("HWDisplayDrawingEngine::Resize error: %s", SDL_GetError());
            return;
        }
        uint32_t pixelFormat = SDL_PIXELFORMAT_UNKNOWN;
        for (uint32_t i = 0; i < rendererInfo.num_texture_formats; i++)
        {
            uint32_t format = rendererInfo.texture_formats[i];
            if (!SDL_ISPIXELFORMAT_FOURCC(format) && !SDL_ISPIXELFORMAT_INDEXED(format)
                && (pixelFormat == SDL_PIXELFORMAT_UNKNOWN || SDL_BYTESPERPIXEL(format) < SDL_BYTESPERPIXEL(pixelFormat)))
            {
                pixelFormat = format;
            }
        }

        ScaleQuality scaleQuality = GetContext()->GetUiContext().GetScaleQuality();
        if (scaleQuality == ScaleQuality::smoothNearestNeighbour)
        {
            scaleQuality = ScaleQuality::linear;
            smoothNN = true;
        }
        else
        {
            smoothNN = false;
        }

        if (smoothNN)
        {
            if (_scaledScreenTexture != nullptr)
            {
                SDL_DestroyTexture(_scaledScreenTexture);
            }

            char scaleQualityBuffer[4];
            snprintf(scaleQualityBuffer, sizeof(scaleQualityBuffer), "%d", static_cast<int32_t>(scaleQuality));
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

            _screenTexture = SDL_CreateTexture(_sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
            Guard::Assert(
                _screenTexture != nullptr, "Failed to create unscaled screen texture (%ux%u, pixelFormat = %u): %s", width,
                height, pixelFormat, SDL_GetError());

            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scaleQualityBuffer);

            uint32_t scale = std::ceil(Config::Get().general.windowScale);
            _scaledScreenTexture = SDL_CreateTexture(
                _sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_TARGET, width * scale, height * scale);

            Guard::Assert(
                _scaledScreenTexture != nullptr,
                "Failed to create scaled screen texture (%ux%u, scale = %u, pixelFormat = %u): %s", width, height, scale,
                pixelFormat, SDL_GetError());
        }
        else
        {
            _screenTexture = SDL_CreateTexture(_sdlRenderer, pixelFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
            Guard::Assert(
                _screenTexture != nullptr, "Failed to create screen texture (%ux%u, pixelFormat = %u): %s", width, height,
                pixelFormat, SDL_GetError());
        }

        uint32_t format;
        SDL_QueryTexture(_screenTexture, &format, nullptr, nullptr, nullptr);
        _screenTextureFormat = SDL_AllocFormat(format);

        X8DrawingEngine::Resize(width, height);
    }

    void SetPalette(const GamePalette& palette) override
    {
        if (_direct8)
        {
            if (_windowSurface != nullptr)
            {
                SDL_Color colours[256];
                for (int32_t i = 0; i < 256; i++)
                    colours[i] = { palette[i].red, palette[i].green, palette[i].blue, 255 };
                SDL_SetPaletteColors(_windowSurface->format->palette, colours, 0, 256);
            }
            return;
        }
        if (_screenTextureFormat != nullptr)
        {
            for (int32_t i = 0; i < 256; i++)
            {
                _paletteHWMapped[i] = SDL_MapRGB(_screenTextureFormat, palette[i].red, palette[i].green, palette[i].blue);
            }

            if (Config::Get().general.enableLightFx)
            {
                auto& lightPalette = LightFx::GetPalette();
                for (int32_t i = 0; i < 256; i++)
                {
                    const auto& src = lightPalette[i];
                    _lightPaletteHWMapped[i] = SDL_MapRGBA(_screenTextureFormat, src.red, src.green, src.blue, src.alpha);
                }
            }
        }
    }

    // Viewport scrolling moves pixels inside the image instead of redrawing them: in direct 8-bit
    // mode the moved area must reach the screen too.
    void CopyRect(int32_t x, int32_t y, int32_t width, int32_t height, int32_t dx, int32_t dy) override
    {
        X8DrawingEngine::CopyRect(x, y, width, height, dx, dy);
        if (_direct8 && (dx != 0 || dy != 0))
            _direct8Rects.push_back({ x, y, width, height });
    }

    void BeginDraw() override
    {
        X8DrawingEngine::BeginDraw();
    }

    void EndDraw() override
    {
        X8DrawingEngine::EndDraw();

        Display();
    }

protected:
    void OnDrawDirtyBlock(int32_t left, int32_t top, int32_t right, int32_t bottom) override
    {
        if (_direct8)
            _direct8Rects.push_back({ left, top, right - left, bottom - top });
        if (gShowDirtyVisuals)
        {
            const auto columns = ((right - left) + (_invalidationGrid.getBlockWidth() - 1)) / _invalidationGrid.getBlockWidth();
            const auto rows = ((bottom - top) + (_invalidationGrid.getBlockHeight() - 1)) / _invalidationGrid.getBlockHeight();
            const auto firstRow = top / _invalidationGrid.getBlockHeight();
            const auto firstColumn = left / _invalidationGrid.getBlockWidth();

            for (uint32_t y = 0; y < rows; y++)
            {
                for (uint32_t x = 0; x < columns; x++)
                {
                    SetDirtyVisualTime(firstColumn + x, firstRow + y, gCurrentRealTimeTicks + kDirtyVisualTime);
                }
            }
        }
    }

private:
    void Display()
    {
        if (_direct8)
        {
            DisplayDirect8();
            return;
        }
        auto* viewport = WindowGetViewport(WindowGetMain());

        if (Config::Get().general.enableLightFx && viewport != nullptr)
        {
            void* pixels;
            int32_t pitch;
            if (SDL_LockTexture(_screenTexture, nullptr, &pixels, &pitch) == 0)
            {
                LightFx::RenderToTexture(
                    *viewport, pixels, pitch, _bits, _width, _height, _paletteHWMapped, _lightPaletteHWMapped);
                SDL_UnlockTexture(_screenTexture);
            }
        }
        else
        {
            CopyBitsToTexture(
                _screenTexture, _bits, static_cast<int32_t>(_width), static_cast<int32_t>(_height), _paletteHWMapped);
        }
        if (smoothNN)
        {
            SDL_SetRenderTarget(_sdlRenderer, _scaledScreenTexture);
            SDL_RenderCopy(_sdlRenderer, _screenTexture, nullptr, nullptr);

            SDL_SetRenderTarget(_sdlRenderer, nullptr);
            SDL_RenderCopy(_sdlRenderer, _scaledScreenTexture, nullptr, nullptr);
        }
        else
        {
            SDL_RenderCopy(_sdlRenderer, _screenTexture, nullptr, nullptr);
        }

        if (gShowDirtyVisuals)
        {
            RenderDirtyVisuals();
        }

        SDL_RenderPresent(_sdlRenderer);
    }

    void DisplayDirect8()
    {
        if (_windowSurface == nullptr)
            return;
        const int32_t w = std::min<int32_t>(_windowSurface->w, static_cast<int32_t>(_width));
        const int32_t h = std::min<int32_t>(_windowSurface->h, static_cast<int32_t>(_height));
        for (auto& r : _direct8Rects)
        {
            const int32_t x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
            r.w = std::min(r.x + r.w, w) - x0;
            r.h = std::min(r.y + r.h, h) - y0;
            r.x = x0;
            r.y = y0;
            if (r.w <= 0 || r.h <= 0 || _bits == _windowSurface->pixels)
                continue;
            for (int32_t y = r.y; y < r.y + r.h; y++)
            {
                std::memcpy(
                    static_cast<uint8_t*>(_windowSurface->pixels) + y * _windowSurface->pitch + r.x,
                    _bits + static_cast<size_t>(y) * _pitch + r.x, r.w);
            }
        }
        // Also with no rectangles: the driver picks up palette changes on every update
        SDL_UpdateWindowSurfaceRects(_window, _direct8Rects.data(), static_cast<int>(_direct8Rects.size()));
        _direct8Rects.clear();
    }

    void CopyBitsToTexture(SDL_Texture* texture, PaletteIndex* src, int32_t width, int32_t height, const uint32_t* palette)
    {
        // The palette holds native pixel values from SDL_MapRGB; store them at the texture's
        // pixel size (the old 16/8-bit paths took the first byte of a uint32, which is the wrong
        // byte on big-endian, and never matched a 16-bit pitch).
        void* pixels;
        int32_t pitch;
        uint32_t format;
        if (SDL_QueryTexture(texture, &format, nullptr, nullptr, nullptr) != 0)
            return;
        const int32_t bytesPerPixel = SDL_BYTESPERPIXEL(format);
        if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0)
        {
            for (int32_t y = 0; y < height; y++)
            {
                auto* row = static_cast<uint8_t*>(pixels) + y * pitch;
                if (bytesPerPixel == 4)
                {
                    auto* dst = reinterpret_cast<uint32_t*>(row);
                    for (int32_t x = 0; x < width; x++)
                        dst[x] = palette[EnumValue(*src++)];
                }
                else if (bytesPerPixel == 2)
                {
                    auto* dst = reinterpret_cast<uint16_t*>(row);
                    for (int32_t x = 0; x < width; x++)
                        dst[x] = static_cast<uint16_t>(palette[EnumValue(*src++)]);
                }
                else
                {
                    for (int32_t x = 0; x < width; x++)
                        row[x] = static_cast<uint8_t>(palette[EnumValue(*src++)]);
                }
            }
            SDL_UnlockTexture(texture);
        }
    }

    uint32_t GetDirtyVisualTime(uint32_t x, uint32_t y)
    {
        uint32_t result = 0;
        uint32_t i = y * _invalidationGrid.getColumnCount() + x;
        if (_dirtyVisualsTime.size() > i)
        {
            result = _dirtyVisualsTime[i];
        }
        return result;
    }

    void SetDirtyVisualTime(uint32_t x, uint32_t y, uint32_t value)
    {
        const auto rows = _invalidationGrid.getRowCount();
        const auto columns = _invalidationGrid.getColumnCount();

        _dirtyVisualsTime.resize(rows * columns);

        uint32_t i = y * _invalidationGrid.getColumnCount() + x;
        if (_dirtyVisualsTime.size() > i)
        {
            _dirtyVisualsTime[i] = value;
        }
    }

    void RenderDirtyVisuals()
    {
        int windowX, windowY, renderX, renderY;
        SDL_GetWindowSize(_window, &windowX, &windowY);
        SDL_GetRendererOutputSize(_sdlRenderer, &renderX, &renderY);

        float scaleX = Config::Get().general.windowScale * renderX / static_cast<float>(windowX);
        float scaleY = Config::Get().general.windowScale * renderY / static_cast<float>(windowY);

        SDL_SetRenderDrawBlendMode(_sdlRenderer, SDL_BLENDMODE_BLEND);
        for (uint32_t y = 0; y < _invalidationGrid.getRowCount(); y++)
        {
            for (uint32_t x = 0; x < _invalidationGrid.getColumnCount(); x++)
            {
                const auto timeEnd = GetDirtyVisualTime(x, y);
                const auto timeLeft = gCurrentRealTimeTicks < timeEnd ? timeEnd - gCurrentRealTimeTicks : 0;
                if (timeLeft > 0)
                {
                    uint8_t alpha = timeLeft * kDirtyRegionAlpha / kDirtyVisualTime;
                    SDL_Rect ddRect;
                    ddRect.x = static_cast<int32_t>(x * _invalidationGrid.getBlockWidth() * scaleX);
                    ddRect.y = static_cast<int32_t>(y * _invalidationGrid.getBlockHeight() * scaleY);
                    ddRect.w = static_cast<int32_t>(_invalidationGrid.getBlockWidth() * scaleX);
                    ddRect.h = static_cast<int32_t>(_invalidationGrid.getBlockHeight() * scaleY);

                    SDL_SetRenderDrawColor(_sdlRenderer, 255, 255, 255, alpha);
                    SDL_RenderFillRect(_sdlRenderer, &ddRect);
                }
            }
        }
    }
};

std::unique_ptr<IDrawingEngine> Ui::CreateHardwareDisplayDrawingEngine(IUiContext& uiContext)
{
    return std::make_unique<HardwareDisplayDrawingEngine>(uiContext);
}
