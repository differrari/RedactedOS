#pragma once

#include "types.h"

/*
    This compression algorithm is (very experimental). 
    It can only encode byte values, and is not optimized in the slightest.
*/

void huffman_encode(sizedptr input, sizedptr output);
