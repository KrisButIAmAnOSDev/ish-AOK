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
   (parsed one: single screen id 0). Caveat: the rect that immediately
   answers a SetDesktopSize can still carry the PRE-resize dimensions (the
   apply is async), and a brand-new connection's ServerInit right after a
   resize can lag one step behind. A client must treat the LAST
   DesktopSize/EDS rect received as authoritative and re-request a full
   update after any size change, and must not key success off the reply's
   status field (wayvnc 0.10.1 returned a nonstandard status=4 while
   demonstrably applying the resize).

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
  fail-the-connection behavior, until the confirming size rect arrives;
  then issue a non-incremental FramebufferUpdateRequest for a clean
  repaint.

### DisplayViewController.m (small)

- In the existing `viewWillTransitionToSize:` completion block: compute the
  target resolution and call `requestDesktopSizeWidth:height:`.
  v1 policy was a fixed pair — 1280x720 landscape, 720x1280 portrait,
  standalone mode only. **Superseded (4131facd, #483/#482):** the size now
  follows the display surface in both standalone and Workspace mode — one
  desktop pixel per point, short side at least 480, long side at most 2560,
  even dimensions — requested on connect and after each resize settles
  (0.4 s), once per size per connection. See DisplayDesktopSizeForViewSize.

### No changes: start-wayland.sh, setup-wayland.sh, guest packages, emulator core.

## Fallback behavior

If the server ignores or refuses the request (older wayvnc, non-headless
output), no size rect arrives and the client keeps its current framebuffer —
exactly today's stretch behavior. Strictly additive.

## Risks

- The realloc/in-flight-rect boundary is the one genuinely fiddly part
  (wrong handling = crash or garbled frame on rotate). Mitigation above.
- JIT-slow reflow transient of already-open windows (~1–2s of stale layout
  after rotate) — cosmetic, self-resolving.
- wayvnc's nonstandard status code — by design we never read it.

## Verification plan

- Protocol layer testable without the app: the same probe technique used
  for this scope (raw RFB SetDesktopSize + screenshot) from the Mac against
  the device session.
- App layer: on-device rotate → fresh VNC screenshot must report the new
  framebuffer dimensions; user visual confirmation for the native side.
- Regression: existing connect/Reconnect flows, plus a rotate performed
  while disconnected (request must not be sent on a dead client).
