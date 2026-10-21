// Аудиофильтр OBS «Голосовая студия»: настройки, интерфейс и связь с DSP-цепочкой.
//
// Потоки:
//   * интерфейс (UI-поток OBS) меняет настройки -> filter_update кладёт их в pending;
//   * аудиопоток в filter_audio забирает pending и обрабатывает звук;
//   * «Умная настройка» идёт в аудиопотоке, а результат применяется задачей в UI-потоке.

#include "voice-filter.hpp"

#include <obs-module.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "presets.hpp"
#include "settings.hpp"
#include "voice-chain.hpp"

using namespace vs::key;

#define TXT(k) obs_module_text("VoiceStudio." k)

namespace {

constexpr float kCalibrationSeconds = 10.0f;
constexpr int kRequestStart = 1;
constexpr int kRequestCancel = 2;

template<typename... Args> std::string format(const char *fmt, Args... args)
{
	char buf[2048];
	std::snprintf(buf, sizeof(buf), fmt, args...);
	return buf;
}

struct VoiceFilter {
	obs_source_t *context = nullptr;
	vs::VoiceChain chain; // только аудиопоток

	std::mutex params_mutex;
	vs::ChainParams pending;
	std::atomic<bool> params_dirty{false};

	// Калибровка, статус, индикаторы — общие с красивой панелью.
	std::shared_ptr<vs::SharedState> shared = std::make_shared<vs::SharedState>();
	int last_progress = -1; // аудиопоток
	std::vector<float> mono; // аудиопоток: выход для спектра

	uint64_t last_timestamp = 0;

	void set_status(const std::string &text, obs_text_info_type type) { shared->set_status(text, (int)type); }
};

// ---------------------------------------------------------------------------
// Реестр экземпляров — по нему панель находит фильтры

struct RegistryEntry {
	obs_weak_source_t *weak;
	std::shared_ptr<vs::SharedState> shared;
};

std::mutex g_registry_mutex;
std::vector<RegistryEntry> g_registry;
vs::LiveMarkerFn g_live_marker = nullptr;

void registry_add(obs_source_t *filter, const std::shared_ptr<vs::SharedState> &shared)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	g_registry.push_back({obs_source_get_weak_source(filter), shared});
}

void registry_remove(const std::shared_ptr<vs::SharedState> &shared)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	for (auto it = g_registry.begin(); it != g_registry.end(); ++it) {
		if (it->shared == shared) {
			obs_weak_source_release(it->weak);
			g_registry.erase(it);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// Настройки -> параметры цепочки

float get_f(obs_data_t *s, const char *k)
{
	return (float)obs_data_get_double(s, k);
}

vs::ChainParams read_params(obs_data_t *s)
{
	vs::ChainParams p;
	p.bypass = obs_data_get_bool(s, kBypass);
	p.input_gain_db = get_f(s, kInputGain);

	p.eq_on = obs_data_get_bool(s, kEqEnabled);
	p.hpf_on = obs_data_get_bool(s, kHpfEnabled);
	p.hpf_freq = get_f(s, kHpfFreq);
	p.hpf_slope = (int)obs_data_get_int(s, kHpfSlope);
	p.low_gain = get_f(s, kLowGain);
	p.low_freq = get_f(s, kLowFreq);
	p.mud_gain = get_f(s, kMudGain);
	p.mud_freq = get_f(s, kMudFreq);
	p.mud_q = get_f(s, kMudQ);
	p.nasal_gain = get_f(s, kNasalGain);
	p.nasal_freq = get_f(s, kNasalFreq);
	p.nasal_q = get_f(s, kNasalQ);
	p.pres_gain = get_f(s, kPresGain);
	p.pres_freq = get_f(s, kPresFreq);
	p.pres_q = get_f(s, kPresQ);
	p.air_gain = get_f(s, kAirGain);
	p.air_freq = get_f(s, kAirFreq);
	p.lpf_freq = get_f(s, kLpfFreq);

	p.denoise_on = obs_data_get_bool(s, kDenoiseEnabled);
	p.denoise_amount = get_f(s, kDenoiseAmount);

	p.gate_on = obs_data_get_bool(s, kGateEnabled);
	const long long gm = obs_data_get_int(s, kGateMode);
	p.gate.mode = gm == 0 ? vs::GateMode::Level : gm == 2 ? vs::GateMode::Both : vs::GateMode::Voice;
	p.gate.threshold_db = get_f(s, kGateThreshold);
	p.gate.range_db = get_f(s, kGateRange);
	p.gate.attack_ms = get_f(s, kGateAttack);
	p.gate.hold_ms = get_f(s, kGateHold);
	p.gate.release_ms = get_f(s, kGateRelease);
	// Чувствительность 0..100% -> порог вероятности речи 0.9..0.1
	p.gate.vad_threshold = 0.9f - 0.8f * vs::clampf(get_f(s, kGateVadSens), 0.0f, 100.0f) / 100.0f;

	p.deess_on = obs_data_get_bool(s, kDeessEnabled);
	p.deess.amount = get_f(s, kDeessAmount);
	p.deess.freq = get_f(s, kDeessFreq);

	p.comp_on = obs_data_get_bool(s, kCompEnabled);
	p.comp.amount = get_f(s, kCompAmount);
	p.comp.attack_ms = get_f(s, kCompAttack);
	p.comp.release_ms = get_f(s, kCompRelease);

	p.warmth_on = obs_data_get_bool(s, kWarmthEnabled);
	p.warmth_amount = get_f(s, kWarmthAmount);

	p.pitch_on = obs_data_get_bool(s, kPitchEnabled);
	p.pitch_semitones = get_f(s, kPitchSemitones);
	p.formant_semitones = get_f(s, kFormantShift);
	p.formant_preserve = obs_data_get_bool(s, kFormantPreserve);
	p.pitch_quality = obs_data_get_int(s, kPitchQuality) ? 1 : 0;

	p.output_gain_db = get_f(s, kOutputGain);
	p.limiter_on = obs_data_get_bool(s, kLimiterEnabled);
	p.limiter_ceiling_db = get_f(s, kLimiterCeiling);
	return p;
}

// ---------------------------------------------------------------------------
// Базовые колбэки OBS

const char *filter_name(void *)
{
	return TXT("FilterName");
}

void filter_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<VoiceFilter *>(data);
	const vs::ChainParams p = read_params(settings);
	{
		std::lock_guard<std::mutex> lock(f->params_mutex);
		f->pending = p;
	}
	f->params_dirty = true;
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new VoiceFilter();
	f->context = source;
	filter_update(f, settings);
	registry_add(source, f->shared);
	return f;
}

void filter_destroy(void *data)
{
	auto *f = static_cast<VoiceFilter *>(data);
	registry_remove(f->shared);
	delete f;
}

void filter_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, kMode, 0);
	obs_data_set_default_string(s, kPreset, vs::kPresetDefault);
	obs_data_set_default_string(s, kAppliedPreset, vs::kPresetDefault);
	obs_data_set_default_bool(s, kBypass, false);
	obs_data_set_default_double(s, kInputGain, 0.0);
	obs_data_set_default_double(s, kGateThreshold, -45.0);
	obs_data_set_default_int(s, kPitchQuality, 1);
	obs_data_set_default_double(s, kOutputGain, 0.0);
	obs_data_set_default_bool(s, kLimiterEnabled, true);
	obs_data_set_default_double(s, kLimiterCeiling, -1.0);

	vs::PresetValues v;
	vs::find_preset(vs::kPresetDefault, v);
	vs::write_preset(s, v, true);
}

// ---------------------------------------------------------------------------
// «Умная настройка»: результат применяется в UI-потоке

double round_to(double v, double step)
{
	return std::round(v / step) * step;
}

void apply_calibration(VoiceFilter *f, obs_source_t *source, const vs::CalibrationResult &r)
{
	if (r.status == vs::CalibrationResult::NoVoice) {
		f->set_status(TXT("Calib.NoVoice"), OBS_TEXT_INFO_WARNING);
		return;
	}

	obs_data_t *s = obs_source_get_settings(source);

	// Поправки считаются от выбранного стиля, поэтому повторный запуск не «накручивает» их.
	vs::PresetValues base;
	if (!vs::find_preset(obs_data_get_string(s, kAppliedPreset), base))
		vs::find_preset(vs::kPresetDefault, base);

	obs_data_set_double(s, kInputGain, r.input_gain_db);
	obs_data_set_double(s, kGateThreshold, r.gate_threshold_db);
	obs_data_set_double(s, kDenoiseAmount, std::max(base.denoise_amount, (double)r.denoise_amount));
	obs_data_set_double(s, kCompAmount, round_to((base.comp_amount + r.comp_amount) * 0.5, 5.0));
	obs_data_set_double(s, kDeessAmount, std::max(base.deess_amount, (double)r.deess_amount));
	obs_data_set_double(s, kHpfFreq, std::max(base.hpf_freq, (double)r.hpf_freq));
	if (r.eq_valid) {
		auto eq = [](double v) { return std::min(12.0, std::max(-12.0, v)); };
		obs_data_set_double(s, kLowGain, eq(base.low_gain + r.eq_low));
		obs_data_set_double(s, kMudGain, eq(base.mud_gain + r.eq_mud));
		obs_data_set_double(s, kNasalGain, eq(base.nasal_gain + r.eq_nasal));
		obs_data_set_double(s, kPresGain, eq(base.pres_gain + r.eq_pres));
		obs_data_set_double(s, kAirGain, eq(base.air_gain + r.eq_air));
	}
	obs_source_update(source, nullptr);
	obs_data_release(s);

	std::string text = format(TXT("Calib.Done"), r.voice_db, r.noise_db, r.snr_db, r.input_gain_db);
	obs_text_info_type type = OBS_TEXT_INFO_NORMAL;
	if (r.status == vs::CalibrationResult::TooQuiet) {
		text += "\n";
		text += TXT("Calib.TooQuiet");
		type = OBS_TEXT_INFO_WARNING;
	}
	if (r.clipping) {
		text += "\n";
		text += TXT("Calib.Clipping");
		type = OBS_TEXT_INFO_WARNING;
	}
	if (r.snr_db < 20.0f) {
		text += "\n";
		text += TXT("Calib.Noisy");
	} else if (r.snr_db > 50.0f) {
		text += "\n";
		text += TXT("Calib.QuietRoom");
	}
	f->set_status(text, type);
}

struct UiJob {
	obs_source_t *source; // держим ссылку, чтобы фильтр не удалили до выполнения задачи
	VoiceFilter *filter;
	bool finished;
	int seconds_left;
	vs::CalibrationResult result;
};

void run_ui_job(void *param)
{
	std::unique_ptr<UiJob> job(static_cast<UiJob *>(param));
	VoiceFilter *f = job->filter;
	if (job->finished) {
		f->shared->calibrating = false;
		apply_calibration(f, job->source, job->result);
		f->shared->calib_generation++;
	} else if (f->shared->calibrating) {
		f->set_status(format(TXT("Calib.Listening"), job->seconds_left), OBS_TEXT_INFO_NORMAL);
	}
	obs_source_update_properties(job->source);
	obs_source_release(job->source);
}

void post_ui_job(VoiceFilter *f, bool finished, int seconds_left)
{
	obs_source_t *ref = obs_source_get_ref(f->context);
	if (!ref)
		return;
	auto *job = new UiJob{ref, f, finished, seconds_left,
			      finished ? f->chain.calibration_result() : vs::CalibrationResult()};
	obs_queue_task(OBS_TASK_UI, run_ui_job, job, false);
}

// ---------------------------------------------------------------------------
// Обработка звука (аудиопоток)

obs_audio_data *filter_audio(void *data, obs_audio_data *audio)
{
	auto *f = static_cast<VoiceFilter *>(data);
	if (!audio || audio->frames == 0)
		return audio;

	vs::DenormalGuard denormals;

	audio_t *out = obs_get_audio();
	const int rate = (int)audio_output_get_sample_rate(out);
	const int channels = (int)std::min<size_t>(audio_output_get_channels(out), vs::kMaxChannels);
	if (!f->chain.configured() || rate != f->chain.sample_rate() || channels != f->chain.channels())
		f->chain.configure(rate, channels);

	if (f->params_dirty.load()) {
		std::unique_lock<std::mutex> lock(f->params_mutex, std::try_to_lock);
		if (lock.owns_lock()) {
			f->chain.set_params(f->pending);
			f->params_dirty = false;
		}
	}

	// Большой скачок времени (переподключили микрофон и т.п.) — начинаем с чистого листа.
	if (f->last_timestamp &&
	    std::llabs((long long)audio->timestamp - (long long)f->last_timestamp) > 1000000000LL)
		f->chain.reset();
	f->last_timestamp = audio->timestamp;

	const int request = f->shared->calib_request.exchange(0);
	if (request == kRequestStart) {
		f->chain.start_calibration(kCalibrationSeconds);
		f->last_progress = (int)kCalibrationSeconds;
	} else if (request == kRequestCancel) {
		f->chain.cancel_calibration();
	}

	float *planes[vs::kMaxChannels];
	int n = 0;
	for (int i = 0; i < channels; i++) {
		if (!audio->data[i])
			break;
		planes[n++] = reinterpret_cast<float *>(audio->data[i]);
	}

	obs_source_t *parent = obs_filter_get_parent(f->context);
	const bool mono_hint = parent && obs_source_get_speaker_layout(parent) == SPEAKERS_MONO;
	f->chain.process(planes, n, (int)audio->frames, mono_hint);

	// Индикаторы и спектр для красивой панели.
	vs::SharedState &sh = *f->shared;
	const vs::ChainMeters &m = f->chain.meters();
	vs::SharedState::raise_max(sh.in_peak, m.in_peak_db);
	vs::SharedState::raise_max(sh.out_peak, m.out_peak_db);
	sh.in_rms.store(m.in_rms_db, std::memory_order_relaxed);
	sh.out_rms.store(m.out_rms_db, std::memory_order_relaxed);
	sh.comp_gr.store(m.comp_gr_db, std::memory_order_relaxed);
	sh.gate_db.store(m.gate_db, std::memory_order_relaxed);
	sh.vad.store(m.vad, std::memory_order_relaxed);
	sh.sample_rate.store(rate, std::memory_order_relaxed);
	sh.calib_seconds_left.store(f->chain.calibrating() ? f->chain.calibration_seconds_left() : 0.0f,
				    std::memory_order_relaxed);
	if (sh.ui_listeners.load(std::memory_order_relaxed) > 0 && n > 0) {
		const int frames = (int)audio->frames;
		if ((int)f->mono.size() < frames)
			f->mono.resize((size_t)frames);
		for (int i = 0; i < frames; i++) {
			float s = 0.0f;
			for (int c = 0; c < n; c++)
				s += planes[c][i];
			f->mono[(size_t)i] = s / (float)n;
		}
		sh.push_ring(f->mono.data(), frames);
	}

	if (f->chain.take_calibration_finished()) {
		post_ui_job(f, true, 0);
	} else if (f->chain.calibrating()) {
		const int left = (int)std::ceil(f->chain.calibration_seconds_left());
		if (left != f->last_progress && left % 2 == 0) {
			f->last_progress = left;
			post_ui_job(f, false, left);
		}
	}
	return audio;
}

// ---------------------------------------------------------------------------
// Интерфейс

const char *kExpertOnly[] = {
	kGateAttack, kGateHold, kGateRelease, kGateVadSens, kHpfSlope,     kLowFreq,         kMudFreq,
	kMudQ,       kNasalFreq, kNasalQ,     kPresFreq,    kPresQ,        kAirFreq,         kLpfFreq,
	kDeessFreq,  kCompAttack, kCompRelease, kFormantPreserve, kPitchQuality, kLimiterCeiling,
};

void set_visible(obs_properties_t *props, const char *name, bool visible)
{
	if (obs_property_t *p = obs_properties_get(props, name))
		obs_property_set_visible(p, visible);
}

bool visibility_modified(obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const bool expert = obs_data_get_int(s, kMode) == 1;
	for (const char *name : kExpertOnly)
		set_visible(props, name, expert);
	set_visible(props, "guide", !expert);

	const long long gate_mode = obs_data_get_int(s, kGateMode);
	set_visible(props, kGateThreshold, gate_mode != 1);
	set_visible(props, kGateVadSens, expert && gate_mode != 0);
	return true;
}

void update_preset_info(obs_properties_t *props, const char *preset)
{
	obs_property_t *info = obs_properties_get(props, "preset_info");
	if (!info)
		return;
	for (const vs::PresetInfo &pi : vs::preset_list()) {
		if (std::strcmp(pi.id, preset) == 0) {
			obs_property_set_description(info, obs_module_text(pi.tip_key));
			return;
		}
	}
	obs_property_set_description(info, "");
}

bool preset_modified(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *s)
{
	const std::string preset = obs_data_get_string(s, kPreset);
	update_preset_info(props, preset.c_str());

	// Этот колбэк вызывается и при открытии окна — применяем стиль, только если его сменили.
	const std::string applied = obs_data_get_string(s, kAppliedPreset);
	if (preset.empty() || preset == applied)
		return true;

	obs_data_set_string(s, kAppliedPreset, preset.c_str());
	if (vs::apply_preset(s, preset.c_str()) && priv)
		filter_update(priv, s); // звук меняется сразу, не дожидаясь отложенного обновления
	return true;
}

void fill_status(obs_property_t *p, VoiceFilter *f)
{
	int type = OBS_TEXT_INFO_NORMAL;
	std::string text = f ? f->shared->get_status(type) : std::string();
	if (text.empty())
		text = TXT("Calib.Hint");
	obs_property_set_description(p, text.c_str());
	obs_property_text_set_info_type(p, (obs_text_info_type)type);
}

bool calibrate_clicked(obs_properties_t *props, obs_property_t *button, void *data)
{
	auto *f = static_cast<VoiceFilter *>(data);
	if (!f)
		return false;
	vs::toggle_calibration(*f->shared);
	obs_property_set_description(button, f->shared->calibrating ? TXT("Calib.Stop") : TXT("Calib.Start"));
	if (obs_property_t *st = obs_properties_get(props, "calib_status"))
		fill_status(st, f);
	return true;
}


bool is_monitoring(VoiceFilter *f)
{
	obs_source_t *parent = f ? obs_filter_get_parent(f->context) : nullptr;
	return parent && obs_source_get_monitoring_type(parent) != OBS_MONITORING_TYPE_NONE;
}

bool monitor_clicked(obs_properties_t *, obs_property_t *button, void *data)
{
	auto *f = static_cast<VoiceFilter *>(data);
	obs_source_t *parent = f ? obs_filter_get_parent(f->context) : nullptr;
	if (!parent)
		return false;
	const bool on = is_monitoring(f);
	obs_source_set_monitoring_type(parent, on ? OBS_MONITORING_TYPE_NONE : OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	obs_property_set_description(button, on ? TXT("Monitor.On") : TXT("Monitor.Off"));
	return true;
}

std::string monitor_info()
{
	const char *name = nullptr;
	const char *id = nullptr;
	obs_get_audio_monitoring_device(&name, &id);
	const std::string device = (name && *name) ? name : TXT("Monitor.DefaultDevice");
	return format(TXT("Monitor.Info"), device.c_str());
}

obs_property_t *add_slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			   double step, const char *suffix, const char *tip = nullptr)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, text, min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	if (tip)
		obs_property_set_long_description(p, tip);
	return p;
}

obs_property_t *add_info(obs_properties_t *props, const char *name, const char *text)
{
	obs_property_t *p = obs_properties_add_text(props, name, text, OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	return p;
}

obs_properties_t *filter_properties(void *data)
{
	auto *f = static_cast<VoiceFilter *>(data);
	const char *dB = TXT("Unit.dB");
	const char *Hz = TXT("Unit.Hz");
	const char *ms = TXT("Unit.ms");
	const char *pct = TXT("Unit.Percent");
	const char *st = TXT("Unit.Semitones");

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	// ---- Живой блок: уровни и кривая эквалайзера (встаёт на место этой невидимой строки)
	if (g_live_marker && f)
		add_info(props, "vs_live", g_live_marker(f->context).c_str());

	// ---- Режим и стиль
	p = obs_properties_add_list(props, kMode, TXT("Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TXT("Mode.Simple"), 0);
	obs_property_list_add_int(p, TXT("Mode.Expert"), 1);
	obs_property_set_long_description(p, TXT("Mode.Tip"));
	obs_property_set_modified_callback(p, visibility_modified);

	add_info(props, "guide", TXT("Guide"));

	p = obs_properties_add_list(props, kPreset, TXT("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const vs::PresetInfo &pi : vs::preset_list())
		obs_property_list_add_string(p, obs_module_text(pi.name_key), pi.id);
	obs_property_set_long_description(p, TXT("Preset.Tip"));
	obs_property_set_modified_callback2(p, preset_modified, f);
	add_info(props, "preset_info", "");

	// ---- Умная настройка
	p = obs_properties_add_button2(props, "calibrate",
				       (f && f->shared->calibrating) ? TXT("Calib.Stop") : TXT("Calib.Start"),
				       calibrate_clicked, f);
	obs_property_set_long_description(p, TXT("Calib.Tip"));
	fill_status(add_info(props, "calib_status", ""), f);

	// ---- Прослушивание и сравнение
	p = obs_properties_add_button2(props, "monitor", is_monitoring(f) ? TXT("Monitor.Off") : TXT("Monitor.On"),
				       monitor_clicked, f);
	obs_property_set_long_description(p, TXT("Monitor.Tip"));
	add_info(props, "monitor_info", monitor_info().c_str());

	p = obs_properties_add_bool(props, kBypass, TXT("Bypass"));
	obs_property_set_long_description(p, TXT("Bypass.Tip"));

	add_slider(props, kInputGain, TXT("InputGain"), -15, 30, 0.5, dB, TXT("InputGain.Tip"));

	// ---- ИИ-шумоподавление
	obs_properties_t *g = obs_properties_create();
	add_slider(g, kDenoiseAmount, TXT("Denoise.Amount"), 0, 100, 1, pct, TXT("Denoise.Amount.Tip"));
	p = obs_properties_add_group(props, kDenoiseEnabled, TXT("Denoise"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(p, TXT("Denoise.Tip"));

	// ---- Шумовой гейт
	g = obs_properties_create();
	p = obs_properties_add_list(g, kGateMode, TXT("Gate.Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TXT("Gate.Mode.Voice"), 1);
	obs_property_list_add_int(p, TXT("Gate.Mode.Level"), 0);
	obs_property_list_add_int(p, TXT("Gate.Mode.Both"), 2);
	obs_property_set_long_description(p, TXT("Gate.Mode.Tip"));
	obs_property_set_modified_callback(p, visibility_modified);
	add_slider(g, kGateThreshold, TXT("Gate.Threshold"), -80, -10, 1, dB, TXT("Gate.Threshold.Tip"));
	add_slider(g, kGateRange, TXT("Gate.Range"), -60, 0, 1, dB, TXT("Gate.Range.Tip"));
	add_slider(g, kGateVadSens, TXT("Gate.VadSens"), 0, 100, 1, pct, TXT("Gate.VadSens.Tip"));
	add_slider(g, kGateAttack, TXT("Gate.Attack"), 0.5, 50, 0.5, ms, TXT("Gate.Attack.Tip"));
	add_slider(g, kGateHold, TXT("Gate.Hold"), 0, 1000, 10, ms, TXT("Gate.Hold.Tip"));
	add_slider(g, kGateRelease, TXT("Gate.Release"), 10, 1000, 10, ms, TXT("Gate.Release.Tip"));
	p = obs_properties_add_group(props, kGateEnabled, TXT("Gate"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(p, TXT("Gate.Tip"));

	// ---- Эквалайзер
	g = obs_properties_create();
	p = obs_properties_add_bool(g, kHpfEnabled, TXT("Hpf"));
	obs_property_set_long_description(p, TXT("Hpf.Tip"));
	add_slider(g, kHpfFreq, TXT("Hpf.Freq"), 20, 500, 5, Hz, TXT("Hpf.Freq.Tip"));
	p = obs_properties_add_list(g, kHpfSlope, TXT("Hpf.Slope"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TXT("Hpf.Slope12"), 12);
	obs_property_list_add_int(p, TXT("Hpf.Slope24"), 24);
	add_slider(g, kLowGain, TXT("Eq.Low"), -12, 12, 0.5, dB, TXT("Eq.Low.Tip"));
	add_slider(g, kLowFreq, TXT("Eq.LowFreq"), 40, 400, 5, Hz);
	add_slider(g, kMudGain, TXT("Eq.Mud"), -12, 12, 0.5, dB, TXT("Eq.Mud.Tip"));
	add_slider(g, kMudFreq, TXT("Eq.MudFreq"), 150, 800, 5, Hz);
	add_slider(g, kMudQ, TXT("Eq.MudQ"), 0.3, 4, 0.05, nullptr, TXT("Eq.Q.Tip"));
	add_slider(g, kNasalGain, TXT("Eq.Nasal"), -12, 12, 0.5, dB, TXT("Eq.Nasal.Tip"));
	add_slider(g, kNasalFreq, TXT("Eq.NasalFreq"), 500, 2500, 10, Hz);
	add_slider(g, kNasalQ, TXT("Eq.NasalQ"), 0.3, 4, 0.05, nullptr, TXT("Eq.Q.Tip"));
	add_slider(g, kPresGain, TXT("Eq.Pres"), -12, 12, 0.5, dB, TXT("Eq.Pres.Tip"));
	add_slider(g, kPresFreq, TXT("Eq.PresFreq"), 1500, 8000, 50, Hz);
	add_slider(g, kPresQ, TXT("Eq.PresQ"), 0.3, 4, 0.05, nullptr, TXT("Eq.Q.Tip"));
	add_slider(g, kAirGain, TXT("Eq.Air"), -12, 12, 0.5, dB, TXT("Eq.Air.Tip"));
	add_slider(g, kAirFreq, TXT("Eq.AirFreq"), 5000, 16000, 100, Hz);
	add_slider(g, kLpfFreq, TXT("Eq.Lpf"), 2000, 20000, 100, Hz, TXT("Eq.Lpf.Tip"));
	p = obs_properties_add_group(props, kEqEnabled, TXT("Eq"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(p, TXT("Eq.Tip"));

	// ---- Де-эссер
	g = obs_properties_create();
	add_slider(g, kDeessAmount, TXT("Deess.Amount"), 0, 100, 1, pct, TXT("Deess.Amount.Tip"));
	add_slider(g, kDeessFreq, TXT("Deess.Freq"), 3000, 12000, 100, Hz, TXT("Deess.Freq.Tip"));
	p = obs_properties_add_group(props, kDeessEnabled, TXT("Deess"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(p, TXT("Deess.Tip"));

	// ---- Компрессор
	g = obs_properties_create();
	add_slider(g, kCompAmount, TXT("Comp.Amount"), 0, 100, 1, pct, TXT("Comp.Amount.Tip"));
	add_slider(g, kCompAttack, TXT("Comp.Attack"), 0.5, 100, 0.5, ms, TXT("Comp.Attack.Tip"));
	add_slider(g, kCompRelease, TXT("Comp.Release"), 20, 1000, 10, ms, TXT("Comp.Release.Tip"));
	p = obs_properties_add_group(props, kCompEnabled, TXT("Comp"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(p, TXT("Comp.Tip"));

	// ---- Тепло
	g = obs_properties_create();
	add_slider(g, kWarmthAmount, TXT("Warmth.Amount"), 0, 100, 1, pct, TXT("Warmth.Amount.Tip"));
	p = obs_properties_add_group(props, kWarmthEnabled, TXT("Warmth"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(p, TXT("Warmth.Tip"));

	// ---- Изменение голоса
	g = obs_properties_create();
	add_slider(g, kPitchSemitones, TXT("Voice.Pitch"), -12, 12, 0.5, st, TXT("Voice.Pitch.Tip"));
	add_slider(g, kFormantShift, TXT("Voice.Formant"), -6, 6, 0.5, st, TXT("Voice.Formant.Tip"));
	p = obs_properties_add_bool(g, kFormantPreserve, TXT("Voice.Preserve"));
	obs_property_set_long_description(p, TXT("Voice.Preserve.Tip"));
	p = obs_properties_add_list(g, kPitchQuality, TXT("Voice.Quality"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TXT("Voice.Quality.High"), 1);
	obs_property_list_add_int(p, TXT("Voice.Quality.Fast"), 0);
	obs_property_set_long_description(p, TXT("Voice.Quality.Tip"));
	add_info(g, "voice_note", TXT("Voice.Note"));
	p = obs_properties_add_group(props, kPitchEnabled, TXT("Voice"), OBS_GROUP_CHECKABLE, g);
	obs_property_set_long_description(p, TXT("Voice.Tip"));

	// ---- Выход
	add_slider(props, kOutputGain, TXT("OutputGain"), -20, 20, 0.5, dB, TXT("OutputGain.Tip"));
	p = obs_properties_add_bool(props, kLimiterEnabled, TXT("Limiter"));
	obs_property_set_long_description(p, TXT("Limiter.Tip"));
	add_slider(props, kLimiterCeiling, TXT("Limiter.Ceiling"), -12, 0, 0.5, dB, TXT("Limiter.Ceiling.Tip"));

	return props;
}

} // namespace

namespace vs {

std::vector<FilterInstance> list_filter_instances()
{
	std::vector<FilterInstance> out;
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	for (const RegistryEntry &e : g_registry) {
		if (obs_source_t *s = obs_weak_source_get_source(e.weak))
			out.push_back({s, e.shared});
	}
	return out;
}

std::shared_ptr<SharedState> find_shared_state(obs_source_t *filter)
{
	std::lock_guard<std::mutex> lock(g_registry_mutex);
	for (const RegistryEntry &e : g_registry) {
		if (obs_weak_source_references_source(e.weak, filter))
			return e.shared;
	}
	return nullptr;
}

void toggle_calibration(SharedState &shared)
{
	if (shared.calibrating.load()) {
		shared.calib_request = kRequestCancel;
		shared.calibrating = false;
		shared.set_status(TXT("Calib.Cancelled"), OBS_TEXT_INFO_NORMAL);
	} else {
		shared.calib_request = kRequestStart;
		shared.calibrating = true;
		shared.calib_seconds_left = kCalibrationSeconds;
		shared.set_status(format(TXT("Calib.Listening"), (int)kCalibrationSeconds), OBS_TEXT_INFO_NORMAL);
	}
}

void set_live_marker_hook(LiveMarkerFn fn)
{
	g_live_marker = fn;
}

} // namespace vs

void register_voice_filter()
{
	obs_source_info info = {};
	info.id = vs::kFilterId;
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
}
