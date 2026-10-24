#include "ScreenBuffer.h"
#include <cstring>
#include <algorithm>

namespace x3270 {

// ── Buffer address code table (RFC 1576 / IBM GA23-0059) ─────────────────────
const uint8_t ScreenBuffer::kCodeTable[64] = {
    0x40,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
    0xC8,0xC9,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,
    0x50,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,
    0xD8,0xD9,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,
    0x60,0x61,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,
    0xE8,0xE9,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,
    0xF8,0xF9,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F
};

ScreenBuffer::ScreenBuffer(TerminalModel model)
    : model_(model)
    , rows_(rowsForModel(model))
    , cols_(colsForModel(model))
    , cells_(rows_ * cols_)
{
    eraseAll();
}

void ScreenBuffer::setAlternateSize(bool alternate) {
    // The default screen is always 24×80; the alternate is the model's size.
    int r = alternate ? rowsForModel(model_) : 24;
    int c = alternate ? colsForModel(model_) : 80;
    alternate_ = alternate;
    if (r != rows_ || c != cols_) {
        rows_ = r;
        cols_ = c;
        cells_.assign(r * c, Cell{});
    }
    eraseAll();
}

// ── Buffer operations ─────────────────────────────────────────────────────────
void ScreenBuffer::eraseAll() {
    for (auto& c : cells_) c = Cell{};
    savedErrorLine_.clear();
    cursorPos_        = 0;
    bufPtr_           = 0;
    currentFgColor_   = 0x00;
    currentBgColor_   = 0x00;
    currentHighlight_ = 0x00;
    dirty_            = true;
}

void ScreenBuffer::eraseAllUnprotected() {
    const int sz = rows_ * cols_;
    // Track the governing field while walking the buffer in order; an
    // unformatted screen (no FA) counts as entirely unprotected.
    int  fa   = findFieldStart(0);
    bool prot = fa >= 0 && cells_[fa].isProtected();
    for (int i = 0; i < sz; ++i) {
        if (cells_[i].isFA) {
            prot = cells_[i].isProtected();
            if (!prot) cells_[i].attr &= ~FA_MDT; // clear MDT on unprotected fields
        } else if (!prot) {
            cells_[i].ch = 0x00;
        }
    }
    dirty_ = true;
}

void ScreenBuffer::resetAllMDT() {
    const int sz = rows_ * cols_;
    for (int i = 0; i < sz; ++i) {
        if (cells_[i].isFA) {
            cells_[i].attr &= ~FA_MDT;
        }
    }
    dirty_ = true;
}

void ScreenBuffer::setCursorToHome() {
    // Walk cells in order and find the first FA cell that is editable (not protected,
    // not bypass).  In 5250 mode, input fields have attr = ffw1ToAttr(FFW1) which
    // leaves FA_PROTECTED(0x20) clear for normal input fields and sets it only for
    // bypass fields.  In 3270 mode this finds the first unprotected field.
    const int sz = rows_ * cols_;
    for (int i = 0; i < sz; ++i) {
        if (cells_[i].isFA && !cells_[i].isProtected()) {
            // Set cursor to the first data position of this field (one past the FA cell)
            cursorPos_ = (i + 1) % sz;
            dirty_ = true;
            return;
        }
    }
    // No editable field found — leave cursor unchanged
}

void ScreenBuffer::writeChar(uint8_t ebcdic) {
    Cell& c     = cells_[bufPtr_];
    c.ch        = ebcdic;
    c.attr      = 0x00;
    c.isFA      = false;
    c.fgColor   = currentFgColor_;
    c.bgColor   = currentBgColor_;
    c.highlight = currentHighlight_;
    bufPtr_ = clamp(bufPtr_ + 1);
    dirty_ = true;
}

void ScreenBuffer::startField(uint8_t attrByte, uint8_t fgColor, uint8_t bgColor, uint8_t highlight, uint16_t fieldLen) {
    Cell& c     = cells_[bufPtr_];
    c.ch        = 0x00;
    c.attr      = attrByte;
    c.isFA      = true;
    c.fgColor   = fgColor;
    c.bgColor   = bgColor;
    c.highlight = highlight;
    c.fieldLen  = fieldLen;
    // Characters in the field inherit these attributes from the FA cell at
    // render time; SA character attributes are deliberately left untouched.
    bufPtr_ = clamp(bufPtr_ + 1);
    dirty_ = true;
}

void ScreenBuffer::startInlineAttr5250(uint8_t attrByte5250) {
    // Inline 5250 attribute byte (0x20-0x3F) — starts an output-only sub-field.
    // Marked FA_PROTECTED so the cursor / input handlers won't treat the area as
    // editable.  fgColor stores the raw 5250 attr byte for the renderer.
    uint8_t safe = (attrByte5250 >= 0x20 && attrByte5250 <= 0x3F) ? attrByte5250 : 0x20;
    Cell& c = cells_[bufPtr_];
    // If the host re-positions onto an existing SF FA and emits an inline attribute,
    // do NOT clobber the field definition — preserve fieldLen / protection bits and
    // simply update the renderer-side colour state for following cells.
    if (!(c.isFA && c.fieldLen > 0)) {
        c.ch        = 0x00;
        c.attr      = FA_PROTECTED;
        c.isFA      = true;
        c.fgColor   = safe;
        c.bgColor   = 0x00;
        c.highlight = 0x00;
        c.fieldLen  = 0;
    }
    currentFgColor_   = safe;
    currentBgColor_   = 0x00;
    currentHighlight_ = 0x00;
    bufPtr_ = clamp(bufPtr_ + 1);
    dirty_ = true;
}

// RA and EUA run up to (not including) the stop address and leave the buffer
// address there.  A stop address equal to the current address means the
// whole buffer, hence do/while.
void ScreenBuffer::repeatToAddress(int destOffset, uint8_t ebcdic) {
    int dest = clamp(destOffset);
    do {
        Cell& c     = cells_[bufPtr_];
        c.ch        = ebcdic;
        c.attr      = 0x00;
        c.isFA      = false;
        c.fgColor   = currentFgColor_;
        c.bgColor   = currentBgColor_;
        c.highlight = currentHighlight_;
        bufPtr_ = clamp(bufPtr_ + 1);
    } while (bufPtr_ != dest);
    dirty_ = true;
}

void ScreenBuffer::eraseUnprotectedToAddress(int destOffset) {
    int  dest = clamp(destOffset);
    int  pos  = bufPtr_;
    int  fa   = findFieldStart(pos);
    bool prot = fa >= 0 && cells_[fa].isProtected();
    do {
        if (cells_[pos].isFA) {
            prot = cells_[pos].isProtected();
        } else if (!prot) {
            cells_[pos].ch = 0x00;
        }
        pos = clamp(pos + 1);
    } while (pos != dest);
    bufPtr_ = dest;
    dirty_ = true;
}

void ScreenBuffer::modifyFieldAttribute(uint8_t type, uint8_t value) {
    Cell& c = cells_[bufPtr_];
    if (!c.isFA) return;
    if      (type == 0xC0) c.attr      = value;
    else if (type == 0x41) c.highlight = value;
    else if (type == 0x42) c.fgColor   = value;
    else if (type == 0x45) c.bgColor   = value;
    dirty_ = true;
}

// ── Field attribute resolution ────────────────────────────────────────────────
// Walk backwards from bufPos to find the governing FA cell.
// The cell's .attr field on FA cells holds the raw attribute byte.
// For non-FA cells, we need to look up the nearest preceding FA.
// We also propagate attrs to all cells during this scan.
int ScreenBuffer::findFieldStart(int bufPos) const {
    const int sz = rows_ * cols_;
    for (int i = 1; i <= sz; ++i) {
        int pos = (bufPos - i + sz) % sz;
        if (cells_[pos].isFA) return pos;
    }
    return -1; // no FA found (entire screen is unformatted)
}

// ── 5250 error line ───────────────────────────────────────────────────────────
void ScreenBuffer::saveErrorLine() {
    if (!savedErrorLine_.empty()) return;   // keep the original under repeated errors
    auto first = cells_.begin() + (rows_ - 1) * cols_;
    savedErrorLine_.assign(first, first + cols_);
}

void ScreenBuffer::restoreErrorLine() {
    if (savedErrorLine_.empty()) return;
    if (static_cast<int>(savedErrorLine_.size()) == cols_)
        std::copy(savedErrorLine_.begin(), savedErrorLine_.end(),
                  cells_.begin() + (rows_ - 1) * cols_);
    savedErrorLine_.clear();
    dirty_ = true;
}

// ── MDT operations ────────────────────────────────────────────────────────────
void ScreenBuffer::setMDT(int bufferPos) {
    int fa = findFieldStart(bufferPos);
    if (fa >= 0) {
        cells_[fa].attr |= FA_MDT;
    }
}

// ── Inbound reply mode ───────────────────────────────────────────────────────
void ScreenBuffer::setReplyMode(ReplyMode mode, std::vector<uint8_t> attrTypes) {
    replyMode_      = mode;
    replyAttrTypes_ = mode == ReplyMode::Character ? std::move(attrTypes) : std::vector<uint8_t>{};
}

void ScreenBuffer::appendCharAttrs(std::vector<uint8_t>& rec, std::vector<uint8_t>* logCopy,
                                   uint8_t fg, uint8_t bg, uint8_t hl, uint8_t cur[3]) const {
    if (hl == 0xF0) hl = 0x00;   // "normal" is the same as no highlighting
    // Same order as x3270: foreground, background, highlighting.
    const uint8_t types[3]  = { 0x42, 0x45, 0x41 };
    const uint8_t values[3] = { fg, bg, hl };
    for (int i = 0; i < 3; ++i) {
        if (values[i] == cur[i]) continue;
        if (std::find(replyAttrTypes_.begin(), replyAttrTypes_.end(), types[i]) == replyAttrTypes_.end()) continue;
        cur[i] = values[i];
        for (auto* out : { &rec, logCopy }) {
            if (!out) continue;
            out->push_back(0x28);    // SA order
            out->push_back(types[i]);
            out->push_back(values[i]);
        }
    }
}

// ── Read Modified ─────────────────────────────────────────────────────────────
std::vector<uint8_t> ScreenBuffer::buildReadModifiedRecord(int aidByte, bool all,
                                                           std::vector<uint8_t>* logCopy) const {
    std::vector<uint8_t> rec;
    // AID byte
    rec.push_back(static_cast<uint8_t>(aidByte));
    // PA keys and Clear are a "short read": the AID byte alone.
    const bool shortRead = aidByte == 0x6C || aidByte == 0x6E || aidByte == 0x6B || aidByte == 0x6D;
    if (shortRead && !all) {
        if (logCopy) *logCopy = rec;
        return rec;
    }
    // Cursor address (2 bytes encoded)
    uint8_t cursorAddr[2];
    encodeAddress(cursorPos_, cursorAddr);
    rec.push_back(cursorAddr[0]);
    rec.push_back(cursorAddr[1]);
    if (logCopy) *logCopy = rec;

    const bool charMode = replyMode_ == ReplyMode::Character;
    uint8_t cur[3] = {};    // character attributes the host already has

    // Unformatted screen (no field attributes at all, e.g. CICS after CLEAR or
    // TSO line mode): send every non-null character in buffer order, no SBAs.
    // (No fields means nothing is hidden.)
    if (findFieldStart(0) < 0) {
        for (const auto& c : cells_) {
            if (c.ch == 0x00) continue;
            if (charMode) appendCharAttrs(rec, logCopy, c.fgColor, c.bgColor, c.highlight, cur);
            rec.push_back(c.ch);
            if (logCopy) logCopy->push_back(c.ch);
        }
        return rec;
    }

    // Every field with its MDT set, protected or not: SBA(first data
    // position) + its non-null characters.
    const int sz = rows_ * cols_;
    for (int i = 0; i < sz; ++i) {
        const Cell& fa = cells_[i];
        if (!fa.isFA || !(fa.attr & FA_MDT)) continue;
        uint8_t addr[2];
        encodeAddress(clamp(i + 1), addr);
        for (auto* out : { &rec, logCopy }) {
            if (!out) continue;
            out->push_back(0x11);    // SBA order
            out->push_back(addr[0]);
            out->push_back(addr[1]);
        }
        const bool hidden = fa.isNonDisplay();
        for (int pos = clamp(i + 1); pos != i && !cells_[pos].isFA; pos = clamp(pos + 1)) {
            const Cell& c = cells_[pos];
            if (c.ch == 0x00) continue;
            // Characters without their own attributes report the field's.
            if (charMode) appendCharAttrs(rec, logCopy, c.fgColor ? c.fgColor : fa.fgColor,
                                          c.bgColor ? c.bgColor : fa.bgColor,
                                          c.highlight ? c.highlight : fa.highlight, cur);
            rec.push_back(c.ch);
            if (logCopy) logCopy->push_back(hidden ? LOG_MASK_CHAR : c.ch);
        }
    }
    return rec;
}

std::vector<uint8_t> ScreenBuffer::buildReadBufferRecord(int aidByte, std::vector<uint8_t>* logCopy) const {
    std::vector<uint8_t> rec;
    rec.reserve(3 + cells_.size() * 2);
    rec.push_back(static_cast<uint8_t>(aidByte));
    uint8_t cursorAddr[2];
    encodeAddress(cursorPos_, cursorAddr);
    rec.push_back(cursorAddr[0]);
    rec.push_back(cursorAddr[1]);
    if (logCopy) *logCopy = rec;
    const bool extended = replyMode_ != ReplyMode::Field;
    const bool charMode = replyMode_ == ReplyMode::Character;
    uint8_t cur[3] = {};    // character attributes the host already has
    auto put = [&](uint8_t b) { rec.push_back(b); if (logCopy) logCopy->push_back(b); };
    int  fa     = findFieldStart(0);
    bool hidden = fa >= 0 && cells_[fa].isNonDisplay();
    for (const auto& c : cells_) {
        if (c.isFA) {
            hidden = c.isNonDisplay();
            const uint8_t attr = kCodeTable[c.attr & 0x3F];   // attribute in graphic form
            if (!extended) {
                put(0x1D);                                    // SF order
                put(attr);
                continue;
            }
            const uint8_t hl = c.highlight == 0xF0 ? 0x00 : c.highlight;
            put(0x29);                                        // SFE order
            put(static_cast<uint8_t>(1 + (c.fgColor != 0) + (c.bgColor != 0) + (hl != 0)));
            put(0xC0); put(attr);
            if (c.fgColor) { put(0x42); put(c.fgColor); }
            if (c.bgColor) { put(0x45); put(c.bgColor); }
            if (hl)        { put(0x41); put(hl); }
        } else {
            if (charMode) appendCharAttrs(rec, logCopy, c.fgColor, c.bgColor, c.highlight, cur);
            rec.push_back(c.ch);
            if (logCopy) logCopy->push_back(hidden && c.ch ? LOG_MASK_CHAR : c.ch);
        }
    }
    return rec;
}

// ── Address encoding/decoding ─────────────────────────────────────────────────
int ScreenBuffer::decodeAddress(uint8_t b0, uint8_t b1) {
    // 14-bit binary encoding: top 2 bits of b0 are 00 (range 0x00–0x3F)
    if ((b0 & 0xC0) == 0x00) {
        return (static_cast<int>(b0 & 0x3F) << 8) | b1;
    }
    // 12-bit code-table encoding: strip top 2 bits and reassemble
    return ((b0 & 0x3F) << 6) | (b1 & 0x3F);
}

void ScreenBuffer::encodeAddress(int offset, uint8_t out[2]) const {
    if (size() > 4096) {
        // 14-bit binary encoding (top 2 bits = 00) for large screens
        out[0] = static_cast<uint8_t>((offset >> 8) & 0x3F);
        out[1] = static_cast<uint8_t>(offset & 0xFF);
    } else {
        // 12-bit code-table encoding
        out[0] = kCodeTable[(offset >> 6) & 0x3F];
        out[1] = kCodeTable[offset & 0x3F];
    }
}

} // namespace x3270
