// =====================================================================
//  dose_log -- one CSV row per finished dose on the microSD card.
//
//  File: /dose_log.csv
//    dose_no,boot,uptime_s,seq,recipe,target_g,actual_g,error_g,error_pct,date_time
//    17,3,1250,12,A,250.0,248.6,-1.4,-0.56,2026-11-01 14:05
//
//  dose_no   1, 2, 3 ... never reused (this is the "dose number" people see)
//  boot      how many times the ESP32 has started
//  uptime_s  seconds since that start
//  seq       the link seq of the DOSE command (1..255, wraps around)
//  error     actual - target, in grams and in percent of the target
//  date_time [1 Nov 2026] when the dose finished, in the phone's time zone
//            (wall_clock.h: the ESP32 takes its time from a phone). EMPTY
//            if no phone had opened the dashboard since the ESP32 started;
//            then boot + uptime_s is the only time stamp.
//
//  Files from before 1 Nov have 9 columns: those rows are still read (no
//  time), and at start-up the old header line is swapped for the new one
//  once (the only time this file is ever rewritten).
//
//  Rows are written from loop() only. The web server reads them.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "live_state.h"

#define DOSE_LOG_HEADER_OLD "dose_no,boot,uptime_s,seq,recipe,target_g,actual_g,error_g,error_pct"
#define DOSE_LOG_HEADER     DOSE_LOG_HEADER_OLD ",date_time"

// One row, with the masses back in decigrams.
struct DoseRow {
  uint32_t no;
  uint32_t boot;
  uint32_t uptimeS;
  uint8_t  seq;
  char     recipe;
  int32_t  targetDg, actualDg;
  char     time[17];                       // "2026-11-01 14:05", or "" if not known
};

void doseLogBegin();                       // after sdBegin(): boot number, newest row
bool doseLogAppend(const DoseResult& d);   // one row per new DONE
uint8_t  doseLogLastSeq();                 // seq of the newest row found at boot (0 = none)
uint32_t doseLogCount();                   // newest dose number (0 = none yet)
uint32_t doseLogBoot();                    // this start's boot number

// Fills out[] with the newest rows, newest first (at most max).
// Returns how many, or -1 if the card could not be read.
int doseLogNewest(DoseRow* out, int max);

// Decigrams as grams with one decimal: 2486 -> "248.6", -14 -> "-1.4".
void formatDg(char* out, size_t size, int32_t dg);
