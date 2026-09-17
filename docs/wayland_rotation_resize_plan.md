# Wayland Display: per-orientation output resize (scope)

Goal (user spec, 2026-07-24): when the device rotates, do NOT stretch the
landscape-shaped canvas into the new viewport. Resize the compositor's actual
output resolution to match the orientation, and let existing windows
reflow/rescale to the new geometry.

## Feasibility: PROVEN on-device (m4pt, wayvnc 0.10.1, labwc 0.20.1, 2026-07-24)

Every load-bearing mechanism was verified live against a real session before
writing this plan:

1. **labwc honors runtime output mode changes** via the
   wlr-output-management protocol: `wlr-randr --output HEADLESS-1
   --custom-mode 720x1280` applied instantly, both directions.
2. **wayvnc implements client-initiated desktop resizing** (the RFB
   SetDesktopSize / ExtendedDesktopSize extension, msg 251 / encodings
   -308 & -223) **and forwards it to the compositor**: a bare protocol
   probe sending SetDesktopSize resized the actual labwc output both
   directions, confirmed via `wlr-randr` after each request. Upstream
   documents this as working *specifically for headless outputs* — exactly
   this stack. **This means zero guest-side work**: no control channel into
   the running session, no wlr-randr dependency, no start-wayland.sh
   changes. The app's existing RFB connection is the whole transport.
3. **Existing maximized windows reflow** to the new geometry. Transient
   caveat: under JIT the reflow of an already-open window can lag (observed
   settling by the time focus returned to it); freshly opened windows (via
   the rc.xml `windowRule identifier="*" → Maximize`) land at the new
   geometry immediately and perfectly.
4. **wayvnc emits ExtendedDesktopSize rects** describing the screen layout
   (parsed one: single screen id 0). The rect that immediately answers a
   SetDesktopSize has reason 1 and neatvnc's nonstandard status 4,
   "forwarded", and carries the size that was ASKED FOR: the apply is async,
   and the real size arrives later as a server-initiated rect (reason 0,
   status 0). A brand-new connection's ServerInit right after a resize can
   lag one step behind.

   **Corrected 2026-09-17 after a wayvnc crash on Devuan 6.** The original
   advice here was to treat every size rect as authoritative and to send a
   non-incremental update request after any size change. With neatvnc 0.9.1
   (Debian 13 / Devuan 6) that kills wayvnc with SIGSEGV, on AOK and on
   native Linux alike. neatvnc 0.9.1 encodes pending damage against the
   current buffer without clamping it (fixed in 0.9.2, "server: Clamp damage
   to fb size"). A non-incremental request's region, and the size neatvnc
   re-announces after one, outlive a buffer that shrinks under them, and the
   raw encoder reads past its end. The client now ignores forwarded replies,
   sends only incremental requests after the one at connect, and holds each
   SetDesktopSize until the first frame has arrived and any earlier request
   has been answered. See `DisplayRFBClient.m`.

   That removes the crash the client caused, not the bug. Screen activity
   queued while a frame is being encoded is damage too, and when a shrink
   lands just then 0.9.1 still reads past the new frame. With a terminal
   printing continuously it did so in 8 of 10 resize runs under AOK's
   emulation, and on native Linux in 8 of 8 once the client took 300 ms per
   frame (3 of them SIGSEGV). No client sequencing avoids it, so
   `start-wayland.sh` runs wayvnc with `--disable-resizing` when
   `wayvnc -V` reports neatvnc older than 0.9.2. Those roots keep the
   default desktop, scaled to fit.

   A second neatvnc bug governs WHEN a SetDesktopSize may be sent, in the
   0.9.1 and 1.0.0 source alike. The answer is written in four pieces, and a
   frame that finishes sending in between gets the server's next message (a
   cursor update) written into the middle of it. Recorded with wayvnc 0.10.0 after a request
   sent mid-way through a 2560x1440 frame: the client read a 65535x65297 rect
   and dropped the connection. The client now sends the size only after an
   update has been read and before it is acknowledged, and never acknowledges
   an update that is only such an answer, so no frame can be on its way.

## Design — entirely app-side

### DisplayRFBClient.m (bulk of the work, ~150–250 lines)

- Advertise `-308` (ExtendedDesktopSize) and `-223` (DesktopSize) in
  `_sendSetEncodings` alongside Raw/CopyRect/Cursor.
- Handle both pseudo-rects in the update loop: reallocate `_framebuffer`,
  update `_framebufferWidth/Height`, notify the delegate so
  DisplayRFBView drops its stale Metal texture (`ensureTextureWithWidth:`
  already handles size changes) and rescales the cursor overlay.
  - Buffer lifecycle: the resize must not race the view's in-flight
    texture upload — reuse the existing `acknowledgeFramebufferRead`
    handshake (perform the realloc on the client's own read thread, same
    place rect payloads are written today).
  - Track the current screen list (id/flags) from EDS rects for echo-back.
- New API: `-requestDesktopSizeWidth:height:` sending message 251 with the
  tracked screen layout (fallback: single screen id 0, flags 0).
- Transition robustness: after sending a resize request, tolerate rects
  that exceed current bounds (drop/clip) instead of the current
  fail-the-connection behavior, until the confirming size rect arrives.
  No non-incremental request afterwards (see the correction in 4 above):
  neatvnc damages the whole new desktop itself when it announces a size.

### DisplayViewController.m (small)

- In the existing `viewWillTransitionToSize:` completion block: compute the
  target resolution and call `requestDesktopSizeWidth:height:`.
  v1 policy was a fixed pair — 1280x720 landscape, 720x1280 portrait,
  standalone mode only. **Superseded (4131facd, #483/#482):** the size now
  follows the display surface in both standalone and Workspace mode — one
  desktop pixel per point, short side at least 480, long side at most 2560,
  even dimensions — requested on connect and after each resize settles
  (0.4 s), once per size per connection. See DisplayDesktopSizeForViewSize.

### No changes: setup-wayland.sh, guest packages, emulator core.

(start-wayland.sh did get one, later: `--disable-resizing` for neatvnc before
0.9.2, see the correction in 4.)

## Fallback behavior

If the server ignores or refuses the request (older wayvnc, non-headless
output), no size rect arrives and the client keeps its current framebuffer —
exactly today's stretch behavior. Strictly additive.

## Risks

- The realloc/in-flight-rect boundary is the one genuinely fiddly part
  (wrong handling = crash or garbled frame on rotate). Mitigation above.
- JIT-slow reflow transient of already-open windows (~1–2s of stale layout
  after rotate) — cosmetic, self-resolving.
- wayvnc's nonstandard status code. Originally never read; since the
  Devuan 6 crash (4 above), a reply with status 4 is read as "forwarded, not
  applied yet".

## Verification plan

- Protocol layer testable without the app: the same probe technique used
  for this scope (raw RFB SetDesktopSize + screenshot) from the Mac against
  the device session.
- App layer: on-device rotate → fresh VNC screenshot must report the new
  framebuffer dimensions; user visual confirmation for the native side.
- Regression: existing connect/Reconnect flows, plus a rotate performed
  while disconnected (request must not be sent on a dead client).
