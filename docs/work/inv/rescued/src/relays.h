// relays.h: Native MCP23017 relay and input handling via I2C expander
#pragma once
#include <stdint.h>
#include "config.h"  // for RELAY_COUNT

// Initialize relay hardware (set pin directions, initial states, interrupts)
// Return false to abort task
bool relaysInit();

// Poll for input events and handle relay control; call in a task or loop
// Return false to abort task
bool relaysLoop();

// Diagnostic: blink each relay once sequentially
void relaysTest();

// Emergency shutdown: force all relays off immediately
void relaysForceAllOff();

// Set full night-mode mask (bitmask of size RELAY_COUNT)
// Returns true if mask was changed
bool relaysSetNight(uint8_t mask);

// Set full relay state mask (bitmask of size RELAY_COUNT)
// Returns true if state was changed
bool relaysSetState(uint8_t mask);

// Set individual night-mode bit at index [0..RELAY_COUNT)
bool relaysSetNightIdx(uint8_t idx, bool on);

// Set individual relay state bit at index [0..RELAY_COUNT)
bool relaysSetStateIdx(uint8_t idx, bool on);

// Toggle a relay (active-low)
void relaysToggle(uint8_t idx);

// Returns true if daylight mode is currently active
// (internal functions relaysIsDay and relaysUpdate removed)