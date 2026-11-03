// =====================================================================
//  sd_store -- the microSD card and its ONE mutex.
//
//  The SD library is not safe to use from two tasks at once, and three
//  tasks want the card: the keypad loop (dose log), the web server
//  (downloads, crop records) and setup(). Every SD access in this sketch
//  goes through the functions below, and each of them takes the mutex,
//  does one small job (open, read or write, close) and gives it back.
//  So a long download never holds the card for more than one piece.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <functional>

#define SD_LINE_MAX 200            // longest CSV line we read back

bool sdBegin();                    // mounts the card; call once in setup()
bool sdReady();                    // card mounted and the last write worked

// Adds one line to the end of a file. If the file is new, header goes first.
bool sdAppendLine(const char* path, const char* line, const char* header);

// Reads up to len bytes starting at offset. Returns the number of bytes
// read (0 at the end of the file), or -1 if the file is missing or the
// card stayed busy for SD_WAIT_MS.
int sdReadAt(const char* path, uint32_t offset, uint8_t* buf, size_t len);

// File size in bytes, or -1 if the file is missing / the card is busy.
int32_t sdFileSize(const char* path);

// Creates a folder if it does not exist yet.
bool sdMakeDir(const char* path);

// Creates the file with just its header line if it does not exist yet.
bool sdCreateIfMissing(const char* path, const char* header);

bool sdRemove(const char* path);                 // true if it is gone (or was never there)
bool sdRename(const char* from, const char* to);

// Writes one big file in pieces (a photo upload). One such file at a time;
// each call takes the mutex once. Begin creates or empties the file.
bool sdWriteBegin(const char* path);
bool sdWriteMore(const uint8_t* data, size_t len);
bool sdWriteEnd();                               // closes it; true if every piece was written

// Splits one CSV line in place at the commas (our files never have commas
// inside a value). Returns the number of fields (at most maxFields).
uint8_t csvSplit(char* line, char* fields[], uint8_t maxFields);

// Calls onLine for every line of the file, first line first. Reads 1 KB at
// a time (one mutex turn each). Lines longer than SD_LINE_MAX are skipped.
// Returns false if the file could not be read.
bool sdForEachLine(const char* path, const std::function<void(char* line)>& onLine);

// Swaps one line of a file for newLine (a corrected crop record or farmer).
// pick() sees the lines sdForEachLine would hand over, with their number
// (the first one is 0); the first line it accepts is replaced and every
// other byte is copied as it is. The new copy is written whole to
// path.new, then swapped in (path -> path.bak, path.new -> path, path.bak
// removed), so the file is never half-written. Only for the small crop
// files: it reads the file twice. Call it from the web server task only,
// the one task that writes those files.
enum SdEditResult : uint8_t { SD_EDIT_OK, SD_EDIT_NOT_FOUND, SD_EDIT_FAILED };
SdEditResult sdReplaceLine(const char* path, const std::function<bool(const char* line, uint32_t lineNo)>& pick,
                           const char* newLine);

// Removes every line pick() accepts (a deleted farmer's rows), the same way
// (a whole new copy, swapped in). dropped = how many. NOT_FOUND = nothing to
// drop, the file was left alone. Empty or over-long lines right after a
// dropped line go with it (our files have none).
SdEditResult sdDropLines(const char* path, const std::function<bool(const char* line, uint32_t lineNo)>& pick, int& dropped);

// At start-up, before the file is used: finishes or undoes an sdReplaceLine
// or sdDropLines that a power cut interrupted.
void sdRecoverEdit(const char* path);
