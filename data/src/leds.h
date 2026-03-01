// leds.h: Control for 3 system LEDs on GPIOs 25, 26, 27
#pragma once
// Class to manage system LEDs
class LedsClass {
public:
  static constexpr uint8_t NUM_LEDS = 3;
  // Initialize LED pins
  void begin();
  // Set LED at index [0..NUM_LEDS-1]
  void set(uint8_t idx, bool on);
  // Get LED state
  bool get(uint8_t idx) const;
  // Get bitmask of all LED states
  uint8_t getMask() const;
  // Blink LED idx N times (fast blink)
  void blink(uint8_t idx, uint8_t times = 3);
  // Pulse LED idx once (smooth fade in/out)
  void pulse(uint8_t idx);
private:
  static const uint8_t pins[NUM_LEDS];
  uint8_t stateMask = 0;
};
// Global instance
extern LedsClass Leds;
// end of header