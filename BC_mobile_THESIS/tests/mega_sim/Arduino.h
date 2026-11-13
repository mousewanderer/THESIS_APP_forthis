// Pretend Arduino for tests/test_power_cut.py: lets the REAL AgriDose_Mega_Link
// sketch run on a laptop, with simulated time, hoppers, augers and EEPROM.
// Every call that takes time goes through simAdvance(), so the power can be cut
// anywhere, even in the middle of an EEPROM save or a weighing.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

void simAdvance(uint64_t us);      // moves the clock and the hoppers; cuts the power when due
uint32_t simMillis();
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
void analogWrite(uint8_t pin, int value);
inline uint32_t millis() { return simMillis(); }
inline void delay(uint32_t ms) { simAdvance((uint64_t)ms * 1000); }

// Serial Monitor: thrown away unless --verbose.
extern bool simVerbose;
struct Print {
  virtual size_t write(const uint8_t* b, size_t n) = 0;
  virtual ~Print() {}
};
struct SerialMonitor {
  void begin(unsigned long) {}
  int available() { return 0; }
  int read() { return -1; }
  void show(const char* s) { if (simVerbose) fputs(s, stderr); }
  void print(const char* s) { show(s); }
  void println(const char* s) { show(s); show("\n"); }
  void println() { show("\n"); }
  template <class T> void num(T v) { char b[32]; snprintf(b, sizeof(b), "%ld", (long)v); show(b); }
  void print(int v) { num(v); }
  void print(long v) { num(v); }
  void print(unsigned int v) { num(v); }
  void print(unsigned long v) { num(v); }
  void print(uint8_t v) { num(v); }
  void print(double v, int d = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", d, v); show(b); }
  template <class T> void println(T v) { print(v); show("\n"); }
  void println(double v, int d) { print(v, d); show("\n"); }
};
extern SerialMonitor Serial;

// The link to the ESP32: the test plays the ESP32 on the other end.
struct LinkPort : Print {
  void begin(unsigned long) {}
  int available();
  int read();
  size_t write(const uint8_t* b, size_t n) override;
};
extern LinkPort Serial2;

// EEPROM: one byte takes 3.4 ms, and a cut in that time leaves it holding junk.
bool eeprom_is_ready();
struct EEPROMClass {
  uint8_t read(int addr);
  void write(int addr, uint8_t value);
};
extern EEPROMClass EEPROM;

// The LCD: the test reads the two lines back.
extern char simLcd[2][17];
class LiquidCrystal_I2C {
 public:
  LiquidCrystal_I2C(uint8_t, uint8_t, uint8_t) {}
  void init() {}
  void backlight() {}
  void clear() {}
  void setCursor(uint8_t c, uint8_t r) { col = c; row = r; }
  void print(const char* s) {
    while (*s && col < 16) simLcd[row][col++] = *s++;
    simLcd[row][16] = 0;
  }
 private:
  uint8_t col = 0, row = 0;
};

// A load cell at the real 10 readings a second.
class HX711 {
 public:
  void begin(uint8_t dt, uint8_t) { hop = dt == 37 ? 0 : dt == 39 ? 1 : 2; }
  void set_scale(float s) { scale = s; }
  void set_offset(long o) { offset = o; }
  long get_offset() { return offset; }
  bool wait_ready_timeout(unsigned long ms);
  bool is_ready();
  float get_units(int times = 1);
  void tare(int times = 10);
 private:
  double readRaw(int times);
  uint8_t hop = 0;
  float scale = 1;
  long offset = 0;
  uint64_t lastUs = 0;
};
