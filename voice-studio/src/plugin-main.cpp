// Точка входа плагина «Голосовая студия» (Voice Studio) для OBS Studio.

#include <obs-module.h>

#include "obs-live.hpp"
#include "update/updater.hpp"
#include "voice-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("voice-studio", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Voice Studio";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("VoiceStudio.Description");
}

bool obs_module_load(void)
{
	register_voice_filter();
	vsui::register_live_block();
	vsup::init_updater();
	blog(LOG_INFO, "[voice-studio] loaded, version %s", VOICE_STUDIO_VERSION);
	return true;
}

void obs_module_unload(void)
{
	vsup::shutdown_updater();
	vsui::unregister_live_block();
}
