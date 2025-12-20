#pragma once

#include "std/string_slice.h"

typedef void (*csv_handler)(string_slice value);

void read_csv(string_slice slice, csv_handler on_val);