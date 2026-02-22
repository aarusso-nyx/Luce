// cli.h: Simple CLI interface (Serial and Telnet) with shared FSM
#pragma once
#include "config.h"
#include <stdint.h>

// Initialize console and start listening
// Return false to abort task
bool cliInit();
// Poll Serial for CLI input (ESC to enter/exit)
// Return false to abort task
bool cliLoop();
// Setup Telnet server socket
// Return false to abort task
bool cliServer();
// Poll Telnet for CLI input (ESC to exit)
// Return false to abort task
bool cliTelnet();