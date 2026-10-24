#pragma once
#include "ScreenBuffer.h"
#include "EbcdicCodec.h"
#include <functional>
#include <string>

namespace x3270 {

/// What pasteText() needs from a keyboard (3270 or 5250).
struct PasteOps {
    std::function<bool()>        locked;
    /// Overlay one character at the cursor and move right one position,
    /// with no auto-skip.  On an attribute or protected position the
    /// character is dropped and the cursor still moves right.
    std::function<void(uint8_t)> overlayChar;
    std::function<void()>        tab;
    std::function<void()>        cursorLeft;
};

/// Paste text as x3270 does by default (kybd.c emulate_uinput with pasting
/// and its default overlayPaste toggle, which implies marginedPaste): the
/// text is laid over the screen as a block whose left edge is the column the
/// paste started in.
///   - characters overlay the screen; protected positions drop them
///   - a newline moves to the start column of the next row (ignored right
///     after the text wrapped there, and as the last character)
///   - wherever the cursor ends up left of the start column, it moves right
///     to that column
///   - returns are ignored, tab is Tab, form feed a space, backspace Left
/// Pasting stops when the keyboard locks (e.g. a numeric-only field rejects
/// a character), at a newline on the last row, or when the cursor wraps back
/// above where it started.
///
/// Returns the number of characters of `text` that were used.
inline size_t pasteText(ScreenBuffer& screen, const EbcdicCodec& codec,
                        const std::u32string& text, const PasteOps& k) {
    const int cols     = screen.cols();
    const int origAddr = screen.cursorPos();
    const int origCol  = origAddr % cols;
    int  lastAddr    = origAddr;
    int  lastRow     = origAddr / cols;
    bool justWrapped = false;

    for (size_t i = 0; i < text.size(); ++i) {
        if (k.locked()) return i;
        int cur = screen.cursorPos();
        if (cur < origAddr) return i;                          // wrapped to the top
        if (cur % cols < origCol) {                            // back to the margin
            cur = (cur / cols) * cols + origCol;
            screen.setCursor(cur);
        }
        if (cur != lastAddr) {
            lastAddr    = cur;
            justWrapped = cur / cols != lastRow;
            lastRow     = cur / cols;
        }

        switch (char32_t c = text[i]) {
        case U'\r':                                             break;
        case U'\b': k.cursorLeft();                             break;
        case U'\t': k.tab();                                    break;
        case U'\f': k.overlayChar(EbcdicCodec::EBCDIC_SPACE);   break;
        case U'\n':
            if (i + 1 == text.size()) return text.size();      // trailing newline
            if (!justWrapped) {
                int row = screen.cursorPos() / cols;
                if (row >= screen.rows() - 1) return i;
                screen.setCursor((row + 1) * cols);
            }
            lastRow     = screen.cursorPos() / cols;
            justWrapped = false;
            break;
        default: {
            int e = codec.fromUnicode(c);
            k.overlayChar(e >= 0 ? static_cast<uint8_t>(e) : 0x6F);   // unmapped: '?'
            break;
        }
        }
    }
    return text.size();
}

} // namespace x3270
