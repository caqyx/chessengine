#pragma once

void engine_init();
// Handles one UCI command line synchronously (including `go`).
void uci_command(const char* line);
