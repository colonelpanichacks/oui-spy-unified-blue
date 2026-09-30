#line 1 "/Users/nicholastenbrink/projects/nesso/oui_spy_nesso/nimble_compat.h"
#ifndef NIMBLE_COMPAT_H
#define NIMBLE_COMPAT_H

#include <NimBLEDevice.h>

// Shim upstream NimBLE 1.x API onto NimBLE-Arduino 2.x
using NimBLEAdvertisedDeviceCallbacks = NimBLEScanCallbacks;

inline void nimbleSetAdvertisedCallbacks(NimBLEScan* scan, NimBLEScanCallbacks* cb) {
  scan->setScanCallbacks(cb, true);
}

inline const uint8_t* nimbleAdvPayload(const NimBLEAdvertisedDevice* dev, size_t* len) {
  const std::vector<uint8_t>& payload = dev->getPayload();
  *len = payload.size();
  return payload.empty() ? nullptr : payload.data();
}

inline const uint8_t* nimbleMacBytes(const NimBLEAdvertisedDevice* dev) {
  return dev->getAddress().getVal();
}

#endif
