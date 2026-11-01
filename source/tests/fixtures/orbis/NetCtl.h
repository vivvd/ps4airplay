#pragma once
#define ORBIS_NET_CTL_INFO_IP_ADDRESS 14
typedef union { char ip_address[16]; } OrbisNetCtlInfo;
int sceNetCtlGetInfo(int, OrbisNetCtlInfo *);
