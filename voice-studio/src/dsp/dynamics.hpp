#pragma once

// Обработка динамики: шумовой гейт, компрессор, де-эссер, «ламповое» тепло, лимитер.
// Все модули работают со связанными каналами (стереокартина не «плывёт»).

#include "biquad.hpp"
#include "common.hpp"

namespace vs {

// ---------------------------------------------------------------------------
// Шумовой гейт (мягкий экспандер): приглушает паузы между фразами.
// Режимы: по громкости, по ИИ-детектору речи или оба условия сразу.

enum class GateMode { Level = 0, Voice = 1, Both = 2 };

struct GateParams {
	GateMode mode = GateMode::Voice;
	float threshold_db = -45.0f;
	float range_db = -20.0f; // насколько приглушать тишину
	float attack_ms = 2.0f;
	float hold_ms = 200.0f;
	float release_ms = 150.0f;
	float vad_threshold = 0.5f;
};

class Gate {
public:
	void init(float sample_rate)
	{
		sr_ = sample_rate;
		env_att_ = smooth_coef(0.5f, sr_);
		env_rel_ = smooth_coef(40.0f, sr_);
		set(p_);
		reset();
	}

	void set(const GateParams &p)
	{
		p_ = p;
		att_ = smooth_coef(std::max(p.attack_ms, 0.1f), sr_);
		rel_ = smooth_coef(std::max(p.release_ms, 1.0f), sr_);
		hold_samples_ = (int)(p.hold_ms * 0.001f * sr_);
	}

	void reset()
	{
		env_ = 0.0f;
		level_open_ = true;
		hold_left_ = hold_samples_;
		gain_db_ = 0.0f;
	}

	// vad < 0 — детектор речи недоступен, гейт работает по громкости.
	void process(float *const *ch, int nch, int frames, float vad)
	{
		GateMode mode = p_.mode;
		if (vad < 0.0f)
			mode = GateMode::Level;
		const bool vad_open = vad >= p_.vad_threshold;
		const float open_lin = db_to_lin(p_.threshold_db);
		const float close_lin = db_to_lin(p_.threshold_db - 4.0f); // гистерезис 4 дБ

		for (int i = 0; i < frames; i++) {
			float peak = 0.0f;
			for (int c = 0; c < nch; c++)
				peak = std::max(peak, std::fabs(ch[c][i]));
			env_ = peak > env_ ? peak + (env_ - peak) * env_att_ : peak + (env_ - peak) * env_rel_;

			if (level_open_ && env_ < close_lin)
				level_open_ = false;
			else if (!level_open_ && env_ > open_lin)
				level_open_ = true;

			bool want_open;
			switch (mode) {
			case GateMode::Level:
				want_open = level_open_;
				break;
			case GateMode::Voice:
				want_open = vad_open;
				break;
			default:
				want_open = vad_open && level_open_;
				break;
			}

			float target = 0.0f;
			if (want_open)
				hold_left_ = hold_samples_;
			else if (hold_left_ > 0)
				hold_left_--;
			else
				target = p_.range_db;

			gain_db_ = target + (gain_db_ - target) * (target > gain_db_ ? att_ : rel_);
			if (gain_db_ > -0.01f)
				continue;
			const float g = db_to_lin(gain_db_);
			for (int c = 0; c < nch; c++)
				ch[c][i] *= g;
		}
	}

	bool is_open() const { return gain_db_ > -1.0f; }
	float gain_db() const { return gain_db_; }

private:
	GateParams p_;
	float sr_ = 48000.0f;
	float env_att_ = 0.0f, env_rel_ = 0.0f;
	float att_ = 0.0f, rel_ = 0.0f;
	int hold_samples_ = 0;
	float env_ = 0.0f;
	bool level_open_ = true;
	int hold_left_ = 0;
	float gain_db_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Компрессор «одной ручкой»: сила 0..100% задаёт порог и степень сжатия,
// автоматическая компенсация сохраняет громкость голоса примерно прежней.

struct CompParams {
	float amount = 35.0f; // 0..100
	float attack_ms = 10.0f;
	float release_ms = 120.0f;
};

class Compressor {
public:
	static constexpr float kReferenceDb = -18.0f; // типичный уровень голоса после «умной настройки»

	void init(float sample_rate)
	{
		sr_ = sample_rate;
		rms_coef_ = smooth_coef(5.0f, sr_);
		set(p_);
		reset();
	}

	void set(const CompParams &p)
	{
		p_ = p;
		const float a = clampf(p.amount, 0.0f, 100.0f);
		threshold_ = -12.0f - 0.24f * a;
		ratio_ = 1.0f + 0.05f * a;
		knee_ = 6.0f;
		att_ = smooth_coef(std::max(p.attack_ms, 0.1f), sr_);
		rel_ = smooth_coef(std::max(p.release_ms, 5.0f), sr_);
		makeup_db_ = -gain_computer(kReferenceDb) * 0.9f;
	}

	void reset()
	{
		ms_ = 0.0f;
		gr_db_ = 0.0f;
	}

	void process(float *const *ch, int nch, int frames)
	{
		if (p_.amount <= 0.0f)
			return;
		for (int i = 0; i < frames; i++) {
			float sq = 0.0f;
			for (int c = 0; c < nch; c++)
				sq = std::max(sq, ch[c][i] * ch[c][i]);
			ms_ = sq + (ms_ - sq) * rms_coef_;
			const float level_db = power_to_db(ms_);
			const float target = gain_computer(level_db);
			gr_db_ = target + (gr_db_ - target) * (target < gr_db_ ? att_ : rel_);
			const float g = db_to_lin(gr_db_ + makeup_db_);
			for (int c = 0; c < nch; c++)
				ch[c][i] *= g;
		}
	}

	float gain_reduction_db() const { return gr_db_; }

private:
	// Мягкое «колено»: возвращает изменение уровня (<= 0 дБ).
	float gain_computer(float x) const
	{
		const float over = x - threshold_;
		if (2.0f * over < -knee_)
			return 0.0f;
		if (2.0f * std::fabs(over) <= knee_) {
			const float t = over + knee_ * 0.5f;
			return (1.0f / ratio_ - 1.0f) * t * t / (2.0f * knee_);
		}
		return (1.0f / ratio_ - 1.0f) * over;
	}

	CompParams p_;
	float sr_ = 48000.0f;
	float threshold_ = -20.0f, ratio_ = 2.0f, knee_ = 6.0f, makeup_db_ = 0.0f;
	float att_ = 0.0f, rel_ = 0.0f, rms_coef_ = 0.0f;
	float ms_ = 0.0f, gr_db_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Де-эссер: динамически приглушает только резкие «с», «ш», «щ».
// Срабатывание зависит от доли высоких частот, а не от общей громкости.

struct DeessParams {
	float amount = 30.0f; // 0..100
	float freq = 6500.0f;
};

class DeEsser {
public:
	void init(float sample_rate)
	{
		sr_ = sample_rate;
		env_att_ = smooth_coef(0.5f, sr_);
		env_rel_ = smooth_coef(25.0f, sr_);
		gain_att_ = smooth_coef(1.0f, sr_);
		gain_rel_ = smooth_coef(45.0f, sr_);
		set(p_);
		reset();
	}

	void set(const DeessParams &p)
	{
		p_ = p;
		const float a = clampf(p.amount, 0.0f, 100.0f);
		threshold_rel_db_ = -1.5f - 0.12f * a;
		max_reduction_db_ = 3.0f + 0.09f * a;
		const BiquadCoefs c = biquad::high_pass(sr_, p.freq, biquad::kButter2Q);
		for (Biquad &b : hp_)
			b.set(c);
	}

	void reset()
	{
		for (Biquad &b : hp_)
			b.reset();
		s_env_ = f_env_ = 0.0f;
		gain_db_ = 0.0f;
	}

	void process(float *const *ch, int nch, int frames)
	{
		if (p_.amount <= 0.0f)
			return;
		const float floor_lin = db_to_lin(-55.0f);
		float high[kMaxChannels];
		for (int i = 0; i < frames; i++) {
			float s = 0.0f, f = 0.0f;
			for (int c = 0; c < nch; c++) {
				high[c] = hp_[c].process(ch[c][i]);
				s = std::max(s, std::fabs(high[c]));
				f = std::max(f, std::fabs(ch[c][i]));
			}
			s_env_ = s > s_env_ ? s + (s_env_ - s) * env_att_ : s + (s_env_ - s) * env_rel_;
			f_env_ = f > f_env_ ? f + (f_env_ - f) * env_att_ : f + (f_env_ - f) * env_rel_;

			float target = 0.0f;
			if (s_env_ > floor_lin && f_env_ > 0.0f) {
				const float over = lin_to_db(s_env_) - lin_to_db(f_env_) - threshold_rel_db_;
				if (over > 0.0f)
					target = -std::min(max_reduction_db_, over * 1.5f);
			}
			gain_db_ = target + (gain_db_ - target) * (target < gain_db_ ? gain_att_ : gain_rel_);
			if (gain_db_ > -0.01f)
				continue;
			const float k = 1.0f - db_to_lin(gain_db_);
			for (int c = 0; c < nch; c++)
				ch[c][i] -= k * high[c];
		}
	}

private:
	DeessParams p_;
	float sr_ = 48000.0f;
	Biquad hp_[kMaxChannels];
	float env_att_ = 0.0f, env_rel_ = 0.0f, gain_att_ = 0.0f, gain_rel_ = 0.0f;
	float threshold_rel_db_ = -6.0f, max_reduction_db_ = 6.0f;
	float s_env_ = 0.0f, f_env_ = 0.0f, gain_db_ = 0.0f;
};

// ---------------------------------------------------------------------------
// «Тепло»: мягкое ламповое насыщение (в основном чётные гармоники).

class Saturator {
public:
	void set(float amount)
	{
		a_ = clampf(amount, 0.0f, 100.0f) / 100.0f;
		drive_ = 1.0f + 2.0f * a_;
		bias_ = 0.15f * a_;
		const float tb = std::tanh(bias_);
		tanh_bias_ = tb;
		norm_ = 1.0f / (drive_ * (1.0f - tb * tb)); // наклон в нуле = 1: тихие звуки не меняются
		mix_ = 0.6f * a_;
	}

	void reset()
	{
		for (int c = 0; c < kMaxChannels; c++)
			x1_[c] = y1_[c] = 0.0f;
	}

	void process(float *const *ch, int nch, int frames)
	{
		if (a_ <= 0.0f)
			return;
		for (int c = 0; c < nch; c++) {
			float *x = ch[c];
			float x1 = x1_[c], y1 = y1_[c];
			for (int i = 0; i < frames; i++) {
				const float in = x[i];
				const float sat = (std::tanh(drive_ * in + bias_) - tanh_bias_) * norm_;
				const float wet = in + mix_ * (sat - in);
				// Убираем постоянную составляющую от асимметрии.
				const float y = wet - x1 + 0.9995f * y1;
				x1 = wet;
				y1 = y;
				x[i] = y;
			}
			x1_[c] = x1;
			y1_[c] = y1;
		}
	}

private:
	float a_ = 0.0f, drive_ = 1.0f, bias_ = 0.0f, tanh_bias_ = 0.0f, norm_ = 1.0f, mix_ = 0.0f;
	float x1_[kMaxChannels] = {}, y1_[kMaxChannels] = {};
};

// ---------------------------------------------------------------------------
// Лимитер с заглядыванием вперёд (1.5 мс): не даёт звуку перегрузиться
// и захрипеть, без слышимых щелчков.

class Limiter {
public:
	void init(float sample_rate, int channels)
	{
		sr_ = sample_rate;
		channels_ = std::min(channels, kMaxChannels);
		look_ = std::max(8, (int)(0.0015f * sr_));
		len_ = look_ + 1;
		for (int c = 0; c < channels_; c++)
			delay_[c].assign((size_t)look_, 0.0f);
		req_.assign((size_t)len_, 1.0f);
		mins_.assign((size_t)len_, 1.0f);
		rel_ = smooth_coef(80.0f, sr_);
		reset();
	}

	void set_ceiling(float ceiling_db) { ceiling_ = db_to_lin(std::min(ceiling_db, 0.0f)); }

	void reset()
	{
		for (int c = 0; c < channels_; c++)
			std::fill(delay_[c].begin(), delay_[c].end(), 0.0f);
		std::fill(req_.begin(), req_.end(), 1.0f);
		std::fill(mins_.begin(), mins_.end(), 1.0f);
		box_sum_ = (double)len_;
		pos_ = 0;
		dpos_ = 0;
		since_limit_ = len_ * 2;
		gain_ = 1.0f;
	}

	int latency() const { return look_; }

	void process(float *const *ch, int nch, int frames)
	{
		nch = std::min(nch, channels_);
		for (int i = 0; i < frames; i++) {
			float peak = 0.0f;
			for (int c = 0; c < nch; c++)
				peak = std::max(peak, std::fabs(ch[c][i]));
			const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;
			req_[(size_t)pos_] = need;
			if (need < 1.0f)
				since_limit_ = 0;
			else if (since_limit_ < len_ * 2)
				since_limit_++;

			// Минимум требуемого усиления за окно заглядывания…
			float m = 1.0f;
			if (since_limit_ <= len_) {
				for (float r : req_)
					m = std::min(m, r);
			}
			// …и сглаживание «коробочным» фильтром той же длины: усиление
			// успевает плавно опуститься до того, как пик дойдёт до выхода.
			box_sum_ += (double)m - (double)mins_[(size_t)pos_];
			mins_[(size_t)pos_] = m;
			float g = (float)(box_sum_ / len_);
			if (g > gain_)
				g = g + (gain_ - g) * rel_;
			gain_ = std::min(g, 1.0f);

			// Звук задержан ровно на look_ отсчётов — столько же «видит вперёд» минимум.
			for (int c = 0; c < nch; c++) {
				float &slot = delay_[c][(size_t)dpos_];
				const float delayed = slot;
				slot = ch[c][i];
				ch[c][i] = clampf(delayed * gain_, -ceiling_, ceiling_);
			}
			if (++pos_ == len_)
				pos_ = 0;
			if (++dpos_ == look_)
				dpos_ = 0;
		}
	}

private:
	float sr_ = 48000.0f;
	int channels_ = 0;
	int look_ = 72, len_ = 73;
	std::vector<float> delay_[kMaxChannels];
	std::vector<float> req_, mins_;
	double box_sum_ = 0.0;
	int pos_ = 0;
	int dpos_ = 0;
	int since_limit_ = 0;
	float gain_ = 1.0f, rel_ = 0.0f, ceiling_ = 0.891f;
};

} // namespace vs
