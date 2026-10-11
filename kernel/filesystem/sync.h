#pragma once

#include "filesystem.h"

void sync_fd(file *ifd, open_file_descriptors *ofile);

void report_fd_mod(file *ifd, open_file_descriptors *ofile, u64 time, range_t range);