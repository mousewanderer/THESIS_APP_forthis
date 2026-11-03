// Laptop unit test for crop_validate.h (no board needed).
//
// Build and run from the project folder (MSYS2 g++):
//   g++ -std=c++11 -Wall -I AgriDose_ESP32_Link tests/test_validate.cpp -o test_validate
//   ./test_validate
#include <stdio.h>
#include "crop_validate.h"

static int failures = 0;

static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

int main() {
  char out[128];

  check(cvInt(" 7 ", 1, 99, out, sizeof(out)) && strcmp(out, "7") == 0, "int: ' 7 ' -> 7");
  check(cvInt("007", 1, 99, out, sizeof(out)) && strcmp(out, "7") == 0, "int: '007' -> 7");
  check(!cvInt("0", 1, 99, out, sizeof(out)), "int: 0 below the minimum");
  check(!cvInt("100", 1, 99, out, sizeof(out)), "int: 100 above the maximum");
  check(!cvInt("-1", 0, 99, out, sizeof(out)) && !cvInt("1.5", 0, 99, out, sizeof(out)) &&
        !cvInt("", 0, 99, out, sizeof(out)) && !cvInt("12a", 0, 99, out, sizeof(out)), "int: sign, decimals, blank, letters rejected");

  check(cvDecimal("5.25", 0, 14, 2, out, sizeof(out)) && strcmp(out, "5.25") == 0, "decimal: pH 5.25 kept");
  check(cvDecimal("5", 0, 14, 2, out, sizeof(out)) && strcmp(out, "5") == 0, "decimal: pH 5 kept as typed");
  check(cvDecimal("5,5", 0, 14, 2, out, sizeof(out)) && strcmp(out, "5.5") == 0, "decimal: comma as decimal point");
  check(!cvDecimal("14.1", 0, 14, 2, out, sizeof(out)), "decimal: pH 14.1 out of range");
  check(!cvDecimal("5.125", 0, 14, 2, out, sizeof(out)), "decimal: too many decimals");
  check(!cvDecimal(".5", 0, 14, 2, out, sizeof(out)) && !cvDecimal("5.", 0, 14, 2, out, sizeof(out)) &&
        !cvDecimal("1e2", 0, 500, 1, out, sizeof(out)) && !cvDecimal("-1", 0, 14, 2, out, sizeof(out)),
        "decimal: '.5', '5.', '1e2', '-1' rejected");

  check(cvDate("2026-10-28", out, sizeof(out)) && strcmp(out, "2026-10-28") == 0, "date: 2026-10-28");
  check(cvDate("2028-02-29", out, sizeof(out)), "date: 29 Feb in a leap year");
  check(!cvDate("2026-02-29", out, sizeof(out)), "date: 29 Feb in a normal year rejected");
  check(!cvDate("2026-13-01", out, sizeof(out)) && !cvDate("2026-04-31", out, sizeof(out)) &&
        !cvDate("26-10-28", out, sizeof(out)) && !cvDate("2026/10/28", out, sizeof(out)) &&
        !cvDate("1999-01-01", out, sizeof(out)), "date: bad month, bad day, wrong format, year 1999 rejected");

  check(cvText("  Pechay, native \"red\" ", 30, out, sizeof(out)) && strcmp(out, "Pechay; native 'red'") == 0,
        "text: trimmed, comma and quotes swapped");
  check(cvText("line1\nline2", 30, out, sizeof(out)) && strcmp(out, "line1 line2") == 0, "text: line break becomes a space");
  check(!cvText("1234567890123456789012345678901", 30, out, sizeof(out)), "text: 31 characters over a 30 limit");
  check(cvText("", 30, out, sizeof(out)) && out[0] == 0, "text: blank is allowed (caller decides)");

  check(cvYesNo(" YES ", out, sizeof(out)) && strcmp(out, "yes") == 0, "yes/no: ' YES ' -> yes");
  check(!cvYesNo("maybe", out, sizeof(out)), "yes/no: 'maybe' rejected");

  check(cvNumberList("12, 13;14  15", 40, out, sizeof(out)) && strcmp(out, "12 13 14 15") == 0, "dose list: '12, 13;14  15' -> '12 13 14 15'");
  check(!cvNumberList("12 x", 40, out, sizeof(out)), "dose list: letters rejected");

  check(cvPhotoList("12, 013", 3, 20, out, sizeof(out)) && strcmp(out, "12 13") == 0, "photos: '12, 013' -> '12 13'");
  check(cvPhotoList("", 3, 20, out, sizeof(out)) && out[0] == 0, "photos: none");
  check(!cvPhotoList("21", 3, 20, out, sizeof(out)) && !cvPhotoList("0", 3, 20, out, sizeof(out)),
        "photos: a number not on the card, or 0, rejected");
  check(!cvPhotoList("1 2 3 4", 3, 20, out, sizeof(out)), "photos: 4 photos over a limit of 3");
  check(!cvPhotoList("5 5", 3, 20, out, sizeof(out)), "photos: the same photo twice rejected");
  check(!cvPhotoList("99999999999999999999", 3, 20, out, sizeof(out)), "photos: huge number rejected");

  check(cvIsBlank("   ") && cvIsBlank("") && !cvIsBlank(" a "), "blank check");

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
  return failures ? 1 : 0;
}
