#ifndef _SIMPLE_LOGGER_HPP
#define _SIMPLE_LOGGER_HPP

#include <iostream>
#include <string_view>
#include <source_location>
#include <chrono>
#include <iomanip>

/**
 * @file simple_logger.hpp
 * @brief Header providing a simple logger that prints colored
 *
 * Compile-time configuration:
 * Users can define LOG_LEVEL before including this header.
 * Example: -DLOG_LEVEL=2 (Only INFO and above)
 */

#ifndef LOG_LEVEL
#define LOG_LEVEL 4  // Default to ERROR
#endif

namespace SimpleLogger {

    /*
     * @brief Log levels
     */
    enum class LogLevel { TRACE = 0, DEBUG = 1, INFO = 2, WARN = 3, ERROR = 4, FATAL = 5, OFF = 6 };

    /**
     * @brief Compile-time constant used to cut off lower priority logs.
     */
    constexpr LogLevel CurrentLogLevelCutoff = static_cast<LogLevel>(LOG_LEVEL);

    class Logger
    {
      public:
        /**
         * @brief Translate an enum into string_view
         *
         * @param level Log level
         * @return `Stringified` log level
         */
        static constexpr std::string_view levelToString(LogLevel level)
        {
            switch (level) {
            case LogLevel::TRACE:
                return "TRACE";
            case LogLevel::DEBUG:
                return "DEBUG";
            case LogLevel::INFO:
                return "INFO ";
            case LogLevel::WARN:
                return "WARN ";
            case LogLevel::ERROR:
                return "ERROR";
            case LogLevel::FATAL:
                return "FATAL";
            default:
                return "UNKNOWN";
            }
        }

        /**
         * @brief Translate log level into a color code for console output
         *
         * @param level Log level
         * @return String containig the code that, on a terminal, colors the text that follows.
         */
        static constexpr std::string_view levelColor(LogLevel level)
        {
            switch (level) {
            case LogLevel::TRACE:
                return "\033[37m";  // White
            case LogLevel::DEBUG:
                return "\033[36m";  // Cyan
            case LogLevel::INFO:
                return "\033[32m";  // Green
            case LogLevel::WARN:
                return "\033[33m";  // Yellow
            case LogLevel::ERROR:
                return "\033[31m";  // Red
            case LogLevel::FATAL:
                return "\033[35m";  // Magenta
            default:
                return "\033[0m";  // Reset
            }
        }

        /**
         * @brief Core logging function.
         *
         * Function that is used to define the macros below. The macros are what will actually be used to log.
         *
         * @param level Logging level
         * @param loc std::source_location used to write [File][line][function_name] in the log
         * @param args Text to log provided by the user
         */
        template<typename... Args>
        static void log(LogLevel level, const std::source_location& loc, Args&&... args)
        {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

            // Print Header: [Time] [Level] [File:Line]
            std::cout << "\033[90m["  // Dim gray for timestamp
                      << std::put_time(std::localtime(&time_t_now), "%H:%M:%S") << "." << std::setfill('0')
                      << std::setw(3) << ms.count() << "]\033[0m ";

            std::cout << levelColor(level) << "[" << levelToString(level) << "]\033[0m ";

            std::cout << "\033[90m[" << loc.file_name() << ":" << loc.line() << " " << loc.function_name()
                      << "]\033[0m ";

            // Print the actual message arguments using a fold expression
            ((std::cout << std::forward<Args>(args)), ...);

            std::cout << "\033[0m" << std::endl;  // Reset color and newline
        }
    };
}  // namespace SimpleLogger

// We use 'if constexpr' against the compile-time constant 'CurrentLogLevelCutoff'.
// If the condition is false, the compiler strips the branch.

#define LOG_TRACE(...)                                                                                                 \
    if constexpr (SimpleLogger::CurrentLogLevelCutoff <= SimpleLogger::LogLevel::TRACE)                                \
    SimpleLogger::Logger::log(SimpleLogger::LogLevel::TRACE, std::source_location::current(), __VA_ARGS__)

#define LOG_DEBUG(...)                                                                                                 \
    if constexpr (SimpleLogger::CurrentLogLevelCutoff <= SimpleLogger::LogLevel::DEBUG)                                \
    SimpleLogger::Logger::log(SimpleLogger::LogLevel::DEBUG, std::source_location::current(), __VA_ARGS__)

#define LOG_INFO(...)                                                                                                  \
    if constexpr (SimpleLogger::CurrentLogLevelCutoff <= SimpleLogger::LogLevel::INFO)                                 \
    SimpleLogger::Logger::log(SimpleLogger::LogLevel::INFO, std::source_location::current(), __VA_ARGS__)

#define LOG_WARN(...)                                                                                                  \
    if constexpr (SimpleLogger::CurrentLogLevelCutoff <= SimpleLogger::LogLevel::WARN)                                 \
    SimpleLogger::Logger::log(SimpleLogger::LogLevel::WARN, std::source_location::current(), __VA_ARGS__)

#define LOG_ERROR(...)                                                                                                 \
    if constexpr (SimpleLogger::CurrentLogLevelCutoff <= SimpleLogger::LogLevel::ERROR)                                \
    SimpleLogger::Logger::log(SimpleLogger::LogLevel::ERROR, std::source_location::current(), __VA_ARGS__)

#define LOG_FATAL(...)                                                                                                 \
    if constexpr (SimpleLogger::CurrentLogLevelCutoff <= SimpleLogger::LogLevel::FATAL)                                \
    SimpleLogger::Logger::log(SimpleLogger::LogLevel::FATAL, std::source_location::current(), __VA_ARGS__)

#endif  // _SIMPLE_LOGGER_HPP
