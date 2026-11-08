#include "sprites.h"

#include <wincodec.h>

#include <string>

#include "net.h"

namespace {

// Four frames per state, except error which is a single still.
// Talking reuses the idle strip: replies are drawn with the idle animation.
const SpriteInfo kSprites[] = {
    {L"booting.png", 4, 2.0f},    // Booting
    {L"idling.png", 4, 2.0f},     // Idle
    {L"thinking.png", 4, 2.5f},   // Thinking
    {L"reading.png", 4, 2.5f},    // Remembering
    {L"idling.png", 4, 2.0f},     // Talking
    {L"error.png", 1, 0.0f},      // Error
};

}  // namespace

const SpriteInfo& SpriteForState(AvatarState state) {
    const int index = (int)state;
    const int count = (int)(sizeof(kSprites) / sizeof(kSprites[0]));
    if (index < 0 || index >= count) return kSprites[(int)AvatarState::Idle];
    return kSprites[index];
}

bool LoadSpriteSheet(ID2D1RenderTarget* target, AvatarState state,
                     SpriteSheet& sheet) {
    const std::wstring root = PanelRoot();
    if (root.empty() || !target) return false;

    const std::wstring path =
        root + L"\\sprites\\" + SpriteForState(state).file;

    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                __uuidof(IWICImagingFactory),
                                (void**)wic.out()))) {
        return false;
    }

    Com<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr,
                                              GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad,
                                              decoder.out()))) {
        return false;
    }

    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.out()))) return false;

    Com<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(converter.out()))) return false;
    if (FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    if (FAILED(target->CreateBitmapFromWicBitmap(converter.p, nullptr,
                                                 sheet.bitmap.out()))) {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    const D2D1_SIZE_U size = sheet.bitmap->GetPixelSize();
    width = size.width;
    height = size.height;

    const int frames = SpriteForState(state).frames;
    if (frames <= 0 || height == 0 || width < (UINT)frames) {
        sheet.bitmap.reset();
        return false;
    }
    // A strip whose width is not a whole number of frames would shear.
    if (width % (UINT)frames != 0) {
        sheet.bitmap.reset();
        return false;
    }

    sheet.frames = frames;
    sheet.fps = SpriteForState(state).fps;
    sheet.frame_w = (int)(width / (UINT)frames);
    sheet.frame_h = (int)height;
    return true;
}
