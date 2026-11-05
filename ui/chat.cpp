#include "chat.h"

#include <algorithm>

#include "theme.h"

namespace {

// Extra room under the text for the "stopped" marker.
constexpr float kStoppedMarker = 15.0f;
constexpr float kToolRow = 15.0f;
constexpr float kChipHeight = 18.0f;
constexpr float kHitRow = 34.0f;

}  // namespace

void ChatModel::add(Role role, std::wstring text) {
    Message message;
    message.role = role;
    message.text = std::move(text);
    messages_.push_back(std::move(message));
}

void ChatModel::clear() {
    messages_.clear();
}

float LayoutChat(ChatModel& chat, IDWriteFactory* dwrite,
                 IDWriteTextFormat* format, float width, bool show_trace,
                 std::vector<Bubble>& out) {
    out.clear();
    if (width <= 0.0f) return 0.0f;

    const float content = width - theme::gutter * 2.0f;
    const float max_bubble = content * theme::bubble_max_ratio;
    const float max_text = std::max(40.0f, max_bubble - theme::bubble_pad_x * 2.0f);

    float y = 10.0f;
    for (size_t index = 0; index < chat.messages().size(); ++index) {
        Message& message = chat.messages()[index];

        Com<IDWriteTextLayout> layout;
        if (FAILED(dwrite->CreateTextLayout(
                message.text.c_str(), (UINT32)message.text.size(), format,
                max_text, 6000.0f, layout.out()))) {
            continue;
        }

        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);

        const bool has_tools = show_trace && !message.tools.empty();
        const bool has_chip = show_trace && !message.hits.empty() &&
                              message.role == Role::Assistant;

        float bubble_w = metrics.widthIncludingTrailingWhitespace +
                         theme::bubble_pad_x * 2.0f;
        // A one-line reply with a chip or tool rows would otherwise wrap them
        // into a narrow column; widen it enough for the trace to be readable.
        if (has_tools || has_chip) {
            bubble_w = std::max(bubble_w, theme::trace_min_width);
        }
        bubble_w = std::clamp(bubble_w, 60.0f, max_bubble);

        Bubble bubble;
        bubble.role = message.role;
        bubble.index = (int)index;
        bubble.stopped = message.stopped;
        bubble.layout = std::move(layout);

        float cursor = theme::bubble_pad_y + metrics.height;
        if (has_tools) {
            cursor += 6.0f;
            bubble.tools_top = cursor;
            cursor += message.tools.size() * kToolRow;
        }
        if (has_chip) {
            cursor += 8.0f;
            bubble.has_chip = true;
            bubble.chip_top = cursor;
            cursor += kChipHeight;
            if (message.expanded) {
                cursor += 2.0f;
                bubble.hits_top = cursor;
                cursor += message.hits.size() * kHitRow + 4.0f;
            }
        }
        if (message.stopped) {
            cursor += 2.0f;
            bubble.stopped_top = cursor;
            cursor += kStoppedMarker - 2.0f;
        }
        cursor += theme::bubble_pad_y;

        bubble.w = bubble_w;
        bubble.h = cursor;
        bubble.x = message.role == Role::User
                       ? width - theme::gutter - bubble_w
                       : theme::gutter;
        bubble.y = y;
        out.push_back(std::move(bubble));
        y += cursor + theme::chat_gap;
    }
    return y;
}
