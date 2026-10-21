#include "analyzer.hpp"

namespace vs {

namespace {

// Средний долговременный спектр речи по третьоктавным полосам
// (приблизительно по Byrne et al., 1994, усреднение мужских и женских голосов), дБ.
struct LtassBand {
	float hz;
	float db;
};
constexpr LtassBand kLtass[] = {
	{63, 38.6f},    {80, 43.5f},    {100, 54.4f},   {125, 57.7f},  {160, 56.8f},  {200, 58.2f},
	{250, 59.7f},   {315, 60.0f},   {400, 62.4f},   {500, 62.6f},  {630, 59.3f},  {800, 56.4f},
	{1000, 53.6f},  {1250, 51.2f},  {1600, 49.0f},  {2000, 47.2f}, {2500, 46.2f}, {3150, 46.7f},
	{4000, 45.5f},  {5000, 44.4f},  {6300, 43.3f},  {8000, 41.5f}, {10000, 39.9f}, {12500, 39.7f},
};
constexpr int kLtassCount = (int)(sizeof(kLtass) / sizeof(kLtass[0]));

inline float round_to(float v, float step)
{
	return std::round(v / step) * step;
}

} // namespace

void Analyzer::start(float sample_rate, float seconds)
{
	sr_ = sample_rate;
	total_ = (long)(seconds * sample_rate);
	done_ = 0;
	frame_len_ = std::max(1, (int)(sample_rate / 100.0f));
	frame_fill_ = 0;
	frame_sq_ = 0.0;
	frames_.clear();
	frames_.reserve((size_t)(seconds * 100.0f) + 16);
	peak_ = 0.0f;
	clipped_ = 0;

	fft_n_ = sample_rate > 50000.0f ? 8192 : 4096;
	fft_.init(fft_n_);
	fft_buf_.clear();
	fft_buf_.reserve((size_t)fft_n_ * 2);
	window_.resize((size_t)fft_n_);
	for (int i = 0; i < fft_n_; i++)
		window_[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * kPi * (float)i / (float)fft_n_);
	spec_.assign((size_t)fft_n_, cfloat());
	speech_power_.assign((size_t)fft_n_ / 2 + 1, 0.0);
	speech_blocks_ = 0;

	result_ = CalibrationResult();
	active_ = true;
}

float Analyzer::seconds_left() const
{
	if (!active_ || sr_ <= 0.0f)
		return 0.0f;
	return (float)(total_ - done_) / sr_;
}

bool Analyzer::feed(const float *mono, int frames, float vad, int clipped)
{
	if (!active_)
		return false;

	clipped_ += clipped;
	for (int i = 0; i < frames; i++) {
		const float x = mono[i];
		peak_ = std::max(peak_, std::fabs(x));

		frame_sq_ += (double)x * x;
		if (++frame_fill_ == frame_len_) {
			frames_.push_back({power_to_db(frame_sq_ / frame_len_), vad});
			frame_fill_ = 0;
			frame_sq_ = 0.0;
		}

		fft_buf_.push_back(x);
		if ((int)fft_buf_.size() == fft_n_) {
			analyze_block(vad);
			fft_buf_.erase(fft_buf_.begin(), fft_buf_.begin() + fft_n_ / 2);
		}
	}

	done_ += frames;
	if (done_ >= total_) {
		finish();
		active_ = false;
		return true;
	}
	return false;
}

void Analyzer::analyze_block(float vad)
{
	double sq = 0.0;
	for (int i = 0; i < fft_n_; i++) {
		const float x = fft_buf_[(size_t)i];
		sq += (double)x * x;
		spec_[(size_t)i] = cfloat(x * window_[(size_t)i], 0.0f);
	}
	const float block_db = power_to_db(sq / fft_n_);
	if (vad < 0.6f || block_db < -65.0f)
		return; // в спектр голоса берём только уверенную речь

	fft_.forward(spec_.data());
	for (int k = 0; k <= fft_n_ / 2; k++)
		speech_power_[(size_t)k] += (double)std::norm(spec_[(size_t)k]);
	speech_blocks_++;
}

double Analyzer::band_power(double lo_hz, double hi_hz) const
{
	const double bin_hz = sr_ / fft_n_;
	const int max_bin = fft_n_ / 2;
	const double lo = lo_hz / bin_hz;
	const double hi = hi_hz / bin_hz;
	if (hi - lo < 1.0) {
		// Полоса уже одного бина: берём ближайший с поправкой на ширину.
		const int k = std::min(max_bin, std::max(1, (int)std::lround((lo + hi) * 0.5)));
		return speech_power_[(size_t)k] * (hi - lo);
	}
	double sum = 0.0;
	for (int k = std::max(1, (int)std::ceil(lo)); k < (int)std::ceil(hi) && k <= max_bin; k++)
		sum += speech_power_[(size_t)k];
	return sum;
}

void Analyzer::compute_eq(CalibrationResult &r) const
{
	if (speech_blocks_ < 4)
		return;

	const double third = std::pow(2.0, 1.0 / 6.0);
	float diff[kLtassCount];
	double norm_sum = 0.0;
	int norm_count = 0;
	for (int b = 0; b < kLtassCount; b++) {
		const double fc = kLtass[b].hz;
		if (fc * third >= sr_ * 0.5) {
			diff[b] = 0.0f;
			continue;
		}
		const float user_db = power_to_db(band_power(fc / third, fc * third));
		diff[b] = user_db - kLtass[b].db;
		if (fc >= 250.0 && fc <= 4000.0) {
			norm_sum += diff[b];
			norm_count++;
		}
	}
	if (!norm_count)
		return;
	const float offset = (float)(norm_sum / norm_count);

	auto region = [&](float lo_hz, float hi_hz) {
		double s = 0.0;
		int n = 0;
		for (int b = 0; b < kLtassCount; b++) {
			if (kLtass[b].hz >= lo_hz && kLtass[b].hz <= hi_hz && kLtass[b].hz * third < sr_ * 0.5) {
				s += diff[b] - offset;
				n++;
			}
		}
		return n ? (float)(s / n) : 0.0f;
	};

	// Исправляем половину отклонения от эталона — мягко, без «перекручивания».
	const float k = -0.5f;
	// Низы поднимаем осторожно: у высоких голосов там просто нет энергии, и подъём дал бы гул.
	r.eq_low = round_to(clampf(k * region(80, 160), -6.0f, 1.5f), 0.5f);
	r.eq_mud = round_to(clampf(k * region(200, 400), -6.0f, 2.0f), 0.5f);
	r.eq_nasal = round_to(clampf(k * region(630, 1250), -5.0f, 2.0f), 0.5f);
	r.eq_pres = round_to(clampf(k * region(2500, 5000), -3.0f, 6.0f), 0.5f);
	// В шумной комнате не добавляем много «воздуха», чтобы не вытащить шипение.
	// Если верхов почти нет совсем (гарнитура, Bluetooth, сжатие), поднимать нечего.
	float air_max = r.snr_db < 30.0f ? 2.0f : 6.0f;
	const float top = region(12000, 13000);
	if (top < -25.0f)
		air_max = std::min(air_max, 1.5f);
	r.eq_air = round_to(clampf(k * region(8000, 12500), -3.0f, air_max), 0.5f);
	r.eq_valid = true;
}

void Analyzer::finish()
{
	CalibrationResult r;

	std::vector<float> voice_db;
	std::vector<float> noise_db;
	std::vector<float> all_db;
	double voice_pow = 0.0;
	for (const FrameStat &f : frames_) {
		all_db.push_back(f.rms_db);
		if (f.vad > 0.6f && f.rms_db > -75.0f) {
			voice_db.push_back(f.rms_db);
			voice_pow += std::pow(10.0, f.rms_db / 10.0);
		} else if (f.vad < 0.15f) {
			noise_db.push_back(f.rms_db);
		}
	}
	r.voice_frames = (int)voice_db.size();
	r.noise_frames = (int)noise_db.size();
	r.peak_db = lin_to_db(peak_);
	r.clipping = clipped_ > 8 || r.peak_db > -0.3f;

	if (r.voice_frames < 80) { // меньше ~0.8 с уверенной речи
		r.status = CalibrationResult::NoVoice;
		result_ = r;
		return;
	}

	r.voice_db = power_to_db(voice_pow / r.voice_frames);
	if (r.noise_frames >= 30) {
		// Медиана, а не среднее: начала и хвосты слов, ошибочно попавшие в паузы,
		// не должны завышать оценку шума.
		std::sort(noise_db.begin(), noise_db.end());
		r.noise_db = noise_db[noise_db.size() / 2];
	} else {
		// Почти не было пауз — берём 10-й перцентиль всех кадров.
		std::sort(all_db.begin(), all_db.end());
		r.noise_db = all_db[all_db.size() / 10];
	}
	r.noise_db = clampf(r.noise_db, -100.0f, r.voice_db - 3.0f);
	r.snr_db = r.voice_db - r.noise_db;

	// Разброс громкости речи (без совсем тихих хвостов).
	double s = 0.0, s2 = 0.0;
	int n = 0;
	for (float d : voice_db) {
		if (d < r.voice_db - 25.0f)
			continue;
		s += d;
		s2 += (double)d * d;
		n++;
	}
	if (n > 1) {
		const double mean = s / n;
		r.dynamics_db = (float)std::sqrt(std::max(0.0, s2 / n - mean * mean));
	}

	// Доля шипящих и низкочастотный гул в спектре речи.
	if (speech_blocks_ >= 4) {
		const double all = band_power(100.0, 10000.0);
		const double sib = band_power(5000.0, std::min(10000.0, sr_ * 0.49));
		const double rumble = band_power(20.0, 70.0);
		if (all > 0.0) {
			r.sibilance_db = power_to_db(sib / all);
			r.hpf_freq = power_to_db(rumble / all) > -22.0f ? 100.0f : 80.0f;
		}
	}

	// --- Рекомендации
	if (r.voice_db < -60.0f)
		r.status = CalibrationResult::TooQuiet;
	else
		r.status = CalibrationResult::Ok;

	r.input_gain_db = round_to(clampf(kTargetVoiceDb - r.voice_db, -15.0f, 30.0f), 0.5f);

	const float noise_after = r.noise_db + r.input_gain_db;
	const float voice_after = r.voice_db + r.input_gain_db;
	// Порог между шумом и голосом: выше шума на 10 дБ, но не ниже уровня дыхания
	// (~30 дБ тише речи) и с запасом 14 дБ до самой речи.
	const float thr = std::min(std::max(noise_after + 10.0f, voice_after - 30.0f), voice_after - 14.0f);
	r.gate_threshold_db = std::round(clampf(thr, -70.0f, -25.0f));

	if (r.snr_db > 45.0f)
		r.denoise_amount = 70.0f;
	else if (r.snr_db > 30.0f)
		r.denoise_amount = 85.0f;
	else
		r.denoise_amount = 100.0f;

	r.comp_amount = round_to(clampf(25.0f + (r.dynamics_db - 4.0f) * 7.0f, 20.0f, 65.0f), 5.0f);

	if (r.sibilance_db > -12.0f)
		r.deess_amount = 60.0f;
	else if (r.sibilance_db > -16.0f)
		r.deess_amount = 45.0f;
	else if (r.sibilance_db > -20.0f)
		r.deess_amount = 30.0f;
	else
		r.deess_amount = 20.0f;

	compute_eq(r);
	result_ = r;
}

} // namespace vs
