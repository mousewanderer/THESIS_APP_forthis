// =====================================================================
//  crop_records -- farmers (users) and their crop records on the microSD.
//  Thesis objective 2.3: crop status logging per user.
//
//  Files in /crop/ (one row per record, the first column is the user ID):
//    users.csv      user_id,name
//    soil.csv       user_id,plot,date_tested,soil_ph,soil_moisture_pct,photos
//    plantings.csv  user_id,plot,crop,variety,date_planted,used_agridose,dose_numbers,photos
//    growth.csv     user_id,plot,stage,date_observed,date_planted,days_after_planting,
//                   height_cm,leaf_count,notes,photos
//    fertilizer.csv user_id,plot,date_applied,fertilizer,amount_kg,notes
//                   (the Recommend tab counts these since the plot's soil test)
//  "photos" holds photo numbers ("12 13"); the files are in crop_photos.h.
//
//  The fields follow agridose-v2/docs/05-phone-app.md (the Records tab)
//  and the team's Blynk "Crop Records" screen (seed type and growth stage
//  lists, date planted and day count in the growth records).
//  Every value is checked here on the ESP32 (crop_validate.h) before it is
//  written, so a bad phone or a hand-made request cannot put rubbish in
//  the files. Nothing in this file talks to the Mega.
//
//  CORRECTIONS: a record (or a farmer's name) entered wrong is fixed in
//  place: its line in the file is replaced (sdReplaceLine), not added
//  again. Each record the page gets carries its line number ("row") and a
//  checksum of the line ("rev"); an edit is only written if that line is
//  still the same, so a phone with an old list can't overwrite another row.
//
//  DELETING (1 Nov): one record can be deleted from its details on the
//  phone (POST /api/delete/<type>), with the same row + rev check as an
//  edit. Its photos go with it. A planting can't be deleted while its plot
//  has growth records and no other planting: delete those first.
//
//  DATA RULES (30 Oct), on every add and edit, with the reason shown on
//  the phone: no two farmers with the same name (capitals and spaces
//  ignored); no duplicate record (same farmer, type and key fields, e.g. a
//  soil test on the same plot and date); no date after the phone's today;
//  a growth record observed on or after its date planted, on a plot that
//  has a planting; a planting's dose numbers must be in the Batch Log.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <functional>

enum RecordType : uint8_t { REC_SOIL, REC_PLANTING, REC_GROWTH, REC_FERT, REC_TYPE_COUNT };

// Returns the value of one form field by name ("" if the phone did not send it).
typedef std::function<String(const char* name)> FieldGetter;

void cropBegin();                                       // after sdBegin(): folder and headers
const char* cropTypeName(RecordType t);                 // "soil", "planting", "growth", "fertilizer"
const char* cropTypePath(RecordType t);                 // its CSV file, e.g. "/crop/soil.csv"
const char* cropUsersPath();                            // "/crop/users.csv"

// Each function writes JSON to out, or returns false with a reason in error.
void cropWriteTypesJson(Print& out);                    // the form fields, so the page can build its forms
bool cropWriteUsersJson(Print& out, String& error, long onlyId = 0);   // onlyId: just that farmer (a farmer's login)
bool cropWriteRecordsJson(RecordType t, long userId, int limit, Print& out, String& error);
bool cropWriteRecordsCsv(RecordType t, long userId, Print& out, String& error);   // one farmer's rows, with the header

bool cropAddUser(const FieldGetter& get, String& error);
bool cropEditUser(const FieldGetter& get, String& error);        // new name for user_id
bool cropDeleteUser(long id, String& error);           // the farmer, their records and photos (not the dose log)
bool cropUserExists(long id, bool& readOk);            // readOk false = users.csv could not be read
bool cropAddRecord(RecordType t, const FieldGetter& get, String& error);
bool cropEditRecord(RecordType t, const FieldGetter& get, String& error);   // + row and rev from the list
bool cropDeleteRecord(RecordType t, const FieldGetter& get, String& error); // user_id, row, rev; with its photos
