#pragma once

// ИИ-шумоподавление на нейросети RNNoise 0.2 (Xiph.Org).
// Нейросеть работает кадрами по 10 мс на 48 кГц; при другой частоте OBS
// сигнал прозрачно передискретизируется. Кроме очищенного звука RNNoise
// выдаёт вероятность речи (VAD) — её использует «умный» шумовой гейт.

#include "common.hpp"
#include "resampler.hpp"

struct DenoiseState;

namespace vs {

class Denoiser {
public:
	static constexpr int kFrame = 480; // 10 мс при 48 кГц
	static constexpr int kRate = 48000;
	// Собственная задержка RNNoise 0.2: перекрытие окон + кадр «заглядывания вперёд».
	static constexpr int kModelDelay = 2 * kFrame;

	Denoiser() = default;
	~Denoiser();
	Denoiser(const Denoiser &) = delete;
	Denoiser &operator=(const Denoiser &) = delete;

	void configure(int sample_rate, int channels);
	void reset();

	// mix: 0..1 — доля очищенного сигнала (0 = только детектор речи).
	// mono: обработать только канал 0 и скопировать результат в остальные.
	void process(float *const *ch, int nch, int frames, float mix, bool mono);

	// Копирует внутреннее состояние канала (нужно при выходе из «моно»-режима).
	void copy_channel_state(int from, int to);

	// Вероятность речи 0..1 для только что выданного звука.
	float vad() const { return vad_; }

	// Задержка, которую добавляет шумодав (в отсчётах частоты OBS).
	int latency_frames() const { return latency_; }

private:
	struct Channel {
		DenoiseState *st = nullptr;
		SampleFifo pending; // вход на 48 кГц, копится до полного кадра
		SampleFifo out;     // выход на частоте OBS
		std::vector<float> dry; // «сухой» сигнал, задержанный как выход нейросети
		size_t dry_pos = 0;
		Resampler up, down;
	};

	void process_channel(Channel &c, float *x, int frames, float mix, float &vad);
	void init_channel(Channel &c);
	void free_states();

	int sample_rate_ = 0;
	int channels_ = 0;
	bool resample_ = false;
	int prefill_ = 0;
	int latency_ = 0;
	Channel ch_[kMaxChannels];
	float vad_ = 0.0f;
};

} // namespace vs
