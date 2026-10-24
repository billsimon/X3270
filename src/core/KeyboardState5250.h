#pragma once
#include "ScreenBuffer.h"
#include "EbcdicCodec.h"
#include <cstdint>
#include <vector>
#include <functional>
#include <string>

namespace x3270 {

// ── 5250 AID codes (client → host, per IBM SA21-9247) ────────────────────────
static constexpr uint8_t AID5250_NO_AID   = 0x60;
static constexpr uint8_t AID5250_ENTER    = 0xF1;
static constexpr uint8_t AID5250_F1       = 0x31;
static constexpr uint8_t AID5250_F2       = 0x32;
static constexpr uint8_t AID5250_F3       = 0x33;
static constexpr uint8_t AID5250_F4       = 0x34;
static constexpr uint8_t AID5250_F5       = 0x35;
static constexpr uint8_t AID5250_F6       = 0x36;
static constexpr uint8_t AID5250_F7       = 0x37;
static constexpr uint8_t AID5250_F8       = 0x38;
static constexpr uint8_t AID5250_F9       = 0x39;
static constexpr uint8_t AID5250_F10      = 0x3A;
static constexpr uint8_t AID5250_F11      = 0x3B;
static constexpr uint8_t AID5250_F12      = 0x3C;
static constexpr uint8_t AID5250_F13      = 0xB1;
static constexpr uint8_t AID5250_F14      = 0xB2;
static constexpr uint8_t AID5250_F15      = 0xB3;
static constexpr uint8_t AID5250_F16      = 0xB4;
static constexpr uint8_t AID5250_F17      = 0xB5;
static constexpr uint8_t AID5250_F18      = 0xB6;
static constexpr uint8_t AID5250_F19      = 0xB7;
static constexpr uint8_t AID5250_F20      = 0xB8;
static constexpr uint8_t AID5250_F21      = 0xB9;
static constexpr uint8_t AID5250_F22      = 0xBA;
static constexpr uint8_t AID5250_F23      = 0xBB;
static constexpr uint8_t AID5250_F24      = 0xBC;
static constexpr uint8_t AID5250_ROLL_UP  = 0xF5; ///< Page Down (Roll Up)
static constexpr uint8_t AID5250_ROLL_DN  = 0xF4; ///< Page Up  (Roll Down)
static constexpr uint8_t AID5250_PRINT    = 0xF6;
static constexpr uint8_t AID5250_HELP     = 0xF3;
static constexpr uint8_t AID5250_CLEAR    = 0xBD;
static constexpr uint8_t AID5250_ATTN     = 0x77;
static constexpr uint8_t AID5250_HOME     = 0x70;

inline uint8_t pf5250AID(int n) {
    static constexpr uint8_t kAIDs[24] = {
        AID5250_F1,  AID5250_F2,  AID5250_F3,  AID5250_F4,
        AID5250_F5,  AID5250_F6,  AID5250_F7,  AID5250_F8,
        AID5250_F9,  AID5250_F10, AID5250_F11, AID5250_F12,
        AID5250_F13, AID5250_F14, AID5250_F15, AID5250_F16,
        AID5250_F17, AID5250_F18, AID5250_F19, AID5250_F20,
        AID5250_F21, AID5250_F22, AID5250_F23, AID5250_F24,
    };
    if (n >= 1 && n <= 24) return kAIDs[n - 1];
    return AID5250_ENTER;
}

// ── KeyboardState5250 ─────────────────────────────────────────────────────────
class KeyboardState5250 {
public:
    enum class LockReason {
        None,
        Connecting,
        System,   ///< Waiting for host response after AID
        OErr,     ///< Typing error (protected/full field)
    };

    /// record goes to the host; logCopy has password field data masked (traffic log).
    using SendRecordCallback = std::function<bool(const std::vector<uint8_t>& record,
                                                  const std::vector<uint8_t>& logCopy)>;
    /// Sends an empty record whose GDS header carries `flags` (ATN / SRQ).
    using SignalCallback     = std::function<bool(uint8_t headerFlags)>;

    KeyboardState5250(ScreenBuffer& screen, EbcdicCodec& codec);

    void setSendCallback(SendRecordCallback cb) { sendCb_ = std::move(cb); }
    void setSignalCallback(SignalCallback cb)   { signalCb_ = std::move(cb); }

    /// Host Read Immediate: send the modified fields now, with no AID key.
    void sendReadImmediate();

    void lock(LockReason reason);
    void unlock();
    bool isLocked()       const { return lockReason_ != LockReason::None; }
    LockReason lockReason() const { return lockReason_; }

    bool isInsertMode() const { return insertMode_; }

    /// Forget a parked Field-Exit-Required cursor.  Call when something other
    /// than the editing keys moves the cursor or changes the screen (a host
    /// record, a mouse click).
    void clearFieldExitRequired() { ferPos_ = -1; }
    void toggleInsert()       { insertMode_ = !insertMode_; }

    // ── Key handlers ──────────────────────────────────────────────────────────
    bool handleChar(uint32_t unicodeChar);  // typed character (any code page graphic)
    bool handleEbcdicChar(uint8_t ebcdic); // raw EBCDIC input (used for pasting)
    bool handleTab(bool backward = false);
    bool handleEnter();
    bool handleClear();
    bool handlePFKey(int n);       ///< F1–F24
    bool handlePageUp();           ///< Roll Down
    bool handlePageDown();         ///< Roll Up
    bool handleHelp();
    bool handleAttn();
    bool handleSysReq();
    bool handleReset();            ///< Clear an error (and its message line)
    bool handleFieldExit();
    bool handleHome();
    bool handleArrow(int dRow, int dCol);
    bool handleBackspace();
    bool handleDelete();
    bool handleEraseField();       ///< Erase to end of field
    bool handleInsert();

    /// Paste text the way x3270 does (see pasteText in Paste.h); returns the
    /// number of characters used, fewer than text.size() if pasting stopped.
    size_t paste(const std::u32string& text);

private:
    ScreenBuffer& screen_;
    EbcdicCodec&  codec_;

    SendRecordCallback sendCb_;
    SignalCallback     signalCb_;
    LockReason  lockReason_ { LockReason::Connecting };
    bool        insertMode_ { false };
    int         ferPos_     { -1 };   ///< filled Field-Exit-Required field: cursor parked here

    /// FA of the input field whose character positions include `pos`, or -1.
    int  inputFieldAt(int pos) const;
    /// One past the last character position of the field whose FA is at `fa`.
    int  fieldEnd(int fa) const;
    std::vector<uint8_t> buildInputRecord(uint8_t aidCode, bool includeModifiedFields,
                                          std::vector<uint8_t>* logCopy = nullptr) const;

    uint8_t currentFieldAttr() const;
    bool    isCurrentFieldEditable() const;
    void    advanceToNextField(bool forward);
    void    moveCursorToFirstUnprotected();
    // autoSkip false (overlay paste): just move right one position after the character.
    bool    insertCharAtCursor(uint8_t ebcdic, bool autoSkip = true);

    // Build and send a 5250 input record.
    // record layout: [AID][cursor_row][cursor_col][SBA+data per modified field...]
    void sendAID(uint8_t aidCode, bool includeModifiedFields);

    // Encode cursor position as 1-indexed [row][col]
    void encodeCursor(uint8_t& row, uint8_t& col) const;
};

} // namespace x3270
