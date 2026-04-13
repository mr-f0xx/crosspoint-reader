// ============================================================
//  ReadingControlsMod.h  —  Reading Controls (Full Mod)
//  Place at: src/activities/reader/ReadingControlsMod.h
//
//  Gesture map
//  ──────────────────────────────────────────────────────────
//  Vol Up   single click   → Font size +
//  Vol Down single click   → Font size -
//  Vol Up   hold           → Cycle line spacing (Tight/Normal/Wide)
//  Left     double-click   → Toggle alignment (Left ↔ Justified)
//  Right    double-click   → Toggle bold  (session-only unless field added)
//  Right    hold           → Rotate screen orientation
//  Back     double-click   → Toggle Dark / Light display mode
//  Confirm  hold           → Show on-screen control guide
// ============================================================

#pragma once

#include <MappedInputManager.h>
#include <cstdint>

class GfxRenderer;  // forward-declare

// Timing (override with #define before #include if desired)
#ifndef RCM_HOLD_MS
#define RCM_HOLD_MS       600UL
#endif
#ifndef RCM_DBL_CLICK_MS
#define RCM_DBL_CLICK_MS  380UL
#endif
#ifndef RCM_GUIDE_MS
#define RCM_GUIDE_MS     4500UL
#endif

class ReadingControlsMod {
 public:
  enum class BackAction : uint8_t {
    None,     // no back activity this frame
    Pending,  // inside double-click window — suppress normal back handling
    GoHome,   // single-click confirmed — caller must execute go-home
  };

  struct Result {
    bool       layoutChanged;     // font/spacing/alignment changed → reset section
    bool       displayChanged;    // orientation changed → call applyOrientation()
    bool       guideToggled;      // guide visibility changed → call requestUpdate()
    bool       confirmSuppressed; // hold-guide fired → skip wasReleased(Confirm)
    bool       pageTurnSuppressed;// Vol Up/Down pending → skip detectPageTurn()
    BackAction backAction;
  };

  ReadingControlsMod();

  // Call at the very top of EpubReaderActivity::loop() before all other input.
  Result update(MappedInputManager& input);

  bool isGuideVisible() const { return _guideVisible; }

  // Call inside render(), after renderContents(), before the function returns.
  // Draws the overlay and issues a FAST_REFRESH.
  void renderGuide(GfxRenderer& renderer) const;

  // Query current mod state from render()
  bool getBold()     const { return _bold;     }
  bool getDarkMode() const { return _darkMode; }

 private:
  enum class GEvent : uint8_t { None, Single, Double, Hold };

  struct BTrack {
    bool          prev;       // isPressed on previous frame
    bool          holdFired;  // hold already dispatched this press cycle
    uint8_t       clicks;     // pending unresolved click count
    unsigned long pressedAt;
    unsigned long releasedAt;
  };

  GEvent _tick(BTrack& t, bool isDown, bool released, unsigned long now);

  BTrack _up{}, _dn{}, _left{}, _right{}, _back{}, _menu{};

  bool          _bold        = false;
  bool          _darkMode    = false;
  bool          _guideVisible = false;
  unsigned long _guideShownAt = 0;
};
