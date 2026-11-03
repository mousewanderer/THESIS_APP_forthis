// =====================================================================
//  crop_photos -- photos attached to crop records, on the microSD card.
//
//  The phone does the heavy work: it makes the compressed copy (at most
//  1024 px, JPEG) before it uploads, and it shrinks a photo over
//  PHOTO_FULL_MAX. The ESP32 only checks the two files and writes them:
//    /crop/photos/0012.jpg             the photo as taken (the save file;
//                                      .png or .webp if it was one)
//    /crop/photos/0012_compressed.jpg  the compressed copy the dashboard shows
//    /crop/photos.csv                  photo_id,user_id,file,bytes,compressed_file,compressed_bytes
//  A crop record keeps its photo numbers in its "photos" column ("12 13").
//  A photo whose record was never saved stays on the card, unused.
//  [1 Nov 2026] When a record is deleted (Delete in its details on the
//  phone), its photos are deleted with it: both files and their index row
//  (photoDeleteIds). A deleted farmer's photos all go (photoDeleteForUser).
// =====================================================================
#pragma once
#include <Arduino.h>

void photoBegin();                 // after cropBegin(): folder, index, newest number
long photoLastId();                // newest photo number (0 = none yet)

// The file of photo `id`: full = the save file, else the compressed copy.
// Returns false if there is no such photo.
bool photoFile(long id, bool full, char* path, size_t pathSize, const char*& type, int32_t& bytes);
long photoOwner(long id);          // the user ID the photo was uploaded for (0 = unknown)
bool photoDeleteForUser(long userId, int& count);   // a deleted farmer's photo files and index rows
// A deleted record's photos: ids is the record's photos field ("12 13").
// Only photos uploaded for userId are touched.
bool photoDeleteIds(const char* ids, long userId, int& count);

// One upload (one photo = two files in one form) arrives in pieces through
// the web server's upload callback. `who` is the request. One upload runs
// at a time; the file "compressed.jpg" must come first, then "full.jpg"
// (or full.png / full.webp).
void photoUploadPiece(const void* who, long userId, const char* filename, size_t index,
                      const uint8_t* data, size_t len, bool final);
bool photoUploadFinish(const void* who, long& id, String& error);   // the whole request has arrived
void photoUploadGone(const void* who);                               // the phone went away mid-upload
