#include "Arduino_Nesso_N1.h"

static bool wireInitialized = true;
bool ExpanderPin::_initialized[2] = {false, false};

MyBoschSensor myIMU(Wire);

// IO expander datasheet from https://www.diodes.com/datasheet/download/PI4IOE5V6408.pdf
// Battery charger datasheet from https://www.awinic.com/en/productDetail/AW32001ACSR
// battery gauge datasheet from https://www.ti.com/product/BQ27220

static void writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

static uint8_t readRegister(uint8_t address, uint8_t reg) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(address, 1);
  return Wire.read();
}

static constexpr uint8_t kGaugeReadRetries = 3;
static constexpr uint8_t kGaugeRetryDelayMs = 1;
static constexpr unsigned long kBatteryFilterRefreshMs = 5000;
static constexpr uint8_t kSocDoubleReadMaxDelta = 8;
static constexpr uint8_t kVoltageSocMaxDelta = 25;
static constexpr uint8_t kMaxSocDeltaPerUpdate = 10;
static constexpr float kStableVoltageDeltaV = 0.05f;
static constexpr float kHighVoltageThresholdV = 3.85f;
static constexpr uint8_t kLowSocAtHighVoltage = 30;
static constexpr uint8_t kEmaSmallChangeThreshold = 3;

static float sLastGoodVoltage = 3.7f;
static uint16_t sLastGoodPercent = 50;
static uint16_t sFilteredPercent = 50;
static float sFilteredVoltage = 3.7f;
static float sLastAcceptedVoltage = 3.7f;
static unsigned long sLastFilterRefreshMs = 0;
static bool sHasGoodGaugeReading = false;

static bool readGaugeRegister8(uint8_t reg, uint8_t &out);
static bool readGaugeRegister16(uint8_t reg, uint16_t &out);

enum BatteryRejectReason : uint8_t {
  BATTERY_ACCEPT = 0,
  BATTERY_REJECT_DOUBLE_READ,
  BATTERY_REJECT_VOLTAGE_SOC,
  BATTERY_REJECT_MAX_DELTA,
  BATTERY_REJECT_RAW_READ,
};

static uint8_t socFromVoltage(float volts) {
  struct OcvPoint {
    float v;
    uint8_t pct;
  };
  static const OcvPoint kCurve[] = {
      {2.50f, 0},  {3.30f, 0},  {3.40f, 5},  {3.60f, 20},
      {3.70f, 45}, {3.80f, 50}, {4.00f, 85}, {4.20f, 100},
  };
  if (volts <= kCurve[0].v) {
    return 0;
  }
  if (volts >= kCurve[7].v) {
    return 100;
  }
  for (uint8_t i = 0; i < 7; ++i) {
    if (volts <= kCurve[i + 1].v) {
      const float span = kCurve[i + 1].v - kCurve[i].v;
      if (span <= 0.0f) {
        return kCurve[i + 1].pct;
      }
      const float t = (volts - kCurve[i].v) / span;
      return (uint8_t)(kCurve[i].pct + t * (float)(kCurve[i + 1].pct - kCurve[i].pct) + 0.5f);
    }
  }
  return 50;
}

static uint8_t absDeltaU8(uint8_t a, uint8_t b) {
  return (a > b) ? (a - b) : (b - a);
}

static bool readRawSoc(uint8_t &soc) {
  if (readGaugeRegister8(NessoBattery::BQ27220_STATE_OF_CHARGE, soc) && soc <= 100) {
    return true;
  }
  uint16_t current_capacity = 0;
  uint16_t total_capacity = 0;
  if (readGaugeRegister16(NessoBattery::BQ27220_REMAIN_CAPACITY, current_capacity) &&
      readGaugeRegister16(NessoBattery::BQ27220_FULL_CAPACITY, total_capacity) &&
      total_capacity > 0) {
    const uint16_t pct = (uint16_t)((uint32_t)current_capacity * 100 / total_capacity);
    if (pct <= 100) {
      soc = (uint8_t)pct;
      return true;
    }
  }
  return false;
}

struct RawBatterySnapshot {
  float volts = 0.0f;
  uint8_t soc = 0;
  uint8_t soc2 = 0;
  bool valid = false;
};

static bool readRawBatterySnapshot(RawBatterySnapshot &snap) {
  uint16_t raw_mv = 0;
  if (!readGaugeRegister16(NessoBattery::BQ27220_VOLTAGE, raw_mv)) {
    return false;
  }
  snap.volts = (float)raw_mv / 1000.0f;
  if (snap.volts < 2.5f || snap.volts > 4.35f) {
    return false;
  }
  if (!readRawSoc(snap.soc)) {
    return false;
  }
  if (!readGaugeRegister8(NessoBattery::BQ27220_STATE_OF_CHARGE, snap.soc2) || snap.soc2 > 100) {
    snap.soc2 = snap.soc;
  }
  snap.valid = true;
  return true;
}

static BatteryRejectReason checkBatteryPlausibility(
    const RawBatterySnapshot &snap, uint16_t filteredPercent, float lastAcceptedVoltage, bool charging) {
  if (absDeltaU8(snap.soc, snap.soc2) > kSocDoubleReadMaxDelta) {
    return BATTERY_REJECT_DOUBLE_READ;
  }

  const uint8_t ocvEstimate = socFromVoltage(snap.volts);
  if (snap.volts > kHighVoltageThresholdV && snap.soc < kLowSocAtHighVoltage) {
    return BATTERY_REJECT_VOLTAGE_SOC;
  }
  if (absDeltaU8(snap.soc, ocvEstimate) > kVoltageSocMaxDelta &&
      fabsf(snap.volts - lastAcceptedVoltage) < kStableVoltageDeltaV) {
    return BATTERY_REJECT_VOLTAGE_SOC;
  }

  const int socDelta = (int)snap.soc - (int)filteredPercent;
  if (abs(socDelta) > kMaxSocDeltaPerUpdate &&
      fabsf(snap.volts - lastAcceptedVoltage) < kStableVoltageDeltaV) {
    if (!(charging && socDelta > 0)) {
      return BATTERY_REJECT_MAX_DELTA;
    }
  }
  return BATTERY_ACCEPT;
}

static uint16_t applyBatteryEma(uint16_t filtered, uint8_t rawSoc) {
  const int delta = abs((int)rawSoc - (int)filtered);
  if (delta <= kEmaSmallChangeThreshold) {
    return (uint16_t)(((filtered * 4) + rawSoc + 2) / 5);
  }
  return rawSoc;
}

static bool isBatteryCharging() {
  uint8_t status = readRegister(NessoBattery::AW32001_I2C_ADDR, NessoBattery::AW3200_SYS_STATUS);
  const uint8_t charge_status = (status >> 3) & 0b11;
  return charge_status == NessoBattery::CHARGING || charge_status == NessoBattery::PRE_CHARGE;
}

#ifdef NESSO_BATTERY_DEBUG
static void logBatteryReject(BatteryRejectReason reason, const RawBatterySnapshot &snap) {
  const char *label = "unknown";
  switch (reason) {
    case BATTERY_REJECT_DOUBLE_READ: label = "double_read"; break;
    case BATTERY_REJECT_VOLTAGE_SOC: label = "voltage_soc"; break;
    case BATTERY_REJECT_MAX_DELTA: label = "max_delta"; break;
    case BATTERY_REJECT_RAW_READ: label = "raw_read"; break;
    default: break;
  }
  Serial.printf(
      "[battery] reject=%s v=%.2f soc=%u soc2=%u filtered=%u\n",
      label, snap.volts, snap.soc, snap.soc2, sFilteredPercent);
}
#endif

static void refreshBatteryFilter(bool force) {
  const unsigned long now = millis();
  if (!force && sLastFilterRefreshMs != 0 && (now - sLastFilterRefreshMs) < kBatteryFilterRefreshMs) {
    return;
  }
  sLastFilterRefreshMs = now;

  RawBatterySnapshot snap;
  if (!readRawBatterySnapshot(snap)) {
#ifdef NESSO_BATTERY_DEBUG
    Serial.println("[battery] reject=raw_read");
#endif
    return;
  }

  const bool charging = isBatteryCharging();

  if (!sHasGoodGaugeReading) {
    sFilteredPercent = snap.soc;
    sFilteredVoltage = snap.volts;
    sLastAcceptedVoltage = snap.volts;
    sLastGoodPercent = snap.soc;
    sLastGoodVoltage = snap.volts;
    sHasGoodGaugeReading = true;
    return;
  }

  const BatteryRejectReason reason =
      checkBatteryPlausibility(snap, sFilteredPercent, sLastAcceptedVoltage, charging);
  if (reason != BATTERY_ACCEPT) {
#ifdef NESSO_BATTERY_DEBUG
    logBatteryReject(reason, snap);
#endif
    return;
  }

  sFilteredPercent = applyBatteryEma(sFilteredPercent, snap.soc);
  sFilteredVoltage = snap.volts;
  sLastAcceptedVoltage = snap.volts;
  sLastGoodPercent = sFilteredPercent;
  sLastGoodVoltage = snap.volts;
}

static bool readGaugeRegister8(uint8_t reg, uint8_t &out) {
  const uint8_t addr = NessoBattery::BQ27220_I2C_ADDR;
  for (uint8_t attempt = 0; attempt < kGaugeReadRetries; ++attempt) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
      delay(kGaugeRetryDelayMs);
      continue;
    }
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) {
      delay(kGaugeRetryDelayMs);
      continue;
    }
    out = Wire.read();
    return true;
  }
  return false;
}

static bool readGaugeRegister16(uint8_t reg, uint16_t &out) {
  const uint8_t addr = NessoBattery::BQ27220_I2C_ADDR;
  for (uint8_t attempt = 0; attempt < kGaugeReadRetries; ++attempt) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
      delay(kGaugeRetryDelayMs);
      continue;
    }
    if (Wire.requestFrom(addr, (uint8_t)2) != 2) {
      delay(kGaugeRetryDelayMs);
      continue;
    }
    const uint8_t lsb = Wire.read();
    const uint8_t msb = Wire.read();
    out = (uint16_t)((msb << 8) | lsb);
    return true;
  }
  return false;
}

static void writeBitRegister(uint8_t address, uint8_t reg, uint8_t bit, uint8_t value) {
  uint8_t val = readRegister(address, reg);
  if (value) {
    writeRegister(address, reg, val | (1 << bit));
  } else {
    writeRegister(address, reg, val & ~(1 << bit));
  }
}

static bool readBitRegister(uint8_t address, uint8_t reg, uint8_t bit) {
  uint8_t val = readRegister(address, reg);
  return ((val & (1 << bit)) > 0);
}

void pinMode(ExpanderPin pin, uint8_t mode) {
  if (!pin.initialized()) {
    Wire.begin();
    // reset all registers to default state
    readRegister(pin.address, 0x1);
    writeRegister(pin.address, 0x1, 0x1);
    readRegister(pin.address, 0x1);
    // set all pins as high as default state
    writeRegister(pin.address, 0x9, 0xFF);
    // interrupt mask to all pins
    writeRegister(pin.address, 0x11, 0xFF);
    // all input
    writeRegister(pin.address, 0x3, 0);
    // clear interrupt status
    readRegister(pin.address, 0x13);
    pin.initialize();
  }
  writeBitRegister(pin.address, 0x3, pin.pin, mode == OUTPUT);
  if (mode == OUTPUT) {
    // remove high impedance
    writeBitRegister(pin.address, 0x7, pin.pin, false);
  } else if (mode == INPUT_PULLUP) {
    // set pull-up resistor
    writeBitRegister(pin.address, 0xB, pin.pin, true);
    writeBitRegister(pin.address, 0xD, pin.pin, true);
  } else if (mode == INPUT_PULLDOWN) {
    // disable pull-up resistor
    writeBitRegister(pin.address, 0xB, pin.pin, true);
    writeBitRegister(pin.address, 0xD, pin.pin, false);
  } else if (mode == INPUT) {
    // disable pull selector resistor
    writeBitRegister(pin.address, 0xB, pin.pin, false);
  }
}

void digitalWrite(ExpanderPin pin, uint8_t val) {
  if (!wireInitialized) {
    Wire.begin(SDA, SCL);
    wireInitialized = true;
  }
  writeBitRegister(pin.address, 0x5, pin.pin, val == HIGH);
}

int digitalRead(ExpanderPin pin) {
  return readBitRegister(pin.address, 0xF, pin.pin);
}

void NessoBattery::begin(uint16_t current, uint16_t voltage, UnderVoltageLockout uvlo, uint16_t dpm_voltage, uint8_t timeout) {
  Wire.begin();
  setChargeCurrent(current);
  setChargeVoltage(voltage);
  setWatchdogTimer(timeout);
  setBatUVLO(uvlo);
  setVinDPMVoltage(dpm_voltage);
  setHiZ(false);
  setChargeEnable(true);
}

void NessoBattery::enableCharge() {
  setChargeEnable(true);
}

void NessoBattery::setChargeEnable(bool enable) {
  // bit 3 set charge enable
  writeBitRegister(AW32001_I2C_ADDR, AW3200_POWER_ON_CFG, 3, !enable);
}

void NessoBattery::setVinDPMVoltage(uint16_t voltage) {
  if (voltage < 3880) {
    voltage = 3880;
  }
  if (voltage > 5080) {
    voltage = 5080;
  }
  uint8_t reg_value = readRegister(AW32001_I2C_ADDR, AW3200_INPUT_SRC);
  // bits 7-4 set Vin DPM voltage
  reg_value &= ~0b01111000;
  reg_value |= ((voltage - 3880) / 80) << 4;
  writeRegister(AW32001_I2C_ADDR, AW3200_INPUT_SRC, reg_value);
}

void NessoBattery::setIinLimitCurrent(uint16_t current) {
  if (!wireInitialized) {
    Wire.begin(SDA, SCL);
    wireInitialized = true;
  }
  if (current < 50) {
    current = 50;
  }
  if (current > 500) {
    current = 500;
  }
  uint8_t reg_value = readRegister(AW32001_I2C_ADDR, AW3200_INPUT_SRC);
  // bits 3-0 set Iin limit current
  reg_value &= ~0b00001111;
  reg_value |= ((current - 50) / 30) & 0b00001111;
  writeRegister(AW32001_I2C_ADDR, AW3200_INPUT_SRC, reg_value);
}

void NessoBattery::setBatUVLO(UnderVoltageLockout uvlo) {
  uint8_t reg_value = readRegister(AW32001_I2C_ADDR, AW3200_POWER_ON_CFG);
  // bits 2-0 set UVLO
  reg_value &= ~0b00000111;
  reg_value |= (uvlo & 0b00000111);
  writeRegister(AW32001_I2C_ADDR, AW3200_POWER_ON_CFG, reg_value);
}

void NessoBattery::setChargeCurrent(uint16_t current) {
  if (current < 8) {
    current = 8;
  }
  if (current > 456) {
    current = 456;
  }
  uint8_t reg_value = readRegister(AW32001_I2C_ADDR, AW3200_CHG_CURRENT);
  // bits 5-0 set charge current
  reg_value &= ~0b00111111;
  reg_value |= ((current - 8) / 8) & 0b00111111;
  writeRegister(AW32001_I2C_ADDR, AW3200_CHG_CURRENT, reg_value);
}

void NessoBattery::setDischargeCurrent(uint16_t current) {
  if (current < 200) {
    current = 200;
  }
  if (current > 3200) {
    current = 3200;
  }
  uint8_t reg_value = readRegister(AW32001_I2C_ADDR, AW3200_TERM_CURRENT);
  // bits 7-4 set discharge current
  reg_value &= ~0b11110000;
  reg_value |= (((current - 200) / 200) & 0b00001111) << 4;
  writeRegister(AW32001_I2C_ADDR, AW3200_TERM_CURRENT, reg_value);
}

void NessoBattery::setChargeVoltage(uint16_t voltage) {
  if (voltage < 3600) {
    voltage = 3600;
  }
  if (voltage > 4545) {
    voltage = 4545;
  }
  uint8_t reg_value = readRegister(AW32001_I2C_ADDR, AW3200_CHG_VOLTAGE);
  // bits 7-2 set charge voltage
  reg_value &= ~0b11111100;
  reg_value |= ((voltage - 3600) / 15) << 2;
  writeRegister(AW32001_I2C_ADDR, AW3200_CHG_VOLTAGE, reg_value);
}

void NessoBattery::setWatchdogTimer(uint8_t sec) {
  uint8_t reg_value = readRegister(AW32001_I2C_ADDR, AW3200_TIMER_WD);
  uint8_t bits = 0;
  switch (sec) {
    case 0:
      bits = 0b00;  // disable watchdog
      break;
    case 40:  bits = 0b01; break;
    case 80:  bits = 0b10; break;
    case 160: bits = 0b11; break;
    default:  bits = 0b11; break;
  }
  // bits 6-5 set watchdog timer
  reg_value &= ~(0b11 << 5);
  reg_value |= (bits << 5);
  writeRegister(AW32001_I2C_ADDR, AW3200_TIMER_WD, reg_value);
}

void NessoBattery::feedWatchdog() {
  // bit 6 set feed watchdog
  writeBitRegister(AW32001_I2C_ADDR, AW3200_CHG_CURRENT, 6, true);
}

void NessoBattery::setShipMode(bool en) {
  // bit 5 set ship mode
  writeBitRegister(AW32001_I2C_ADDR, AW3200_MAIN_CTRL, 5, en);
}

NessoBattery::ChargeStatus NessoBattery::getChargeStatus() {
  uint8_t status = readRegister(AW32001_I2C_ADDR, AW3200_SYS_STATUS);
  // bits 4-3 set charge status
  uint8_t charge_status = (status >> 3) & 0b11;
  return static_cast<ChargeStatus>(charge_status);
}

void NessoBattery::setHiZ(bool enable) {
  // bit 4 set Hi-Z mode
  writeBitRegister(AW32001_I2C_ADDR, AW3200_POWER_ON_CFG, 4, enable);
}

void NessoBattery::getBatteryStatus(float &volts, uint16_t &percent) {
  refreshBatteryFilter(true);
  volts = sHasGoodGaugeReading ? sFilteredVoltage : 0.0f;
  percent = sHasGoodGaugeReading ? sFilteredPercent : 0;
}

float NessoBattery::getVoltage() {
  refreshBatteryFilter(false);
  return sHasGoodGaugeReading ? sFilteredVoltage : 0.0f;
}

float NessoBattery::getCurrent() {
  uint16_t raw = 0;
  if (!readGaugeRegister16(BQ27220_CURRENT, raw)) {
    return 0.0f;
  }
  return (float)((int16_t)raw) / 1000.0f;
}

uint16_t NessoBattery::getChargeLevel() {
  refreshBatteryFilter(false);
  return sHasGoodGaugeReading ? sFilteredPercent : 0;
}

int16_t NessoBattery::getAvgPower() {
  uint16_t raw = 0;
  if (!readGaugeRegister16(BQ27220_AVG_POWER, raw)) {
    return 0;
  }
  return (int16_t)raw;
}

float NessoBattery::getTemperature() {
  uint16_t raw = 0;
  if (!readGaugeRegister16(BQ27220_TEMPERATURE, raw)) {
    return 0.0f;
  }
  return ((float)raw / 10.0f) - 273.15f;
}

uint16_t NessoBattery::getCycleCount() {
  uint16_t raw = 0;
  if (!readGaugeRegister16(BQ27220_CYCLE_COUNT, raw)) {
    return 0;
  }
  return raw;
}

ExpanderPin LORA_LNA_ENABLE(5);
ExpanderPin LORA_ANTENNA_SWITCH(6);
ExpanderPin LORA_ENABLE(7);
ExpanderPin KEY1(0);
ExpanderPin KEY2(1);
ExpanderPin POWEROFF((1 << 8) | 0);
ExpanderPin LCD_RESET((1 << 8) | 1);
ExpanderPin GROVE_POWER_EN((1 << 8) | 2);
ExpanderPin VIN_DETECT((1 << 8) | 5);
ExpanderPin LCD_BACKLIGHT((1 << 8) | 6);
ExpanderPin LED_BUILTIN((1 << 8) | 7);
