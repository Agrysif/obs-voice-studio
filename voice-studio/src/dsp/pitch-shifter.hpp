#pragma once

// Смена высоты голоса фазовым вокодером с сохранением формант.
//
// Обычный pitch-shift сдвигает вместе с высотой и «окраску» голоса (форманты) —
// отсюда эффект бурундука. Здесь спектральная огибающая (форманты) выделяется
// через кепстр и накладывается обратно отдельно, поэтому можно:
//   * менять высоту, сохраняя естественный тембр;
//   * менять только «размер» голоса (форманты), не трогая высоту.

#include <vector>

#include "common.hpp"
#include "fft.hpp"

namespace vs {

class PitchShifter {
public:
	// quality: 0 — быстро (меньше задержка), 1 — качественно (лучше для низких голосов).
	void init(int sample_rate, int quality);
	void reset();
	void set(float pitch_semitones, float formant_semitones, bool preserve_formants);
	void process(float *x, int n);

	int latency() const { return n_; }
	int quality() const { return quality_; }

private:
	void process_frame();
	void compute_envelope();
	float envelope_at(float bin) const;

	int sample_rate_ = 48000;
	int quality_ = 1;
	int n_ = 2048;
	int half_ = 1024;
	int hop_ = 512;
	int osamp_ = 4;
	int lifter_ = 77;
	int rover_ = 0;

	float ratio_ = 1.0f;
	float formant_ratio_ = 1.0f;
	bool preserve_ = true;

	FFT fft_;
	std::vector<float> window_, in_fifo_, out_fifo_, accum_;
	std::vector<float> last_phase_, sum_phase_, ana_mag_, ana_freq_, syn_mag_, syn_freq_, env_;
	std::vector<cfloat> spec_, cep_;
};

} // namespace vs
