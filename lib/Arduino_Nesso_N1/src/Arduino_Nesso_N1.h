#ifndef Arduino_Nesso_N1_h
#define Arduino_Nesso_N1_h

#if defined(__cplusplus)

#include <Arduino.h>
#include <Wire.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#include <Arduino_BMI270_BMM150.h>

#undef LORA_LNA_ENABLE
#undef LORA_ANTENNA_SWITCH
#undef LORA_ENABLE
#undef POWEROFF
#undef GROVE_POWER_EN
#undef VIN_DETECT
#undef LCD_RESET
#undef LCD_BACKLIGHT
#undef LED_BUILTIN
#undef KEY1
#undef KEY2

// address: 0x43/0x44
class ExpanderPin {
public:
  ExpanderPin(uint16_t _pin) : pin(_pin & 0xFF), address(_pin & 0x100 ? 0x44 : 0x43){};
  uint8_t pin;
  uint8_t address;
  bool initialized() {
    return _initialized[address - 0x43];
  }
  void initialize() {
    _initialized[address - 0x43] = true;
  }
private:
  static bool _initialized[2];
};


extern ExpanderPin LORA_LNA_ENABLE;
extern ExpanderPin LORA_ANTENNA_SWITCH;
extern ExpanderPin LORA_ENABLE;
extern ExpanderPin POWEROFF;
extern ExpanderPin GROVE_POWER_EN;
extern ExpanderPin VIN_DETECT;
extern ExpanderPin LCD_RESET;
extern ExpanderPin LCD_BACKLIGHT;
extern ExpanderPin LED_BUILTIN;
extern ExpanderPin KEY1;
extern ExpanderPin KEY2;

void pinMode(ExpanderPin pin, uint8_t mode);
void digitalWrite(ExpanderPin pin, uint8_t val);
int digitalRead(ExpanderPin pin);

class NessoBattery {
public:
  static constexpr uint8_t AW32001_I2C_ADDR = 0x49;
  static constexpr uint8_t BQ27220_I2C_ADDR = 0x55;

  enum AW32001Reg : uint8_t {
    AW3200_INPUT_SRC = 0x00,
    AW3200_POWER_ON_CFG = 0x01,
    AW3200_CHG_CURRENT = 0x02,
    AW3200_TERM_CURRENT = 0x03,
    AW3200_CHG_VOLTAGE = 0x04,
    AW3200_TIMER_WD = 0x05,
    AW3200_MAIN_CTRL = 0x06,
    AW3200_SYS_CTRL = 0x07,
    AW3200_SYS_STATUS = 0x08,
    AW3200_FAULT_STATUS = 0x09,
    AW3200_CHIP_ID = 0x0A,
  };

  enum BQ27220Reg : uint8_t {
    BQ27220_VOLTAGE = 0x08,
    BQ27220_CURRENT = 0x0C,
    BQ27220_REMAIN_CAPACITY = 0x10,
    BQ27220_FULL_CAPACITY = 0x12,
    BQ27220_AVG_POWER = 0x24,
    BQ27220_TEMPERATURE = 0x28,
    BQ27220_CYCLE_COUNT = 0x2A,
    BQ27220_STATE_OF_CHARGE = 0x2C,
  };

  enum ChargeStatus {
    NOT_CHARGING = 0,
    PRE_CHARGE = 1,
    CHARGING = 2,
    FULL_CHARGE = 3,
  };

  enum UnderVoltageLockout {
    UVLO_2430mV = 0,
    UVLO_2490mV = 1,
    UVLO_2580mV = 2,
    UVLO_2670mV = 3,
    UVLO_2760mV = 4,
    UVLO_2850mV = 5,
    UVLO_2940mV = 6,
    UVLO_3030mV = 7,
  };

  NessoBattery(){};
  void begin(
    uint16_t current = 256, uint16_t voltage = 4200, UnderVoltageLockout uvlo = UVLO_2580mV, uint16_t dpm_voltage = 4520, uint8_t timeout = 0
  );  // default: charge current 256mA, battery 4200mV, uvlo 2580mV, DMP 4520mV, disable watchdog

  // AW32001 functions
  void enableCharge();                         // enable charging
  void setChargeEnable(bool enable);           // charge control
  void setVinDPMVoltage(uint16_t voltage);     // set input voltage limit, 3880mV ~ 5080mV(step 80mV, default 4520mV)
  void setIinLimitCurrent(uint16_t current);   // set input current limit, 50mA ~ 500mA(step 30mA, default 500mA)
  void setBatUVLO(UnderVoltageLockout uvlo);   // set battery under voltage lockout(2430mV, 2490mV, 2580mV, 2670mV, 2760mV, 2850mV, 2940mV, 3030mV)
  void setChargeCurrent(uint16_t current);     // set charging current, 8mA ~ 456mA(step 8mA, default 128mA)
  void setDischargeCurrent(uint16_t current);  // set discharging current, 200mA ~ 3200mA(step 200mA, default 2000mA)
  void setChargeVoltage(uint16_t voltage);     // set charging voltage, 3600mV ~ 4545mV(step 15mV, default 4200mV)
  void setWatchdogTimer(uint8_t sec);          // set charge watchdog timeout(0s, 40s, 80s, 160s, default 160s, 0 to disable)
  void feedWatchdog();                         // feed watchdog timer
  void setShipMode(bool en);                   // set ship mode
  ChargeStatus getChargeStatus();              // get charge status
  void setHiZ(bool enable);                    // set Hi-Z mode, true: USB -x-> SYS, false: USB -> SYS

  // BQ27220 functions
  void getBatteryStatus(float &volts, uint16_t &percent);  // filtered snapshot for display
  float getVoltage();         // get battery voltage in Volts
  float getCurrent();         // get battery current in Amperes
  uint16_t getChargeLevel();  // get battery charge level in percents
  int16_t getAvgPower();      // get average power in mWatts, can be negative
  float getTemperature();     // get battery temperature in Celsius
  uint16_t getCycleCount();   // get battery cycle count
};


class NessoDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  NessoDisplay(void) {
    {
      auto cfg = _bus_instance.config();

      cfg.pin_mosi = 21;
      cfg.pin_miso = 22;
      cfg.pin_sclk = 20;
      cfg.pin_dc = 16;
      cfg.freq_write = 40000000;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();

      cfg.invert = true;
      cfg.pin_cs = 17;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = 135;
      cfg.panel_height = 240;
      cfg.offset_x = 52;
      cfg.offset_y = 40;

      _panel_instance.config(cfg);
    }

    setPanel(&_panel_instance);
  }
  bool begin() {
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(LCD_BACKLIGHT, OUTPUT);
    pinMode(LCD_RESET, OUTPUT);
    pinMode(KEY1, INPUT_PULLUP);
    pinMode(KEY2, INPUT_PULLUP);
    digitalWrite(LCD_BACKLIGHT, HIGH);
    digitalWrite(LCD_RESET, LOW);
    delay(100);
    digitalWrite(LCD_RESET, HIGH);
    return lgfx::LGFX_Device::begin();
  }
};

class NessoTouch {
private:
  TwoWire *_wire;
  uint8_t _addr;
  bool _inited;

  // FT6x36 register definitions
  static const uint8_t FT6X36_ADDR = 0x38;
  static const uint8_t FT6X36_TD_STAT_REG = 0x02;
  static const uint8_t FT6X36_P1_XH_REG = 0x03;

  uint8_t readRegister(uint8_t reg) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission();
    _wire->requestFrom(_addr, (uint8_t)1);
    return _wire->read();
  }

  void readRegisters(uint8_t reg, uint8_t *buf, uint8_t len) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission();
    _wire->requestFrom(_addr, len);
    for (uint8_t i = 0; i < len; i++) {
      buf[i] = _wire->read();
    }
  }

public:
  NessoTouch(TwoWire &wire = Wire)
    : _wire(&wire), _addr(FT6X36_ADDR), _inited(false) {}

  bool begin() {
    if (_inited) return true;
    
    _wire->beginTransmission(_addr);
    if (_wire->endTransmission() == 0) {
      _inited = true;
      return true;
    }
    return false;
  }

  // Check if touch is detected
  bool isTouched() {
    if (!_inited) return false;
    uint8_t touchPoints = readRegister(FT6X36_TD_STAT_REG) & 0x0F;
    return (touchPoints == 1);  // Only handle single touch
  }

  // Read touch coordinates
  bool read(int16_t &x, int16_t &y) {
    if (!_inited) return false;
    
    uint8_t data[4];
    readRegisters(FT6X36_P1_XH_REG, data, 4);
    
    uint8_t touchPoints = readRegister(FT6X36_TD_STAT_REG) & 0x0F;
    if (touchPoints != 1) {
      return false;
    }
    
    x = ((data[0] & 0x0F) << 8) | data[1];
    y = ((data[2] & 0x0F) << 8) | data[3];
    
    return true;
  }
};

class MyBoschSensor: public BoschSensorClass {

  public:
    MyBoschSensor(TwoWire& wire = Wire) : BoschSensorClass(wire) {};
    bool begin() {
      return BoschSensorClass::begin(BOSCH_ACCELEROMETER_ONLY);
    }

  protected:
    virtual int8_t configure_sensor(struct bmi2_dev *dev)
    {
      int8_t rslt;
      uint8_t sens_list[2] = { BMI2_ACCEL, BMI2_GYRO };

      // This configures the second gpio expander and clears its interrupt status
      pinMode(LORA_ENABLE, INPUT);

      struct bmi2_int_pin_config int_pin_cfg;
      int_pin_cfg.pin_type = BMI2_INT1;
      int_pin_cfg.int_latch = BMI2_INT_LATCH;
      int_pin_cfg.pin_cfg[0].lvl = BMI2_INT_ACTIVE_LOW;
      int_pin_cfg.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
      int_pin_cfg.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
      int_pin_cfg.pin_cfg[0].input_en = BMI2_INT_INPUT_DISABLE;

      struct bmi2_sens_config sens_cfg[2];
      sens_cfg[0].type = BMI2_ACCEL;
      sens_cfg[0].cfg.acc.bwp = BMI2_ACC_OSR2_AVG2;
      sens_cfg[0].cfg.acc.odr = BMI2_ACC_ODR_25HZ;
      sens_cfg[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
      sens_cfg[0].cfg.acc.range = BMI2_ACC_RANGE_4G;
      sens_cfg[1].type = BMI2_GYRO;
      sens_cfg[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
      sens_cfg[1].cfg.gyr.bwp = BMI2_GYR_OSR2_MODE;
      sens_cfg[1].cfg.gyr.odr = BMI2_GYR_ODR_25HZ;
      sens_cfg[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
      sens_cfg[1].cfg.gyr.ois_range = BMI2_GYR_OIS_2000;

      rslt = bmi2_set_int_pin_config(&int_pin_cfg, dev);
      if (rslt != BMI2_OK)
        return rslt;

      rslt = bmi2_map_data_int(BMI2_DRDY_INT, BMI2_INT1, dev);
      if (rslt != BMI2_OK)
        return rslt;

      rslt = bmi2_set_sensor_config(sens_cfg, 2, dev);
      if (rslt != BMI2_OK)
        return rslt;

      rslt = bmi2_sensor_enable(sens_list, 2, dev);
      if (rslt != BMI2_OK)
        return rslt;

      return rslt;
    }
};

extern MyBoschSensor myIMU;
#undef IMU
#define IMU myIMU

#endif
#endif