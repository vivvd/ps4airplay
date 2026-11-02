#pragma once
#define ORBIS_SYSMODULE_AV_PLAYER 0xa5
int sceSysmoduleLoadModule(int id);

#define ORBIS_SYSMODULE_INTERNAL_HTTP 0x8000000au
#define ORBIS_SYSMODULE_INTERNAL_SSL 0x8000000bu
uint32_t sceSysmoduleLoadModuleInternal(uint32_t id);
