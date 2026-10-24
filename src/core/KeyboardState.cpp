#include "KeyboardState.h"
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

// First character of the next unprotected field after `from` (exclusive of
// fields with no character positions), or -1 if there is none.
int KeyboardState::nextUnprotected(int from) const {
    const int sz = screen_.size();
    for (int i = 0; i < sz; ++i) {
        int pos  = (from + i) % sz;
        int next = (pos + 1) % sz;
        const Cell& c = screen_.at(pos);
        if (c.isFA && !c.isProtected() && !screen_.at(next).isFA) return next;
    }
    return -1;
}

void KeyboardState::moveCursorToFirstUnprotected() {
    int pos = nextUnprotected(0);
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

bool KeyboardState::insertCharAtCursor(uint8_t ebcdic) {
    int cur = screen_.cursorPos();
    const Cell& cell = screen_.at(cur);
    if (cell.isFA) return false; // cursor is on FA position — skip

    if (insertMode_) {
        // Find end of field (position of next FA, exclusive)
        int fieldEnd = (cur + 1) % screen_.size();
        for (int i = 0; i < screen_.size(); ++i) {
            if (screen_.at(fieldEnd).isFA) break;
            fieldEnd = (fieldEnd + 1) % screen_.size();
        }
        // If the last character in the field is non-null the field is full —
        // lock with OErr (same behaviour as a real 3270 in insert mode).
        int lastCell = (fieldEnd - 1 + screen_.size()) % screen_.size();
        if (lastCell != cur && screen_.at(lastCell).ch != 0x00) {
            lock(LockReason::OErr);
            return false;
        }
        // Shift cells right from lastCell back to cur
        int shiftEnd = lastCell;
        while (shiftEnd != cur) {
            int prev = (shiftEnd - 1 + screen_.size()) % screen_.size();
            screen_.at(shiftEnd).ch = screen_.at(prev).ch;
            shiftEnd = prev;
        }
    }

    screen_.at(cur).ch = ebcdic;
    screen_.setMDT(cur);
    screen_.markDirty();

    // Advance cursor.  After the last position of a field the cursor moves
    // past the next attribute byte; if that field is autoskip (protected +
    // numeric) it tabs on to the next unprotected field.
    int next = (cur + 1) % screen_.size();
    if (screen_.at(next).isFA && screen_.at(next).isSkip()) {
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
void KeyboardState::sendAID(uint8_t aidCode, bool includeModifiedFields) {
    std::vector<uint8_t> record, logCopy;
    if (includeModifiedFields) {
        record = screen_.buildReadModifiedRecord(aidCode, false, &logCopy);
    } else {
        // PA keys and CLEAR are a "short read": the AID byte alone.
        record.push_back(aidCode);
        logCopy = record;
    }
    if (sendCb_ && sendCb_(record, logCopy)) lock(LockReason::System);
}

void KeyboardState::sendPAKey(uint8_t aidCode) {
    sendAID(aidCode, false);
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
    if (!isCurrentFieldEditable()) {
        lock(LockReason::OErr);
        return false;
    }
    return insertCharAtCursor(ebcdic);
}

bool KeyboardState::handleTab(bool backward) {
    if (isLocked()) return false;
    advanceToNextField(!backward);
    return true;
}

bool KeyboardState::handleEnter() {
    if (isLocked()) return false;
    insertMode_ = false;
    sendAID(AID_ENTER, true);
    return true;
}

bool KeyboardState::handleClear() {
    // Like the other AID keys, Clear is rejected while input is inhibited
    // (Reset first).  It returns the screen to the default (24×80) size.
    if (isLocked()) return false;
    insertMode_ = false;
    screen_.setAlternateSize(false);
    sendAID(AID_CLEAR, false);
    return true;
}

bool KeyboardState::handlePF(int n) {
    if (isLocked()) return false;
    sendAID(pfAID(n), true);
    return true;
}

bool KeyboardState::handlePA(int n) {
    if (isLocked()) return false;
    uint8_t aidCode = (n == 1) ? AID_PA1 : (n == 2) ? AID_PA2 : AID_PA3;
    sendPAKey(aidCode);
    return true;
}

bool KeyboardState::handleBackspace() {
    if (isLocked()) return false;
    if (!isCurrentFieldEditable()) return false;
    int cur = screen_.cursorPos();
    int prev = (cur - 1 + screen_.size()) % screen_.size();
    // Don't move past an FA
    if (screen_.at(prev).isFA) return false;
    screen_.setCursor(prev);
    screen_.at(prev).ch = 0x00;
    screen_.setMDT(prev);
    screen_.markDirty();
    return true;
}

bool KeyboardState::handleDelete() {
    if (isLocked()) return false;
    if (!isCurrentFieldEditable()) return false;
    int cur = screen_.cursorPos();
    // Shift everything left within the field
    int pos = cur;
    while (true) {
        int next = (pos + 1) % screen_.size();
        if (screen_.at(next).isFA) {
            screen_.at(pos).ch = 0x00;
            break;
        }
        screen_.at(pos).ch = screen_.at(next).ch;
        pos = next;
    }
    screen_.setMDT(cur);
    screen_.markDirty();
    return true;
}

bool KeyboardState::handleHome() {
    if (isLocked()) return false;
    moveCursorToFirstUnprotected();
    return true;
}

bool KeyboardState::handleEraseEOF() {
    if (isLocked()) return false;
    if (!isCurrentFieldEditable()) return false;
    int cur = screen_.cursorPos();
    int pos = cur;
    while (!screen_.at(pos).isFA) {
        screen_.at(pos).ch = 0x00;
        pos = (pos + 1) % screen_.size();
        if (pos == cur) break; // wrapped all the way around
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
    if (isLocked()) return false;
    // Move to the start of the next line if that is an input position,
    // otherwise to the first input position after it.
    int curRow = screen_.cursorPos() / screen_.cols();
    int nextRowStart = ((curRow + 1) % screen_.rows()) * screen_.cols();
    int fa = screen_.findFieldStart(nextRowStart);
    if (fa < 0 || (!screen_.at(nextRowStart).isFA && !screen_.at(fa).isProtected())) {
        screen_.setCursor(nextRowStart);   // unformatted, or inside an input field
        return true;
    }
    int pos = nextUnprotected(nextRowStart);
    screen_.setCursor(pos >= 0 ? pos : 0);
    return true;
}

bool KeyboardState::handleCursorUp() {
    if (isLocked()) return false;
    int pos = screen_.cursorPos();
    screen_.setCursor((pos - screen_.cols() + screen_.size()) % screen_.size());
    return true;
}

bool KeyboardState::handleCursorDown() {
    if (isLocked()) return false;
    int pos = screen_.cursorPos();
    screen_.setCursor((pos + screen_.cols()) % screen_.size());
    return true;
}

bool KeyboardState::handleCursorLeft() {
    if (isLocked()) return false;
    screen_.setCursor((screen_.cursorPos() - 1 + screen_.size()) % screen_.size());
    return true;
}

bool KeyboardState::handleCursorRight() {
    if (isLocked()) return false;
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

} // namespace x3270
