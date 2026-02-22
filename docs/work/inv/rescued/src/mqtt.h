// mqtt.h: handles MQTT connectivity and topics
// mqtt.h: handles MQTT connectivity and topics
#pragma once
#include "config.h" // for SensorEventData
// Query current MQTT connection state
bool mqttIsConnected();


// Initialize MQTT with configured broker URI; client ID and root topic are derived from device name
// Return false to abort task
bool mqttInit();

// Maintain MQTT connection; call in main loop
// Return false to abort task
bool mqttLoop();
// Publish sensor data over MQTT: send SensorEventData struct
void publishSensorData(const SensorEventData& s);
// Publish a log message over MQTT
void publishLog(const char* message, bool retained = false);
// Publish a numeric or string payload under '<device>/<subtopic>'
void mqttPublish(const char* subtopic, const char* payload, bool retained = true);
void mqttPublish(const char* subtopic, int value, bool retained = true);
// Overload for floating-point values
void mqttPublish(const char* subtopic, float value, bool retained = true);


// end of header