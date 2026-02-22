#pragma once
#include <vector>
#include <string>

class Logger {
public:
  // Log levels, in increasing verbosity
  enum Level { DEBUG = 0, INFO = 1, SYSTEM = 2, WARNING = 3, ERROR = 4 };
  // Initialize log system; mounts LittleFS and loads existing logs
  static void begin(Level level = INFO);
  // Set global log level (alias for console level): DEBUG for verbose, ERROR to suppress warnings
  static void setLevel(Level level);
  // Configure console format (format string)
  static void setConsoleFormat(const std::string& fmt);
  static const std::string& getConsoleFormat();
  // Configure file format (format string)
  static void setFileFormat(const std::string& fmt);
  static const std::string& getFileFormat();
  // Get global log level (alias for console level)
  static Level getLevel();
  // Set or get console log level (used for console echo filtering)
  static void setConsoleLevel(Level level);
  static Level getConsoleLevel();
  // Set or get file log level (used for file persistence filtering)
  static void setFileLevel(Level level);
  static Level getFileLevel();
  // Enable or disable serial CLI mode (suppresses serial logs when true)
  static void setSerialCliMode(bool on);
  // Customizable format strings (via setConsoleFormat / setFileFormat)
  // Main log function: level, module, action, source file/line/function, formatted message
  static void log(Level level,
                  const char* module,
                  const char* action,
                  const char* file,
                  int line,
                  const char* func,
                  const char* fmt, ...);
  // Access in-memory log buffer
  static const std::vector<std::string>& getBuffer();
  // Get or set the ring-buffer capacity (in-memory log lines, 0-1023)
  static size_t getMaxLines();
  static void setMaxLines(size_t n);
private:
  // Separate log levels for console echo and file persistence
  static Level consoleLevel;
  static Level fileLevel;
  static std::vector<std::string> ringBuffer;
  // Maximum in-memory log lines (ring buffer capacity)
  static size_t maxLines;
  static const char* logFileName;
  // Serial CLI mode suppresses serial echo
  static bool _serialCliMode;
  // Format strings for console and file output
  static std::string consoleFormat;
  static std::string fileFormat;
};

// Convenience macros: pass source info automatically
#define LOGSYS(module, action, fmt, ...)   Logger::log(Logger::SYSTEM,  module, action, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOGINFO(module, action, fmt, ...)  Logger::log(Logger::INFO,    module, action, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOGDEBUG(module, action, fmt, ...) Logger::log(Logger::DEBUG,   module, action, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOGWARN(module, action, fmt, ...)  Logger::log(Logger::WARNING, module, action, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOGERR(module, action, fmt, ...)   Logger::log(Logger::ERROR,   module, action, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)