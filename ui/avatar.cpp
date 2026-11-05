#include "avatar.h"

#include "theme.h"

D2D1_COLOR_F avatar_color(AvatarState state) {
    switch (state) {
        case AvatarState::Booting:
            return theme::rgb(0x6B7280);
        case AvatarState::Thinking:
            return theme::rgb(0xF59E0B);
        case AvatarState::Remembering:
            return theme::rgb(0xA78BFA);
        case AvatarState::Talking:
            return theme::rgb(0x34D399);
        case AvatarState::Error:
            return theme::rgb(0xF87171);
        case AvatarState::Idle:
        default:
            return theme::accent;
    }
}

const wchar_t* avatar_name(AvatarState state) {
    switch (state) {
        case AvatarState::Booting:
            return L"booting";
        case AvatarState::Thinking:
            return L"thinking";
        case AvatarState::Remembering:
            return L"remembering";
        case AvatarState::Talking:
            return L"talking";
        case AvatarState::Error:
            return L"error";
        case AvatarState::Idle:
        default:
            return L"idle";
    }
}
