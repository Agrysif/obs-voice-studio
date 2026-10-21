#include "voice-chain.hpp"

namespace vs {

namespace {

void apply_gain(float *const *ch, int nch, int frames, SmoothedValue &gain)
{
	if (gain.settled()) {
		const float g = gain.target();
		if (g == 1.0f)
			return;
		for (int c = 0; c < nch; c++)
			for (int i = 0; i < frames; i++)
				ch[c][i] *= g;
		return;
	}
	for (int i = 0; i < frames; i++) {
		const float g = gain.next();
		for (int c = 0; c < nch; c++)
			ch[c][i] *= g;
	}
}

void measure(float *const *ch, int nch, int frames, float &peak_db, float &rms_db)
{
	float peak = 0.0f;
	double sq = 0.0;
	for (int c = 0; c < nch; c++) {
		for (int i = 0; i < frames; i++) {
			const float x = ch[c][i];
			peak = std::max(peak, std::fabs(x));
			sq += (double)x * x;
		}
	}
	peak_db = std::max(-120.0f, lin_to_db(peak));
	rms_db = std::max(-120.0f, power_to_db(sq / ((double)frames * nch)));
}

} // namespace

void VoiceChain::configure(int sample_rate, int channels)
{
	sr_ = sample_rate;
	nch_ = std::min(std::max(channels, 1), kMaxChannels);
	const float fs = (float)sr_;

	in_gain_.set_time(10.0f, fs);
	out_gain_.set_time(10.0f, fs);
	in_gain_.reset(db_to_lin(p_.input_gain_db));
	out_gain_.reset(db_to_lin(p_.output_gain_db));

	hpf_.reset();
	eq_.reset();
	denoiser_.configure(sr_, nch_);
	gate_.init(fs);
	deesser_.init(fs);
	comp_.init(fs);
	saturator_.reset();
	for (int c = 0; c < nch_; c++)
		pitch_[c].init(sr_, p_.pitch_quality);
	limiter_.init(fs, nch_);
	analyzer_.cancel();

	for (int c = 0; c < nch_; c++)
		dry_[c].assign(kDryCapacity, 0.0f);
	dry_pos_ = 0;
	mono_buf_.assign(8192, 0.0f);

	denoise_running_ = false;
	pitch_running_ = false;
	limiter_running_ = false;
	mono_ = false;
	mono_run_ = 0;
	last_vad_ = -1.0f;

	apply_params();
}

void VoiceChain::set_params(const ChainParams &p)
{
	p_ = p;
	apply_params();
}

void VoiceChain::apply_params()
{
	if (!sr_)
		return;
	const double fs = sr_;

	in_gain_.set_target(db_to_lin(p_.input_gain_db));
	out_gain_.set_target(db_to_lin(p_.output_gain_db));

	const bool hpf = p_.eq_on && p_.hpf_on;
	if (hpf && p_.hpf_slope >= 24) {
		hpf_.set_slot(0, biquad::high_pass(fs, p_.hpf_freq, biquad::kButter4Q1));
		hpf_.set_slot(1, biquad::high_pass(fs, p_.hpf_freq, biquad::kButter4Q2));
	} else if (hpf) {
		hpf_.set_slot(0, biquad::high_pass(fs, p_.hpf_freq, biquad::kButter2Q));
		hpf_.set_slot(1, BiquadCoefs());
	} else {
		hpf_.set_slot(0, BiquadCoefs());
		hpf_.set_slot(1, BiquadCoefs());
	}

	if (p_.eq_on) {
		eq_.set_slot(0, biquad::low_shelf(fs, p_.low_freq, p_.low_gain));
		eq_.set_slot(1, biquad::peaking(fs, p_.mud_freq, p_.mud_q, p_.mud_gain));
		eq_.set_slot(2, biquad::peaking(fs, p_.nasal_freq, p_.nasal_q, p_.nasal_gain));
		eq_.set_slot(3, biquad::peaking(fs, p_.pres_freq, p_.pres_q, p_.pres_gain));
		eq_.set_slot(4, biquad::high_shelf(fs, p_.air_freq, p_.air_gain));
		const bool lpf = p_.lpf_freq < 19500.0f && p_.lpf_freq < fs * 0.45;
		eq_.set_slot(5, lpf ? biquad::low_pass(fs, p_.lpf_freq, biquad::kButter4Q1) : BiquadCoefs());
		eq_.set_slot(6, lpf ? biquad::low_pass(fs, p_.lpf_freq, biquad::kButter4Q2) : BiquadCoefs());
	} else {
		for (int s = 0; s < 7; s++)
			eq_.set_slot(s, BiquadCoefs());
	}

	gate_.set(p_.gate);
	deesser_.set(p_.deess);
	comp_.set(p_.comp);
	saturator_.set(p_.warmth_on ? p_.warmth_amount : 0.0f);
	for (int c = 0; c < nch_; c++) {
		if (pitch_[c].quality() != p_.pitch_quality)
			pitch_[c].init(sr_, p_.pitch_quality);
		pitch_[c].set(p_.pitch_semitones, p_.formant_semitones, p_.formant_preserve);
	}
	limiter_.set_ceiling(p_.limiter_ceiling_db);
}

void VoiceChain::reset()
{
	if (!sr_)
		return;
	hpf_.reset();
	eq_.reset();
	denoiser_.reset();
	gate_.reset();
	deesser_.reset();
	comp_.reset();
	saturator_.reset();
	for (int c = 0; c < nch_; c++) {
		pitch_[c].reset();
		std::fill(dry_[c].begin(), dry_[c].end(), 0.0f);
	}
	limiter_.reset();
	in_gain_.reset(in_gain_.target());
	out_gain_.reset(out_gain_.target());
}

bool VoiceChain::denoise_needed() const
{
	// Нейросеть нужна и для шумоподавления, и как детектор речи для гейта и анализа.
	return p_.denoise_on || (p_.gate_on && p_.gate.mode != GateMode::Level) || analyzer_.active();
}

bool VoiceChain::pitch_needed() const
{
	return p_.pitch_on && (std::fabs(p_.pitch_semitones) > 0.01f || std::fabs(p_.formant_semitones) > 0.01f);
}

int VoiceChain::latency() const
{
	int l = 0;
	if (denoise_running_)
		l += denoiser_.latency_frames();
	if (pitch_running_)
		l += pitch_[0].latency();
	if (limiter_running_)
		l += limiter_.latency();
	return l;
}

void VoiceChain::start_calibration(float seconds)
{
	if (!sr_)
		return;
	analyzer_.start((float)sr_, seconds);
	calib_finished_ = false;
}

void VoiceChain::cancel_calibration()
{
	analyzer_.cancel();
	calib_finished_ = false;
}

bool VoiceChain::take_calibration_finished()
{
	const bool r = calib_finished_;
	calib_finished_ = false;
	return r;
}

void VoiceChain::process(float *const *ch, int nch, int frames, bool mono_hint)
{
	if (!sr_ || frames <= 0)
		return;
	nch = std::min(nch, nch_);
	if (nch <= 0)
		return;
	const int mask = kDryCapacity - 1;
	if (frames > kDryCapacity / 2)
		frames = kDryCapacity / 2; // OBS таких блоков не присылает, но подстрахуемся

	// Обычный микрофон моно: тяжёлые модули (нейросеть, смена голоса) считаем один раз.
	bool identical = nch > 1;
	for (int c = 1; c < nch && identical; c++)
		identical = std::memcmp(ch[c], ch[0], (size_t)frames * sizeof(float)) == 0;
	mono_run_ = identical ? mono_run_ + frames : 0;
	const bool mono = identical && (mono_hint || mono_run_ >= sr_);
	if (mono_ && !mono) {
		// Каналы разошлись: до этого момента они были одинаковыми, состояние канала 0 подходит всем.
		for (int c = 1; c < nch; c++) {
			denoiser_.copy_channel_state(0, c);
			pitch_[c] = pitch_[0];
		}
	}
	mono_ = mono;

	// Для «Умной настройки» берём исходный сигнал — до любой обработки.
	int clipped = 0;
	const bool analyzing = analyzer_.active();
	if (analyzing) {
		if ((int)mono_buf_.size() < frames)
			mono_buf_.resize((size_t)frames);
		const float inv = 1.0f / (float)nch;
		for (int i = 0; i < frames; i++) {
			float s = 0.0f;
			for (int c = 0; c < nch; c++) {
				s += ch[c][i];
				if (std::fabs(ch[c][i]) >= 0.99f)
					clipped++;
			}
			mono_buf_[(size_t)i] = s * inv;
		}
	}

	measure(ch, nch, frames, meters_.in_peak_db, meters_.in_rms_db);

	// Сохраняем исходник для режима «сравнить с оригиналом».
	for (int c = 0; c < nch; c++) {
		float *d = dry_[c].data();
		for (int i = 0; i < frames; i++)
			d[(dry_pos_ + i) & mask] = ch[c][i];
	}

	// 1. Усиление микрофона
	apply_gain(ch, nch, frames, in_gain_);

	// 2. Срез низкого гула (до нейросети, чтобы гул не мешал ей)
	hpf_.process(ch, nch, frames);

	// 3. ИИ-шумоподавление + детектор речи
	const bool need_dn = denoise_needed();
	if (need_dn && !denoise_running_)
		denoiser_.reset();
	denoise_running_ = need_dn;
	float vad = -1.0f;
	if (need_dn) {
		const float mix = p_.denoise_on ? clampf(p_.denoise_amount / 100.0f, 0.0f, 1.0f) : 0.0f;
		denoiser_.process(ch, nch, frames, mix, mono);
		vad = denoiser_.vad();
	}
	last_vad_ = vad;

	// 4. Шумовой гейт
	if (p_.gate_on)
		gate_.process(ch, nch, frames, vad);

	// 5. Эквалайзер
	eq_.process(ch, nch, frames);

	// 6. Де-эссер
	if (p_.deess_on)
		deesser_.process(ch, nch, frames);

	// 7. Компрессор
	if (p_.comp_on)
		comp_.process(ch, nch, frames);

	// 8. Тепло
	saturator_.process(ch, nch, frames);

	// 9. Смена голоса
	const bool need_pitch = pitch_needed();
	if (need_pitch && !pitch_running_) {
		for (int c = 0; c < nch; c++)
			pitch_[c].reset();
	}
	pitch_running_ = need_pitch;
	if (need_pitch) {
		const int count = mono ? 1 : nch;
		for (int c = 0; c < count; c++)
			pitch_[c].process(ch[c], frames);
		if (mono) {
			for (int c = 1; c < nch; c++)
				std::memcpy(ch[c], ch[0], (size_t)frames * sizeof(float));
		}
	}

	// 10. Итоговая громкость
	apply_gain(ch, nch, frames, out_gain_);

	// 11. Лимитер
	if (p_.limiter_on && !limiter_running_)
		limiter_.reset();
	limiter_running_ = p_.limiter_on;
	if (p_.limiter_on)
		limiter_.process(ch, nch, frames);

	if (analyzing && analyzer_.feed(mono_buf_.data(), frames, vad, clipped))
		calib_finished_ = true;

	// Сравнение с оригиналом: отдаём исходник с той же задержкой, что и обработка,
	// поэтому переключение не «прыгает» во времени.
	if (p_.bypass) {
		const int delay = std::min(latency(), kDryCapacity - frames - 1);
		for (int c = 0; c < nch; c++) {
			const float *d = dry_[c].data();
			for (int i = 0; i < frames; i++)
				ch[c][i] = d[(dry_pos_ + i - delay) & mask];
		}
	}
	dry_pos_ = (dry_pos_ + frames) & mask;

	measure(ch, nch, frames, meters_.out_peak_db, meters_.out_rms_db);
	meters_.comp_gr_db = p_.comp_on ? comp_.gain_reduction_db() : 0.0f;
	meters_.gate_db = p_.gate_on ? gate_.gain_db() : 0.0f;
	meters_.vad = vad;
}

} // namespace vs
