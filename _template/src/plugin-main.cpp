// Шаблон плагина OBS: простой аудиофильтр «Громкость».
// Замените логику в filter_audio и свойства в filter_properties на свои.

#include <obs-module.h>

#include <cmath>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

struct Filter {
	obs_source_t *context = nullptr;
	float gain = 1.0f;
};

const char *filter_name(void *)
{
	return obs_module_text("Template.FilterName");
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<Filter *>(data);
	f->gain = std::pow(10.0f, (float)obs_data_get_double(settings, "gain_db") / 20.0f);
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new Filter();
	f->context = source;
	filter_update(f, settings);
	return f;
}

void filter_destroy(void *data)
{
	delete static_cast<Filter *>(data);
}

void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "gain_db", 0.0);
}

obs_properties_t *filter_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_float_slider(props, "gain_db", obs_module_text("Template.Gain"), -30.0, 30.0, 0.5);
	obs_property_float_set_suffix(p, " dB");
	return props;
}

obs_audio_data *filter_audio(void *data, obs_audio_data *audio)
{
	auto *f = static_cast<Filter *>(data);
	const size_t channels = audio_output_get_channels(obs_get_audio());
	for (size_t c = 0; c < channels && c < MAX_AV_PLANES; c++) {
		auto *x = reinterpret_cast<float *>(audio->data[c]);
		if (!x)
			continue;
		for (uint32_t i = 0; i < audio->frames; i++)
			x[i] *= f->gain;
	}
	return audio;
}

} // namespace

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = PLUGIN_NAME "_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = filter_name;
	info.create = filter_create;
	info.destroy = filter_destroy;
	info.update = filter_update;
	info.get_defaults = filter_defaults;
	info.get_properties = filter_properties;
	info.filter_audio = filter_audio;
	obs_register_source(&info);

	blog(LOG_INFO, "[%s] loaded, version %s", PLUGIN_NAME, PLUGIN_VERSION);
	return true;
}
