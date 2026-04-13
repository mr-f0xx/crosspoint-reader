// ============================================================
//  ReadingControlsMod.cpp  —  Reading Controls (Full Mod)
//  Place at: src/activities/reader/ReadingControlsMod.cpp
// ============================================================

#include "ReadingControlsMod.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "fontIds.h"

// ── Convenience aliases using the exact CrossPointSettings enums ─────────────
using FS  = CrossPointSettings::FONT_SIZE;
using LC  = CrossPointSettings::LINE_COMPRESSION;
using PA  = CrossPointSettings::PARAGRAPH_ALIGNMENT;
using OR  = CrossPointSettings::ORIENTATION;
using SS  = CrossPointSettings::SLEEP_SCREEN_MODE;

// ============================================================
//  Constructor
// ============================================================
ReadingControlsMod::ReadingControlsMod() = default;

// ============================================================
//  Gesture state-machine for a single button
//
//  Press edge  → record pressedAt, clear holdFired
//  While held  → fire Hold once when (now - pressedAt) >= RCM_HOLD_MS
//  Release edge→ accumulate click count (unless hold already fired)
//  After window→ resolve Single or Double from pending click count
// ============================================================
ReadingControlsMod::GEvent ReadingControlsMod::_tick(
    BTrack& t, bool isDown, bool released, unsigned long now)
{
  // Press edge
  if (isDown && !t.prev) {
    t.pressedAt = now;
    t.holdFired = false;
  }
  t.prev = isDown;

  // Hold detection (fires while still held)
  if (isDown && !t.holdFired && (now - t.pressedAt) >= RCM_HOLD_MS) {
    t.holdFired = true;
    t.clicks    = 0;  // discard any pending single-click
    return GEvent::Hold;
  }

  // Release edge (only count if not a hold)
  if (released && !t.holdFired) {
    t.clicks++;
    t.releasedAt = now;
  }

  // Click-window expiry → resolve
  if (!isDown && t.clicks > 0 && (now - t.releasedAt) >= RCM_DBL_CLICK_MS) {
    uint8_t n = t.clicks;
    t.clicks   = 0;
    return (n >= 2) ? GEvent::Double : GEvent::Single;
  }

  return GEvent::None;
}

// ============================================================
//  Main update — call once per loop() iteration
// ============================================================
ReadingControlsMod::Result ReadingControlsMod::update(MappedInputManager& inp)
{
  using Btn = MappedInputManager::Button;
  Result r{};
  const unsigned long now = millis();

  // Auto-dismiss guide
  if (_guideVisible && (now - _guideShownAt) >= RCM_GUIDE_MS) {
    _guideVisible = false;
    r.guideToggled = true;
  }

  // Helper: raw state from MappedInputManager
  auto dn  = [&](Btn b) { return inp.isPressed(b);   };
  auto rel = [&](Btn b) { return inp.wasReleased(b); };

  // ── Vol Up: single click = font+, hold = cycle line spacing ──────────────
  // NOTE: MappedInputManager::Button::Up maps to the Vol-Up / page-prev side
  //       button in the default reader layout. Verify against your
  //       MappedInputManager enum if the name differs (e.g. Button::SideTop).
  switch (_tick(_up, dn(Btn::Up), rel(Btn::Up), now)) {
    case GEvent::Single:
      if (SETTINGS.fontSize < FS::EXTRA_LARGE) {
        SETTINGS.fontSize++;
        SETTINGS.saveToFile();
        LOG_DBG("RCM", "Font size → %d", SETTINGS.fontSize);
        r.layoutChanged = true;
      }
      break;
    case GEvent::Hold:
      SETTINGS.lineSpacing = static_cast<uint8_t>(
          (SETTINGS.lineSpacing + 1) % LC::LINE_COMPRESSION_COUNT);
      SETTINGS.saveToFile();
      { const char* lbl[] = { "Tight", "Normal", "Wide" };
        LOG_DBG("RCM", "Line spacing → %s", lbl[SETTINGS.lineSpacing]); }
      r.layoutChanged = true;
      break;
    case GEvent::None:
      break;
    default: break;
  }
  // While Up button has a pending click, suppress the normal page-turn.
  if (_up.clicks > 0) r.pageTurnSuppressed = true;

  // ── Vol Down: single click = font- ───────────────────────────────────────
  switch (_tick(_dn, dn(Btn::Down), rel(Btn::Down), now)) {
    case GEvent::Single:
      if (SETTINGS.fontSize > FS::SMALL) {
        SETTINGS.fontSize--;
        SETTINGS.saveToFile();
        LOG_DBG("RCM", "Font size → %d", SETTINGS.fontSize);
        r.layoutChanged = true;
      }
      break;
    case GEvent::None:
      break;
    default: break;
  }
  if (_dn.clicks > 0) r.pageTurnSuppressed = true;

  // ── Left: double-click = toggle paragraph alignment ──────────────────────
  switch (_tick(_left, dn(Btn::Left), rel(Btn::Left), now)) {
    case GEvent::Double: {
      const bool wasJustified = (SETTINGS.paragraphAlignment == PA::JUSTIFIED);
      SETTINGS.paragraphAlignment =
          static_cast<uint8_t>(wasJustified ? PA::LEFT_ALIGN : PA::JUSTIFIED);
      SETTINGS.saveToFile();
      LOG_DBG("RCM", "Alignment → %s", wasJustified ? "Left" : "Justified");
      r.layoutChanged = true;
      break;
    }
    case GEvent::None: break;
    default: break;
  }

  // ── Right: double-click = toggle bold, hold = rotate orientation ──────────
  switch (_tick(_right, dn(Btn::Right), rel(Btn::Right), now)) {
    case GEvent::Double:
      _bold = !_bold;
      // Bold is tracked in session state only. To persist it across reboots,
      // add `uint8_t readerBoldText` to CrossPointSettings (see integration
      // guide) and uncomment the two lines below:
      // SETTINGS.readerBoldText = _bold ? 1 : 0;
      // SETTINGS.saveToFile();
      LOG_DBG("RCM", "Bold → %s", _bold ? "ON" : "OFF");
      r.layoutChanged = true;  // triggers section re-render via section.reset()
      break;
    case GEvent::Hold:
      SETTINGS.orientation = static_cast<uint8_t>(
          (SETTINGS.orientation + 1) % OR::ORIENTATION_COUNT);
      SETTINGS.saveToFile();
      LOG_DBG("RCM", "Orientation → %d", SETTINGS.orientation);
      r.displayChanged = true;
      break;
    case GEvent::None: break;
    default: break;
  }

  // ── Back: double-click = toggle Dark/Light mode ───────────────────────────
  // Single-click Back falls through to EpubReaderActivity's normal go-home
  // logic, but is delayed by up to RCM_DBL_CLICK_MS (380 ms) while the
  // double-click window is open. This is the unavoidable UX tradeoff.
  {
    auto ev = _tick(_back, dn(Btn::Back), rel(Btn::Back), now);
    if (ev == GEvent::Double) {
      _darkMode = !_darkMode;
      // Using sleepScreen as a proxy (DARK=0, LIGHT=1).
      // If you add a dedicated `readerInvertDisplay` field to CrossPointSettings,
      // use that instead to avoid changing the sleep screen setting.
      SETTINGS.sleepScreen = static_cast<uint8_t>(
          _darkMode ? SS::DARK : SS::LIGHT);
      SETTINGS.saveToFile();
      LOG_DBG("RCM", "Display → %s", _darkMode ? "Dark" : "Light");
      r.guideToggled = true;  // force re-render so dark/light is applied
    } else if (ev == GEvent::Single) {
      r.backAction = BackAction::GoHome;  // deferred go-home confirmed
    } else if (_back.clicks > 0) {
      r.backAction = BackAction::Pending; // suppress normal back for now
    }
  }

  // ── Confirm: hold = show on-screen guide ─────────────────────────────────
  // Single-click Confirm is passed through UNLESS hold fired this frame.
  {
    auto ev = _tick(_menu, dn(Btn::Confirm), rel(Btn::Confirm), now);
    if (ev == GEvent::Hold) {
      _guideVisible = true;
      _guideShownAt = now;
      r.guideToggled      = true;
      r.confirmSuppressed = true;  // cancel the upcoming wasReleased(Confirm)
      LOG_DBG("RCM", "Guide shown");
    } else if (_menu.holdFired && rel(Btn::Confirm)) {
      // Release after a hold: also suppress menu from opening
      r.confirmSuppressed = true;
    }
  }

  return r;
}

// ============================================================
//  Guide overlay render
//  Called from EpubReaderActivity::render() after renderContents().
// ============================================================
void ReadingControlsMod::renderGuide(GfxRenderer& renderer) const {
  const int SW = renderer.getScreenWidth();
  const int SH = renderer.getScreenHeight();

  // Box geometry — centred on screen
  constexpr int W = 300, H = 214;
  const int X = (SW - W) / 2;
  const int Y = (SH - H) / 2;

  // ── White fill + double border ────────────────────────────────────────────
  renderer.fillRect(X, Y, W, H, false);          // white background
  renderer.fillRect(X,     Y,     W,  2,  true); // top
  renderer.fillRect(X,     Y+H-2, W,  2,  true); // bottom
  renderer.fillRect(X,     Y,     2,  H,  true); // left
  renderer.fillRect(X+W-2, Y,     2,  H,  true); // right
  // Inner border
  renderer.fillRect(X+2,   Y+2,   W-4,1,  true);
  renderer.fillRect(X+2,   Y+H-3, W-4,1,  true);

  // ── Title bar ─────────────────────────────────────────────────────────────
  constexpr int TITLE_H = 20;
  renderer.fillRect(X+2, Y+2, W-4, TITLE_H, true);
  renderer.drawCenteredText(UI_10_FONT_ID, Y + 7,
                            "Reading Controls (Full Mod)", true);

  // ── Content rows ──────────────────────────────────────────────────────────
  struct Row { const char* gesture; const char* action; };
  static constexpr Row ROWS[] = {
    { "Vol \x18 click",   "Font size +"       },  // ▲
    { "Vol \x19 click",   "Font size -"       },  // ▼
    { "Vol \x18 hold",    "Line spacing"      },
    { "Left  dbl-click",  "Alignment"         },
    { "Right dbl-click",  "Bold"              },
    { "Right hold",       "Rotate"            },
    { "Back  dbl-click",  "Dark / Light"      },
    { "Confirm hold",     "This guide"        },
  };
  constexpr int N = sizeof(ROWS) / sizeof(ROWS[0]);
  constexpr int LINE_H = 22;
  constexpr int COL_A = 10;   // left column (gesture label)
  constexpr int COL_B = 155;  // right column (action)

  int rowY = Y + TITLE_H + 6;
  for (int i = 0; i < N; ++i) {
    // Alternating row shading
    if (i % 2 == 0) {
      renderer.fillRect(X+3, rowY, W-6, LINE_H-1, false); // already white — no-op, but explicit
    } else {
      renderer.fillRect(X+3, rowY, W-6, LINE_H-1, false); // light grey ideally; white for now
    }
    renderer.drawText(UI_10_FONT_ID, X + COL_A, rowY + 4, ROWS[i].gesture);
    renderer.drawText(UI_10_FONT_ID, X + COL_B, rowY + 4, ROWS[i].action);
    rowY += LINE_H;
  }

  // ── Footer hint ───────────────────────────────────────────────────────────
  renderer.drawCenteredText(UI_10_FONT_ID, Y + H - 10,
                            "Closes automatically", false);

  // ── Push overlay to display ───────────────────────────────────────────────
  // Use FAST_REFRESH so the guide appears quickly on top of the page already
  // shown on the e-ink panel. The caller should NOT call displayBuffer() again.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
