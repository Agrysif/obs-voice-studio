#pragma once

// Потоковый передискретизатор с фиксированным отношением частот
// (windowed-sinc, окно Кайзера, 32 отвода, 256 фаз с линейной интерполяцией).
// Нужен, чтобы нейросеть RNNoise (работает строго на 48 кГц) работала
// и при частоте OBS 44.1 кГц.

#include "common.hpp"

namespace vs {

class Resampler {
public:
	static constexpr int kHalf = 16;
	static constexpr int kTaps = 2 * kHalf;
	static constexpr int kPhases = 256;

	void init(double in_rate, double out_rate)
	{
		step_ = in_rate / out_rate;
		const double fc = std::min(1.0, out_rate / in_rate) * 0.95;
		const double beta = 8.0;
		table_.assign((size_t)(kPhases + 1) * kTaps, 0.0f);
		for (int p = 0; p <= kPhases; p++) {
			const double frac = (double)p / kPhases;
			double sum = 0.0;
			float *row = &table_[(size_t)p * kTaps];
			for (int k = 0; k < kTaps; k++) {
				const double x = frac - (double)(k - kHalf + 1); // расстояние до отсчёта
				const double r = x / kHalf;
				double w = 0.0;
				if (std::fabs(r) < 1.0)
					w = bessel_i0(beta * std::sqrt(1.0 - r * r)) / bessel_i0(beta);
				const double u = fc * x;
				const double sinc = std::fabs(u) < 1e-9 ? 1.0
									: std::sin(3.141592653589793 * u) /
										  (3.141592653589793 * u);
				const double h = fc * sinc * w;
				row[k] = (float)h;
				sum += h;
			}
			for (int k = 0; k < kTaps; k++)
				row[k] = (float)(row[k] / sum);
		}
		reset();
	}

	void reset()
	{
		// Слева — нули, чтобы первый выходной отсчёт совпадал по времени с первым входным.
		hist_.assign(kHalf - 1, 0.0f);
		pos_ = kHalf - 1;
	}

	// Добавляет в out столько выходных отсчётов, сколько можно вычислить.
	void process(const float *in, int n, SampleFifo &out)
	{
		hist_.insert(hist_.end(), in, in + n);
		while ((size_t)pos_ + kHalf < hist_.size()) {
			const int i = (int)pos_;
			const double phase = (pos_ - i) * kPhases;
			const int p0 = (int)phase;
			const float t = (float)(phase - p0);
			const float *c0 = &table_[(size_t)p0 * kTaps];
			const float *c1 = c0 + kTaps;
			const float *x = &hist_[(size_t)(i - kHalf + 1)];
			float y = 0.0f;
			for (int k = 0; k < kTaps; k++)
				y += x[k] * (c0[k] + t * (c1[k] - c0[k]));
			out.push(y);
			pos_ += step_;
		}
		const int drop = (int)pos_ - kHalf + 1;
		if (drop > 0) {
			hist_.erase(hist_.begin(), hist_.begin() + drop);
			pos_ -= drop;
		}
	}

private:
	static double bessel_i0(double x)
	{
		double sum = 1.0, term = 1.0;
		for (int k = 1; k < 32; k++) {
			term *= (x / (2.0 * k)) * (x / (2.0 * k));
			sum += term;
		}
		return sum;
	}

	double step_ = 1.0;
	double pos_ = 0.0;
	std::vector<float> table_;
	std::vector<float> hist_;
};

} // namespace vs
