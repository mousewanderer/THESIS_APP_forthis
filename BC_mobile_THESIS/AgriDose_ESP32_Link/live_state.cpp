#include "live_state.h"
#include "config.h"

static LiveState state;
static SemaphoreHandle_t mutex = nullptr;

void liveBegin() {
  mutex = xSemaphoreCreateMutex();
  memset(&state, 0, sizeof(state));
  state.flag = '?';
}

void liveTouch() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  state.everHeard = true;
  state.lastFrameMs = millis();
  state.frameCount++;
  xSemaphoreGive(mutex);
}

void liveSetWeight(int32_t dg, char flag) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  state.weightDg = dg;
  state.flag = flag;
  xSemaphoreGive(mutex);
}

void liveSetHoppers(const int32_t dg[3]) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  memcpy(state.hopperDg, dg, sizeof(state.hopperDg));
  state.haveHoppers = true;
  xSemaphoreGive(mutex);
}

void liveSetDone(const DoseResult& d) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  state.lastDone = d;
  state.doneCount++;
  xSemaphoreGive(mutex);
}

LiveState liveGet() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  LiveState copy = state;
  xSemaphoreGive(mutex);
  return copy;
}

bool liveMegaOnline(const LiveState& s) {
  return s.everHeard && millis() - s.lastFrameMs < LINK_OFFLINE_MS;
}
