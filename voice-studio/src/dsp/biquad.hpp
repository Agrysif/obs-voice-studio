#pragma once

// Биквадратные фильтры по формулам «Audio EQ Cookbook» (R. Bristow-Johnson)
// и цепочка таких фильтров на несколько каналов.

#include "common.hpp"

namespace vs {

struct BiquadCoefs {
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

	bool is_identity() const { return b0 == 1.0 && b1 == 0.0 && b2 == 0.0 && a1 == 0.0 && a2 == 0.0; }
};

namespace biquad {

inline double clamp_freq(double fs, double f0)
{
	return std::min(std::max(f0, 10.0), fs * 0.49);
}

inline BiquadCoefs normalized(double b0, double b1, double b2, double a0, double a1, double a2)
{
	BiquadCoefs c;
	c.b0 = b0 / a0;
	c.b1 = b1 / a0;
	c.b2 = b2 / a0;
	c.a1 = a1 / a0;
	c.a2 = a2 / a0;
	return c;
}

inline BiquadCoefs peaking(double fs, double f0, double q, double gain_db)
{
	if (std::fabs(gain_db) < 1e-3)
		return {};
	const double A = std::pow(10.0, gain_db / 40.0);
	const double w0 = 2.0 * 3.141592653589793 * clamp_freq(fs, f0) / fs;
	const double alpha = std::sin(w0) / (2.0 * std::max(q, 0.05));
	const double cw = std::cos(w0);
	return normalized(1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A, 1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
}

inline BiquadCoefs low_shelf(double fs, double f0, double gain_db, double slope = 1.0)
{
	if (std::fabs(gain_db) < 1e-3)
		return {};
	const double A = std::pow(10.0, gain_db / 40.0);
	const double w0 = 2.0 * 3.141592653589793 * clamp_freq(fs, f0) / fs;
	const double cw = std::cos(w0);
	const double alpha = std::sin(w0) / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
	const double sa = 2.0 * std::sqrt(A) * alpha;
	return normalized(A * ((A + 1.0) - (A - 1.0) * cw + sa), 2.0 * A * ((A - 1.0) - (A + 1.0) * cw),
			  A * ((A + 1.0) - (A - 1.0) * cw - sa), (A + 1.0) + (A - 1.0) * cw + sa,
			  -2.0 * ((A - 1.0) + (A + 1.0) * cw), (A + 1.0) + (A - 1.0) * cw - sa);
}

inline BiquadCoefs high_shelf(double fs, double f0, double gain_db, double slope = 1.0)
{
	if (std::fabs(gain_db) < 1e-3)
		return {};
	const double A = std::pow(10.0, gain_db / 40.0);
	const double w0 = 2.0 * 3.141592653589793 * clamp_freq(fs, f0) / fs;
	const double cw = std::cos(w0);
	const double alpha = std::sin(w0) / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
	const double sa = 2.0 * std::sqrt(A) * alpha;
	return normalized(A * ((A + 1.0) + (A - 1.0) * cw + sa), -2.0 * A * ((A - 1.0) + (A + 1.0) * cw),
			  A * ((A + 1.0) + (A - 1.0) * cw - sa), (A + 1.0) - (A - 1.0) * cw + sa,
			  2.0 * ((A - 1.0) - (A + 1.0) * cw), (A + 1.0) - (A - 1.0) * cw - sa);
}

inline BiquadCoefs high_pass(double fs, double f0, double q)
{
	const double w0 = 2.0 * 3.141592653589793 * clamp_freq(fs, f0) / fs;
	const double cw = std::cos(w0);
	const double alpha = std::sin(w0) / (2.0 * q);
	return normalized((1.0 + cw) / 2.0, -(1.0 + cw), (1.0 + cw) / 2.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

inline BiquadCoefs low_pass(double fs, double f0, double q)
{
	const double w0 = 2.0 * 3.141592653589793 * clamp_freq(fs, f0) / fs;
	const double cw = std::cos(w0);
	const double alpha = std::sin(w0) / (2.0 * q);
	return normalized((1.0 - cw) / 2.0, 1.0 - cw, (1.0 - cw) / 2.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
}

// Добротности секций фильтра Баттерворта 4-го порядка (24 дБ/окт).
constexpr double kButter4Q1 = 0.5411961;
constexpr double kButter4Q2 = 1.3065630;
constexpr double kButter2Q = 0.7071068;

} // namespace biquad

// Один биквад (транспонированная прямая форма II, состояние в double).
class Biquad {
public:
	void set(const BiquadCoefs &c) { c_ = c; }
	void reset() { z1_ = z2_ = 0.0; }
	float process(float x)
	{
		const double in = x;
		const double y = c_.b0 * in + z1_;
		z1_ = c_.b1 * in - c_.a1 * y + z2_;
		z2_ = c_.b2 * in - c_.a2 * y;
		return (float)y;
	}

private:
	BiquadCoefs c_;
	double z1_ = 0.0, z2_ = 0.0;
};

// Цепочка из фиксированного числа «слотов» на все каналы.
// Слот с единичными коэффициентами пропускается (ползунок на 0 дБ ничего не стоит).
class FilterChain {
public:
	static constexpr int kMaxSlots = 8;

	void set_slot(int slot, const BiquadCoefs &c)
	{
		if (slot < 0 || slot >= kMaxSlots)
			return;
		const bool was_active = active_[slot];
		active_[slot] = !c.is_identity();
		for (int ch = 0; ch < kMaxChannels; ch++) {
			biquads_[ch][slot].set(c);
			// При включении/выключении слота начинаем с чистого состояния.
			if (was_active != active_[slot])
				biquads_[ch][slot].reset();
		}
	}

	void reset()
	{
		for (int ch = 0; ch < kMaxChannels; ch++)
			for (int s = 0; s < kMaxSlots; s++)
				biquads_[ch][s].reset();
	}

	bool any_active() const
	{
		for (bool a : active_)
			if (a)
				return true;
		return false;
	}

	void process(float *const *ch, int nch, int frames)
	{
		for (int s = 0; s < kMaxSlots; s++) {
			if (!active_[s])
				continue;
			for (int c = 0; c < nch && c < kMaxChannels; c++) {
				Biquad &bq = biquads_[c][s];
				float *x = ch[c];
				for (int i = 0; i < frames; i++)
					x[i] = bq.process(x[i]);
			}
		}
	}

private:
	bool active_[kMaxSlots] = {};
	Biquad biquads_[kMaxChannels][kMaxSlots];
};

} // namespace vs
