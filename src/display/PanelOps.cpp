#include "PanelOps.h"
#include <string.h>
#include "InfoRaw.h"
#include "../utils/LogBus.h"

namespace MegaOpen
{
    static void copyStr(char* dst, const char* src)
    {
        if (!src) { dst[0] = '\0'; return; }
        strncpy(dst, src, PanelOps::STR_MAX - 1);
        dst[PanelOps::STR_MAX - 1] = '\0';
    }

    // Taken by the library: a render's ticket, or Ok from an op that has none.
    static bool accepted(const affa::Submitted& s) { return s.result == affa::Result::Ok; }

    const char* PanelOps::opName(Op o)
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
            case Op::InfoRaw:        return "info/raw";
            case Op::Key:            return "key";
            case Op::Abort:          return "abort";
            case Op::Resync:         return "resync";
            case Op::Clear:          return "clear";
            case Op::NavBitmap:      return "pane";
        }
        return "?";
    }

    bool PanelOps::holdsGlass(Op o)
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
            case Op::InfoRaw:
                return true;
            default:
                return false;
        }
    }

    bool PanelOps::postText(const char* text, bool deliberate)
    {
        Req r; r.op = Op::Text; copyStr(r.a, text);
        r.deliberate = deliberate;
        return accepted(submit(r));
    }

    bool PanelOps::postTime(const char* hhmm)
    {
        Req r; r.op = Op::Time; copyStr(r.a, hhmm);
        return accepted(submit(r));
    }

    bool PanelOps::postPower(bool on)
    {
        Req r; r.op = Op::Power; r.flag = on;
        return accepted(submit(r));
    }

    bool PanelOps::postMenu(const char* h, const char* r0, const char* r1, uint8_t scroll)
    {
        Req r; r.op = Op::Menu;
        copyStr(r.a, h); copyStr(r.b, r0); copyStr(r.c, r1);
        r.u16 = scroll;
        return accepted(submit(r));
    }

    bool PanelOps::postHighlight(uint8_t row)
    {
        Req r; r.op = Op::Highlight; r.u16 = row;
        return accepted(submit(r));
    }

    bool PanelOps::postPopup(const char* text, uint8_t icon, uint8_t srcIcon, uint8_t fmt)
    {
        Req r; r.op = Op::Popup; copyStr(r.a, text);
        r.p[0] = icon; r.p[1] = srcIcon; r.p[2] = fmt;
        return accepted(submit(r));
    }

    bool PanelOps::postConfirmBox(const char* caption, const char* row0, const char* row1)
    {
        Req r; r.op = Op::ConfirmBox;
        copyStr(r.a, caption); copyStr(r.b, row0); copyStr(r.c, row1);
        return accepted(submit(r));
    }

    bool PanelOps::postInfoPopup(const char* l1, const char* l2, const char* l3)
    {
        Req r; r.op = Op::InfoPopup;
        copyStr(r.a, l1); copyStr(r.b, l2); copyStr(r.c, l3);
        return accepted(submit(r));
    }

    bool PanelOps::postInfoRaw(const char* text, uint8_t len, uint8_t prefix, uint8_t offset)
    {
        Req r; r.op = Op::InfoRaw;
        copyStr(r.a, text);
        r.p[0] = len; r.p[1] = prefix; r.p[2] = offset;
        return accepted(submit(r));
    }

    bool PanelOps::postMessageBox(const char* row0, const char* row1, const char* labels,
                                     uint8_t buttonCount, uint8_t selected)
    {
        Req r; r.op = Op::MessageBox;
        copyStr(r.a, row0); copyStr(r.b, row1); copyStr(r.c, labels);
        // Max two buttons: the library's length formula is 105 + 6*buttons.
        r.p[0] = buttonCount > 2 ? 2 : buttonCount;
        // No buttons: the OEM sends 0xFF as the selection.
        r.p[1] = r.p[0] ? (selected < r.p[0] ? selected : 0) : 0xFF;
        return accepted(submit(r));
    }

    bool PanelOps::postSelectButton(uint8_t index)
    {
        Req r; r.op = Op::SelectButton; r.u16 = index;
        return accepted(submit(r));
    }

    bool PanelOps::postTextStyled(const char* text, uint8_t icon, uint8_t srcIcon,
                                     uint8_t fmt, uint8_t iconBank2)
    {
        Req r; r.op = Op::TextStyled; copyStr(r.a, text);
        r.p[0] = icon; r.p[1] = srcIcon; r.p[2] = fmt; r.p[3] = iconBank2;
        return accepted(submit(r));
    }

    bool PanelOps::postHidePopup()
    {
        Req r; r.op = Op::HidePopup;
        return accepted(submit(r));
    }

    bool PanelOps::postFullscreen(const char* l1, const char* l2, const char* l3)
    {
        Req r; r.op = Op::Fullscreen;
        copyStr(r.a, l1); copyStr(r.b, l2); copyStr(r.c, l3);
        return accepted(submit(r));
    }

    bool PanelOps::postKey(uint16_t wireCode, bool hold, bool wire)
    {
        Req r; r.op = Op::Key; r.u16 = wireCode; r.flag = hold; r.p[0] = wire ? 1 : 0;
        return accepted(submit(r));
    }

    bool PanelOps::postAbort()
    {
        Req r; r.op = Op::Abort;
        return accepted(submit(r));
    }

    bool PanelOps::postResync()
    {
        Req r; r.op = Op::Resync; r.deliberate = false;
        return accepted(submit(r));
    }

    bool PanelOps::postClear()
    {
        Req r; r.op = Op::Clear; r.deliberate = false;
        return accepted(submit(r));
    }

    void PanelOps::attach(affa::AffaDisplayBase* d, affa::CarminatDisplay* carminat)
    {
        _d = d;
        _carminat = carminat;
    }

    affa::Submitted PanelOps::submit(const Req& r)
    {
        affa::Submitted s = affa::Submitted::refused(affa::Result::NotSupported);
        if (_d) {
            // A person's screen, taken, holds the glass for a while; a repaint
            // decided before one was taken is not drawn over it (the ui learns
            // of the screen on its next pass and holds the glass from there).
            // Checked and rendered as one step (GlassGate.h).
            const GlassVerdict v = _glass.run(r.yields, r.epoch,
                                              r.deliberate && holdsGlass(r.op),
                                              [&]() { s = render(r); return accepted(s); });
            if (v == GlassVerdict::Yielded) s = affa::Submitted::refused(affa::Result::Aborted);
            // The lock is held for one ring push; not had within
            // GLASS_WAIT_MS, it is refused like a full ring.
            if (v == GlassVerdict::Busy)    s = affa::Submitted::refused(affa::Result::QueueFull);
        }

        if (accepted(s)) {
            _posted.fetch_add(1, std::memory_order_relaxed);
            // A key taken off the panel task is applied there; its verdict
            // comes through onComplete (keyDone), or is already here.
            uint8_t early;
            if (r.op == Op::Key && s.ticket && _keys.expect(s.ticket, early))
                logKey((affa::Result)early);
        } else {
            _refused.fetch_add(1, std::memory_order_relaxed);
            // The ui task says what matters about its own, rate-limited:
            // logging each refused repaint here would flood the log.
            if (!r.quiet)
                LogBus::printf("[disp] %s -> %s", opName(r.op), affa::resultName(s.result));
        }
        return s;
    }

    bool PanelOps::keyDone(affa::TxTicket t, affa::Result r)
    {
        if (!_keys.complete(t, (uint8_t)r)) return false;
        logKey(r);
        return true;
    }

    // The owner's verdict on a posted key, as the call-site refusals are logged.
    void PanelOps::logKey(affa::Result r)
    {
        if (r != affa::Result::Ok)
            LogBus::printf("[disp] %s -> %s", opName(Op::Key), affa::resultName(r));
    }

    affa::Submitted PanelOps::render(const Req& r)
    {
        affa::AffaDisplayBase& d = *_d;
        // The ops that are not renders have no ticket, only a result.
        affa::Submitted res{affa::kNoTicket, affa::Result::Ok};
        switch (r.op) {
            case Op::Text:           res = d.setText(r.a); break;
            case Op::Time:           res = d.setTime(r.a); break;
            case Op::Power:          res = d.setPower(r.flag); break;
            case Op::Menu:           res = d.showMenu(r.a, r.b, r.c, (uint8_t)r.u16); break;
            case Op::Highlight:      res = d.highlightItem((uint8_t)r.u16); break;
            case Op::Popup:          res = d.showPopupText(r.a, r.p[0], r.p[1], r.p[2]); break;
            case Op::HidePopup:      res = d.hidePopup(); break;
            case Op::Fullscreen:     res = d.showFullscreenText(r.a, r.b, r.c); break;
            case Op::ConfirmBox:     res = d.showConfirmBox(r.a, r.b, r.c); break;
            case Op::InfoPopup:      res = d.showInfoPopup(r.a, r.b, r.c); break;

            // Carminat-only screens; an UpdateList panel has none, so guard
            // on _carminat rather than cast.
            case Op::TextStyled:
            case Op::MessageBox:
            case Op::SelectButton: {
                affa::CarminatDisplay* const c = _carminat;
                if (!c) { res = affa::Submitted::refused(affa::Result::NotSupported); break; }
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
            // DEV ONLY: through the library's public enqueue, like any render
            // (the glass gate above it), never raw TWAI. The frame is built
            // and bounded by InfoRaw::build; out of range is BadArgument.
            case Op::InfoRaw: {
                if (!_carminat) { res = affa::Submitted::refused(affa::Result::NotSupported); break; }
                char t[InfoRaw::CELLS_MAX + 1];
                affa::toAscii(r.a, t, sizeof(t));
                uint8_t f[InfoRaw::FRAME_MAX];
                const size_t n = InfoRaw::build(f, r.p[0], r.p[1], r.p[2], t);
                if (!n) { res = affa::Submitted::refused(affa::Result::BadArgument); break; }
                affa::TxOptions opt;
                opt.coalesce = false;
                res = _carminat->enqueue(0x151, f, (uint8_t)n, opt);
                break;
            }
            // The pane: copied into the library's pool at the call.
            case Op::NavBitmap:
                res = (_carminat && r.img)
                          ? _carminat->showNavBitmap(r.img)
                          : affa::Submitted::refused(affa::Result::NotSupported);
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
                (void)d.abortPending();
                break;
            // Resets both FSMs and cancels the queue on the panel task: what
            // is still queued then completes Cancelled. Off that task it is
            // posted, and false means the library's ring was full.
            case Op::Resync:
                res.result = d.begin() ? affa::Result::Ok : affa::Result::QueueFull;
                if (accepted(res)) _resyncs.fetch_add(1, std::memory_order_relaxed);
                break;
            // Nothing for the library: the ui task acts on the count.
            case Op::Clear:
                _clears.fetch_add(1, std::memory_order_relaxed);
                break;
        }
        return res;
    }
}
