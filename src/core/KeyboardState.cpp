#include "KeyboardState.h"
#include "Paste.h"
#include <algorithm>

namespace x3270 {

KeyboardState::KeyboardState(ScreenBuffer& screen, EbcdicCodec& codec)
    : screen_(screen), codec_(codec) {}

// ── Lock/unlock ───────────────────────────────────────────────────────────────
void KeyboardState::lock(LockReason reason) {
    lockReason_ = reason;
}

void KeyboardState::unlock() {
    lockReason_ = LockReason::None;
    insertMode_ = false;
}

// ── Internal helpers ──────────────────────────────────────────────────────────
uint8_t KeyboardState::currentFieldAttr() const {
    int pos = screen_.cursorPos();
    // Walk back to find the governing FA
    for (int i = 0; i < screen_.size(); ++i) {
        int p = (pos - i + screen_.size()) % screen_.size();
        if (screen_.at(p).isFA) return screen_.at(p).attr;
    }
    return 0x00; // unformatted screen — default unprotected
}

bool KeyboardState::isCurrentFieldEditable() const {
    uint8_t attr = currentFieldAttr();
    bool prot    = (attr & FA_PROTECTED) != 0;
    bool numeric = (attr & FA_NUMERIC)   != 0;
    bool skip    = prot && numeric;
    return !prot && !skip;
}

bool KeyboardState::requireEditableCursor() {
    // An attribute position is protected even when it starts an input field.
    if (screen_.at(screen_.cursorPos()).isFA || !isCurrentFieldEditable()) {
        lock(LockReason::OErr);
        return false;
    }
    return true;
}

bool KeyboardState::readyToMoveCursor() {
    if (lockReason_ == LockReason::OErr) lockReason_ = LockReason::None;
    return !isLocked();
}

// First character of the next unprotected field after `from` (exclusive of
// fields with no character positions), or -1 if there is none.
void KeyboardState::moveCursorToFirstUnprotected() {
    int pos = screen_.nextUnprotected(0);
    screen_.setCursor(pos >= 0 ? pos : 0);
}

void KeyboardState::advanceToNextField(bool forward) {
    int cur   = screen_.cursorPos();
    int delta = forward ? 1 : -1;
    for (int i = 1; i <= screen_.size(); ++i) {
        int pos = (cur + delta * i + screen_.size() * 2) % screen_.size();
        const Cell& c = screen_.at(pos);
        if (c.isFA && !c.isProtected() && !screen_.at((pos + 1) % screen_.size()).isFA) {
            int target = (pos + 1) % screen_.size();
            // BackTab: if we are already at the first character of this
            // field (target == cur), this is the *current* field's FA --
            // keep scanning backward to find the previous input field.
            if (!forward && target == cur) continue;
            screen_.setCursor(target);
            return;
        }
    }
}

void KeyboardState::moveCharacter(int from, int to) {
    const Cell& src = screen_.at(from);
    Cell&       dst = screen_.at(to);
    dst.ch        = src.ch;
    dst.fgColor   = src.fgColor;
    dst.bgColor   = src.bgColor;
    dst.highlight = src.highlight;
}

int KeyboardState::lastPosInField(int pos) const {
    const int sz = screen_.size();
    for (int i = 1; i < sz; ++i) {
        int p = (pos + i) % sz;
        if (screen_.at(p).isFA) return (p - 1 + sz) % sz;
    }
    // No other attribute: `pos` is the only FA (its field wraps round to
    // it), or the screen is unformatted and ends at the last position.
    return screen_.at(pos).isFA ? (pos - 1 + sz) % sz : sz - 1;
}

bool KeyboardState::insertCharAtCursor(uint8_t ebcdic, bool autoSkip) {
    int cur = screen_.cursorPos();
    if (insertMode_) {
        // If the last character in the field is non-null the field is full —
        // lock with OErr (same behaviour as a real 3270 in insert mode).
        int lastCell = lastPosInField(cur);
        if (lastCell != cur && screen_.at(lastCell).ch != 0x00) {
            lock(LockReason::OErr);
            return false;
        }
        // Shift cells right from lastCell back to cur
        int shiftEnd = lastCell;
        while (shiftEnd != cur) {
            int prev = (shiftEnd - 1 + screen_.size()) % screen_.size();
            moveCharacter(prev, shiftEnd);
            shiftEnd = prev;
        }
    }

    // A typed character has no character attributes of its own: it takes
    // the field's colour and highlighting, as in x3270.
    Cell& typed = screen_.at(cur);
    typed.ch = ebcdic;
    typed.fgColor = typed.bgColor = typed.highlight = 0x00;
    screen_.setMDT(cur);
    screen_.markDirty();

    // Advance cursor.  After the last position of a field the cursor moves
    // past the next attribute byte; if that field is autoskip (protected +
    // numeric) it tabs on to the next unprotected field.
    int next = (cur + 1) % screen_.size();
    if (!autoSkip) {
        screen_.setCursor(next);
    } else if (screen_.at(next).isFA && screen_.at(next).isSkip()) {
        screen_.setCursor(next);
        advanceToNextField(true);
    } else {
        while (screen_.at(next).isFA && next != cur)
            next = (next + 1) % screen_.size();
        screen_.setCursor(next);
    }
    return true;
}

// ── AID transmission ──────────────────────────────────────────────────────────
void KeyboardState::sendAID(uint8_t aidCode) {
    // PA keys and CLEAR are a "short read" (the AID byte alone); the
    // record builder handles that.  Later host reads report this AID.
    std::vector<uint8_t> logCopy;
    auto record = screen_.buildReadModifiedRecord(aidCode, false, &logCopy);
    screen_.setAid(aidCode);
    if (sendCb_ && sendCb_(record, logCopy)) lock(LockReason::System);
}

// ── Key handlers ──────────────────────────────────────────────────────────────
bool KeyboardState::handleChar(uint32_t unicodeChar) {
    int ebcdic = codec_.fromUnicode(unicodeChar);
    if (ebcdic < 0) return false;   // not in this code page
    return handleEbcdicChar(static_cast<uint8_t>(ebcdic));
}

bool KeyboardState::handleEbcdicChar(uint8_t ebcdic) {
    if (isLocked()) {
        // Only escalate to OErr when we're in a normal System lock;
        // Connecting and OErr states must not be overwritten.
        if (lockReason_ == LockReason::System)
            lock(LockReason::OErr);
        return false;
    }
    if (!requireEditableCursor()) return false;
    return insertCharAtCursor(ebcdic);
}

bool KeyboardState::handleTab(bool backward) {
    if (!readyToMoveCursor()) return false;
    advanceToNextField(!backward);
    return true;
}

bool KeyboardState::handleEnter() {
    if (isLocked()) return false;
    insertMode_ = false;
    sendAID(AID_ENTER);
    return true;
}

bool KeyboardState::handleClear() {
    // Like the other AID keys, Clear is rejected while input is inhibited
    // (Reset first).  It returns the screen to the default (24×80) size.
    if (isLocked()) return false;
    insertMode_ = false;
    screen_.setAlternateSize(false);
    sendAID(AID_CLEAR);
    return true;
}

bool KeyboardState::handlePF(int n) {
    if (isLocked()) return false;
    sendAID(pfAID(n));
    return true;
}

bool KeyboardState::handlePA(int n) {
    if (isLocked()) return false;
    uint8_t aidCode = (n == 1) ? AID_PA1 : (n == 2) ? AID_PA2 : AID_PA3;
    sendAID(aidCode);
    return true;
}

bool KeyboardState::handleBackspace() {
    // As x3270's Erase: nothing at the first character of a field;
    // otherwise move left and Delete there, which shifts the rest of the
    // field left (and is an operator error on a protected position).
    if (isLocked()) return false;
    int cur  = screen_.cursorPos();
    int prev = (cur - 1 + screen_.size()) % screen_.size();
    if (cur != 0 && !screen_.at(cur).isFA && screen_.at(prev).isFA) return true;
    screen_.setCursor(prev);
    return handleDelete();
}

bool KeyboardState::handleDelete() {
    if (isLocked()) return false;
    if (!requireEditableCursor()) return false;
    int cur = screen_.cursorPos();
    // Shift everything left within the field
    int last = lastPosInField(cur);
    for (int pos = cur; pos != last; pos = (pos + 1) % screen_.size())
        moveCharacter((pos + 1) % screen_.size(), pos);
    screen_.at(last).ch = 0x00;   // keeps its character attributes, as in x3270
    screen_.setMDT(cur);
    screen_.markDirty();
    return true;
}

bool KeyboardState::handleHome() {
    if (!readyToMoveCursor()) return false;
    moveCursorToFirstUnprotected();
    return true;
}

bool KeyboardState::handleFieldEnd() {
    if (!readyToMoveCursor()) return false;
    int cur = screen_.cursorPos();
    if (screen_.at(cur).isFA) return true;
    int fa = screen_.findFieldStart(cur);
    // Does nothing on an unformatted screen, an attribute or a protected field.
    if (fa < 0 || screen_.at(fa).isProtected()) return true;
    const int sz  = screen_.size();
    const int end = lastPosInField(cur);
    int last = -1;
    for (int pos = (fa + 1) % sz; ; pos = (pos + 1) % sz) {
        uint8_t ch = screen_.at(pos).ch;
        if (ch != 0x00 && ch != 0x40) last = pos;
        if (pos == end) break;
    }
    int target = last < 0 ? (fa + 1) % sz
               : last == end ? last                  // field is full
               : (last + 1) % sz;
    screen_.setCursor(target);
    return true;
}

bool KeyboardState::handleEraseEOF() {
    if (isLocked()) return false;
    if (!requireEditableCursor()) return false;
    int cur  = screen_.cursorPos();
    int last = lastPosInField(cur);
    for (int pos = cur; ; pos = (pos + 1) % screen_.size()) {
        screen_.at(pos).ch = 0x00;
        if (pos == last) break;
    }
    screen_.setMDT(cur);
    screen_.markDirty();
    return true;
}

bool KeyboardState::handleEraseInput() {
    if (isLocked()) return false;
    screen_.eraseAllUnprotected();
    moveCursorToFirstUnprotected();
    return true;
}

bool KeyboardState::handleNewLine() {
    if (!readyToMoveCursor()) return false;
    // Move to the start of the next line if that is an input position,
    // otherwise to the first input position after it.
    int curRow = screen_.cursorPos() / screen_.cols();
    int nextRowStart = ((curRow + 1) % screen_.rows()) * screen_.cols();
    int fa = screen_.findFieldStart(nextRowStart);
    if (fa < 0 || (!screen_.at(nextRowStart).isFA && !screen_.at(fa).isProtected())) {
        screen_.setCursor(nextRowStart);   // unformatted, or inside an input field
        return true;
    }
    int pos = screen_.nextUnprotected(nextRowStart);
    screen_.setCursor(pos >= 0 ? pos : 0);
    return true;
}

bool KeyboardState::handleCursorUp() {
    if (!readyToMoveCursor()) return false;
    int pos = screen_.cursorPos();
    screen_.setCursor((pos - screen_.cols() + screen_.size()) % screen_.size());
    return true;
}

bool KeyboardState::handleCursorDown() {
    if (!readyToMoveCursor()) return false;
    int pos = screen_.cursorPos();
    screen_.setCursor((pos + screen_.cols()) % screen_.size());
    return true;
}

bool KeyboardState::handleCursorLeft() {
    if (!readyToMoveCursor()) return false;
    screen_.setCursor((screen_.cursorPos() - 1 + screen_.size()) % screen_.size());
    return true;
}

bool KeyboardState::handleCursorRight() {
    if (!readyToMoveCursor()) return false;
    screen_.setCursor((screen_.cursorPos() + 1) % screen_.size());
    return true;
}

bool KeyboardState::handleReset() {
    if (lockReason_ == LockReason::OErr || lockReason_ == LockReason::System) {
        unlock();
    }
    insertMode_ = false;
    return true;
}

// ── Paste ─────────────────────────────────────────────────────────────────────
size_t KeyboardState::paste(const std::u32string& text) {
    PasteOps ops;
    ops.locked      = [this] { return isLocked(); };
    ops.overlayChar = [this](uint8_t e) {
        int cur = screen_.cursorPos();
        int fa  = screen_.findFieldStart(cur);
        if (fa == cur || (fa >= 0 && screen_.at(fa).isProtected()))
            screen_.setCursor(cur + 1);              // protected: dropped
        else
            insertCharAtCursor(e, false);
    };
    ops.tab        = [this] { handleTab(false); };
    ops.cursorLeft = [this] { handleCursorLeft(); };
    return pasteText(screen_, codec_, text, ops);
}

} // namespace x3270
