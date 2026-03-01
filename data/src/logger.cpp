#include "logger.h"

#include <algorithm>
#include <cstring>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <sys/stat.h>
#include <stdio.h>

#include "util.h"
#include "http.h"
#include "mqtt.h"
#include "lcd.h"  // for LCD echo of system and error messages

// Static member initialization
// Console log level (used for console echo filtering)
Logger::Level Logger::consoleLevel = Logger::INFO;
// File log level (used for file persistence filtering)
Logger::Level Logger::fileLevel    = Logger::INFO;
// (removed Arduino Print* support)
// In-memory ring buffer of log lines
std::vector<std::string> Logger::ringBuffer;
// In-memory ring buffer capacity (modifiable)
size_t Logger::maxLines = 1000;
const char* Logger::logFileName = "/logs.txt";
bool Logger::_serialCliMode = false;
// Persistent log file handle (native FILE*) to avoid reopening on each write
static FILE* loggerFile = nullptr;
// Format strings for console and file output (customizable prefixes)
std::string Logger::consoleFormat = "[%m] [%a]";
std::string Logger::fileFormat    = "[%t] [%u] <%T> {%m|%a} (%s:%n/%f) ";

namespace {
// Replace custom % codes with actual log metadata
static std::string formatPrefix(const std::string& fmt,
                                const char* module,
                                const char* action,
                                const char* file,
                                int line,
                                const char* func) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); ++i) {
    char c = fmt[i];
    if (c == '%' && i + 1 < fmt.size()) {
      char code = fmt[++i];
      char buf[64];
      switch (code) {
        case 't': utilGetIsoTime(buf, sizeof(buf)); out += buf; break;
        case 'u': utilGetUptimeFormatted(buf, sizeof(buf)); out += buf; break;
        case 'p': {
          const char* raw = pcTaskGetTaskName(nullptr);
          out += (raw && strcmp(raw, "loopTask") != 0) ? raw : "MAIN";
          break;
        }
        case 'm': out += module; break;
        case 'a': out += action; break;
        case 's': out += file; break;
        case 'n': out += std::to_string(line); break;
        case 'f': out += func; break;
        case '%': out += '%'; break;
        default: out += '%'; out += code; break;
      }
    } else {
      out += c;
    }
  }
  return out;
}
} // anonymous namespace

void Logger::begin(Level level) {
  // Initialize console level
  consoleLevel = level;
  // Load existing log lines into ring buffer from native LittleFS mount
  {
    std::string fullpath = std::string("/littlefs") + logFileName;
    struct stat st;
    if (stat(fullpath.c_str(), &st) == 0) {
      FILE* file = fopen(fullpath.c_str(), "r");
      size_t count = 0;
      if (file) {
        char buf[256];
        while (fgets(buf, sizeof(buf), file)) {
          std::string line(buf);
          // Trim newline and carriage return
          if (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
          if (!line.empty() && line.back() == '\r') line.pop_back();
          ringBuffer.push_back(line);
          count++;
          if (ringBuffer.size() > maxLines) ringBuffer.erase(ringBuffer.begin());
        }
        fclose(file);
      }
      // If existing log file longer than buffer, rewrite to trim
      if (count > ringBuffer.size()) {
        FILE* wfile = fopen(fullpath.c_str(), "w");
        if (wfile) {
          for (auto &l : ringBuffer) {
            fprintf(wfile, "%s\n", l.c_str());
          }
          fclose(wfile);
        }
      }
    }
    // Open log file for persistent appends and keep it open
    loggerFile = fopen(fullpath.c_str(), "a");
  }
}

void Logger::setSerialCliMode(bool on) {
  _serialCliMode = on;
}

// Set global log level (alias for console log level)
void Logger::setLevel(Level level) {
  consoleLevel = level;
}



// Get global log level (alias for console log level)
Logger::Level Logger::getLevel() {
  return consoleLevel;
}

// Configure console format string
void Logger::setConsoleFormat(const std::string& fmt) {
  consoleFormat = fmt;
}
const std::string& Logger::getConsoleFormat() {
  return consoleFormat;
}
// Configure file format string
void Logger::setFileFormat(const std::string& fmt) {
  fileFormat = fmt;
}
const std::string& Logger::getFileFormat() {
  return fileFormat;
}
// Set or get console log level explicitly
void Logger::setConsoleLevel(Level level) {
  consoleLevel = level;
}
Logger::Level Logger::getConsoleLevel() {
  return consoleLevel;
}
// Set or get file log level explicitly
void Logger::setFileLevel(Level level) {
  fileLevel = level;
}
Logger::Level Logger::getFileLevel() {
  return fileLevel;
}


// Main log function with source info
void Logger::log(Level level,
                  const char* module,
                  const char* action,
                  const char* file,
                  int line,
                  const char* func,
                  const char* fmt, ...) {
  // No global filtering: console and file levels applied separately below
  // Format user message
  char details[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(details, sizeof(details), fmt, args);
  va_end(args);

  // Build message body
  std::string body(details);

  // Build full detail (file) and console prefixes based on format strings
  std::string fullLine = formatPrefix(fileFormat, module, action, file, line, func)
                       + body;
  std::string echoLine = formatPrefix(consoleFormat, module, action, file, line, func)
                       + body;

  // Console output (filtered by consoleLevel)
  if (level >= consoleLevel) {
    printf("%s\n", echoLine.c_str());
    if (level == SYSTEM || level == ERROR) {
      lcdShowBoot("%s", body.c_str());
    }
  }

  // Store console output in ring buffer (in-memory)
  ringBuffer.push_back(echoLine);
  if (ringBuffer.size() > maxLines) {
    ringBuffer.erase(ringBuffer.begin());
  }
  // Persist to file (filtered by fileLevel), using native FILE* handle
  if (level >= fileLevel && loggerFile) {
    fprintf(loggerFile, "%s\n", fullLine.c_str());
    fflush(loggerFile);
  }

  // Broadcast over WebSocket and MQTT (filtered by fileLevel)
  if (level >= fileLevel) {
    publishLog(fullLine.c_str(), false);
    broadcastLog(fullLine.c_str());
  }
}
// Return reference to in-memory log ring buffer
const std::vector<std::string>& Logger::getBuffer() {
  return ringBuffer;
}
// Get the current ring-buffer capacity
size_t Logger::getMaxLines() {
  return maxLines;
}
// Set the ring-buffer capacity (0-1023) and trim if needed
void Logger::setMaxLines(size_t n) {
  if (n > 1023) n = 1023;
  maxLines = n;
  while (ringBuffer.size() > maxLines) ringBuffer.erase(ringBuffer.begin());
}