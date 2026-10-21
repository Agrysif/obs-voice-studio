#pragma once

// Общие мелочи для DSP: перевод дБ, сглаживание, FIFO-буфер.
// Весь код в папке dsp/ не зависит от OBS — его можно проверять отдельной
// тестовой программой (см. tests/offline-test.cpp).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define VS_HAVE_SSE 1
#endif

namespace vs {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kMaxChannels = 8;

inline float db_to_lin(float db)
{
	return std::pow(10.0f, db * 0.05f);
}

inline float lin_to_db(float lin)
{
	return 20.0f * std::log10(std::max(lin, 1e-9f));
}

inline float power_to_db(double power)
{
	return 10.0f * (float)std::log10(std::max(power, 1e-18));
}

inline float clampf(float v, float lo, float hi)
{
	return std::min(std::max(v, lo), hi);
}

// Коэффициент однополюсного сглаживания: за time_ms проходится ~63% пути до цели.
inline float smooth_coef(float time_ms, float sample_rate)
{
	if (time_ms <= 0.0f || sample_rate <= 0.0f)
		return 0.0f;
	return std::exp(-1.0f / (time_ms * 0.001f * sample_rate));
}

// Плавно меняющийся множитель громкости: ползунки не щёлкают.
class SmoothedValue {
public:
	void reset(float v) { cur_ = target_ = v; }
	void set_target(float v) { target_ = v; }
	void set_time(float time_ms, float sample_rate) { coef_ = smooth_coef(time_ms, sample_rate); }
	float next()
	{
		cur_ = target_ + (cur_ - target_) * coef_;
		return cur_;
	}
	bool settled() const { return std::fabs(cur_ - target_) < 1e-6f; }
	float value() const { return cur_; }
	float target() const { return target_; }

private:
	float cur_ = 1.0f;
	float target_ = 1.0f;
	float coef_ = 0.995f;
};

// Простой FIFO для отсчётов. Память перераспределяется только при росте,
// после «прогрева» работа идёт без выделений.
class SampleFifo {
public:
	void reserve(size_t n) { buf_.reserve(n); }
	void clear()
	{
		buf_.clear();
		rd_ = 0;
	}
	size_t size() const { return buf_.size() - rd_; }
	const float *data() const { return buf_.data() + rd_; }
	void push(const float *x, size_t n) { buf_.insert(buf_.end(), x, x + n); }
	void push(float x) { buf_.push_back(x); }
	void push_zeros(size_t n) { buf_.resize(buf_.size() + n, 0.0f); }

	// Достаёт n отсчётов; если данных меньше — дополняет нулями. Возвращает, сколько было реально.
	size_t pop(float *out, size_t n)
	{
		size_t avail = std::min(n, size());
		if (avail)
			std::memcpy(out, data(), avail * sizeof(float));
		if (avail < n)
			std::memset(out + avail, 0, (n - avail) * sizeof(float));
		discard(avail);
		return avail;
	}

	void discard(size_t n)
	{
		rd_ += std::min(n, size());
		if (rd_ == buf_.size()) {
			buf_.clear();
			rd_ = 0;
		} else if (rd_ > 4096 && rd_ * 2 > buf_.size()) {
			buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)rd_);
			rd_ = 0;
		}
	}

private:
	std::vector<float> buf_;
	size_t rd_ = 0;
};

// Включает flush-to-zero/denormals-are-zero на время обработки:
// без этого затухающие IIR-фильтры на тишине могут резко нагружать процессор.
class DenormalGuard {
public:
	DenormalGuard()
	{
#ifdef VS_HAVE_SSE
		saved_ = _mm_getcsr();
		_mm_setcsr(saved_ | 0x8040);
#endif
	}
	~DenormalGuard()
	{
#ifdef VS_HAVE_SSE
		_mm_setcsr(saved_);
#endif
	}
	DenormalGuard(const DenormalGuard &) = delete;
	DenormalGuard &operator=(const DenormalGuard &) = delete;

private:
#ifdef VS_HAVE_SSE
	unsigned int saved_ = 0;
#endif
};

} // namespace vs
