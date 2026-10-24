#include "DataStreamParser.h"
#include <cstring>

namespace x3270 {

DataStreamParser::DataStreamParser(ScreenBuffer& screen, EbcdicCodec& codec)
    : screen_(screen), codec_(codec) {}

// ── Main entry point ──────────────────────────────────────────────────────────
void DataStreamParser::processRecord(const std::vector<uint8_t>& record) {
    if (record.empty()) return;

    // WSF records are handled separately (need full record to parse SFs)
    if (record[0] == CMD_WRITE_STRUCTURED_FIELD ||
        record[0] == CMD_WRITE_STRUCTURED_FIELD_SNA) {
        handleWSF(record);
        return;
    }

    // Reset FSM for each new record
    state_ = ParseState::Command;

    for (uint8_t b : record) {
        if (state_ == ParseState::SkipRecord) break;  // discard rest of WSF/unknown record
        switch (state_) {
        case ParseState::Command:
            handleCommand(b);
            break;
        case ParseState::WCC:
            handleWCC(b);
            break;
        case ParseState::Data:
            handleDataByte(b);
            break;
        case ParseState::SF_Attr:
            screen_.startField(b);
            state_ = ParseState::Data;
            break;
        case ParseState::SBA_Byte1:
            sbaByte1_ = b;
            state_ = ParseState::SBA_Byte2;
            break;
        case ParseState::SBA_Byte2:
            screen_.setBufferAddress(ScreenBuffer::decodeAddress(sbaByte1_, b));
            state_ = ParseState::Data;
            break;
        case ParseState::RA_Addr1:
            raAddr1_ = b;
            state_ = ParseState::RA_Addr2;
            break;
        case ParseState::RA_Addr2:
            sbaByte1_ = b; // reuse to store addr2
            state_ = ParseState::RA_Char;
            break;
        case ParseState::RA_Char:
            if (b == ORD_GE) { state_ = ParseState::RA_GEChar; break; }
            [[fallthrough]];
        case ParseState::RA_GEChar:
            screen_.repeatToAddress(ScreenBuffer::decodeAddress(raAddr1_, sbaByte1_), b);
            state_ = ParseState::Data;
            break;
        case ParseState::EUA_Addr1:
            euaAddr1_ = b;
            state_ = ParseState::EUA_Addr2;
            break;
        case ParseState::EUA_Addr2:
            screen_.eraseUnprotectedToAddress(ScreenBuffer::decodeAddress(euaAddr1_, b));
            state_ = ParseState::Data;
            break;
        case ParseState::SFE_Count:
            sfeCount_    = b;
            sfeCurAttr_  = 0x00;
            sfeFgColor_  = 0x00;
            sfeBgColor_  = 0x00;
            sfeHighlight_= 0x00;
            if (sfeCount_ == 0) {
                screen_.startField(0x00);
                state_ = ParseState::Data;
            } else {
                state_ = ParseState::SFE_Type;
            }
            break;
        case ParseState::SFE_Type:
            sfeType_ = b;
            state_ = ParseState::SFE_Value;
            break;
        case ParseState::SFE_Value:
            // Attribute type dispatch:
            //   0xC0 = basic field attribute (FA byte)
            //   0x41 = extended highlighting (0xF1=blink, 0xF2=reverse, 0xF4=underscore)
            //   0x42 = foreground colour (0xF1-0xF7 IBM 3279 palette)
            //   0x45 = background colour
            if      (sfeType_ == 0xC0) sfeCurAttr_   = b;
            else if (sfeType_ == 0x41) sfeHighlight_  = b;
            else if (sfeType_ == 0x42) sfeFgColor_    = b;
            else if (sfeType_ == 0x45) sfeBgColor_    = b;
            --sfeCount_;
            if (sfeCount_ == 0) {
                screen_.startField(sfeCurAttr_, sfeFgColor_, sfeBgColor_, sfeHighlight_);
                state_ = ParseState::Data;
            } else {
                state_ = ParseState::SFE_Type;
            }
            break;
        case ParseState::SA_Type:
            sfeType_ = b;
            state_ = ParseState::SA_Value;
            break;
        case ParseState::SA_Value:
            // SA (Set Attribute) applies to subsequent characters until field boundary.
            // Type codes match SFE: 0x42=fg colour, 0x45=bg colour, 0x41=highlight.
            // A value of 0x00 resets that attribute to the field default.
            // Type 0x00 resets all character attributes.
            if      (sfeType_ == 0x00) screen_.resetCharAttributes();
            else if (sfeType_ == 0x42) screen_.setCurrentFgColor(b);
            else if (sfeType_ == 0x45) screen_.setCurrentBgColor(b);
            else if (sfeType_ == 0x41) screen_.setCurrentHighlight(b);
            state_ = ParseState::Data;
            break;
        case ParseState::MF_Count:
            // Modify Field changes the attributes of the field whose FA is at
            // the buffer address, then steps past it.  If the address is not
            // an FA the pairs are skipped and the address does not move.
            mfCount_ = b;
            mfOnFA_  = screen_.at(screen_.bufferPointer()).isFA;
            if (mfCount_ == 0) {
                if (mfOnFA_) screen_.setBufferAddress(screen_.bufferPointer() + 1);
                state_ = ParseState::Data;
            } else {
                state_ = ParseState::MF_Type;
            }
            break;
        case ParseState::MF_Type:
            mfType_ = b;
            state_ = ParseState::MF_Value;
            break;
        case ParseState::MF_Value:
            if (mfOnFA_) screen_.modifyFieldAttribute(mfType_, b);
            --mfCount_;
            if (mfCount_ > 0) {
                state_ = ParseState::MF_Type;
            } else {
                if (mfOnFA_) screen_.setBufferAddress(screen_.bufferPointer() + 1);
                state_ = ParseState::Data;
            }
            break;
        case ParseState::GE_Char:
            // Graphic Escape: display character from alternate code page
            // Phase 1: treat as normal EBCDIC character
            writeData(b);
            state_ = ParseState::Data;
            break;
        case ParseState::SkipRecord:
            break; // unreachable — caught by early-exit before the switch
        }
    }
}

// ── Command dispatch ──────────────────────────────────────────────────────────
void DataStreamParser::handleCommand(uint8_t cmd) {
    switch (cmd) {
    case CMD_WRITE:
    case CMD_WRITE_SNA:
        doWrite(false, false);
        break;
    case CMD_ERASE_WRITE:
    case CMD_ERASE_WRITE_SNA:
        doWrite(true, false);
        break;
    case CMD_ERASE_WRITE_ALT:
    case CMD_ERASE_WRITE_ALT_SNA:
        doWrite(true, true);
        break;
    case CMD_ERASE_ALL_UNPROTECTED:
    case CMD_ERASE_ALL_UNPROTECTED_SNA:
        doEraseAllUnprotected();
        state_ = ParseState::SkipRecord; // no further data
        break;
    case CMD_READ_BUFFER:
    case CMD_READ_BUFFER_SNA:
        if (sendCb_) { std::vector<uint8_t> log; auto rec = screen_.buildReadBufferRecord(screen_.aid(), &log); sendCb_(rec, log); }
        state_ = ParseState::SkipRecord;
        break;
    case CMD_READ_MODIFIED:
    case CMD_READ_MODIFIED_SNA:
        // Host-solicited reads carry the AID of the last operator action
        // (a short read if that was PA or Clear).
        if (sendCb_) {
            std::vector<uint8_t> log;
            auto data = screen_.buildReadModifiedRecord(screen_.aid(), false, &log);
            sendCb_(data, log);
        }
        state_ = ParseState::SkipRecord;
        break;
    case CMD_READ_MODIFIED_ALL:
    case CMD_READ_MODIFIED_ALL_SNA:
        // Read Modified All: as Read Modified, but never a short read.
        if (sendCb_) {
            std::vector<uint8_t> log;
            auto data = screen_.buildReadModifiedRecord(screen_.aid(), true, &log);
            sendCb_(data, log);
        }
        state_ = ParseState::SkipRecord;
        break;
    case CMD_WRITE_STRUCTURED_FIELD:
    case CMD_WRITE_STRUCTURED_FIELD_SNA:
        // Handled in processRecord before the FSM — should not reach here
        state_ = ParseState::SkipRecord;
        break;
    default:
        // Unknown command — ignore the record rather than paint it as text
        state_ = ParseState::SkipRecord;
        break;
    }
}

void DataStreamParser::doWrite(bool eraseFirst, bool alternate) {
    if (eraseFirst) {
        // Erase/Write selects the default 24×80 screen, Erase/Write Alternate
        // the model's alternate size; both reset bufPtr_, cursorPos_ and SA.
        screen_.setAlternateSize(alternate);
    }
    // SA character attributes last only until the end of a Write.
    screen_.resetCharAttributes();
    // Every Write starts at the cursor address.
    screen_.setBufferAddress(screen_.cursorPos());
    lastWasOrder_ = true;
    lastZpt_      = false;
    eraseWrite_   = eraseFirst;
    state_ = ParseState::WCC;
}

void DataStreamParser::doEraseAllUnprotected() {
    // EAU also resets MDTs, moves the cursor to the first unprotected field
    // (address 0 if there is none) and unlocks the keyboard.
    screen_.eraseAllUnprotected();
    screen_.setCursor(0);
    screen_.setCursorToHome();
    screen_.setAid(AID_NONE);
    if (unlockCb_) unlockCb_();
}

// ── Write Control Character ───────────────────────────────────────────────────
void DataStreamParser::handleWCC(uint8_t wcc) {
    // Reset on an Erase/Write returns inbound replies to field mode.
    if ((wcc & WCC_RESET) && eraseWrite_) screen_.setReplyMode(ScreenBuffer::ReplyMode::Field);
    if (wcc & WCC_RESET_MDT) screen_.resetAllMDT();
    if (wcc & WCC_ALARM)     { if (alarmCb_) alarmCb_(); }
    if (wcc & WCC_UNLOCK)    { screen_.setAid(AID_NONE); if (unlockCb_) unlockCb_(); }
    state_ = ParseState::Data;
}

// ── Data / Order byte dispatch ────────────────────────────────────────────────
void DataStreamParser::writeData(uint8_t b) {
    screen_.writeChar(b);
    lastWasOrder_ = false;
    lastZpt_      = false;
}

// Program Tab: move to the first character of the next unprotected field,
// without wrapping (address 0 if there is none).  When PT follows data rather
// than a command or order, the rest of the current field is nulled first.
void DataStreamParser::programTab() {
    const int sz  = screen_.size();
    const int cur = screen_.bufferPointer();
    const Cell& here = screen_.at(cur);
    if (here.isFA && !here.isProtected()) {
        screen_.setBufferAddress(cur + 1);
        lastZpt_ = false;
        return;
    }
    int target = screen_.nextUnprotected(cur);
    if (target < cur) target = 0;                  // none, or only by wrapping

    if (!lastWasOrder_ || lastZpt_) {
        for (int pos = cur; pos != target && !screen_.at(pos).isFA; pos = (pos + 1) % sz)
            screen_.at(pos).ch = 0x00;
        lastZpt_ = (target == 0);
        screen_.markDirty();
    } else {
        lastZpt_ = false;
    }
    screen_.setBufferAddress(target);
}

void DataStreamParser::handleDataByte(uint8_t b) {
    if (b == ORD_PT) {
        programTab();
        lastWasOrder_ = true;
        return;
    }
    switch (b) {
    case ORD_SF: case ORD_SFE: case ORD_SBA: case ORD_SA: case ORD_IC:
    case ORD_RA: case ORD_EUA: case ORD_MF:
        lastWasOrder_ = true;
        lastZpt_      = false;
        break;
    default:
        break;
    }

    // Check if this byte is an order code
    switch (b) {
    case ORD_SF:   state_ = ParseState::SF_Attr;  break;
    case ORD_SFE:  state_ = ParseState::SFE_Count; break;
    case ORD_SBA:  state_ = ParseState::SBA_Byte1; break;
    case ORD_SA:   state_ = ParseState::SA_Type;   break;
    case ORD_IC:   screen_.insertCursorHere();     break; // IC: no params
    case ORD_RA:   state_ = ParseState::RA_Addr1;  break;
    case ORD_EUA:  state_ = ParseState::EUA_Addr1; break;
    case ORD_GE:   state_ = ParseState::GE_Char;   break;
    case ORD_MF:   state_ = ParseState::MF_Count;  break;
    default:
        // Plain data byte — write to buffer
        writeData(b);
        break;
    }
}

// ── setGraphicsBuffer ─────────────────────────────────────────────────────────
void DataStreamParser::setGraphicsBuffer(GraphicsBuffer& buf) {
    graphics_   = &buf;
    gocaParser_ = std::make_unique<GocaParser>(buf, codec_);
}

// ── Write Structured Field ────────────────────────────────────────────────────
// Parse each SF in the record.  Respond to Read Partition Query with a minimal
// Query Reply so ISPF/GDDM can identify the terminal and paint its screens.
//
// GOCA-bearing SF types (confirmed by traffic captures; update as needed):
//   0x01 — Read Partition (Query / QueryList)
//   0x0D — Begin/End of Graphics — signals a GOCA frame boundary
//   0x0E — Write Graphics Object (GOCA) — payload is a GOCA order stream
//   0x0F — Erase Graphics — discard the current graphics frame
//
// NOTE: The exact SF type bytes for GOCA records are implementation-defined by
// the host application (typically GDDM on VM/CMS).  The values above match the
// common GDDM-generated structured fields observed in typical TN3270E captures.
// Verify against Debug Window captures and adjust if the host uses different codes.
void DataStreamParser::handleWSF(const std::vector<uint8_t>& record) {
    bool graphicsUpdated = false;

    size_t i = 1; // skip WSF command byte
    while (i + 2 < record.size()) {
        // Each SF: [length_hi][length_lo][type][data...]
        // Length field includes itself (min 3 bytes).
        size_t sfLen = (static_cast<size_t>(record[i]) << 8) | record[i + 1];
        if (sfLen == 0) sfLen = record.size() - i;   // length 0: to the end of the record
        if (sfLen < 3 || i + sfLen > record.size()) break;

        uint8_t sfType = record[i + 2];

        // Pointer to the SF data payload (after the 3-byte header).
        const uint8_t* payload     = record.data() + i + 3;
        size_t         payloadSize = (sfLen >= 3) ? (sfLen - 3) : 0;

        switch (sfType) {

        case 0x01:
            // ── Read Partition ────────────────────────────────────────────────
            if (sfLen >= 5 && sendCb_) {
                uint8_t readType = record[i + 4]; // [partition_id][type]
                // Reads of the partition reply with the Read Partition AID.
                constexpr uint8_t AID_READ_PARTITION = 0x61;
                std::vector<uint8_t> rec, log;
                switch (readType) {
                case 0x02: // Query
                case 0x03: // Query List (we always reply with everything)
                    rec = log = buildQueryReply();
                    break;
                case 0xF2: // Read Buffer
                    rec = screen_.buildReadBufferRecord(AID_READ_PARTITION, &log);
                    break;
                case 0xF6: // Read Modified
                    rec = screen_.buildReadModifiedRecord(AID_READ_PARTITION, false, &log);
                    break;
                case 0x6E: // Read Modified All
                    rec = screen_.buildReadModifiedRecord(AID_READ_PARTITION, true, &log);
                    break;
                default:
                    break;
                }
                if (!rec.empty()) sendCb_(rec, log);
            }
            break;

        case 0x03:
            // ── Erase/Reset: flag 0x80 selects the alternate screen size ──────
            if (payloadSize >= 1) {
                screen_.setAlternateSize((payload[0] & 0x80) != 0);
                screen_.resetCharAttributes();
            }
            break;

        case 0x09:
            // ── Set Reply Mode: [partition][mode][attribute types...] ─────────
            if (payloadSize >= 2 && payload[0] == 0x00 && payload[1] <= 0x02) {
                screen_.setReplyMode(static_cast<ScreenBuffer::ReplyMode>(payload[1]),
                                     std::vector<uint8_t>(payload + 2, payload + payloadSize));
            }
            break;

        case 0x40:
            // ── Outbound 3270DS: [partition][command][WCC][orders/data] ───────
            // An ordinary Write/EW/EWA/EAU carried inside a structured field.
            if (payloadSize >= 2 &&
                payload[1] != CMD_WRITE_STRUCTURED_FIELD &&
                payload[1] != CMD_WRITE_STRUCTURED_FIELD_SNA) {
                processRecord(std::vector<uint8_t>(payload + 1, payload + payloadSize));
            }
            break;

        case 0x0D:
            // ── Begin/End of Graphics ─────────────────────────────────────────
            // Signals the start of a new GOCA frame.  Reset parser state so
            // stale drawing commands from a previous frame are not re-applied.
            if (graphics_ && gocaParser_) {
                graphics_->clear();
                gocaParser_->reset();
            }
            break;

        case 0x0E:
            // ── Write Graphics Object (GOCA order stream) ─────────────────────
            if (graphics_ && gocaParser_ && payloadSize > 0) {
                gocaParser_->parseOrders(payload, payloadSize);
                graphicsUpdated = true;
            }
            break;

        case 0x0F:
            // ── Erase Graphics ────────────────────────────────────────────────
            if (graphics_ && gocaParser_) {
                graphics_->clear();
                gocaParser_->reset();
                graphicsUpdated = true;
            }
            break;

        default:
            // Unknown SF type — safely skip using sfLen.
            break;
        }

        i += sfLen;
    }

    if (graphicsUpdated && graphics_) {
        graphics_->markDirty();          // fires the update callback wired in Phase 6
        if (graphicsUpdateCb_) graphicsUpdateCb_();
    }
}

// ── Query Reply builder ───────────────────────────────────────────────────────
// AID 0x88 followed by one Query Reply structured field per supported
// function.  Every QR is [len_hi][len_lo][0x81][QCODE][body], where the
// length counts the whole SF (IBM GA23-0059 ch. 6; layouts match x3270).
std::vector<uint8_t> DataStreamParser::buildQueryReply() const {
    std::vector<uint8_t> r;
    r.push_back(0x88);

    auto put16 = [&r](int v) {
        r.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        r.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    // Append one QR: header with a placeholder length, body, then patch length.
    auto qr = [&r](uint8_t qcode, auto&& body) {
        size_t start = r.size();
        r.push_back(0x00); r.push_back(0x00);
        r.push_back(0x81);
        r.push_back(qcode);
        body();
        size_t len = r.size() - start;
        r[start]     = static_cast<uint8_t>(len >> 8);
        r[start + 1] = static_cast<uint8_t>(len & 0xFF);
    };

    // The host formats for the alternate (largest) size.
    const int rows = screen_.maxRows();
    const int cols = screen_.maxCols();

    // ── Summary (0x80): every QR in this reply, including itself ────────────
    qr(0x80, [&] {
        for (uint8_t q : { 0x80, 0x81, 0x86, 0x87, 0x88, 0xA6 }) r.push_back(q);
    });

    // ── Usable Area (0x81) ───────────────────────────────────────────────────
    qr(0x81, [&] {
        r.push_back(0x01);           // 12/14-bit addressing allowed
        r.push_back(0x00);           // no special character features
        put16(cols);                 // width in cells
        put16(rows);                 // height in cells
        r.push_back(0x01);           // units: millimetres
        put16(1); put16(4);          // Xr = 1/4 mm per pel
        put16(1); put16(4);          // Yr = 1/4 mm per pel
        r.push_back(0x09);           // AW: cell width in pels  (kGocaCellW)
        r.push_back(0x0C);           // AH: cell height in pels (kGocaCellH)
        put16(rows * cols);          // buffer size
    });

    // ── Color (0x86): default → green, then the 7 base colours ──────────────
    qr(0x86, [&] {
        r.push_back(0x00);           // flags
        r.push_back(0x08);           // 8 attribute/colour pairs
        r.push_back(0x00); r.push_back(0xF4);
        for (uint8_t c = 0xF1; c <= 0xF7; ++c) { r.push_back(c); r.push_back(c); }
    });

    // ── Highlighting (0x87) ─────────────────────────────────────────────────
    qr(0x87, [&] {
        r.push_back(0x05);           // 5 pairs
        r.push_back(0x00); r.push_back(0xF0); // default    → normal
        r.push_back(0xF1); r.push_back(0xF1); // blink
        r.push_back(0xF2); r.push_back(0xF2); // reverse
        r.push_back(0xF4); r.push_back(0xF4); // underscore
        r.push_back(0xF8); r.push_back(0xF8); // intensify
    });

    // ── Reply Modes (0x88): field, extended field and character ─────────────
    qr(0x88, [&] {
        for (uint8_t m : { 0x00, 0x01, 0x02 }) r.push_back(m);
    });

    // ── Implicit Partition (0xA6): default and alternate screen sizes ───────
    qr(0xA6, [&] {
        r.push_back(0x00); r.push_back(0x00); // reserved
        r.push_back(0x0B);           // length of this self-defining parameter
        r.push_back(0x01);           // implicit partition sizes
        r.push_back(0x00);           // reserved
        put16(80);  put16(24);       // default width, height
        put16(cols); put16(rows);    // alternate width, height
    });

    return r;
}

} // namespace x3270
