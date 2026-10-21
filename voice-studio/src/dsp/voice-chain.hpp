#pragma once

// Полная цепочка обработки голоса:
//
//   вход -> усиление -> срез гула -> ИИ-шумодав -> гейт -> эквалайзер
//        -> де-эссер -> компрессор -> тепло -> смена голоса -> громкость -> лимитер
//
// Порядок выбран как в студийной практике: сначала убираем мусор (гул, шум,
// паузы), потом формируем тембр, потом выравниваем громкость и защищаем от перегруза.

#include "analyzer.hpp"
#include "biquad.hpp"
#include "common.hpp"
#include "denoiser.hpp"
#include "dynamics.hpp"
#include "pitch-shifter.hpp"

namespace vs {

struct ChainParams {
	bool bypass = false;
	float input_gain_db = 0.0f;

	bool eq_on = true;
	bool hpf_on = true;
	float hpf_freq = 80.0f;
	int hpf_slope = 24; // 12 или 24 дБ/окт
	float low_gain = 0.0f, low_freq = 120.0f;
	float mud_gain = -2.0f, mud_freq = 300.0f, mud_q = 1.0f;
	float nasal_gain = 0.0f, nasal_freq = 1000.0f, nasal_q = 1.4f;
	float pres_gain = 2.0f, pres_freq = 4000.0f, pres_q = 0.9f;
	float air_gain = 1.5f, air_freq = 10000.0f;
	float lpf_freq = 20000.0f; // >= 19500 — выключен

	bool denoise_on = true;
	float denoise_amount = 85.0f;

	bool gate_on = true;
	GateParams gate;

	bool deess_on = true;
	DeessParams deess;

	bool comp_on = true;
	CompParams comp;

	bool warmth_on = false;
	float warmth_amount = 25.0f;

	bool pitch_on = false;
	float pitch_semitones = 0.0f;
	float formant_semitones = 0.0f;
	bool formant_preserve = true;
	int pitch_quality = 1;

	float output_gain_db = 0.0f;
	bool limiter_on = true;
	float limiter_ceiling_db = -1.0f;
};

// Живые индикаторы для интерфейса (за последний обработанный блок).
struct ChainMeters {
	float in_peak_db = -120.0f;  // вход до обработки
	float in_rms_db = -120.0f;
	float out_peak_db = -120.0f; // выход после лимитера
	float out_rms_db = -120.0f;
	float comp_gr_db = 0.0f;     // насколько сжимает компрессор
	float gate_db = 0.0f;        // насколько гейт приглушает
	float vad = -1.0f;           // вероятность речи (−1 — детектор выключен)
};

class VoiceChain {
public:
	void configure(int sample_rate, int channels);
	bool configured() const { return sr_ > 0; }
	int sample_rate() const { return sr_; }
	int channels() const { return nch_; }

	void set_params(const ChainParams &p);
	const ChainParams &params() const { return p_; }
	void reset();

	// Обработка «на месте». mono_hint — источник заведомо моно (как в OBS).
	void process(float *const *ch, int nch, int frames, bool mono_hint);

	// Общая задержка обработки в отсчётах.
	int latency() const;
	float vad() const { return last_vad_; }
	const ChainMeters &meters() const { return meters_; }

	// «Умная настройка»
	void start_calibration(float seconds);
	void cancel_calibration();
	bool calibrating() const { return analyzer_.active(); }
	float calibration_seconds_left() const { return analyzer_.seconds_left(); }
	bool take_calibration_finished();
	const CalibrationResult &calibration_result() const { return analyzer_.result(); }

private:
	void apply_params();
	bool denoise_needed() const;
	bool pitch_needed() const;

	ChainParams p_;
	int sr_ = 0;
	int nch_ = 0;

	SmoothedValue in_gain_, out_gain_;
	FilterChain hpf_, eq_;
	Denoiser denoiser_;
	Gate gate_;
	DeEsser deesser_;
	Compressor comp_;
	Saturator saturator_;
	PitchShifter pitch_[kMaxChannels];
	Limiter limiter_;
	Analyzer analyzer_;

	bool denoise_running_ = false;
	bool pitch_running_ = false;
	bool limiter_running_ = false;
	bool calib_finished_ = false;
	float last_vad_ = -1.0f;
	ChainMeters meters_;

	bool mono_ = false;
	long mono_run_ = 0;
	std::vector<float> mono_buf_;

	// Линия задержки для сравнения «до/после» без скачка во времени.
	static constexpr int kDryCapacity = 1 << 15;
	std::vector<float> dry_[kMaxChannels];
	int dry_pos_ = 0;
};

} // namespace vs
