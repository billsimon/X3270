#pragma once
#include <cstdint>
#include <vector>
#include <functional>
#include "TerminalModel.h"

namespace x3270 {

// ── Field Attribute byte bit layout ──────────────────────────────────────────
// Bit 2: Protected  (1=output only, 0=input)
// Bit 3: Numeric    (1=numeric only; bits 2+3 both 1 → Skip)
// Bits 4-5: Display (00=normal, 01=normal+LP, 10=intensified, 11=non-display)
// Bit 7: MDT        (Modified Data Tag — set when user edits the field)
static constexpr uint8_t FA_PROTECTED   = 0x20;
static constexpr uint8_t FA_NUMERIC     = 0x10;
static constexpr uint8_t FA_DISP_NORMAL = 0x00;
static constexpr uint8_t FA_DISP_LP     = 0x04;  // bits[3:2]=01 → normal + light-pen detect
static constexpr uint8_t FA_INTENSIFIED = 0x08;  // bits[3:2]=10 → intensified
static constexpr uint8_t FA_NONDISPLAY  = 0x0C;  // bits[3:2]=11 → non-display (hidden)
static constexpr uint8_t FA_MDT        = 0x01;

// AID byte meaning "no AID" (no operator action since the last restore)
static constexpr uint8_t AID_NONE = 0x60;

// Byte that stands in for hidden-field data in the copies of outbound records
// written to the traffic log (EBCDIC '*', the same in every supported code page).
static constexpr uint8_t LOG_MASK_CHAR = 0x5C;

/// 5250 display attributes 0x27/0x2F/0x37/0x3F are non-display.
inline bool isNonDisplay5250(uint8_t displayAttr) {
    return displayAttr >= 0x20 && displayAttr <= 0x3F && (displayAttr & 0x07) == 0x07;
}

// ── Cell ─────────────────────────────────────────────────────────────────────
struct Cell {
    uint8_t  ch        { 0x00 }; // EBCDIC character byte (0x00 = empty/null)
    uint8_t  attr      { 0x00 }; // (FA cells only) field attribute byte; use findFieldStart()
                                 // to get the field governing a character cell
    bool     isFA      { false }; // true if this cell IS a field attribute position
    // Extended colour/highlight attributes. On FA cells these are the field's
    // attributes (SFE); on character cells they are character attributes (SA).
    // 0x00 = inherit from the governing field; 0xF1-0xF7 = IBM 3279 colour index
    uint8_t  fgColor   { 0x00 }; // foreground colour (0=field-default)
    uint8_t  bgColor   { 0x00 }; // background colour (0=black)
    uint8_t  highlight { 0x00 }; // 0=normal, 0xF2=reverse, 0xF4=underscore, 0xF1=blink
    uint16_t fieldLen  { 0 };    // (FA cells only) usable character count; 0 = infer from layout
    uint8_t  ffw1      { 0 };    // (5250 input-field FA only) raw Field Format Word bytes
    uint8_t  ffw2      { 0 };

    bool isProtected()  const { return (attr & FA_PROTECTED) != 0; }
    bool isNumeric()    const { return (attr & FA_NUMERIC)   != 0; }
    bool isSkip()       const { return (attr & (FA_PROTECTED|FA_NUMERIC)) == (FA_PROTECTED|FA_NUMERIC); }
    bool isNonDisplay() const { return (attr & 0x0C) == FA_NONDISPLAY; }
    bool isIntensified()const { return (attr & 0x0C) == FA_INTENSIFIED; }
    bool isMDT()        const { return (attr & FA_MDT) != 0; }
};

// ── ScreenBuffer ──────────────────────────────────────────────────────────────
class ScreenBuffer {
public:
    explicit ScreenBuffer(TerminalModel model = TerminalModel::Model2);

    // Runtime grid dimensions (the current default or alternate size)
    int rows()  const { return rows_; }
    int cols()  const { return cols_; }
    int size()  const { return rows_ * cols_; }
    TerminalModel model() const { return model_; }

    // Largest grid this buffer can take: the model's alternate size.
    int maxRows() const { return rowsForModel(model_); }
    int maxCols() const { return colsForModel(model_); }

    /// Switch between the default 24×80 screen (Erase/Write, Clear key) and
    /// the model's alternate screen (Erase/Write Alternate).  Erases the buffer.
    void setAlternateSize(bool alternate);
    bool isAlternateSize() const { return alternate_; }

    // ── Buffer access ─────────────────────────────────────────────────────────
    Cell&       at(int offset)       { return cells_[offset]; }
    const Cell& at(int offset) const { return cells_[offset]; }
    Cell&       at(int row, int col) { return cells_[row * cols_ + col]; }

    int   cursorPos()  const { return cursorPos_; }
    void  setCursor(int pos) { cursorPos_ = clamp(pos); }

    // ── Screen operations (called by DataStreamParser) ────────────────────────

    /// Erase entire buffer and reset all fields (used by Erase/Write)
    void eraseAll();

    /// Erase all unprotected fields (Erase All Unprotected command)
    void eraseAllUnprotected();

    /// Write a character at bufferPointer_, then advance it
    void writeChar(uint8_t ebcdic);

    /// Start a new field at bufferPointer_.
    /// fgColor/bgColor/highlight are IBM 3279 extended colour codes (0x00=default, 0xF1-0xF7).
    void startField(uint8_t attrByte,
                    uint8_t fgColor   = 0x00,
                    uint8_t bgColor   = 0x00,
                    uint8_t highlight = 0x00,
                    uint16_t fieldLen = 0);

    /// Place an inline 5250 display-attribute byte (0x20-0x3F) at the current
    /// buffer pointer. The attribute occupies a buffer position (rendered as a
    /// blank) and governs all subsequent cells until the next attribute/field.
    /// The cell is marked protected by default — input fields are introduced
    /// only via Start-of-Field orders (which call startField() with FFW-derived
    /// attr bits).
    void startInlineAttr5250(uint8_t attrByte5250);

    /// Update the "current" SA-order colour/highlight applied to subsequent writeChar calls.
    void setCurrentFgColor(uint8_t c)   { currentFgColor_   = c; }
    void setCurrentBgColor(uint8_t c)   { currentBgColor_   = c; }
    void setCurrentHighlight(uint8_t c) { currentHighlight_ = c; }

    /// Reset the SA character attributes to "inherit from field" (start of each Write).
    void resetCharAttributes() { currentFgColor_ = currentBgColor_ = currentHighlight_ = 0x00; }

    /// Set buffer pointer (SetBufferAddress order)
    void setBufferAddress(int offset) { bufPtr_ = clamp(offset); }
    int  bufferPointer() const        { return bufPtr_; }

    /// Repeat character from current pointer to destOffset
    void repeatToAddress(int destOffset, uint8_t ebcdic);

    /// Erase unprotected characters from current pointer to destOffset
    void eraseUnprotectedToAddress(int destOffset);

    /// Insert cursor marker at current bufferPointer_
    void insertCursorHere() { cursorPos_ = bufPtr_; }

    /// Modify Field: apply one attribute type/value pair to the FA at the
    /// buffer pointer (0xC0 = basic attribute, 0x41 highlight, 0x42/0x45 colour).
    void modifyFieldAttribute(uint8_t type, uint8_t value);

    // ── Inbound reply mode (Set Reply Mode structured field) ─────────────────
    enum class ReplyMode : uint8_t { Field = 0x00, ExtendedField = 0x01, Character = 0x02 };
    /// attrTypes: in Character mode, the attribute types (0x41 highlight,
    /// 0x42 foreground, 0x45 background) reported with SA orders.
    void setReplyMode(ReplyMode mode, std::vector<uint8_t> attrTypes = {});
    ReplyMode replyMode() const { return replyMode_; }

    // ── AID of the last operator action ───────────────────────────────────────
    /// Host-initiated reads report it; a keyboard-restoring Write resets it
    /// to AID_NONE.
    uint8_t aid() const      { return aid_; }
    void    setAid(uint8_t a) { aid_ = a; }

    // ── Read Modified (build client→host AID data) ────────────────────────────
    /// Build Read Modified data: AID + cursor + {SBA(field)+data} for every
    /// field with its MDT set.  For a PA or Clear AID this is a short read
    /// (the AID alone) unless `all` (Read Modified All) is set.
    /// If logCopy is given it receives the same record with the data of
    /// non-display (password) fields replaced by LOG_MASK_CHAR, for logging.
    std::vector<uint8_t> buildReadModifiedRecord(int aidByte, bool all = false,
                                                 std::vector<uint8_t>* logCopy = nullptr) const;

    /// Build a Read Buffer reply: AID + cursor + every buffer position in
    /// order.  Field attributes are sent as SF, or as SFE with their colour
    /// and highlighting outside Field reply mode; Character reply mode adds
    /// SA orders for character attributes.
    /// logCopy: as for buildReadModifiedRecord.
    std::vector<uint8_t> buildReadBufferRecord(int aidByte,
                                               std::vector<uint8_t>* logCopy = nullptr) const;

    /// Clear MDT bit on all unprotected fields
    void resetAllMDT();

    /// Move cursor to the first character of the first non-protected, non-bypass field.
    /// Called by the 5250 parser when CC2 UNLOCK bit is set and no explicit IC order
    /// was seen in the WTD data stream (mirrors tn5250_display_set_cursor_home).
    void setCursorToHome();

    /// Set MDT on the field that owns the given buffer position
    void setMDT(int bufferPos);

    /// 5250 Write Error Code: remember the last (message) row so the message
    /// can be removed again when the operator presses Reset.
    void saveErrorLine();
    /// Put back the row saved by saveErrorLine(); no-op if none is saved.
    void restoreErrorLine();
    bool hasSavedErrorLine() const { return !savedErrorLine_.empty(); }

    // ── 3270 Buffer Address encoding/decoding ────────────────────────────────
    static int  decodeAddress(uint8_t b0, uint8_t b1);
    /// Uses 14-bit addressing for every address once the current screen
    /// exceeds 4096 positions, as x3270 does; 12-bit otherwise.
    void encodeAddress(int offset, uint8_t out[2]) const;

    // ── Dirty tracking (for UI redraws) ──────────────────────────────────────
    bool isDirty() const  { return dirty_; }
    void clearDirty()     { dirty_ = false; }
    void markDirty()      { dirty_ = true; }

    /// Find the field attribute cell that governs bufPos.
    /// Returns the index of the FA cell, or -1 if the screen is unformatted.
    int findFieldStart(int bufPos) const;

private:
    int clamp(int pos) const {
        int sz = rows_ * cols_;
        if (pos < 0) pos = (pos % sz + sz) % sz;
        return pos % sz;
    }

    TerminalModel model_;
    bool    alternate_        { true };   // power-on: model (alternate) size
    int     rows_;
    int     cols_;
    std::vector<Cell> cells_;
    int     cursorPos_        { 0 };
    int     bufPtr_           { 0 };
    bool    dirty_            { false };
    uint8_t currentFgColor_   { 0x00 };  // active SA character fg colour
    uint8_t currentBgColor_   { 0x00 };  // active SA character bg colour
    uint8_t currentHighlight_ { 0x00 };  // active SA highlight (reverse/underscore/blink)
    ReplyMode replyMode_      { ReplyMode::Field };
    std::vector<uint8_t> replyAttrTypes_; // Character mode: types reported with SA
    uint8_t aid_              { AID_NONE };
    std::vector<Cell> savedErrorLine_;   // see saveErrorLine()

    // Character reply mode: append SA orders for the listed attribute types
    // whose value differs from what the host already has (`cur`: fg, bg,
    // highlight, starting at 0 for each record).
    void appendCharAttrs(std::vector<uint8_t>& rec, std::vector<uint8_t>* logCopy,
                         uint8_t fg, uint8_t bg, uint8_t hl, uint8_t cur[3]) const;

    // Buffer address code table (64 entries, 6-bit index → 8-bit wire byte)
    static const uint8_t kCodeTable[64];
};

} // namespace x3270
