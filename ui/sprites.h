// Sprite sheets: one horizontal strip per avatar state.
#pragma once

#include <d2d1.h>

#include "avatar.h"
#include "com.h"

struct SpriteSheet {
    Com<ID2D1Bitmap> bitmap;
    int frames = 1;
    float fps = 0.0f;
    int frame_w = 0;
    int frame_h = 0;
};

struct SpriteInfo {
    const wchar_t* file;
    int frames;
    float fps;
};

// The table is indexed by AvatarState.
const SpriteInfo& SpriteForState(AvatarState state);

// Loads the strip for `state` into `sheet` at the given render target.
// Returns false when the file is missing or unreadable; callers fall back to
// the coloured placeholder.
bool LoadSpriteSheet(ID2D1RenderTarget* target, AvatarState state,
                     SpriteSheet& sheet);
