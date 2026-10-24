#pragma once
#include "ScreenBuffer.h"
#include "EbcdicCodec.h"
#include "GraphicsBuffer.h"
#include "GocaParser.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <functional>

namespace x3270 {

// ── 3270 Host Commands ────────────────────────────────────────────────────────
static constexpr uint8_t CMD_WRITE              = 0x01;
static constexpr uint8_t CMD_WRITE_SNA          = 0xF1;
static constexpr uint8_t CMD_ERASE_WRITE        = 0x05;
static constexpr uint8_t CMD_ERASE_WRITE_SNA    = 0xF5;
static constexpr uint8_t CMD_ERASE_WRITE_ALT    = 0x0D;
static constexpr uint8_t CMD_ERASE_WRITE_ALT_SNA= 0x7E;
static constexpr uint8_t CMD_READ_BUFFER        = 0x02;
static constexpr uint8_t CMD_READ_BUFFER_SNA    = 0xF2;
static constexpr uint8_t CMD_READ_MODIFIED      = 0x06;
static constexpr uint8_t CMD_READ_MODIFIED_SNA  = 0xF6;
static constexpr uint8_t CMD_READ_MODIFIED_ALL  = 0x0E;
static constexpr uint8_t CMD_READ_MODIFIED_ALL_SNA = 0x6E;
static constexpr uint8_t CMD_ERASE_ALL_UNPROTECTED     = 0x0F;
static constexpr uint8_t CMD_ERASE_ALL_UNPROTECTED_SNA = 0x6F;
static constexpr uint8_t CMD_WRITE_STRUCTURED_FIELD     = 0x11;
static constexpr uint8_t CMD_WRITE_STRUCTURED_FIELD_SNA = 0xF3;

// ── 3270 Orders ───────────────────────────────────────────────────────────────
static constexpr uint8_t ORD_SF  = 0x1D; // Start Field
static constexpr uint8_t ORD_SFE = 0x29; // Start Field Extended
static constexpr uint8_t ORD_SBA = 0x11; // Set Buffer Address
static constexpr uint8_t ORD_SA  = 0x28; // Set Attribute
static constexpr uint8_t ORD_IC  = 0x13; // Insert Cursor
static constexpr uint8_t ORD_PT  = 0x05; // Program Tab
static constexpr uint8_t ORD_RA  = 0x3C; // Repeat to Address
static constexpr uint8_t ORD_EUA = 0x12; // Erase Unprotected to Address
static constexpr uint8_t ORD_GE  = 0x08; // Graphic Escape (next char from alt codepage)
static constexpr uint8_t ORD_MF  = 0x2C; // Modify Field

// Write Control Character (WCC) flags
static constexpr uint8_t WCC_RESET       = 0x40; // bit 1: reset
static constexpr uint8_t WCC_ALARM       = 0x04; // bit 5: sound alarm
static constexpr uint8_t WCC_UNLOCK      = 0x02; // bit 6: unlock keyboard
static constexpr uint8_t WCC_RESET_MDT   = 0x01; // bit 7: reset all MDT

class DataStreamParser {
public:
    using AlarmCallback          = std::function<void()>;
    using UnlockCallback        = std::function<void()>;
    /// data goes to the host; logCopy has password field data masked (traffic log).
    using SendCallback          = std::function<void(const std::vector<uint8_t>& data,
                                                     const std::vector<uint8_t>& logCopy)>;
    using GraphicsUpdateCallback= std::function<void()>;

    DataStreamParser(ScreenBuffer& screen, EbcdicCodec& codec);

    void setAlarmCallback(AlarmCallback cb)              { alarmCb_          = std::move(cb); }
    void setUnlockCallback(UnlockCallback cb)            { unlockCb_         = std::move(cb); }
    void setSendCallback(SendCallback cb)                { sendCb_           = std::move(cb); }
    void setGraphicsUpdateCallback(GraphicsUpdateCallback cb) { graphicsUpdateCb_ = std::move(cb); }

    /// Wire a GraphicsBuffer and its companion GocaParser.
    /// Must be called before the first processRecord() if GOCA support is desired.
    void setGraphicsBuffer(GraphicsBuffer& buf);

    /// Process a complete 3270 record from the host.
    /// For TN3270E mode, strip the 5-byte header before calling.
    void processRecord(const std::vector<uint8_t>& record);

private:
    enum class ParseState {
        Command,
        WCC,
        Data,           // scanning for orders vs data bytes
        SkipRecord,     // WSF or unknown — consume rest of record silently
        SF_Attr,        // expecting 1 attribute byte
        SBA_Byte1,      // expecting 2-byte address
        SBA_Byte2,
        RA_Addr1,       // RA: 2-byte dest address + 1 char
        RA_Addr2,
        RA_Char,
        RA_GEChar,      // RA: character introduced by GE
        EUA_Addr1,      // EUA: 2-byte dest address
        EUA_Addr2,
        SFE_Count,      // SFE: count byte
        SFE_Type,       // SFE: attribute type
        SFE_Value,      // SFE: attribute value
        SA_Type,
        SA_Value,
        MF_Count,
        MF_Type,
        MF_Value,
        GE_Char,        // next byte is alternate code page char
    };

    void handleCommand(uint8_t cmd);
    void handleWCC(uint8_t wcc);
    void handleDataByte(uint8_t b);
    void handleOrder(uint8_t order);

    void doWrite(bool eraseFirst, bool alternate);
    void doEraseAllUnprotected();
    void programTab();
    void writeData(uint8_t b);

    /// Parse a Write Structured Field record and respond to Read Partition Query.
    void handleWSF(const std::vector<uint8_t>& record);

    /// Build a Query Reply record (AID=0x88 + SFs) reflecting this screen's dimensions.
    std::vector<uint8_t> buildQueryReply() const;

    ScreenBuffer& screen_;
    EbcdicCodec&  codec_;

    ParseState   state_   { ParseState::Command };
    uint8_t      sbaByte1_{ 0 };
    uint8_t      raAddr1_ { 0 };
    uint8_t      euaAddr1_{ 0 };
    uint8_t      sfeCount_{ 0 };
    uint8_t      sfeType_ { 0 };
    uint8_t      sfeCurAttr_{ 0 };
    uint8_t      sfeFgColor_{ 0 };   // SFE/SA foreground colour accumulator
    uint8_t      sfeBgColor_{ 0 };   // SFE/SA background colour accumulator
    uint8_t      sfeHighlight_{ 0 }; // SFE/SA highlight accumulator
    uint8_t      mfCount_ { 0 };
    uint8_t      mfType_  { 0 };
    bool         mfOnFA_  { false }; // MF applies only when the buffer address is an FA

    // PT null-fills the rest of the field only when it follows data; these
    // track the previous item in the Write (as x3270's last_cmd / last_zpt).
    bool         lastWasOrder_ { true };
    bool         lastZpt_      { false };

    AlarmCallback           alarmCb_;
    UnlockCallback          unlockCb_;
    SendCallback            sendCb_;
    GraphicsUpdateCallback  graphicsUpdateCb_;

    GraphicsBuffer*              graphics_   { nullptr };
    std::unique_ptr<GocaParser>  gocaParser_;
};

} // namespace x3270
