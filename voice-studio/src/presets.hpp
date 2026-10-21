#pragma once

// Готовые стили звучания. Стиль задаёт тембр и характер обработки, но не трогает
// то, что зависит от конкретного микрофона и комнаты (усиление входа, порог гейта,
// итоговую громкость) — эти значения подбирает «Умная настройка».

#include <obs-data.h>

#include <vector>

namespace vs {

struct PresetValues {
	bool denoise_enabled = true;
	double denoise_amount = 85;

	bool gate_enabled = true;
	int gate_mode = 1; // 0 — по громкости, 1 — по голосу (ИИ), 2 — оба условия
	double gate_range = -20;
	double gate_attack = 2;
	double gate_hold = 200;
	double gate_release = 150;
	double gate_vad_sens = 50;

	bool eq_enabled = true;
	bool hpf_enabled = true;
	double hpf_freq = 80;
	int hpf_slope = 24;
	double low_gain = 0, low_freq = 120;
	double mud_gain = -2, mud_freq = 300, mud_q = 1.0;
	double nasal_gain = 0, nasal_freq = 1000, nasal_q = 1.4;
	double pres_gain = 2, pres_freq = 4000, pres_q = 0.9;
	double air_gain = 1.5, air_freq = 10000;
	double lpf_freq = 20000;

	bool deess_enabled = true;
	double deess_amount = 30, deess_freq = 6500;

	bool comp_enabled = true;
	double comp_amount = 35, comp_attack = 10, comp_release = 120;

	bool warmth_enabled = false;
	double warmth_amount = 25;

	bool pitch_enabled = false;
	double pitch_semitones = 0, formant_shift = 0;
	bool formant_preserve = true;
};

struct PresetInfo {
	const char *id;
	const char *name_key; // ключ перевода
	const char *tip_key;
};

constexpr const char *kPresetCustom = "custom";
constexpr const char *kPresetDefault = "natural";

const std::vector<PresetInfo> &preset_list();

// false — такого стиля нет (или это «Своя настройка»).
bool find_preset(const char *id, PresetValues &out);

// Записывает значения стиля в настройки (или в значения по умолчанию).
void write_preset(obs_data_t *settings, const PresetValues &v, bool as_default);

// Применяет стиль по id. Возвращает false для «Своей настройки» и неизвестных id.
bool apply_preset(obs_data_t *settings, const char *id);

} // namespace vs
