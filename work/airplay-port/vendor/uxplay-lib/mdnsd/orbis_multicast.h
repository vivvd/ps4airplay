/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ORBIS_MULTICAST_H
#define ORBIS_MULTICAST_H
/* OpenOrbis 0.5.4 netinet/in.h retains Linux values for these options.
 * Orbis BSD socket ABI values: ps4dev/ps4sdk include/netinet/in.h.
 * Include after the platform socket headers. Host builds stay unchanged. */
#ifdef LAB_PS4
#undef IP_MULTICAST_IF
#undef IP_MULTICAST_TTL
#undef IP_MULTICAST_LOOP
#undef IP_ADD_MEMBERSHIP
#undef IP_DROP_MEMBERSHIP
#define IP_MULTICAST_IF 9
#define IP_MULTICAST_TTL 10
#define IP_MULTICAST_LOOP 11
#define IP_ADD_MEMBERSHIP 12
#define IP_DROP_MEMBERSHIP 13
#endif
#endif
