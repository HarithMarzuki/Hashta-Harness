// The message list and its layout. No drawing here: these are plain rects in
// DIPs that the renderer fills.
#pragma once

#include <dwrite.h>

#include <string>
#include <vector>

#include "com.h"

enum class Role { User, Assistant };

// One message the harness recalled for this turn, as the engine reported it.
struct MemoryHit {
    long long id = 0;
    std::wstring role;
    std::wstring at;
    double score = 0.0;
    std::wstring preview;
};

// One search_memory round, shown under the reply it belongs to.
struct ToolRow {
    std::wstring text;
    bool result = false;
};

struct Message {
    Role role = Role::Assistant;
    std::wstring text;
    bool streaming = false;
    bool stopped = false;

    // Debug surface, filled from `recall` and `tool` events.
    std::vector<MemoryHit> hits;
    int recall_tokens = 0;
    std::vector<ToolRow> tools;
    bool expanded = false;
};

struct Bubble {
    Role role = Role::Assistant;
    int index = 0;   // into ChatModel::messages()
    float x = 0.0f;  // content space, top-left
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    bool stopped = false;
    bool has_chip = false;
    float tools_top = -1.0f;  // -1 means absent, else offset from bubble top
    float chip_top = -1.0f;
    float hits_top = -1.0f;
    float stopped_top = -1.0f;
    Com<IDWriteTextLayout> layout;
};

class ChatModel {
public:
    void add(Role role, std::wstring text);
    void clear();

    std::vector<Message>& messages() { return messages_; }
    const std::vector<Message>& messages() const { return messages_; }

private:
    std::vector<Message> messages_;
};

// Wraps every message at `width` DIPs and returns the total content height.
// Fills `out` with one rect per message, in content space. `show_trace` hides
// the memory chips and tool rows when the user turns them off.
float LayoutChat(ChatModel& chat, IDWriteFactory* dwrite,
                 IDWriteTextFormat* format, float width, bool show_trace,
                 std::vector<Bubble>& out);
