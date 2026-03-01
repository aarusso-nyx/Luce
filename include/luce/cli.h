#pragma once

#include <cstdbool>
#include <cstddef>

void cli_startup();
int cli_execute_command(int argc, char* argv[]);
std::size_t tokenize_cli_line(char* line, char* argv[], std::size_t max_args);
bool cli_command_is_mutating(const char* command);
bool cli_command_is_readonly(const char* command);
