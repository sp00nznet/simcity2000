/*
 * stipend.c - a sample mod: the state pays the city $500 at the start of
 * every game month. A worked example of sc2k_mod.h; build it with the project
 * (it lands in build\mods\) or on its own as any 32-bit DLL.
 */
#include "sc2k_mod.h"

static const sc2k_api *api;
static int32_t last_month = -1;

static void tick(void) {
    int32_t month = api->days() / 25;
    if (last_month < 0 || month < last_month) { last_month = month; return; }   /* a city just loaded */
    if (month > last_month) {
        last_month = month;
        api->set_funds(api->funds() + 500);
        api->log("stipend: +$500");
    }
}

__declspec(dllexport) int sc2k_mod_register(const sc2k_api *a, sc2k_mod_info *info) {
    if (a->version != SC2K_MOD_API) return 0;
    api = a;
    info->name = "City stipend";
    info->description = "The state pays the city $500 at the start of every game month.";
    info->tick = tick;
    return 1;
}
