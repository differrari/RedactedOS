#pragma once

#define FORMAT_IP(ipv4) ((ipv4 >> 24) & 0xFF), ((ipv4 >> 16) & 0xFF), ((ipv4 >> 8) & 0xFF), ((ipv4) & 0xFF)
#define IP_ENCODE(ip1,ip2,ip3,ip4) (((ipv4 << 24) & 0xFF) | ((ipv4 << 16) & 0xFF) | ((ipv4 << 8) & 0xFF) | ((ipv4) & 0xFF))