// The avatar's states and their placeholder look.
#pragma once

#include <d2d1.h>

enum class AvatarState { Booting, Idle, Thinking, Remembering, Talking, Error };

D2D1_COLOR_F avatar_color(AvatarState state);
const wchar_t* avatar_name(AvatarState state);
