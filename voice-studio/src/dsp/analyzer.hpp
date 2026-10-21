#pragma once

// «Умная настройка»: несколько секунд слушает микрофон и подбирает параметры.
//
// ИИ-детектор речи (VAD из RNNoise) отделяет фразы от пауз, поэтому
// пользователю не нужно отдельно молчать и отдельно говорить. По фразам
// измеряются громкость, динамика, доля шипящих и спектр голоса; по паузам —
// шум помещения. Спектр сравнивается со средним спектром речи (LTASS),
// и эквалайзер получает мягкие поправки под конкретный микрофон и голос.

#include <vector>

#include "common.hpp"
#include "fft.hpp"

namespace vs {

struct CalibrationResult {
	enum Status { Ok = 0, NoVoice = 1, TooQuiet = 2 };

	int status = NoVoice;

	// Измерения
	float voice_db = -100.0f;    // средний уровень речи, дБFS RMS
	float noise_db = -100.0f;    // уровень шума в паузах
	float peak_db = -100.0f;     // максимальный пик
	float snr_db = 0.0f;         // отношение сигнал/шум
	float dynamics_db = 0.0f;    // разброс громкости речи
	float sibilance_db = -30.0f; // доля 5–10 кГц в речи
	bool clipping = false;       // микрофон перегружен
	int voice_frames = 0;
	int noise_frames = 0;

	// Рекомендации
	float input_gain_db = 0.0f;
	float denoise_amount = 85.0f;
	float gate_threshold_db = -45.0f;
	float comp_amount = 35.0f;
	float deess_amount = 30.0f;
	float hpf_freq = 80.0f;
	// Поправки эквалайзера (прибавляются к выбранному стилю)
	float eq_low = 0.0f, eq_mud = 0.0f, eq_nasal = 0.0f, eq_pres = 0.0f, eq_air = 0.0f;
	bool eq_valid = false;
};

class Analyzer {
public:
	static constexpr float kTargetVoiceDb = -18.0f;

	void start(float sample_rate, float seconds);
	void cancel() { active_ = false; }
	bool active() const { return active_; }
	float seconds_left() const;

	// Возвращает true один раз — когда анализ только что завершился.
	bool feed(const float *mono, int frames, float vad, int clipped);

	const CalibrationResult &result() const { return result_; }

private:
	struct FrameStat {
		float rms_db;
		float vad;
	};

	void analyze_block(float vad);
	void finish();
	void compute_eq(CalibrationResult &r) const;
	double band_power(double lo_hz, double hi_hz) const;

	bool active_ = false;
	float sr_ = 48000.0f;
	long total_ = 0;
	long done_ = 0;

	int frame_len_ = 480;
	int frame_fill_ = 0;
	double frame_sq_ = 0.0;
	std::vector<FrameStat> frames_;

	float peak_ = 0.0f;
	long clipped_ = 0;

	int fft_n_ = 4096;
	FFT fft_;
	std::vector<float> fft_buf_;
	std::vector<float> window_;
	std::vector<cfloat> spec_;
	std::vector<double> speech_power_;
	int speech_blocks_ = 0;

	CalibrationResult result_;
};

} // namespace vs
