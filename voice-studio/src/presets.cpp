#include "presets.hpp"

#include <cstring>

#include "settings.hpp"

namespace vs {

namespace {

PresetValues natural()
{
	return PresetValues();
}

PresetValues podcast()
{
	PresetValues p;
	p.denoise_amount = 90;
	p.gate_range = -30;
	p.hpf_freq = 90;
	p.low_gain = 2.5;
	p.mud_gain = -3;
	p.nasal_gain = -1.5;
	p.pres_gain = 3;
	p.air_gain = 2.5;
	p.deess_amount = 45;
	p.comp_amount = 60;
	p.comp_attack = 8;
	p.comp_release = 100;
	p.warmth_enabled = true;
	p.warmth_amount = 30;
	return p;
}

PresetValues stream()
{
	PresetValues p;
	p.denoise_amount = 100;
	p.gate_mode = 2;
	p.gate_range = -35;
	p.hpf_freq = 100;
	p.mud_gain = -3;
	p.nasal_gain = -1;
	p.pres_gain = 4;
	p.pres_freq = 3500;
	p.air_gain = 2;
	p.deess_amount = 40;
	p.comp_amount = 50;
	return p;
}

PresetValues deep()
{
	PresetValues p;
	p.gate_range = -25;
	p.hpf_freq = 70;
	p.low_gain = 4;
	p.low_freq = 110;
	p.mud_gain = -1.5;
	p.mud_freq = 350;
	p.nasal_gain = -2;
	p.pres_gain = 1;
	p.air_gain = 0.5;
	p.deess_amount = 40;
	p.comp_amount = 50;
	p.warmth_enabled = true;
	p.warmth_amount = 45;
	return p;
}

PresetValues bright()
{
	PresetValues p;
	p.low_gain = -1;
	p.mud_gain = -3;
	p.nasal_gain = -1;
	p.pres_gain = 4.5;
	p.pres_freq = 3500;
	p.air_gain = 4;
	p.deess_amount = 50;
	p.comp_amount = 40;
	return p;
}

PresetValues noisy()
{
	PresetValues p;
	p.denoise_amount = 100;
	p.gate_mode = 2;
	p.gate_range = -45;
	p.gate_hold = 250;
	p.hpf_freq = 120;
	p.low_gain = -1;
	p.mud_gain = -2;
	p.pres_gain = 3;
	p.air_gain = 0;
	p.deess_amount = 30;
	p.comp_amount = 45;
	return p;
}

PresetValues minimal()
{
	PresetValues p;
	p.denoise_amount = 100;
	p.gate_enabled = false;
	p.mud_gain = 0;
	p.pres_gain = 0;
	p.air_gain = 0;
	p.deess_enabled = false;
	p.comp_enabled = false;
	return p;
}

PresetValues phone()
{
	PresetValues p;
	p.denoise_amount = 100;
	p.gate_mode = 2;
	p.gate_range = -40;
	p.hpf_freq = 400;
	p.mud_gain = 0;
	p.nasal_gain = 5;
	p.nasal_freq = 1200;
	p.nasal_q = 1.0;
	p.pres_gain = 3;
	p.pres_freq = 2500;
	p.air_gain = 0;
	p.lpf_freq = 3400;
	p.deess_enabled = false;
	p.comp_amount = 70;
	p.warmth_enabled = true;
	p.warmth_amount = 75;
	return p;
}

PresetValues monster()
{
	PresetValues p;
	p.denoise_amount = 100;
	p.gate_range = -30;
	p.low_gain = 3;
	p.comp_amount = 50;
	p.warmth_enabled = true;
	p.warmth_amount = 40;
	p.pitch_enabled = true;
	p.pitch_semitones = -5;
	p.formant_shift = -3;
	return p;
}

PresetValues cartoon()
{
	PresetValues p;
	p.denoise_amount = 100;
	p.gate_range = -30;
	p.low_gain = -2;
	p.air_gain = 2;
	p.comp_amount = 45;
	p.pitch_enabled = true;
	p.pitch_semitones = 7;
	p.formant_shift = 4;
	return p;
}

struct PresetEntry {
	PresetInfo info;
	PresetValues (*make)();
};

const std::vector<PresetEntry> &entries()
{
	static const std::vector<PresetEntry> list = {
		{{"natural", "VoiceStudio.Preset.Natural", "VoiceStudio.Preset.Natural.Tip"}, natural},
		{{"podcast", "VoiceStudio.Preset.Podcast", "VoiceStudio.Preset.Podcast.Tip"}, podcast},
		{{"stream", "VoiceStudio.Preset.Stream", "VoiceStudio.Preset.Stream.Tip"}, stream},
		{{"deep", "VoiceStudio.Preset.Deep", "VoiceStudio.Preset.Deep.Tip"}, deep},
		{{"bright", "VoiceStudio.Preset.Bright", "VoiceStudio.Preset.Bright.Tip"}, bright},
		{{"noisy", "VoiceStudio.Preset.Noisy", "VoiceStudio.Preset.Noisy.Tip"}, noisy},
		{{"minimal", "VoiceStudio.Preset.Minimal", "VoiceStudio.Preset.Minimal.Tip"}, minimal},
		{{"phone", "VoiceStudio.Preset.Phone", "VoiceStudio.Preset.Phone.Tip"}, phone},
		{{"monster", "VoiceStudio.Preset.Monster", "VoiceStudio.Preset.Monster.Tip"}, monster},
		{{"cartoon", "VoiceStudio.Preset.Cartoon", "VoiceStudio.Preset.Cartoon.Tip"}, cartoon},
	};
	return list;
}

} // namespace

const std::vector<PresetInfo> &preset_list()
{
	static const std::vector<PresetInfo> list = [] {
		std::vector<PresetInfo> l;
		for (const PresetEntry &e : entries())
			l.push_back(e.info);
		l.push_back({kPresetCustom, "VoiceStudio.Preset.Custom", "VoiceStudio.Preset.Custom.Tip"});
		return l;
	}();
	return list;
}

bool find_preset(const char *id, PresetValues &out)
{
	if (!id)
		return false;
	for (const PresetEntry &e : entries()) {
		if (std::strcmp(e.info.id, id) == 0) {
			out = e.make();
			return true;
		}
	}
	return false;
}

void write_preset(obs_data_t *s, const PresetValues &v, bool as_default)
{
	auto set_bool = [&](const char *k, bool x) {
		as_default ? obs_data_set_default_bool(s, k, x) : obs_data_set_bool(s, k, x);
	};
	auto set_int = [&](const char *k, int x) {
		as_default ? obs_data_set_default_int(s, k, x) : obs_data_set_int(s, k, x);
	};
	auto set_dbl = [&](const char *k, double x) {
		as_default ? obs_data_set_default_double(s, k, x) : obs_data_set_double(s, k, x);
	};
	using namespace key;

	set_bool(kDenoiseEnabled, v.denoise_enabled);
	set_dbl(kDenoiseAmount, v.denoise_amount);

	set_bool(kGateEnabled, v.gate_enabled);
	set_int(kGateMode, v.gate_mode);
	set_dbl(kGateRange, v.gate_range);
	set_dbl(kGateAttack, v.gate_attack);
	set_dbl(kGateHold, v.gate_hold);
	set_dbl(kGateRelease, v.gate_release);
	set_dbl(kGateVadSens, v.gate_vad_sens);

	set_bool(kEqEnabled, v.eq_enabled);
	set_bool(kHpfEnabled, v.hpf_enabled);
	set_dbl(kHpfFreq, v.hpf_freq);
	set_int(kHpfSlope, v.hpf_slope);
	set_dbl(kLowGain, v.low_gain);
	set_dbl(kLowFreq, v.low_freq);
	set_dbl(kMudGain, v.mud_gain);
	set_dbl(kMudFreq, v.mud_freq);
	set_dbl(kMudQ, v.mud_q);
	set_dbl(kNasalGain, v.nasal_gain);
	set_dbl(kNasalFreq, v.nasal_freq);
	set_dbl(kNasalQ, v.nasal_q);
	set_dbl(kPresGain, v.pres_gain);
	set_dbl(kPresFreq, v.pres_freq);
	set_dbl(kPresQ, v.pres_q);
	set_dbl(kAirGain, v.air_gain);
	set_dbl(kAirFreq, v.air_freq);
	set_dbl(kLpfFreq, v.lpf_freq);

	set_bool(kDeessEnabled, v.deess_enabled);
	set_dbl(kDeessAmount, v.deess_amount);
	set_dbl(kDeessFreq, v.deess_freq);

	set_bool(kCompEnabled, v.comp_enabled);
	set_dbl(kCompAmount, v.comp_amount);
	set_dbl(kCompAttack, v.comp_attack);
	set_dbl(kCompRelease, v.comp_release);

	set_bool(kWarmthEnabled, v.warmth_enabled);
	set_dbl(kWarmthAmount, v.warmth_amount);

	set_bool(kPitchEnabled, v.pitch_enabled);
	set_dbl(kPitchSemitones, v.pitch_semitones);
	set_dbl(kFormantShift, v.formant_shift);
	set_bool(kFormantPreserve, v.formant_preserve);
}

bool apply_preset(obs_data_t *settings, const char *id)
{
	PresetValues v;
	if (!find_preset(id, v))
		return false;
	write_preset(settings, v, false);
	return true;
}

} // namespace vs
