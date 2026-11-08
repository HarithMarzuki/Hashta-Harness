// Window, state and the geometry every part of the panel shares.
#pragma once

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <vector>

#include "avatar.h"
#include "chat.h"
#include "com.h"
#include "sprites.h"

// The four bands, in DIPs. Recomputed on resize; the renderer and the hit
// testing both read them, so there is one source of truth for the layout.
struct Metrics {
    float w = 432.0f;
    float h = 721.0f;
    float avatar_h = 267.0f;
    float settings_y = 267.0f;
    float chat_y = 323.0f;
    float chat_h = 342.0f;
    float composer_y = 665.0f;

    D2D1_RECT_F avatar{};
    D2D1_RECT_F clear_btn{};
    D2D1_RECT_F trace_btn{};
    D2D1_RECT_F gear{};
    D2D1_RECT_F pill{};
    D2D1_RECT_F send{};
};

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    HFONT edit_font = nullptr;
    HBRUSH edit_bg = nullptr;
    UINT dpi = 96;
    float dip_w = 432.0f;
    float dip_h = 721.0f;

    Com<ID2D1Factory> d2d;
    Com<IDWriteFactory> dwrite;
    Com<ID2D1HwndRenderTarget> target;
    Com<ID2D1SolidColorBrush> brush;
    Com<IDWriteTextFormat> body;
    Com<IDWriteTextFormat> body_center;
    Com<IDWriteTextFormat> small;
    Com<IDWriteTextFormat> small_mid;
    Com<IDWriteTextFormat> small_center;
    Com<IDWriteTextFormat> small_right;
    Com<IDWriteTextFormat> pill_format;
    Com<IDWriteTextFormat> title;

    ChatModel chat;
    AvatarState avatar = AvatarState::Booting;
    std::vector<Bubble> bubbles;
    float content_h = 0.0f;
    float scroll = 0.0f;
    float scroll_target = 0.0f;
    bool layout_dirty = true;
    float composer_h = 56.0f;
    std::wstring chat_model = L"waiting";
    std::wstring embed_model = L"waiting";

    // Animated avatar: one strip per state, loaded on first use, and the
    // frame clock that drives it.
    SpriteSheet sprite_slots[6];
    bool sprite_attempted[6] = {};
    AvatarState anim_state = AvatarState::Booting;
    ULONGLONG anim_start = 0;
    int anim_frame = 0;

    bool busy = false;
    bool connected = false;
    bool closing = false;
    int streaming = -1;
    bool pin_bottom = true;
    std::wstring toast;

    // Phase 5: the debug surface. Recalled hits and tool rounds belong to the
    // turn and are handed to the assistant message that answers it.
    bool show_trace = false;  // off until Trace is pressed
    bool show_stats = false;
    std::vector<MemoryHit> pending_hits;
    int pending_tokens = 0;
    std::vector<ToolRow> pending_tools;
    int stored_messages = 0;
    int stored_embedded = 0;
    std::string engine_phase = "booting";
};

int AppRun(HINSTANCE instance);

Metrics MetricsFor(const App& app);
D2D1_RECT_F ToastRect(App* app, const Metrics& m);
void AppRelayout(App* app);
void AppPaint(App* app);
void AppSend(App* app);
void AppStopTurn(App* app);
void AppResetConversation(App* app);
void AppScroll(App* app, float delta_dips);
void AppScrollToBottom(App* app);
void AppUpdateComposer(App* app);
void SyncAnimation(App* app);
void AppToggleTrace(App* app);
void AppRefreshStats(App* app);
bool AppHitChip(App* app, float x, float y);
void HandleEvent(App* app, const std::string& payload);

inline bool InRect(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}
