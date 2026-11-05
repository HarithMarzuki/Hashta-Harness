#include <algorithm>
#include <cmath>
#include <cwchar>

#include "app.h"
#include "text.h"
#include "theme.h"

namespace {

void FillRound(App* app, const D2D1_RECT_F& rect, float radius,
               D2D1_COLOR_F color) {
    app->brush->SetColor(color);
    app->target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius),
                                      app->brush.p);
}

void Label(App* app, IDWriteTextFormat* format, const wchar_t* text,
           const D2D1_RECT_F& rect, D2D1_COLOR_F color) {
    app->brush->SetColor(color);
    app->target->DrawText(text, (UINT32)wcslen(text), format, rect,
                          app->brush.p, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// "2026-09-02T02:00:00+08:00 (Wed)" -> "2026-09-02 02:00".
std::wstring ShortStamp(const std::wstring& stamp) {
    if (stamp.size() < 16) return stamp;
    std::wstring out = stamp.substr(0, 16);
    out[10] = L' ';
    return out;
}

std::wstring Capitalized(const std::wstring& value) {
    std::wstring out = value;
    if (!out.empty() && out[0] >= L'a' && out[0] <= L'z') out[0] -= 32;
    return out;
}

D2D1_COLOR_F ModelColor(const std::wstring& state) {
    if (state == L"ready") return theme::rgb(0x34D399);    // green
    if (state == L"loading") return theme::rgb(0xF59E0B);  // amber
    return theme::rgb(0xF87171);                           // waiting / failed
}

void DrawModelStatus(App* app, const Metrics& m) {
    const std::wstring chat =
        L"Language model: " + Capitalized(app->chat_model);
    const std::wstring embed = L"Embedder: " + Capitalized(app->embed_model);
    Label(app, app->small.p, chat.c_str(),
          D2D1::RectF(14.0f, 10.0f, m.w - 14.0f, 27.0f),
          ModelColor(app->chat_model));
    Label(app, app->small.p, embed.c_str(),
          D2D1::RectF(14.0f, 29.0f, m.w - 14.0f, 46.0f),
          ModelColor(app->embed_model));
}

void DrawAvatar(App* app, const Metrics& m) {
    D2D1_RECT_F box = m.avatar;
    if (app->avatar == AvatarState::Talking) {
        // A gentle breathe while words are arriving; the only motion in the
        // panel until real sprites exist.
        const float scale = 1.0f + 0.025f * std::sin(app->pulse * 6.2831853f);
        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        const float hw = (box.right - box.left) * 0.5f * scale;
        const float hh = (box.bottom - box.top) * 0.5f * scale;
        box = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
    }

    const float side = box.right - box.left;
    FillRound(app, box, side * 0.11f, avatar_color(app->avatar));

    // The state name is the whole point of the placeholder: it proves the
    // state machine is driving the draw before any sprite exists. Nothing
    // else is ever drawn in here; errors live in the toast.
    Label(app, app->title.p, avatar_name(app->avatar), box, theme::accent_ink);
}

void DrawToast(App* app, const Metrics& m) {
    if (app->toast.empty()) return;
    const D2D1_RECT_F box = ToastRect(app, m);
    if (box.right <= box.left) return;

    FillRound(app, box, 10.0f, theme::rgb(0x3A1F22));
    app->target->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    Com<IDWriteTextLayout> layout;
    if (SUCCEEDED(app->dwrite->CreateTextLayout(
            app->toast.c_str(), (UINT32)app->toast.size(), app->small.p,
            box.right - box.left - 24.0f, box.bottom - box.top,
            layout.out()))) {
        app->brush->SetColor(theme::rgb(0xF8A0A0));
        app->target->DrawTextLayout(D2D1::Point2F(box.left + 12.0f,
                                                  box.top + 9.0f),
                                    layout.p, app->brush.p,
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    app->target->PopAxisAlignedClip();
}

void DrawSettings(App* app, const Metrics& m) {
    FillRound(app, D2D1::RectF(0.0f, m.settings_y, m.w,
                               m.settings_y + theme::settings_h),
              0.0f, theme::panel);

    FillRound(app, m.clear_btn, 16.0f, theme::bubble);
    FillRound(app, m.trace_btn, 16.0f,
              app->show_trace ? theme::bubble_user : theme::bubble);
    Label(app, app->pill_format.p, L"Clear", m.clear_btn, theme::text);
    Label(app, app->pill_format.p, L"Trace", m.trace_btn,
          app->show_trace ? theme::text : theme::dim);

    const float cy = (m.gear.top + m.gear.bottom) * 0.5f;
    app->brush->SetColor(app->connected ? theme::accent : theme::dim);
    app->target->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(m.gear.left - 12.0f, cy), 4.0f, 4.0f),
        app->brush.p);

    const D2D1_POINT_2F gear_center = D2D1::Point2F(
        (m.gear.left + m.gear.right) * 0.5f, (m.gear.top + m.gear.bottom) * 0.5f);
    app->brush->SetColor(app->show_stats ? theme::text : theme::dim);
    app->target->DrawEllipse(D2D1::Ellipse(gear_center, 7.5f, 7.5f),
                             app->brush.p, 1.5f);
    app->target->FillEllipse(D2D1::Ellipse(gear_center, 2.5f, 2.5f),
                             app->brush.p);
}

void DrawBubbleTrace(App* app, const Bubble& bubble, const Message& message,
                     float top) {
    const float left = bubble.x + theme::bubble_pad_x;
    const float right = bubble.x + bubble.w - theme::bubble_pad_x;

    if (bubble.tools_top >= 0.0f) {
        float y = top + bubble.tools_top;
        for (const ToolRow& row : message.tools) {
            const std::wstring line =
                (row.result ? L"\u2192 " : L"\u00b7 ") + row.text;
            Label(app, app->small_mid.p, line.c_str(),
                  D2D1::RectF(left, y, right, y + theme::tool_row_h),
                  theme::dim);
            y += theme::tool_row_h;
        }
    }

    if (bubble.has_chip) {
        const float y = top + bubble.chip_top;
        const D2D1_RECT_F chip =
            D2D1::RectF(bubble.x + 8.0f, y, bubble.x + bubble.w - 8.0f,
                        y + theme::chip_h);
        FillRound(app, chip, theme::chip_h * 0.5f, theme::chip_fill);

        wchar_t text[96];
        // %ls: in a wide printf, %s means a *multibyte* string per ISO C.
        std::swprintf(text, 96, L"%d memories \u00b7 %d tokens  %ls",
                      (int)message.hits.size(), message.recall_tokens,
                      message.expanded ? L"\u2212" : L"+");
        Label(app, app->small_mid.p, text,
              D2D1::RectF(chip.left + 10.0f, chip.top, chip.right - 8.0f,
                          chip.bottom),
              theme::dim);
    }

    if (bubble.hits_top >= 0.0f) {
        float y = top + bubble.hits_top;
        for (const MemoryHit& hit : message.hits) {
            wchar_t head[160];
            std::swprintf(head, 160, L"%.2f  %ls  %ls", hit.score,
                          ShortStamp(hit.at).c_str(), hit.role.c_str());
            Label(app, app->small_mid.p, head,
                  D2D1::RectF(left + 6.0f, y, right, y + 15.0f), theme::dim);
            Label(app, app->small.p, hit.preview.c_str(),
                  D2D1::RectF(left + 6.0f, y + 15.0f, right, y + 31.0f),
                  theme::text);
            y += theme::hit_row_h;
        }
    }

    if (bubble.stopped) {
        const D2D1_RECT_F marker =
            D2D1::RectF(bubble.x + 8.0f, top + bubble.stopped_top,
                        bubble.x + bubble.w - 10.0f,
                        top + bubble.stopped_top + 14.0f);
        Label(app, app->small_right.p, L"stopped", marker, theme::dim);
    }
}

void DrawChat(App* app, const Metrics& m) {
    if (app->layout_dirty) AppRelayout(app);
    if (app->pin_bottom) {
        const float bottom = std::max(0.0f, app->content_h - m.chat_h);
        app->scroll = bottom;
        app->scroll_target = bottom;
    }

    const D2D1_RECT_F clip =
        D2D1::RectF(0.0f, m.chat_y, m.w, m.chat_y + m.chat_h);
    app->target->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (const Bubble& bubble : app->bubbles) {
        const float top = m.chat_y + bubble.y - app->scroll;
        if (top > clip.bottom || top + bubble.h < clip.top) continue;

        const D2D1_COLOR_F fill =
            bubble.role == Role::User ? theme::bubble_user : theme::bubble;
        FillRound(app, D2D1::RectF(bubble.x, top, bubble.x + bubble.w,
                                   top + bubble.h),
                  18.0f, fill);

        app->brush->SetColor(theme::text);
        app->target->DrawTextLayout(
            D2D1::Point2F(bubble.x + theme::bubble_pad_x,
                          top + theme::bubble_pad_y),
            bubble.layout.p, app->brush.p, D2D1_DRAW_TEXT_OPTIONS_CLIP);

        if (bubble.index >= 0 &&
            bubble.index < (int)app->chat.messages().size()) {
            DrawBubbleTrace(app, bubble, app->chat.messages()[bubble.index],
                            top);
        }
    }

    app->target->PopAxisAlignedClip();
}

void DrawComposer(App* app, const Metrics& m) {
    FillRound(app, D2D1::RectF(0.0f, m.composer_y, m.w, m.h), 0.0f,
              theme::panel);
    FillRound(app, m.pill, 20.0f, theme::bubble);
    FillRound(app, m.send, 20.0f, app->busy ? theme::dim : theme::accent);

    if (app->busy) {
        // A square reads as "stop" where the arrow reads as "send".
        const float cx = (m.send.left + m.send.right) * 0.5f;
        const float cy = (m.send.top + m.send.bottom) * 0.5f;
        FillRound(app, D2D1::RectF(cx - 6.0f, cy - 6.0f, cx + 6.0f, cy + 6.0f),
                  2.5f, theme::bg);
    } else {
        Label(app, app->body_center.p, L"\u2192", m.send, theme::accent_ink);
    }
}

void DrawStats(App* app, const Metrics& m) {
    app->brush->SetColor(theme::scrim);
    app->target->FillRectangle(
        D2D1::RectF(0.0f, m.chat_y, m.w, m.chat_y + m.chat_h), app->brush.p);

    const D2D1_RECT_F card = D2D1::RectF(28.0f, m.chat_y + 36.0f, m.w - 28.0f,
                                         m.chat_y + 236.0f);
    FillRound(app, card, 14.0f, theme::panel);

    const float left = card.left + 18.0f;
    const float right = card.right - 18.0f;
    float y = card.top + 14.0f;

    Label(app, app->pill_format.p, L"Panel",
          D2D1::RectF(left, y, right, y + 20.0f), theme::text);
    y += 28.0f;

    struct Row {
        const wchar_t* name;
        std::wstring value;
    };
    wchar_t turns[32];
    std::swprintf(turns, 32, L"%d", (int)app->chat.messages().size());
    wchar_t stored[32];
    std::swprintf(stored, 32, L"%d", app->stored_messages);
    wchar_t embedded[32];
    std::swprintf(embedded, 32, L"%d", app->stored_embedded);

    const Row rows[] = {
        {L"engine", Widen(app->engine_phase)},
        {L"link", app->connected ? L"connected" : L"disconnected"},
        {L"avatar", avatar_name(app->avatar)},
        {L"live turns", turns},
        {L"stored", stored},
        {L"embedded", embedded},
    };
    for (const Row& row : rows) {
        Label(app, app->small_mid.p, row.name,
              D2D1::RectF(left, y, left + 96.0f, y + 20.0f), theme::dim);
        Label(app, app->small_mid.p, row.value.c_str(),
              D2D1::RectF(left + 100.0f, y, right, y + 20.0f), theme::text);
        y += 21.0f;
    }

    Label(app, app->small.p, L"click anywhere to close",
          D2D1::RectF(left, card.bottom - 26.0f, right, card.bottom - 8.0f),
          theme::dim);
}

}  // namespace

void AppPaint(App* app) {
    if (!app->target.p || !app->brush.p) return;

    app->target->BeginDraw();
    app->target->Clear(theme::bg);

    const Metrics m = MetricsFor(*app);
    DrawModelStatus(app, m);
    DrawAvatar(app, m);
    DrawSettings(app, m);
    DrawChat(app, m);
    DrawToast(app, m);
    DrawComposer(app, m);
    if (app->show_stats) DrawStats(app, m);

    const HRESULT hr = app->target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        app->brush.reset();
        app->target.reset();
    }
}
