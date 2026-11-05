#include "app.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "json.h"
#include "net.h"
#include "text.h"
#include "theme.h"

#define WM_APP_SEND (WM_APP + 1)
#define WM_APP_STOP (WM_APP + 4)
#define WM_APP_NEW (WM_APP + 5)

namespace {

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI * Fn)(HANDLE);
    auto fn = (Fn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn && fn((HANDLE)-4)) return;  // per-monitor v2
    SetProcessDPIAware();
}

UINT SystemDpi() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef UINT(WINAPI * Fn)();
    auto fn = (Fn)GetProcAddress(user32, "GetDpiForSystem");
    return fn ? fn() : 96;
}

Com<IDWriteTextFormat> MakeFormat(IDWriteFactory* dwrite, const wchar_t* family,
                                  float size, DWRITE_FONT_WEIGHT weight,
                                  DWRITE_TEXT_ALIGNMENT align,
                                  DWRITE_PARAGRAPH_ALIGNMENT paragraph) {
    Com<IDWriteTextFormat> format;
    if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                                        format.out()))) {
        return format;
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(paragraph);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return format;
}

void CreateFormats(App* app) {
    app->body = MakeFormat(app->dwrite.p, L"Segoe UI", 14.0f,
                           DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                           DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    app->body_center = MakeFormat(app->dwrite.p, L"Segoe UI", 14.0f,
                                  DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_CENTER,
                                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    app->small = MakeFormat(app->dwrite.p, L"Segoe UI", 11.0f,
                            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
                            DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    app->small_mid = MakeFormat(app->dwrite.p, L"Segoe UI", 11.0f,
                                DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_TEXT_ALIGNMENT_LEADING,
                                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    app->small_center = MakeFormat(app->dwrite.p, L"Segoe UI", 11.0f,
                                   DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_TEXT_ALIGNMENT_CENTER,
                                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    app->small_right = MakeFormat(app->dwrite.p, L"Segoe UI", 11.0f,
                                  DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_TRAILING,
                                  DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    app->pill_format = MakeFormat(app->dwrite.p, L"Segoe UI", 13.0f,
                                  DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_TEXT_ALIGNMENT_CENTER,
                                  DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    app->title = MakeFormat(app->dwrite.p, L"Segoe UI Semibold", 20.0f,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD,
                            DWRITE_TEXT_ALIGNMENT_CENTER,
                            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void EnsureTarget(App* app) {
    if (app->target.p || !app->d2d.p) return;

    RECT client;
    GetClientRect(app->hwnd, &client);
    const D2D1_SIZE_U size = D2D1::SizeU((UINT32)(client.right - client.left),
                                         (UINT32)(client.bottom - client.top));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN));
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
        D2D1::HwndRenderTargetProperties(app->hwnd, size, D2D1_PRESENT_OPTIONS_NONE);

    if (FAILED(app->d2d->CreateHwndRenderTarget(props, hwnd_props,
                                               app->target.out()))) {
        return;
    }
    app->target->SetDpi((float)app->dpi, (float)app->dpi);
    app->target->CreateSolidColorBrush(theme::text, app->brush.out());
}

void UpdateFont(App* app) {
    if (app->edit_font) DeleteObject(app->edit_font);
    const int height = -MulDiv(14, (int)app->dpi, 96);
    app->edit_font = CreateFontW(
        height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (app->edit && app->edit_font) {
        SendMessageW(app->edit, WM_SETFONT, (WPARAM)app->edit_font, TRUE);
    }
}

void PositionEdit(App* app) {
    if (!app->edit) return;
    const Metrics m = MetricsFor(*app);
    const float scale = (float)app->dpi / 96.0f;
    const float pill_w = m.pill.right - m.pill.left;
    const float pill_h = m.pill.bottom - m.pill.top;
    const float x = m.pill.left + theme::bubble_pad_x;
    const float y = m.pill.top + 7.0f;
    SetWindowPos(app->edit, nullptr, (int)(x * scale), (int)(y * scale),
                 (int)((pill_w - theme::bubble_pad_x * 2.0f) * scale),
                 (int)((pill_h - 14.0f) * scale),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ClampScroll(App* app) {
    const Metrics m = MetricsFor(*app);
    const float max_scroll = std::max(0.0f, app->content_h - m.chat_h);
    app->scroll = std::clamp(app->scroll, 0.0f, max_scroll);
    app->scroll_target = std::clamp(app->scroll_target, 0.0f, max_scroll);
}

std::wstring Trimmed(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) return L"";
    std::wstring text((size_t)length + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize((size_t)length);

    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

void AttachTrace(App* app, Message& message) {
    if (message.role != Role::Assistant) return;
    // Called again when the reply lands, after the deltas already attached.
    // Moving from the emptied pending vectors would wipe the trace.
    if (app->pending_hits.empty() && app->pending_tools.empty()) return;
    message.hits = std::move(app->pending_hits);
    message.recall_tokens = app->pending_tokens;
    message.tools = std::move(app->pending_tools);
    app->pending_hits.clear();
    app->pending_tokens = 0;
    app->pending_tools.clear();
}

void EnsureStreaming(App* app) {
    if (app->streaming >= 0) return;
    app->chat.add(Role::Assistant, L"");
    app->streaming = (int)app->chat.messages().size() - 1;
    Message& message = app->chat.messages().back();
    message.streaming = true;
    AttachTrace(app, message);
}

void ApplyEvent(App* app, const json::Value& event) {
    const std::string type = event.str("type");

    if (type == "status") {
        const std::string phase = event.str("phase");
        const std::wstring text = Widen(event.str("text"));
        app->engine_phase = phase;
        if (const json::Value* models = event.find("models")) {
            app->chat_model = Widen(models->str("chat", "loading"));
            app->embed_model = Widen(models->str("embed", "loading"));
        }
        if (phase == "boot") {
            app->avatar = AvatarState::Booting;
        } else if (phase == "ready") {
            app->avatar = AvatarState::Idle;
        } else if (phase == "warning") {
            // The status line already shows the disabled embedder in red.
        } else if (phase == "error") {
            app->avatar = AvatarState::Error;
            app->toast = text;
            app->busy = false;
        }
    } else if (type == "state") {
        app->chat.clear();
        app->streaming = -1;
        app->pending_hits.clear();
        app->pending_tools.clear();
        app->pending_tokens = 0;
        const json::Value* messages = event.find("messages");
        if (messages && messages->type == json::Value::Type::Array) {
            for (const json::Value& item : messages->array) {
                const Role role =
                    item.str("role") == "user" ? Role::User : Role::Assistant;
                app->chat.add(role, Widen(item.str("text")));
            }
        }
        app->busy = event.flag("busy");
        const std::string phase = event.str("phase");
        app->engine_phase = phase;
        if (const json::Value* models = event.find("models")) {
            app->chat_model = Widen(models->str("chat", "loading"));
            app->embed_model = Widen(models->str("embed", "loading"));
        }
        if (phase == "ready") {
            app->avatar = AvatarState::Idle;
        } else if (phase == "error") {
            app->avatar = AvatarState::Error;
        } else {
            app->avatar = AvatarState::Booting;
        }
        const json::Value* stats = event.find("stats");
        if (stats) {
            app->stored_messages = (int)stats->num("messages");
            app->stored_embedded = (int)stats->num("embedded");
        }
        app->pin_bottom = true;
    } else if (type == "user") {
        app->pending_hits.clear();
        app->pending_tools.clear();
        app->pending_tokens = 0;
        app->chat.add(Role::User, Widen(event.str("text")));
    } else if (type == "delta") {
        EnsureStreaming(app);
        app->chat.messages()[(size_t)app->streaming].text +=
            Widen(event.str("text"));
        app->avatar = AvatarState::Talking;
    } else if (type == "recall") {
        app->pending_hits.clear();
        app->pending_tokens = (int)event.num("tokens");
        const json::Value* hits = event.find("hits");
        if (hits && hits->type == json::Value::Type::Array) {
            for (const json::Value& item : hits->array) {
                MemoryHit hit;
                hit.id = (long long)item.num("id");
                hit.role = Widen(item.str("role"));
                hit.at = Widen(item.str("at"));
                hit.score = item.num("score");
                hit.preview = Widen(item.str("preview"));
                app->pending_hits.push_back(std::move(hit));
            }
        }
    } else if (type == "tool") {
        const std::string phase = event.str("phase");
        app->avatar = AvatarState::Remembering;
        if (phase == "call") {
            ToolRow row;
            row.text = Widen(event.str("text"));
            app->pending_tools.push_back(std::move(row));
        } else if (phase == "result") {
            std::wstring text = Widen(event.str("result"));
            const size_t newline = text.find(L'\n');
            if (newline != std::wstring::npos) text.resize(newline);
            if (text.empty()) text = Widen(event.str("text"));
            ToolRow row;
            row.text = std::move(text);
            row.result = true;
            app->pending_tools.push_back(std::move(row));
        }
    } else if (type == "reply") {
        const std::wstring text = Widen(event.str("text"));
        const bool stopped = event.flag("stopped");
        if (app->streaming >= 0) {
            std::vector<Message>& messages = app->chat.messages();
            Message& message = messages[(size_t)app->streaming];
            if (!text.empty()) message.text = text;
            message.streaming = false;
            message.stopped = stopped;
            AttachTrace(app, message);
            if (message.text.empty()) {
                messages.erase(messages.begin() + app->streaming);
            }
            app->streaming = -1;
        } else if (!text.empty()) {
            app->chat.add(Role::Assistant, text);
            Message& message = app->chat.messages().back();
            message.stopped = stopped;
            AttachTrace(app, message);
        }
        app->busy = false;
        app->avatar = AvatarState::Idle;
        app->toast.clear();
    } else if (type == "cancelled") {
        if (app->streaming >= 0) {
            app->chat.messages()[(size_t)app->streaming].stopped = true;
        }
    } else if (type == "error") {
        app->chat.add(Role::Assistant,
                      L"engine error: " + Widen(event.str("message")));
        app->avatar = AvatarState::Error;
        app->busy = false;
    }

    app->layout_dirty = true;
    InvalidateRect(app->hwnd, nullptr, FALSE);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                          DWORD_PTR ref) {
    App* app = (App*)ref;
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;

    if (msg == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        // EM_SETCUEBANNER is ignored on a multiline edit, so the hint is
        // painted over the empty control instead.
        if (GetWindowTextLengthW(hwnd) == 0 && app->edit_font) {
            HDC dc = GetDC(hwnd);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int saved_mode = SetBkMode(dc, TRANSPARENT);
            const COLORREF saved_color =
                SetTextColor(dc, RGB(0x8A, 0x85, 0x93));
            HGDIOBJ saved_font = SelectObject(dc, app->edit_font);
            DrawTextW(dc, L"Enter message here...", -1, &rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, saved_font);
            SetTextColor(dc, saved_color);
            SetBkMode(dc, saved_mode);
            ReleaseDC(hwnd, dc);
        }
        return result;
    }

    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(hwnd, nullptr, TRUE);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            PostMessageW(app->hwnd, WM_APP_STOP, 0, 0);
            return 0;
        }
        if (control && (wp == 'N' || wp == 'n')) {
            PostMessageW(app->hwnd, WM_APP_NEW, 0, 0);
            return 0;
        }
        if (wp == VK_RETURN && !shift) {
            PostMessageW(app->hwnd, WM_APP_SEND, 0, 0);
            return 0;
        }
    }
    // The control would also turn the key press into a character; swallow it
    // so a plain Enter never inserts a line break.
    if (msg == WM_CHAR && wp == VK_RETURN && !shift) return 0;

    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            app = (App*)cs->lpCreateParams;
            app->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);

            app->edit_bg = CreateSolidBrush(RGB(0x26, 0x23, 0x2B));
            app->edit = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                    ES_WANTRETURN | ES_NOHIDESEL,
                0, 0, 10, 10, hwnd, (HMENU)100, app->instance, nullptr);
            SendMessageW(app->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
            SendMessageW(app->edit, EM_SETLIMITTEXT, 8000, 0);
            SetWindowSubclass(app->edit, EditProc, 1, (DWORD_PTR)app);
            UpdateFont(app);
            PositionEdit(app);
            SetFocus(app->edit);
            SetTimer(hwnd, 1, 33, nullptr);  // avatar pulse
            return 0;
        }

        case WM_SIZE: {
            if (!app) return 0;
            const int px_w = LOWORD(lp);
            const int px_h = HIWORD(lp);
            app->dip_w = px_w * 96.0f / (float)app->dpi;
            app->dip_h = px_h * 96.0f / (float)app->dpi;
            if (app->target.p) {
                app->target->Resize(D2D1::SizeU((UINT32)px_w, (UINT32)px_h));
            }
            PositionEdit(app);
            AppUpdateComposer(app);
            AppRelayout(app);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (app) {
                EnsureTarget(app);
                AppPaint(app);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_GETMINMAXINFO: {
            auto* info = (MINMAXINFO*)lp;
            const UINT dpi = app ? app->dpi : SystemDpi();
            RECT rc = {0, 0, MulDiv(380, (int)dpi, 96), MulDiv(600, (int)dpi, 96)};
            AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
            info->ptMinTrackSize.x = rc.right - rc.left;
            info->ptMinTrackSize.y = rc.bottom - rc.top;
            return 0;
        }

        case WM_DPICHANGED: {
            if (!app) return 0;
            app->dpi = HIWORD(wp);
            if (app->target.p) {
                app->target->SetDpi((float)app->dpi, (float)app->dpi);
            }
            const RECT* suggested = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateFont(app);
            PositionEdit(app);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!app) return 0;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const float x = pt.x * 96.0f / (float)app->dpi;
            const float y = pt.y * 96.0f / (float)app->dpi;
            const Metrics m = MetricsFor(*app);
            if (x >= 0.0f && x <= m.w && y >= m.chat_y &&
                y <= m.chat_y + m.chat_h) {
                const float steps = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f;
                AppScroll(app, -steps * 60.0f);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!app) return 0;
            const float x = GET_X_LPARAM(lp) * 96.0f / (float)app->dpi;
            const float y = GET_Y_LPARAM(lp) * 96.0f / (float)app->dpi;
            const Metrics m = MetricsFor(*app);

            if (app->show_stats) {
                app->show_stats = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!app->toast.empty() && InRect(ToastRect(app, m), x, y)) {
                app->toast.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (AppHitChip(app, x, y)) {
                SetFocus(app->edit);
                return 0;
            }

            if (InRect(m.send, x, y)) {
                if (app->busy) {
                    AppStopTurn(app);
                } else {
                    AppSend(app);
                }
            } else if (InRect(m.clear_btn, x, y) && !app->busy) {
                AppResetConversation(app);
            } else if (InRect(m.trace_btn, x, y)) {
                AppToggleTrace(app);
            } else if (InRect(m.gear, x, y)) {
                AppRefreshStats(app);
                app->show_stats = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetFocus(app->edit);
            return 0;
        }

        case WM_TIMER: {
            if (!app) return 0;
            bool repaint = false;
            if (app->avatar == AvatarState::Talking) {
                app->pulse += 0.045f;
                if (app->pulse > 1.0f) app->pulse -= 1.0f;
                repaint = true;
            }
            const float delta = app->scroll_target - app->scroll;
            if (std::fabs(delta) > 0.4f) {
                app->scroll += delta * 0.35f;
                repaint = true;
            } else if (delta != 0.0f) {
                app->scroll = app->scroll_target;
                repaint = true;
            }
            if (repaint) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_COMMAND:
            if (app && LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE) {
                InvalidateRect(app->edit, nullptr, TRUE);
                AppUpdateComposer(app);
            }
            return 0;

        case WM_KEYDOWN:
            if (app) {
                if (wp == VK_ESCAPE) AppStopTurn(app);
                if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                    (wp == 'N' || wp == 'n')) {
                    AppResetConversation(app);
                }
            }
            return 0;

        case WM_APP_STOP:
            if (app) AppStopTurn(app);
            return 0;

        case WM_APP_NEW:
            if (app) AppResetConversation(app);
            return 0;

        case WM_CTLCOLOREDIT: {
            if (!app) break;
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(0xEC, 0xEA, 0xEF));
            SetBkColor(dc, RGB(0x26, 0x23, 0x2B));
            return (LRESULT)app->edit_bg;
        }

        case WM_APP_SEND:
            if (app) AppSend(app);
            return 0;

        case WM_APP_EVENT: {
            auto* payload = (std::string*)lp;
            if (app && payload) HandleEvent(app, *payload);
            delete payload;
            return 0;
        }

        case WM_APP_NET:
            if (app) {
                app->connected = wp != 0;
                if (!app->connected && !app->closing &&
                    app->avatar != AvatarState::Booting) {
                    app->toast = L"engine disconnected";
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_SETFOCUS:
            if (app && app->edit) SetFocus(app->edit);
            return 0;

        case WM_CLOSE:
            if (app) app->closing = true;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            EventsStop();
            EngineStop();
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

Metrics MetricsFor(const App& app) {
    Metrics m;
    m.w = app.dip_w > 0.0f ? app.dip_w : 432.0f;
    m.h = app.dip_h > 0.0f ? app.dip_h : 721.0f;

    m.avatar_h = std::max(160.0f, m.h * theme::avatar_band_ratio);
    m.settings_y = m.avatar_h;
    m.chat_y = m.settings_y + theme::settings_h;
    m.composer_y = m.h - app.composer_h;
    m.chat_h = std::max(80.0f, m.composer_y - m.chat_y);

    // The two model lines own the top of the avatar band.
    constexpr float status_reserve = 46.0f;
    const float side = std::clamp(
        std::min(200.0f, m.avatar_h - status_reserve - 16.0f), 120.0f, 200.0f);
    const float avatar_x = (m.w - side) * 0.5f;
    const float avatar_y =
        status_reserve + (m.avatar_h - status_reserve - side) * 0.5f;
    m.avatar = D2D1::RectF(avatar_x, avatar_y, avatar_x + side, avatar_y + side);

    m.clear_btn = D2D1::RectF(12.0f, m.settings_y + 12.0f, 80.0f,
                            m.settings_y + 44.0f);
    m.trace_btn = D2D1::RectF(88.0f, m.settings_y + 12.0f, 166.0f,
                               m.settings_y + 44.0f);
    m.gear = D2D1::RectF(m.w - 44.0f, m.settings_y + 16.0f, m.w - 16.0f,
                         m.settings_y + 44.0f);

    m.pill = D2D1::RectF(12.0f, m.composer_y + 8.0f, m.w - 12.0f - 68.0f - 8.0f,
                         m.h - 8.0f);
    m.send = D2D1::RectF(m.w - 12.0f - 68.0f, m.h - 48.0f, m.w - 12.0f,
                         m.h - 8.0f);
    return m;
}

D2D1_RECT_F ToastRect(App* app, const Metrics& m) {
    if (app->toast.empty() || !app->dwrite.p || !app->small.p) {
        return D2D1::RectF();
    }
    const float text_w = m.w - 24.0f - 24.0f;
    float height = 38.0f;
    Com<IDWriteTextLayout> layout;
    if (SUCCEEDED(app->dwrite->CreateTextLayout(
            app->toast.c_str(), (UINT32)app->toast.size(), app->small.p,
            text_w, 400.0f, layout.out()))) {
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        height = std::clamp(metrics.height + 20.0f, 34.0f, 72.0f);
    }
    // Sits just above the composer, inside the chat band.
    return D2D1::RectF(12.0f, m.composer_y - height - 8.0f, m.w - 12.0f,
                       m.composer_y - 8.0f);
}

void AppRelayout(App* app) {
    if (!app->dwrite.p || !app->body.p) return;
    const Metrics m = MetricsFor(*app);
    app->content_h = LayoutChat(app->chat, app->dwrite.p, app->body.p, m.w,
                                app->show_trace, app->bubbles);
    ClampScroll(app);
    app->layout_dirty = false;
}

void AppScroll(App* app, float delta_dips) {
    const Metrics m = MetricsFor(*app);
    const float max_scroll = std::max(0.0f, app->content_h - m.chat_h);
    app->scroll_target =
        std::clamp(app->scroll_target + delta_dips, 0.0f, max_scroll);
    app->pin_bottom = app->scroll_target >= max_scroll - 1.0f;
    InvalidateRect(app->hwnd, nullptr, FALSE);
}

void AppScrollToBottom(App* app) {
    const Metrics m = MetricsFor(*app);
    const float bottom = std::max(0.0f, app->content_h - m.chat_h);
    app->scroll = bottom;
    app->scroll_target = bottom;
    app->pin_bottom = true;
}

void AppUpdateComposer(App* app) {
    if (!app->edit) return;
    // EM_GETLINECOUNT counts wrapped display lines, which is exactly what the
    // composer needs to grow by.
    const int lines = (int)SendMessageW(app->edit, EM_GETLINECOUNT, 0, 0);
    const int shown = std::clamp(lines, 1, 5);
    const float wanted = theme::composer_h + (shown - 1) * 19.0f;
    if (std::fabs(wanted - app->composer_h) < 0.5f) return;
    app->composer_h = wanted;
    PositionEdit(app);
    AppRelayout(app);
    InvalidateRect(app->hwnd, nullptr, FALSE);
}

void AppSend(App* app) {
    if (app->busy) return;
    const std::wstring text = Trimmed(app->edit);
    if (text.empty()) return;

    if (!ApiSend(text)) {
        app->chat.add(Role::Assistant,
                      L"Could not reach the engine service. Is it running?");
        app->avatar = AvatarState::Error;
        app->layout_dirty = true;
        InvalidateRect(app->hwnd, nullptr, FALSE);
        return;
    }

    SetWindowTextW(app->edit, L"");
    AppUpdateComposer(app);  // SetWindowText does not send EN_CHANGE
    app->busy = true;
    app->avatar = AvatarState::Thinking;
    app->toast.clear();
    app->pin_bottom = true;
    InvalidateRect(app->hwnd, nullptr, FALSE);
    SetFocus(app->edit);
}

void AppStopTurn(App* app) {
    if (app->busy) ApiStopTurn();
}

void AppResetConversation(App* app) {
    if (app->busy) return;
    if (!ApiReset()) {
        app->avatar = AvatarState::Error;
        app->toast = L"could not reset the conversation";
        InvalidateRect(app->hwnd, nullptr, FALSE);
        return;
    }
    app->chat.clear();
    app->bubbles.clear();
    app->content_h = 0.0f;
    app->scroll = 0.0f;
    app->scroll_target = 0.0f;
    app->streaming = -1;
    app->pending_hits.clear();
    app->pending_tools.clear();
    app->pending_tokens = 0;
    app->pin_bottom = true;
    app->layout_dirty = true;
    app->avatar = AvatarState::Idle;
    app->toast.clear();
    InvalidateRect(app->hwnd, nullptr, FALSE);
}

void AppToggleTrace(App* app) {
    app->show_trace = !app->show_trace;
    AppRelayout(app);
    InvalidateRect(app->hwnd, nullptr, FALSE);
}

void AppRefreshStats(App* app) {
    std::string body;
    if (!ApiState(body)) return;
    json::Value state;
    if (!json::Parse(body, state)) return;
    app->engine_phase = state.str("phase");
    app->busy = state.flag("busy");
    const json::Value* stats = state.find("stats");
    if (stats) {
        app->stored_messages = (int)stats->num("messages");
        app->stored_embedded = (int)stats->num("embedded");
    }
}

bool AppHitChip(App* app, float x, float y) {
    if (!app->show_trace) return false;
    const Metrics m = MetricsFor(*app);
    const float content_y = y - m.chat_y + app->scroll;
    for (const Bubble& bubble : app->bubbles) {
        if (!bubble.has_chip) continue;
        if (x < bubble.x || x > bubble.x + bubble.w) continue;
        const float top = bubble.y + bubble.chip_top;
        if (content_y < top || content_y > top + theme::chip_h) continue;
        std::vector<Message>& messages = app->chat.messages();
        if (bubble.index >= 0 && bubble.index < (int)messages.size()) {
            messages[(size_t)bubble.index].expanded =
                !messages[(size_t)bubble.index].expanded;
            AppRelayout(app);
            InvalidateRect(app->hwnd, nullptr, FALSE);
        }
        return true;
    }
    return false;
}

void HandleEvent(App* app, const std::string& payload) {
    json::Value event;
    if (!json::Parse(payload, event)) return;
    ApplyEvent(app, event);
}

int AppRun(HINSTANCE instance) {
    EnableDpiAwareness();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  16, 16, LR_DEFAULTCOLOR);
    wc.lpszClassName = L"HashtaPanel";
    if (!RegisterClassExW(&wc)) return 1;

    App app;
    app.instance = instance;
    app.dpi = SystemDpi();

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory), nullptr,
                                 (void**)app.d2d.out()))) {
        return 1;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   (IUnknown**)app.dwrite.out()))) {
        return 1;
    }
    CreateFormats(&app);

    RECT rc = {0, 0, MulDiv(432, (int)app.dpi, 96),
               MulDiv(721, (int)app.dpi, 96)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int frame_w = rc.right - rc.left;
    const int frame_h = rc.bottom - rc.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - frame_w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - frame_h) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hashta",
                                WS_OVERLAPPEDWINDOW, x, y, frame_w, frame_h,
                                nullptr, nullptr, instance, &app);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    AppRelayout(&app);
    AppScrollToBottom(&app);
    UpdateWindow(hwnd);

    if (!EngineStart()) {
        app.avatar = AvatarState::Error;
        app.toast = L"could not start engine_server.py";
    } else {
        EventsStart(hwnd);
    }
    InvalidateRect(hwnd, nullptr, FALSE);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (app.edit_font) DeleteObject(app.edit_font);
    if (app.edit_bg) DeleteObject(app.edit_bg);
    return 0;
}
