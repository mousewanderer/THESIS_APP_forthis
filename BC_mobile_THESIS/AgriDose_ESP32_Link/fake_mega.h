// =====================================================================
//  fake_mega -- a pretend Mega, for testing with only the ESP32 on USB.
//  Compiled in only when FAKE_MEGA is 1 (config.h).
//
//  It runs inside the link task. Frames the ESP32 "sends" arrive in
//  fakeMegaReceive(); the frames it answers with are handed back byte by
//  byte through fakeMegaReadByte(), so they go through the real AgriLink
//  parser exactly like bytes from the UART would.
//
//  What it does: answers PING/TARE/STOP/DOSE like the Mega (same checks,
//  same seq memory), sends WT every 200 ms, runs a dose at about 10 g/s,
//  sends DONE (actual = target +/- up to 3 g) until GOT. When idle it
//  starts a demo dose every FAKE_AUTO_DOSE_S seconds, and every fourth
//  demo dose stops with a fault (flag F) to test that screen too.
// =====================================================================
#pragma once

void fakeMegaBegin();
void fakeMegaReceive(const char* body);   // a frame the ESP32 sent
void fakeMegaTick();                       // call often
int  fakeMegaReadByte();                   // next byte "from the Mega", or -1 if none
