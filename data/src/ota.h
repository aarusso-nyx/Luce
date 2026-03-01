// ota.h: Over-The-Air update helpers
#pragma once

// #include <>

// Initialize native OTA with configured hostname; return false to abort task
bool otaInit();

// Poll OTA state; call regularly in a task; return false to abort task
bool otaLoop();

// end of header