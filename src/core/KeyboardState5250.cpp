#include "KeyboardState5250.h"
#include "Paste.h"
#include "DataStream5250Parser.h"   // FFW bit definitions
#include <algorithm>

namespace x3270 {

KeyboardState5250::KeyboardState5250(ScreenBuffer& screen, EbcdicCodec& codec)
    : screen_(screen), codec_(codec) {}

// ── Lock/unlock ───────────────────────────────────────────────────────────────

void KeyboardState5250::lock(LockReason reason) {
    lockReason_ = reason;
}

void KeyboardState5250::unlock() {
    lockReason_ = LockReason::None;
    insertMode_ = false;
    // Leaving the error state other than by Reset (e.g. a host CC2 unlock):
    // the row saved by Write Error Code no longer belongs on the screen.
    screen_.discardErrorLine();
}

// ── Internal helpers ──────────────────────────────────────────────────────────

void KeyboardState5250::encodeCursor(uint8_t& row, uint8_t& col) const {
    int pos  = screen_.cursorPos();
    row      = static_cast<uint8_t>(pos / screen_.cols() + 1); // 1-indexed
    col      = static_cast<uint8_t>(pos % screen_.cols() + 1);
}

uint8_t KeyboardState5250::currentFieldAttr() const {
    int pos = screen_.cursorPos();
    for (int i = 0; i < screen_.size(); ++i) {
        int p = (pos - i + screen_.size()) % screen_.size();
        if (screen_.at(p).isFA) return screen_.at(p).attr;
    }
    return 0x00;
}

bool KeyboardState5250::isCurrentFieldEditable() const {
    uint8_t attr = currentFieldAttr();
    bool prot    = (attr & FA_PROTECTED) != 0;
    bool skip    = (attr & (FA_PROTECTED | FA_NUMERIC)) == (FA_PROTECTED | FA_NUMERIC);
    return !prot && !skip;
}

void KeyboardState5250::moveCursorToFirstUnprotected() {
    for (int i = 0; i < screen_.size(); ++i) {
        const Cell& c = screen_.at(i);
        if (c.isFA && !c.isProtected() && !c.isSkip()) {
            screen_.setCursor((i + 1) % screen_.size());
            return;
        }
    }
    screen_.setCursor(0);
}

void KeyboardState5250::advanceToNextField(bool forward) {
    int cur   = screen_.cursorPos();
    int delta = forward ? 1 : -1;
    for (int i = 1; i <= screen_.size(); ++i) {
        int pos = (cur + delta * i + screen_.size() * 2) % screen_.size();
        const Cell& c = screen_.at(pos);
        // Tab lands only on input fields. Inline 5250 attribute bytes create
        // FA cells marked FA_PROTECTED (output sub-fields for colour); those
        // must be skipped, otherwise Tab gets stuck on each colour boundary
        // before ever reaching the next input field (e.g. the password field).
        if (c.isFA && !c.isProtected() && !c.isSkip()) {
            int target = (pos + 1) % screen_.size();
            if (!forward && target == cur) continue;
            screen_.setCursor(target);
            return;
        }
    }
}

int KeyboardState5250::inputFieldAt(int pos) const {
    const int sz = screen_.size();
    if (screen_.at(pos).isFA) return -1;
    int fa = screen_.findFieldStart(pos);
    if (fa < 0) return -1;
    const Cell& f = screen_.at(fa);
    if (f.isProtected()) return -1;   // bypass field or output-only attribute
    if (f.fieldLen > 0 && (pos - fa - 1 + sz) % sz >= f.fieldLen) return -1;
    return fa;
}

int KeyboardState5250::fieldEnd(int fa) const {
    const int sz = screen_.size();
    const int len = screen_.at(fa).fieldLen;
    int pos = (fa + 1) % sz;
    for (int k = 0; k < (len > 0 ? len : sz - 1); ++k) {
        if (screen_.at(pos).isFA) break;
        pos = (pos + 1) % sz;
    }
    return pos;
}

// Characters a field's FFW shift/edit bits accept (IBM 5250 FFW bits 0-2).
static bool fieldAccepts(uint8_t ffw1, uint8_t e) {
    const bool digit = e >= 0xF0 && e <= 0xF9;
    const bool upper = (e >= 0xC1 && e <= 0xC9) || (e >= 0xD1 && e <= 0xD9) || (e >= 0xE2 && e <= 0xE9);
    const bool lower = (e >= 0x81 && e <= 0x89) || (e >= 0x91 && e <= 0x99) || (e >= 0xA2 && e <= 0xA9);
    const bool punct = e == 0x40 || e == 0x6B || e == 0x4B || e == 0x60;   // space , . -
    if ((ffw1 & 0xC0) != 0x40) return true;     // no FFW (identifier bits are 01)
    switch (ffw1 & FFW1_SHIFT_MASK) {
    case FFW1_ALPHA_ONLY:  return upper || lower || punct;
    case FFW1_NUM_ONLY:    return digit || punct || e == 0x4E;   // + too
    case FFW1_DIGITS_ONLY:
    case FFW1_SIGNED_NUM:  return digit;
    default:               return true;
    }
}

static uint8_t toUpperEbcdic(uint8_t e) {
    if ((e >= 0x81 && e <= 0x89) || (e >= 0x91 && e <= 0x99) || (e >= 0xA2 && e <= 0xA9))
        return static_cast<uint8_t>(e + 0x40);
    return e;
}

bool KeyboardState5250::insertCharAtCursor(uint8_t ebcdic, bool autoSkip) {
    const int sz = screen_.size();
    int cur = screen_.cursorPos();
    const Cell& cell = screen_.at(cur);
    if (cell.isFA) return false;

    // Find the governing FA to get field length and format
    int faPos = screen_.findFieldStart(cur);
    bool lastPosInField = false;
    uint8_t ffw2 = 0;
    int end = sz;
    if (faPos >= 0) {
        const Cell& fa = screen_.at(faPos);
        ffw2 = fa.ffw2;
        end  = fieldEnd(faPos);
        if (fa.fieldLen > 0) {
            // Compute position within field (0-based from first char after FA)
            int posInField = (cur - faPos - 1 + sz) % sz;
            lastPosInField = (posInField == static_cast<int>(fa.fieldLen) - 1);
            if (posInField >= static_cast<int>(fa.fieldLen)) {
                // Field is full — lock keyboard with OErr
                lock(LockReason::OErr);
                return false;
            }
        }
        // A filled Field-Exit-Required field accepts nothing but Field Exit
        // (or an AID); data keys are errors.
        if (cur == ferPos_ || !fieldAccepts(fa.ffw1, ebcdic)) {
            lock(LockReason::OErr);
            return false;
        }
        if (ffw2 & FFW2_MONOCASE) ebcdic = toUpperEbcdic(ebcdic);
    }

    if (insertMode_) {
        int last = (end - 1 + sz) % sz;
        if (last != cur && screen_.at(last).ch != 0x00) {   // no room to shift
            lock(LockReason::OErr);
            return false;
        }
        for (int p = last; p != cur; ) {
            int prev = (p - 1 + sz) % sz;
            screen_.at(p).ch = screen_.at(prev).ch;
            p = prev;
        }
    }

    screen_.at(cur).ch = ebcdic;
    screen_.setMDT(cur);
    screen_.markDirty();

    // Filling the last position of an input field advances to the next input
    // field — unless the field requires Field Exit, in which case the cursor
    // stays put.  Auto-enter fields send Enter once filled.
    int next = (cur + 1) % sz;
    bool filled = faPos >= 0 && (lastPosInField || screen_.at(next).isFA);
    if (!autoSkip) {
        screen_.setCursor(next);
    } else if (filled && (ffw2 & FFW2_FER)) {
        ferPos_ = cur;
    } else if (filled) {
        advanceToNextField(true);
        if (ffw2 & FFW2_AUTO_ENTER) sendAID(AID5250_ENTER, true);
    } else if (!screen_.at(next).isFA) {
        screen_.setCursor(next);
    }
    return true;
}

// ── AID transmission ──────────────────────────────────────────────────────────

void KeyboardState5250::sendAID(uint8_t aidCode, bool includeModifiedFields) {
    ferPos_ = -1;
    std::vector<uint8_t> logCopy;
    auto record = buildInputRecord(aidCode, includeModifiedFields, &logCopy);
    if (sendCb_ && sendCb_(record, logCopy)) {
        lock(LockReason::System);
    }
}

void KeyboardState5250::sendReadImmediate() {
    // Read Immediate answers with AID 0x00 and leaves the keyboard alone.
    std::vector<uint8_t> logCopy;
    auto record = buildInputRecord(0x00, true, &logCopy);
    if (sendCb_) sendCb_(record, logCopy);
}

std::vector<uint8_t> KeyboardState5250::buildInputRecord(uint8_t aidCode, bool includeModifiedFields,
                                                         std::vector<uint8_t>* logCopy) const {
    uint8_t curRow, curCol;
    encodeCursor(curRow, curCol);

    // 5250 input record format (IBM SA21-9247 §7.2 "Transmitting Data"):
    //   [row][col][AID][SBA+data per MDT field...]
    // row/col are 1-indexed, placed BEFORE the AID byte.
    std::vector<uint8_t> record;
    record.push_back(curRow);
    record.push_back(curCol);
    record.push_back(aidCode);

    if (includeModifiedFields) {
        // Walk all cells; for each modified unprotected field, emit SBA + data
        // 5250 SBA inside input records: 0x11 [row] [col]
        int inField       = 0; // 0=no field, 1=unprotected modified
        int fieldStart    = -1;
        bool hidden       = false; // field has a non-display attribute
        std::vector<uint8_t> fieldData;
        std::vector<std::pair<size_t, size_t>> hiddenRanges; // [begin, end) in record

        auto flushField = [&](int faPos) {
            if (inField == 1 && !fieldData.empty()) {
                // The FA is at faPos; characters start one position after it
                int startOffset = (fieldStart + 1) % screen_.size();
                uint8_t sbaRow = static_cast<uint8_t>(startOffset / screen_.cols() + 1);
                uint8_t sbaCol = static_cast<uint8_t>(startOffset % screen_.cols() + 1);
                record.push_back(0x11); // SBA order
                record.push_back(sbaRow);
                record.push_back(sbaCol);
                // Trim trailing nulls
                while (!fieldData.empty() && fieldData.back() == 0x00)
                    fieldData.pop_back();
                if (hidden) hiddenRanges.push_back({record.size(), record.size() + fieldData.size()});
                record.insert(record.end(), fieldData.begin(), fieldData.end());
                fieldData.clear();
            }
            (void)faPos;
            inField = 0;
            fieldStart = -1;
        };

        int fieldLeft = 0;   // character positions remaining in the current field
        for (int i = 0; i < screen_.size(); ++i) {
            const Cell& c = screen_.at(i);
            if (c.isFA) {
                flushField(i - 1 >= 0 ? i - 1 : screen_.size() - 1);
                if (!c.isProtected() && !c.isSkip() && c.isMDT()) {
                    inField    = 1;
                    fieldStart = i;
                    fieldLeft  = c.fieldLen > 0 ? c.fieldLen : screen_.size();
                    hidden     = c.hidesData(true);
                } else {
                    inField    = 0;
                    fieldStart = -1;
                }
            } else if (inField == 1 && fieldLeft > 0) {
                fieldData.push_back(c.ch);   // only the field's own positions
                --fieldLeft;
            }
        }
        // Flush last field
        flushField(screen_.size() - 1);

        if (logCopy) {
            *logCopy = record;
            for (auto [b, e] : hiddenRanges)
                std::fill(logCopy->begin() + b, logCopy->begin() + e, LOG_MASK_CHAR);
        }
    } else if (logCopy) {
        *logCopy = record;
    }
    return record;
}

// ── Key handlers ──────────────────────────────────────────────────────────────

bool KeyboardState5250::handleChar(uint32_t unicodeChar) {
    int ebcdic = codec_.fromUnicode(unicodeChar);
    if (ebcdic < 0) return false;   // not in this code page
    return handleEbcdicChar(static_cast<uint8_t>(ebcdic));
}

bool KeyboardState5250::handleEbcdicChar(uint8_t ebcdic) {
    if (isLocked()) {
        if (lockReason_ == LockReason::System) lock(LockReason::OErr);
        return false;
    }
    if (!isCurrentFieldEditable()) {
        lock(LockReason::OErr);
        return false;
    }
    return insertCharAtCursor(ebcdic);
}

bool KeyboardState5250::handleTab(bool backward) {
    if (isLocked()) return false;
    ferPos_ = -1;
    advanceToNextField(!backward);
    return true;
}

bool KeyboardState5250::handleEnter() {
    if (isLocked()) return false;
    insertMode_ = false;
    sendAID(AID5250_ENTER, true);
    return true;
}

bool KeyboardState5250::handleClear() {
    unlock();
    screen_.eraseAll();
    sendAID(AID5250_CLEAR, false);
    return true;
}

bool KeyboardState5250::handlePFKey(int n) {
    if (isLocked()) return false;
    sendAID(pf5250AID(n), true);
    return true;
}

bool KeyboardState5250::handlePageUp() {
    if (isLocked()) return false;
    sendAID(AID5250_ROLL_DN, false);
    return true;
}

bool KeyboardState5250::handlePageDown() {
    if (isLocked()) return false;
    sendAID(AID5250_ROLL_UP, false);
    return true;
}

bool KeyboardState5250::handleHelp() {
    if (isLocked()) return false;
    sendAID(AID5250_HELP, false);
    return true;
}

// Attention and System Request are not AIDs: they are empty records with the
// ATN / SRQ flag in the GDS header (tn5250 TN5250_RECORD_H_ATN / _H_SRQ).
bool KeyboardState5250::handleAttn() {
    ferPos_ = -1;
    if (isLocked()) unlock();
    return signalCb_ ? signalCb_(0x40) : false;
}

bool KeyboardState5250::handleSysReq() {
    ferPos_ = -1;
    return signalCb_ ? signalCb_(0x04) : false;
}

bool KeyboardState5250::handleReset() {
    screen_.restoreErrorLine();   // before unlock(), which discards it
    if (lockReason_ == LockReason::OErr || lockReason_ == LockReason::System) unlock();
    screen_.markDirty();
    insertMode_ = false;
    return true;
}

bool KeyboardState5250::handleFieldExit() {
    if (isLocked()) return false;
    const int sz = screen_.size();
    int cur = screen_.cursorPos();
    int fa  = inputFieldAt(cur);
    if (fa < 0) { lock(LockReason::OErr); return false; }

    // Erase from the cursor to the end of the field; in a filled
    // Field-Exit-Required field the cursor sits on the last character typed.
    int end = fieldEnd(fa);
    for (int p = (cur == ferPos_) ? (cur + 1) % sz : cur; p != end; p = (p + 1) % sz)
        screen_.at(p).ch = 0x00;
    ferPos_ = -1;
    screen_.setMDT(cur);

    // Right-adjust fields (FFW2 low bits 5 = zero fill, 6 = blank fill).
    uint8_t adjust = screen_.at(fa).ffw2 & 0x07;
    if (adjust == 5 || adjust == 6) {
        std::vector<uint8_t> chars;
        for (int p = (fa + 1) % sz; p != end; p = (p + 1) % sz)
            if (screen_.at(p).ch != 0x00) chars.push_back(screen_.at(p).ch);
        int width = (end - fa - 1 + sz) % sz;
        int pad   = width - static_cast<int>(chars.size());
        int p     = (fa + 1) % sz;
        for (int k = 0; k < width; ++k, p = (p + 1) % sz)
            screen_.at(p).ch = k < pad ? (adjust == 5 ? 0xF0 : 0x40) : chars[k - pad];
    }
    screen_.markDirty();

    advanceToNextField(true);
    if (screen_.at(fa).ffw2 & FFW2_AUTO_ENTER) sendAID(AID5250_ENTER, true);
    return true;
}

bool KeyboardState5250::handleHome() {
    if (isLocked()) return false;
    ferPos_ = -1;
    moveCursorToFirstUnprotected();
    return true;
}

bool KeyboardState5250::handleArrow(int dRow, int dCol) {
    if (isLocked()) return false;
    ferPos_ = -1;
    int cur  = screen_.cursorPos();
    int row  = cur / screen_.cols();
    int col  = cur % screen_.cols();
    row = (row + dRow + screen_.rows()) % screen_.rows();
    col = (col + dCol + screen_.cols()) % screen_.cols();
    screen_.setCursor(row * screen_.cols() + col);
    return true;
}

// The editing keys only ever change the character positions of an input
// field; protected text and the gap after a field's last position are left alone.
bool KeyboardState5250::handleBackspace() {
    if (isLocked()) return false;
    int cur = screen_.cursorPos();
    int target = (cur == ferPos_) ? cur : cur - 1;   // parked FER cursor: erase in place
    ferPos_ = -1;
    if (target < 0 || inputFieldAt(target) < 0) return false;
    screen_.setCursor(target);
    screen_.at(target).ch = 0x00;
    screen_.setMDT(target);
    screen_.markDirty();
    return true;
}

bool KeyboardState5250::handleDelete() {
    if (isLocked()) return false;
    const int sz = screen_.size();
    int cur = screen_.cursorPos();
    int fa  = inputFieldAt(cur);
    if (fa < 0) return false;
    // Shift the rest of the field left from the cursor
    int end = fieldEnd(fa);
    int pos = cur;
    for (int next = (pos + 1) % sz; next != end; pos = next, next = (next + 1) % sz)
        screen_.at(pos).ch = screen_.at(next).ch;
    screen_.at(pos).ch = 0x00;
    ferPos_ = -1;
    screen_.setMDT(cur);
    screen_.markDirty();
    return true;
}

bool KeyboardState5250::handleEraseField() {
    if (isLocked()) return false;
    const int sz = screen_.size();
    int cur = screen_.cursorPos();
    int fa  = inputFieldAt(cur);
    if (fa < 0) return false;
    for (int p = cur, end = fieldEnd(fa); p != end; p = (p + 1) % sz)
        screen_.at(p).ch = 0x00;
    ferPos_ = -1;
    screen_.setMDT(cur);
    screen_.markDirty();
    return true;
}

bool KeyboardState5250::handleInsert() {
    if (isLocked()) return false;
    toggleInsert();
    return true;
}

// ── Paste ─────────────────────────────────────────────────────────────────────
size_t KeyboardState5250::paste(const std::u32string& text) {
    PasteOps ops;
    ops.locked      = [this] { return isLocked(); };
    ops.overlayChar = [this](uint8_t e) {
        int cur = screen_.cursorPos();
        if (inputFieldAt(cur) < 0)
            screen_.setCursor(cur + 1);              // not an input position: dropped
        else
            insertCharAtCursor(e, false);
    };
    ops.tab        = [this] { handleTab(false); };
    ops.cursorLeft = [this] { handleArrow(0, -1); };
    return pasteText(screen_, codec_, text, ops);
}

} // namespace x3270
