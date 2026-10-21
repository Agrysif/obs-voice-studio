#pragma once

// Небольшое комплексное БПФ (radix-2) — для анализа спектра и смены высоты голоса.

#include <complex>
#include <vector>

#include "common.hpp"

namespace vs {

using cfloat = std::complex<float>;

class FFT {
public:
	void init(int n)
	{
		n_ = n;
		int bits = 0;
		while ((1 << bits) < n)
			bits++;
		rev_.assign(n, 0);
		for (int i = 0; i < n; i++) {
			int r = 0;
			for (int b = 0; b < bits; b++)
				if (i & (1 << b))
					r |= 1 << (bits - 1 - b);
			rev_[i] = r;
		}
		tw_.resize(n / 2);
		for (int k = 0; k < n / 2; k++) {
			const double a = -2.0 * 3.141592653589793 * k / n;
			tw_[k] = cfloat((float)std::cos(a), (float)std::sin(a));
		}
	}

	int size() const { return n_; }

	// Прямое преобразование без нормировки.
	void forward(cfloat *x) const { transform(x, false); }

	// Обратное преобразование с нормировкой 1/N.
	void inverse(cfloat *x) const
	{
		transform(x, true);
		const float s = 1.0f / (float)n_;
		for (int i = 0; i < n_; i++)
			x[i] *= s;
	}

private:
	void transform(cfloat *x, bool inv) const
	{
		for (int i = 0; i < n_; i++)
			if (i < rev_[i])
				std::swap(x[i], x[rev_[i]]);

		for (int len = 2; len <= n_; len <<= 1) {
			const int half = len >> 1;
			const int step = n_ / len;
			for (int i = 0; i < n_; i += len) {
				for (int j = 0; j < half; j++) {
					cfloat w = tw_[j * step];
					if (inv)
						w = std::conj(w);
					const cfloat u = x[i + j];
					const cfloat v = x[i + j + half] * w;
					x[i + j] = u + v;
					x[i + j + half] = u - v;
				}
			}
		}
	}

	int n_ = 0;
	std::vector<int> rev_;
	std::vector<cfloat> tw_;
};

} // namespace vs
