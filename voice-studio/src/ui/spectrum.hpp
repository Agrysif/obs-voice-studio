#pragma once

// Спектр для живого блока: БПФ последних отсчётов -> плавные логарифмические полосы 20 Гц..20 кГц.

#include <algorithm>
#include <cmath>
#include <vector>

#include "fft.hpp"

namespace vsui {

class SpectrumAnalyzer {
public:
	// ~85 мс при 48 кГц: хватает разрешения на низах и спектр не мельтешит.
	static constexpr int kSize = 4096;

	SpectrumAnalyzer()
	{
		fft_.init(kSize);
		window_.resize(kSize);
		double sum = 0.0;
		for (int i = 0; i < kSize; i++) {
			window_[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * (float)i / (float)kSize);
			sum += window_[(size_t)i];
		}
		norm_ = (float)(2.0 / sum);
		buf_.resize(kSize);
		power_.resize(kSize / 2 + 1);
	}

	// samples — kSize последних отсчётов; out — заранее заданного размера (число полос).
	void compute(const float *samples, int sample_rate, std::vector<float> &out)
	{
		for (int i = 0; i < kSize; i++)
			buf_[(size_t)i] = vs::cfloat(samples[i] * window_[(size_t)i], 0.0f);
		fft_.forward(buf_.data());
		for (int k = 0; k <= kSize / 2; k++) {
			const float m = std::abs(buf_[(size_t)k]) * norm_;
			power_[(size_t)k] = m * m;
		}

		const int bands = (int)out.size();
		const double bin_hz = (double)sample_rate / kSize;
		const int max_bin = kSize / 2;
		for (int b = 0; b < bands; b++) {
			const double f0 = 20.0 * std::pow(1000.0, (double)b / bands);
			const double f1 = 20.0 * std::pow(1000.0, (double)(b + 1) / bands);
			const double k0 = f0 / bin_hz, k1 = f1 / bin_hz;
			double p;
			if (k1 - k0 < 2.0) {
				// Узкая полоса (низы): берём значение между соседними бинами, без «ступенек».
				const double kc = std::clamp(std::sqrt(k0 * k1), 1.0, (double)max_bin - 1.0);
				const int i = (int)kc;
				const double t = kc - i;
				p = power_[(size_t)i] * (1.0 - t) + power_[(size_t)i + 1] * t;
			} else {
				// Широкая полоса (верха): средняя мощность бинов внутри неё.
				const int a = std::max(1, (int)std::ceil(k0));
				const int c = std::min(max_bin, (int)std::floor(k1));
				double s = 0.0;
				for (int k = a; k <= c; k++)
					s += power_[(size_t)k];
				p = s / std::max(1, c - a + 1);
			}
			out[(size_t)b] = 10.0f * (float)std::log10(std::max(p, 1e-14));
		}
	}

private:
	vs::FFT fft_;
	std::vector<float> window_;
	std::vector<vs::cfloat> buf_;
	std::vector<float> power_;
	float norm_ = 1.0f;
};

} // namespace vsui
