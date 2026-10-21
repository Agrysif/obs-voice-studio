#include "pitch-shifter.hpp"

namespace vs {

namespace {
constexpr float kTwoPi = 2.0f * kPi;

inline float wrap_phase(float p)
{
	return p - kTwoPi * std::floor(p / kTwoPi + 0.5f);
}
} // namespace

void PitchShifter::init(int sample_rate, int quality)
{
	sample_rate_ = sample_rate;
	quality_ = quality ? 1 : 0;
	// ~43 мс окна при 48 кГц в режиме качества, ~21 мс в быстром.
	n_ = (quality_ ? 2048 : 1024) * (sample_rate >= 88200 ? 2 : 1);
	half_ = n_ / 2;
	osamp_ = 4;
	hop_ = n_ / osamp_;
	// Кепстральный фильтр ~1.6 мс: отделяет огибающую (форманты) от гармоник голоса.
	lifter_ = std::max(16, std::min(half_ / 2, (int)(0.0016f * (float)sample_rate)));

	fft_.init(n_);
	window_.resize((size_t)n_);
	for (int i = 0; i < n_; i++)
		window_[(size_t)i] = 0.5f - 0.5f * std::cos(kTwoPi * (float)i / (float)n_);

	in_fifo_.assign((size_t)n_, 0.0f);
	out_fifo_.assign((size_t)hop_, 0.0f);
	accum_.assign((size_t)n_, 0.0f);
	last_phase_.assign((size_t)half_ + 1, 0.0f);
	sum_phase_.assign((size_t)half_ + 1, 0.0f);
	ana_mag_.assign((size_t)half_ + 1, 0.0f);
	ana_freq_.assign((size_t)half_ + 1, 0.0f);
	syn_mag_.assign((size_t)half_ + 1, 0.0f);
	syn_freq_.assign((size_t)half_ + 1, 0.0f);
	env_.assign((size_t)half_ + 1, 1.0f);
	spec_.assign((size_t)n_, cfloat());
	cep_.assign((size_t)n_, cfloat());
	reset();
}

void PitchShifter::reset()
{
	std::fill(in_fifo_.begin(), in_fifo_.end(), 0.0f);
	std::fill(out_fifo_.begin(), out_fifo_.end(), 0.0f);
	std::fill(accum_.begin(), accum_.end(), 0.0f);
	std::fill(last_phase_.begin(), last_phase_.end(), 0.0f);
	std::fill(sum_phase_.begin(), sum_phase_.end(), 0.0f);
	rover_ = n_ - hop_;
}

void PitchShifter::set(float pitch_semitones, float formant_semitones, bool preserve_formants)
{
	ratio_ = std::pow(2.0f, clampf(pitch_semitones, -24.0f, 24.0f) / 12.0f);
	formant_ratio_ = std::pow(2.0f, clampf(formant_semitones, -12.0f, 12.0f) / 12.0f);
	preserve_ = preserve_formants;
}

void PitchShifter::process(float *x, int n)
{
	const int fifo_latency = n_ - hop_;
	for (int i = 0; i < n; i++) {
		in_fifo_[(size_t)rover_] = x[i];
		x[i] = out_fifo_[(size_t)(rover_ - fifo_latency)];
		if (++rover_ >= n_) {
			rover_ = fifo_latency;
			process_frame();
		}
	}
}

void PitchShifter::compute_envelope()
{
	// Логарифм амплитудного спектра -> кепстр -> оставляем низкие «кефренси» -> обратно.
	for (int k = 0; k <= half_; k++)
		cep_[(size_t)k] = cfloat(std::log(std::max(ana_mag_[(size_t)k], 1e-7f)), 0.0f);
	for (int k = half_ + 1; k < n_; k++)
		cep_[(size_t)k] = cep_[(size_t)(n_ - k)];

	fft_.inverse(cep_.data());

	const int taper = std::max(4, lifter_ / 4);
	for (int q = 0; q < n_; q++) {
		const int qq = std::min(q, n_ - q);
		float w;
		if (qq <= lifter_ - taper)
			w = 1.0f;
		else if (qq <= lifter_)
			w = 0.5f + 0.5f * std::cos(kPi * (float)(qq - (lifter_ - taper)) / (float)taper);
		else
			w = 0.0f;
		cep_[(size_t)q] = cfloat(cep_[(size_t)q].real() * w, 0.0f);
	}

	fft_.forward(cep_.data());
	for (int k = 0; k <= half_; k++)
		env_[(size_t)k] = std::max(std::exp(cep_[(size_t)k].real()), 1e-7f);
}

float PitchShifter::envelope_at(float bin) const
{
	if (bin <= 0.0f)
		return env_[0];
	if (bin >= (float)half_)
		return env_[(size_t)half_];
	const int i = (int)bin;
	const float t = bin - (float)i;
	return env_[(size_t)i] + t * (env_[(size_t)i + 1] - env_[(size_t)i]);
}

void PitchShifter::process_frame()
{
	const float expct = kTwoPi * (float)hop_ / (float)n_;
	const float to_bins = (float)osamp_ / kTwoPi;

	// --- Анализ: амплитуда и «истинная» частота каждого бина.
	for (int i = 0; i < n_; i++)
		spec_[(size_t)i] = cfloat(in_fifo_[(size_t)i] * window_[(size_t)i], 0.0f);
	fft_.forward(spec_.data());

	for (int k = 0; k <= half_; k++) {
		const cfloat v = spec_[(size_t)k];
		const float phase = std::atan2(v.imag(), v.real());
		float d = phase - last_phase_[(size_t)k];
		last_phase_[(size_t)k] = phase;
		d = wrap_phase(d - (float)k * expct);
		ana_freq_[(size_t)k] = (float)k + d * to_bins;
		ana_mag_[(size_t)k] = std::abs(v);
	}

	// --- Сдвиг: возбуждение (гармоники) переносим, огибающую (форманты) — отдельно.
	const bool use_env = preserve_ || std::fabs(formant_ratio_ - 1.0f) > 1e-4f;
	if (use_env)
		compute_envelope();

	std::fill(syn_mag_.begin(), syn_mag_.end(), 0.0f);
	std::fill(syn_freq_.begin(), syn_freq_.end(), 0.0f);
	for (int k = 0; k <= half_; k++) {
		const int idx = (int)std::lround((float)k * ratio_);
		if (idx > half_)
			break;
		const float m = use_env ? ana_mag_[(size_t)k] / env_[(size_t)k] : ana_mag_[(size_t)k];
		syn_mag_[(size_t)idx] += m;
		syn_freq_[(size_t)idx] = ana_freq_[(size_t)k] * ratio_;
	}
	if (use_env) {
		// preserve: форманты остаются на месте (только «размер» голоса).
		// иначе: форманты едут вместе с высотой, как у обычного pitch-shift.
		const float f = preserve_ ? formant_ratio_ : formant_ratio_ * ratio_;
		for (int k = 0; k <= half_; k++)
			syn_mag_[(size_t)k] *= envelope_at((float)k / f);
	}

	// --- Синтез: накапливаем фазу и собираем сигнал обратно.
	for (int k = 0; k <= half_; k++) {
		const float d = (syn_freq_[(size_t)k] - (float)k) / to_bins + (float)k * expct;
		const float ph = wrap_phase(sum_phase_[(size_t)k] + d);
		sum_phase_[(size_t)k] = ph;
		spec_[(size_t)k] = std::polar(syn_mag_[(size_t)k], ph);
	}
	spec_[0] = cfloat(spec_[0].real(), 0.0f);
	spec_[(size_t)half_] = cfloat(spec_[(size_t)half_].real(), 0.0f);
	for (int k = half_ + 1; k < n_; k++)
		spec_[(size_t)k] = std::conj(spec_[(size_t)(n_ - k)]);
	fft_.inverse(spec_.data());

	// Сумма квадратов окна Ханна при перекрытии osamp равна osamp * 3/8.
	const float norm = 1.0f / ((float)osamp_ * 0.375f);
	for (int i = 0; i < n_; i++)
		accum_[(size_t)i] += window_[(size_t)i] * spec_[(size_t)i].real() * norm;

	std::copy(accum_.begin(), accum_.begin() + hop_, out_fifo_.begin());
	std::copy(accum_.begin() + hop_, accum_.end(), accum_.begin());
	std::fill(accum_.end() - hop_, accum_.end(), 0.0f);
	std::copy(in_fifo_.begin() + hop_, in_fifo_.end(), in_fifo_.begin());
}

} // namespace vs
