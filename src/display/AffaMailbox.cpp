#include "AffaMailbox.h"
#include <string.h>

namespace MegaOpen
{
    static void copyStr(char* dst, const char* src)
    {
        if (!src) { dst[0] = '\0'; return; }
        strncpy(dst, src, AffaMailbox::STR_MAX - 1);
        dst[AffaMailbox::STR_MAX - 1] = '\0';
    }

    const char* AffaMailbox::opName(Op o)
    {
        switch (o) {
            case Op::Text:           return "text";
            case Op::TextStyled:     return "text/styled";
            case Op::Time:           return "time";
            case Op::Power:          return "power";
            case Op::Menu:           return "menu";
            case Op::Highlight:      return "highlight";
            case Op::Popup:          return "popup";
            case Op::HidePopup:      return "popup/hide";
            case Op::Fullscreen:     return "fullscreen";
            case Op::ConfirmBox:     return "confirm";
            case Op::MessageBox:     return "msgbox";
            case Op::SelectButton:   return "msgbox/select";
            case Op::InfoPopup:      return "info";
            case Op::Key:            return "key";
            case Op::Abort:          return "abort";
            case Op::FullscreenGeom: return "fullscreen/geom";
            case Op::Resync:         return "resync";
            case Op::Clear:          return "clear";
            case Op::NavBitmap:      return "pane";
        }
        return "?";
    }

    bool AffaMailbox::holdsGlass(Op o)
    {
        switch (o) {
            case Op::Text:
            case Op::TextStyled:
            case Op::Menu:
            case Op::Highlight:
            case Op::Fullscreen:
            case Op::ConfirmBox:
            case Op::MessageBox:
            case Op::SelectButton:
            case Op::InfoPopup:
            case Op::FullscreenGeom:
                return true;
            default:
                return false;
        }
    }

    bool AffaMailbox::begin(uint8_t depth)
    {
        if (_q) return true;
        _q = xQueueCreate(depth, sizeof(Req));
        return _q != nullptr;
    }

    AffaMailbox::ReqId AffaMailbox::post(const Req& in)
    {
        Req r = in;
        r.id = _ids.next();
        // Zero wait: an HTTP handler must never block behind the display.
        if (!_q || xQueueSend(_q, &r, 0) != pdTRUE) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            return MailboxIds::NO_REQ;
        }
        _posted.fetch_add(1, std::memory_order_relaxed);
        return r.id;
    }

    AffaMailbox::ReqId AffaMailbox::postRows(const Req& in)
    {
        if (!_q) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            return MailboxIds::NO_REQ;
        }
        Req r = in;
        r.id = _ids.next();
        _rowsLock.lock();
        const bool replaced = _rowsFull;
        _rows = r;
        _rowsFull = true;
        _rowsLock.unlock();
        if (replaced) _rowsCoalesced.fetch_add(1, std::memory_order_relaxed);
        _posted.fetch_add(1, std::memory_order_relaxed);
        return r.id;
    }

    AffaMailbox::ReqId AffaMailbox::postNavBitmap(uint8_t slot, uint16_t epoch)
    {
        Req r;
        r.op = Op::NavBitmap;
        r.slot = slot;
        r.track = true;
        r.yields = true;
        r.epoch = epoch;
        r.deliberate = false;
        return post(r);
    }

    bool AffaMailbox::postText(const char* text, bool deliberate)
    {
        Req r; r.op = Op::Text; copyStr(r.a, text);
        r.deliberate = deliberate;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postTime(const char* hhmm)
    {
        Req r; r.op = Op::Time; copyStr(r.a, hhmm);
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postPower(bool on)
    {
        Req r; r.op = Op::Power; r.flag = on;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postMenu(const char* h, const char* r0, const char* r1, uint8_t scroll)
    {
        Req r; r.op = Op::Menu;
        copyStr(r.a, h); copyStr(r.b, r0); copyStr(r.c, r1);
        r.u16 = scroll;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postHighlight(uint8_t row)
    {
        Req r; r.op = Op::Highlight; r.u16 = row;
        return post(r) != MailboxIds::NO_REQ;
    }

    affa::Result sendFullscreen(affa::CarminatDisplay& d, const char* l1, const char* l2,
                                const char* l3, uint8_t declLen, uint8_t b5, uint8_t start)
    {
        uint8_t p[2 + 0x70];
        if (declLen < 34 || declLen > sizeof(p) - 2 || start < 32 || start >= declLen)
            return affa::Result::BadArgument;
        memset(p, 0x00, sizeof(p));
        p[0] = 0x10;                  // ISO-TP first frame
        p[1] = declLen;
        p[2] = 0x21;                  // screen command
        p[3] = 0x05;                  // fullscreen
        p[4] = 0xFF;
        p[5] = 0x00;
        p[6] = 0x00;
        p[7] = b5 ? b5 : (uint8_t)(declLen - 0x20);
        for (size_t k = 2 + 32; k < 2u + declLen; ++k) p[k] = 0x20;

        size_t q = 2 + start;
        const size_t end = 2u + declLen;
        for (const char* row : {l1, l2, l3}) {
            char t[40];
            if (!row || !affa::toAscii(row, t, sizeof(t))) continue;
            for (const char* c = t; *c && q < end - 1; ++c) p[q++] = (uint8_t)*c;
            if (q < end) p[q++] = 0x0D;
        }

        affa::TxOptions opt;
        opt.coalesce = false;
        return (d.enqueue(0x151, p, (uint8_t)end, opt) == affa::kNoTicket) ? d.lastResult()
                                                                           : affa::Result::Ok;
    }

    bool AffaMailbox::postPopup(const char* text, uint8_t icon, uint8_t srcIcon, uint8_t fmt)
    {
        Req r; r.op = Op::Popup; copyStr(r.a, text);
        r.p[0] = icon; r.p[1] = srcIcon; r.p[2] = fmt;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postConfirmBox(const char* caption, const char* row0, const char* row1)
    {
        Req r; r.op = Op::ConfirmBox;
        copyStr(r.a, caption); copyStr(r.b, row0); copyStr(r.c, row1);
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postInfoPopup(const char* l1, const char* l2, const char* l3)
    {
        Req r; r.op = Op::InfoPopup;
        copyStr(r.a, l1); copyStr(r.b, l2); copyStr(r.c, l3);
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postMessageBox(const char* row0, const char* row1, const char* labels,
                                     uint8_t buttonCount, uint8_t selected)
    {
        Req r; r.op = Op::MessageBox;
        copyStr(r.a, row0); copyStr(r.b, row1); copyStr(r.c, labels);
        // Max two buttons: the library's length formula is 105 + 6*buttons.
        r.p[0] = buttonCount > 2 ? 2 : buttonCount;
        // No buttons: the OEM sends 0xFF as the selection.
        r.p[1] = r.p[0] ? (selected < r.p[0] ? selected : 0) : 0xFF;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postSelectButton(uint8_t index)
    {
        Req r; r.op = Op::SelectButton; r.u16 = index;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postTextStyled(const char* text, uint8_t icon, uint8_t srcIcon,
                                     uint8_t fmt, uint8_t iconBank2)
    {
        Req r; r.op = Op::TextStyled; copyStr(r.a, text);
        r.p[0] = icon; r.p[1] = srcIcon; r.p[2] = fmt; r.p[3] = iconBank2;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postHidePopup()
    {
        Req r; r.op = Op::HidePopup;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postFullscreen(const char* l1, const char* l2, const char* l3)
    {
        Req r; r.op = Op::Fullscreen;
        copyStr(r.a, l1); copyStr(r.b, l2); copyStr(r.c, l3);
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postKey(uint16_t wireCode, bool hold, bool wire)
    {
        Req r; r.op = Op::Key; r.u16 = wireCode; r.flag = hold; r.p[0] = wire ? 1 : 0;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postAbort()
    {
        Req r; r.op = Op::Abort;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postResync()
    {
        Req r; r.op = Op::Resync; r.deliberate = false;
        return post(r) != MailboxIds::NO_REQ;
    }

    bool AffaMailbox::postClear()
    {
        Req r; r.op = Op::Clear; r.deliberate = false;
        return post(r) != MailboxIds::NO_REQ;
    }

    uint32_t AffaMailbox::pending() const
    {
        const uint32_t q = _q ? (uint32_t)uxQueueMessagesWaiting(_q) : 0u;
        return q + (_rowsFull ? 1u : 0u);
    }

    affa::Result AffaMailbox::render(affa::AffaDisplayBase& d, const Req& r)
    {
        affa::Result res = affa::Result::Ok;
        switch (r.op) {
            case Op::Text:           res = d.setText(r.a); break;
            case Op::Time:           res = d.setTime(r.a); break;
            case Op::Power:          res = d.setPower(r.flag); break;
            case Op::Menu:           res = d.showMenu(r.a, r.b, r.c, (uint8_t)r.u16); break;
            case Op::Highlight:      res = d.highlightItem((uint8_t)r.u16); break;
            case Op::Popup:          res = d.showPopupText(r.a, r.p[0], r.p[1], r.p[2]); break;
            case Op::HidePopup:      res = d.hidePopup(); break;
            case Op::FullscreenGeom:
                res = _carminat ? sendFullscreen(*_carminat, r.a, r.b, r.c,
                                                 r.p[0], r.p[1], r.p[2])
                                : affa::Result::NotSupported;
                break;
            case Op::Fullscreen:     res = d.showFullscreenText(r.a, r.b, r.c); break;
            case Op::ConfirmBox:     res = d.showConfirmBox(r.a, r.b, r.c); break;
            case Op::InfoPopup:      res = d.showInfoPopup(r.a, r.b, r.c); break;

            // Carminat-only screens; an UpdateList panel has none, so guard
            // on _carminat rather than cast.
            case Op::TextStyled:
            case Op::MessageBox:
            case Op::SelectButton: {
                affa::CarminatDisplay* const c = _carminat;
                if (!c) { res = affa::Result::NotSupported; break; }
                if (r.op == Op::TextStyled) {
                    res = c->setTextStyled(r.a, r.p[0], r.p[1], r.p[2], r.p[3]);
                    break;
                }
                if (r.op == Op::SelectButton) {
                    res = c->selectBoxButton((uint8_t)r.u16);
                    break;
                }
                // "YES,NO" -> two labels; the library truncates to six bytes.
                char l0[8] = {0}, l1[8] = {0};
                const char* lab[2] = {l0, l1};
                const char* comma = strchr(r.c, ',');
                if (comma) {
                    const size_t n = (size_t)(comma - r.c);
                    memcpy(l0, r.c, n < sizeof(l0) - 1 ? n : sizeof(l0) - 1);
                    snprintf(l1, sizeof(l1), "%s", comma + 1);
                } else {
                    snprintf(l0, sizeof(l0), "%s", r.c);
                }
                res = c->showMessageBox(r.a, r.b, r.p[0] ? lab : nullptr,
                                        r.p[0], r.p[1] == 0xFF ? 0 : r.p[1]);
                break;
            }
            // The pane: the library keeps the pointer until the ticket
            // completes, which is why the slot is only released then.
            case Op::NavBitmap:
                res = (_carminat && _pane && r.slot < PaneBuffers::SLOTS)
                          ? _carminat->showNavBitmap(_pane->data(r.slot))
                          : affa::Result::NotSupported;
                break;
            case Op::Key:
                // Local by default: in the radio role we receive key frames,
                // and Local takes the same path as a key decoded off the bus.
                // p[0] asks for Wire: the frame itself, onto the bus.
                res = d.pressKey((affa::Key)r.u16,
                                 r.flag ? affa::KeyEdge::Hold : affa::KeyEdge::Click,
                                 r.p[0] ? affa::KeySource::Wire : affa::KeySource::Local);
                break;
            case Op::Abort:
                d.abortPending();
                res = affa::Result::Ok;
                break;
            // Resets both FSMs and cancels the queue: whatever was posted
            // before it and is still queued completes Cancelled (the tracked
            // ones come back to the ui through libComplete, slots released).
            case Op::Resync:
                res = d.begin() ? affa::Result::Ok : affa::Result::LinkDown;
                ++_resyncs;
                break;
            // Nothing for the library: the ui task acts on the count.
            case Op::Clear:
                res = affa::Result::Ok;
                ++_clears;
                break;
        }
        return res;
    }

    void AffaMailbox::complete(const MailboxIds::Completion& c)
    {
        // The slot first: the ui may compose into it the moment it hears.
        if (_pane && c.slot != MailboxIds::NO_SLOT) _pane->release(c.slot);
        if (_done) _done(c, _doneCtx);
    }

    void AffaMailbox::apply(affa::AffaDisplayBase& d, const Req& r)
    {
        affa::Result res;
        // An automatic repaint decided before a person's screen went up: drop
        // it rather than draw over that screen. The ui learns of the screen
        // from PanelStatus on its next pass and holds the glass from there.
        if (r.yields && r.epoch != _deliberateSeq) {
            res = affa::Result::Aborted;
            ++_yielded;
            if (r.track) complete(MailboxIds::TicketMap::dropped(r.id, r.slot, (uint8_t)res));
            else if (_pane && r.slot != MailboxIds::NO_SLOT) _pane->release(r.slot);
        } else if (r.track) {
            // Read lastEnqueued() on both sides of the call: a new ticket is
            // this request's; none means refused or nothing to send, and the
            // verdict is synthesised now. A coalesced predecessor completes
            // Aborted from inside the call, under its own, older ticket.
            _map.beforeRender();
            const affa::TxTicket before = d.lastEnqueued();
            res = render(d, r);
            const affa::TxTicket after = d.lastEnqueued();
            MailboxIds::Completion c;
            if (_map.afterRender(r.id, r.slot, res == affa::Result::Ok, (uint8_t)res,
                                 before, after, c))
                complete(c);
        } else {
            res = render(d, r);
            // Only NavBitmap borrows, and it is always tracked: an untracked
            // borrow has no verdict to wait for, so a refusal gives it back.
            if (_pane && r.slot != MailboxIds::NO_SLOT && res != affa::Result::Ok)
                _pane->release(r.slot);
        }

        // A person's screen, accepted: the glass is theirs for a while.
        if (res == affa::Result::Ok && r.deliberate && holdsGlass(r.op)) ++_deliberateSeq;

        if (_report) _report(r, res, _ctx);
    }

    uint8_t AffaMailbox::drain(affa::AffaDisplayBase& d)
    {
        if (!_q) return 0;

        uint8_t n = 0;
        Req r;
        // Bounded so a busy producer cannot starve poll().
        while (n < 16 && xQueueReceive(_q, &r, 0) == pdTRUE) {
            apply(d, r);
            ++n;
        }
        // The rows last: a deliberate screen drained above overtakes them.
        if (_rowsFull) {
            _rowsLock.lock();
            const bool had = _rowsFull;
            if (had) { r = _rows; _rowsFull = false; }
            _rowsLock.unlock();
            if (had) { apply(d, r); ++n; }
        }
        return n;
    }

    void AffaMailbox::libComplete(affa::TxTicket t, affa::Result r)
    {
        MailboxIds::Completion c;
        if (_map.complete(t, (uint8_t)r, c)) complete(c);
    }
}
