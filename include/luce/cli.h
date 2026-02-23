#pragma once

#include <cstdbool>

void cli_startup();
int cli_execute_command(int argc, char* argv[]);
int cli_execute_command_readonly(int argc, char* argv[], bool* denied_mutation);
