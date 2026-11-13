// =====================================================================
//  stock_check.h -- is there enough in the hoppers for a dose? (10 Nov 2026)
//
//  WHY: a hopper that runs out halfway pauses the dose with an EMPTY fault.
//  The ESP32 already has every hopper's weight (the Mega's HW frames) and the
//  recipe split (agriSplitDose in AgriLink.h, the same one the Mega uses), so
//  it can say before the start which hopper is short.
//
//  THE RULE: a hopper is short when its weight is under its share of the dose
//  plus its spare (STOCK_SPARE_G in config.h): material the auger can't reach,
//  and the 5 g a HW reading may be behind. A hopper the recipe doesn't use is
//  never short.
//
//  Used by the keypad (AgriDose_ESP32_Link.ino: the LOW <hopper> screen) and
//  the dashboard (web_server.cpp: "stock_max_g" in the status). No Arduino
//  calls, so tests/test_stock.cpp checks it on a laptop.
// =====================================================================
#pragma once
#include "AgriLink.h"

// Hopper names on the LCD, by hopper index (0 = A, 1 = B, 2 = C), as on the Mega's own screens.
static const char* const STOCK_NAMES[3] = {"HULL", "COMPOST", "ASH"};

// Which hoppers are short for `grams` of recipe r: bit i = hopper i.
//   haveDg  the hopper weights, dg (from HW)
//   spareG  STOCK_SPARE_G, grams
//   needDg  gets each hopper's share plus its spare, dg (what the LCD shows as NEED)
static inline uint8_t stockShortMask(const AgriRecipe& r, int32_t grams, const int32_t haveDg[3],
                                     const int32_t spareG[3], int32_t needDg[3]) {
  agriSplitDose(r, grams * 10, needDg[0], needDg[1], needDg[2]);
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (needDg[i] <= 0) { needDg[i] = 0; continue; }      // this recipe doesn't use hopper i
    needDg[i] += spareG[i] * 10;
    if (haveDg[i] < needDg[i]) mask |= (uint8_t)(1 << i);
  }
  return mask;
}

// The biggest dose of recipe r, in whole grams from AGRILINK_DOSE_MIN_G to MAX_G,
// that the stock covers; 0 when not even the smallest one fits. shortMask gets
// the hoppers that stop it going higher (0 when the biggest dose fits).
static inline int32_t stockMaxDoseG(const AgriRecipe& r, const int32_t haveDg[3], const int32_t spareG[3],
                                    uint8_t& shortMask) {
  int32_t needDg[3];
  for (int32_t g = AGRILINK_DOSE_MAX_G; g >= AGRILINK_DOSE_MIN_G; g--) {
    if (stockShortMask(r, g, haveDg, spareG, needDg) == 0) {
      shortMask = g < AGRILINK_DOSE_MAX_G ? stockShortMask(r, g + 1, haveDg, spareG, needDg) : 0;
      return g;
    }
  }
  shortMask = stockShortMask(r, AGRILINK_DOSE_MIN_G, haveDg, spareG, needDg);
  return 0;
}
