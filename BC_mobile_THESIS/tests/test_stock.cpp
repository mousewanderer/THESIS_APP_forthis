// Laptop unit test for stock_check.h (no board needed): the stock check the
// keypad does before a dose, and the "enough stock for" numbers on the dashboard.
// The expected numbers were worked out by hand from agriSplitDose's rounding
// (written next to each check), not taken from the code under test.
//
// Build and run from the project folder (MSYS2 g++):
//   g++ -std=c++11 -Wall -I AgriDose_ESP32_Link tests/test_stock.cpp -o test_stock
//   ./test_stock
#include <stdio.h>
#include "stock_check.h"

static int failures = 0;

static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

static const int32_t SPARE[3] = {20, 20, 20};     // STOCK_SPARE_G in config.h
static const AgriRecipe& A = AGRI_RECIPES[0];      // 25 / 70 / 5
static const AgriRecipe& B = AGRI_RECIPES[1];      // 30 / 65 / 5
static const AgriRecipe& C = AGRI_RECIPES[2];      // 30 / 60 / 10

static uint8_t shortFor(const AgriRecipe& r, int32_t grams, int32_t h, int32_t c, int32_t a, int32_t need[3]) {
  int32_t have[3] = {h, c, a};
  return stockShortMask(r, grams, have, SPARE, need);
}

static int32_t maxFor(const AgriRecipe& r, int32_t h, int32_t c, int32_t a, uint8_t& mask) {
  int32_t have[3] = {h, c, a};
  return stockMaxDoseG(r, have, SPARE, mask);
}

int main() {
  int32_t need[3];
  uint8_t mask;

  // ---- recipe A, 900 g: hull 225.0 g, compost 630.0 g, ash 45.0 g; plus 20 g spare each ----
  check(shortFor(A, 900, 2450, 6500, 650, need) == 0 && need[0] == 2450 && need[1] == 6500 && need[2] == 650,
        "A 900 g: needs 245 / 650 / 65 g with the spare, and exactly that is enough");
  check(shortFor(A, 900, 2450, 6499, 650, need) == 0b010, "0.1 g less compost: compost (bit 1) is short");
  check(shortFor(A, 900, 2449, 6499, 649, need) == 0b111, "0.1 g less in all three: all three are short");
  // ---- recipe A, 300 g: hull 75.0, compost 210.0, ash 15.0 ----
  check(shortFor(A, 300, 950, 2300, 350, need) == 0 && need[0] == 950 && need[1] == 2300 && need[2] == 350,
        "A 300 g: needs 95 / 230 / 35 g");
  // the same split the Mega uses (so the keypad and the Mega agree on each share)
  int32_t h, c, a;
  agriSplitDose(C, 7770, h, c, a);
  shortFor(C, 777, 0, 0, 0, need);
  check(need[0] - 200 == h && need[1] - 200 == c && need[2] - 200 == a, "NEED minus the spare = the Mega's split (C 777 g)");

  // ---- a negative reading (the scale's zero moved) counts as short ----
  check(shortFor(B, 300, -30, 36000, 9000, need) == 0b001, "hull reads -3 g: hull is short");

  // ---- a hopper the recipe doesn't use is never short ----
  const AgriRecipe ONLY_COMPOST = {'X', 0, 100, 0};
  check(shortFor(ONLY_COMPOST, 600, -500, 6200, -500, need) == 0 && need[0] == 0 && need[2] == 0,
        "a recipe without hull or ash: empty hull and ash don't matter");
  check(shortFor(ONLY_COMPOST, 600, -500, 6199, -500, need) == 0b010, "but its compost still does");

  // ---- the biggest dose the stock covers ----
  check(maxFor(A, 42000, 36000, 9000, mask) == 900 && mask == 0, "full hoppers: 900 g of A, nothing limits it");
  check(maxFor(B, 42000, 36000, 9000, mask) == 900 && maxFor(C, 42000, 36000, 9000, mask) == 900, "and of B and C");
  // compost 410 g, A: compost share must stay <= 390.0 g.
  // 557 g: hull (5570*25+50)/100 = 1393, ash (5570*5+50)/100 = 279, compost 3898 -> fits
  // 558 g: hull 1395, ash 279, compost 3906 -> does not
  check(maxFor(A, 42000, 4100, 9000, mask) == 557 && mask == 0b010, "compost 410 g: A up to 557 g, compost limits it");
  // ash 40 g: A and B (5 %) up to 400 g ((50*400+50)/100 = 200 dg; 401 g gives 201); C (10 %) only to 200 g -> 0
  check(maxFor(A, 42000, 36000, 400, mask) == 400 && mask == 0b100, "ash 40 g: A up to 400 g, ash limits it");
  check(maxFor(B, 42000, 36000, 400, mask) == 400 && mask == 0b100, "ash 40 g: B up to 400 g");
  check(maxFor(C, 42000, 36000, 400, mask) == 0 && mask == 0b100, "ash 40 g: C not even 300 g, and ash is the one to refill");
  check(maxFor(A, 42000, 1000, 9000, mask) == 0 && mask == 0b010, "compost 100 g: no dose of A, refill compost");
  check(maxFor(A, 900, 1000, 100, mask) == 0 && mask == 0b111, "all three nearly empty: all three to refill");

  // ---- sweep: the answer is the biggest dose that fits, and the limit is named ----
  uint32_t seed = 12345;
  int bad = 0;
  for (int n = 0; n < 3000; n++) {
    int32_t have[3];
    for (int i = 0; i < 3; i++) { seed = seed * 1103515245u + 12345u; have[i] = (int32_t)((seed >> 8) % 9000) - 200; }
    const AgriRecipe& r = AGRI_RECIPES[n % 3];
    int32_t g = stockMaxDoseG(r, have, SPARE, mask);
    int32_t top = g ? g : AGRILINK_DOSE_MIN_G - 1;
    for (int32_t x = AGRILINK_DOSE_MAX_G; x > top; x--) if (stockShortMask(r, x, have, SPARE, need) == 0) bad++;   // a bigger one fits
    if (g && stockShortMask(r, g, have, SPARE, need) != 0) bad++;                                                    // the answer doesn't fit
    if (g < AGRILINK_DOSE_MAX_G && mask == 0) bad++;                                                                 // no hopper named
    if (g == AGRILINK_DOSE_MAX_G && mask != 0) bad++;
  }
  check(bad == 0, "3000 random stocks: always the biggest dose that fits, with the limiting hopper named");

  // ---- the LCD lines fit 16 columns at the largest numbers ----
  char l1[40];
  snprintf(l1, sizeof(l1), "NEED %ld HAS %ld", 650L, 649L);   // compost for 900 g of A, one gram short
  check(strlen(l1) <= 16 && strlen("REFILL HOPPER OR") <= 16 && strlen("LOW COMPOST") <= 16, "the STOCK_LOW screens fit 16 columns");

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
