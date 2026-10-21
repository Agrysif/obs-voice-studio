#include "denoiser.hpp"

#include <rnnoise.h>

namespace vs {

Denoiser::~Denoiser()
{
	free_states();
}

void Denoiser::free_states()
{
	for (Channel &c : ch_) {
		if (c.st) {
			rnnoise_destroy(c.st);
			c.st = nullptr;
		}
	}
}

void Denoiser::configure(int sample_rate, int channels)
{
	sample_rate_ = sample_rate;
	channels_ = std::min(channels, kMaxChannels);
	resample_ = sample_rate_ != kRate;

	if (resample_) {
		const int frame_at_host = (kFrame * sample_rate_ + kRate - 1) / kRate;
		prefill_ = frame_at_host + 2 * Resampler::kTaps + 8;
		// Передискретизатор не сдвигает сигнал во времени — задержка только от буфера и модели.
		latency_ = prefill_ + (kModelDelay * sample_rate_ + kRate / 2) / kRate;
	} else {
		prefill_ = kFrame;
		latency_ = prefill_ + kModelDelay;
	}

	free_states();
	for (int i = 0; i < channels_; i++) {
		ch_[i].st = rnnoise_create(nullptr);
		if (resample_) {
			ch_[i].up.init(sample_rate_, kRate);
			ch_[i].down.init(kRate, sample_rate_);
		}
		init_channel(ch_[i]);
	}
}

void Denoiser::init_channel(Channel &c)
{
	c.pending.clear();
	c.pending.reserve(4 * kFrame);
	c.out.clear();
	c.out.reserve(16384);
	c.out.push_zeros((size_t)prefill_);
	c.dry.assign(kModelDelay, 0.0f);
	c.dry_pos = 0;
	if (resample_) {
		c.up.reset();
		c.down.reset();
	}
}

void Denoiser::reset()
{
	for (int i = 0; i < channels_; i++) {
		if (ch_[i].st)
			rnnoise_init(ch_[i].st, nullptr);
		init_channel(ch_[i]);
	}
	vad_ = 0.0f;
}

void Denoiser::copy_channel_state(int from, int to)
{
	if (from == to || from < 0 || to < 0 || from >= channels_ || to >= channels_)
		return;
	Channel &src = ch_[from];
	Channel &dst = ch_[to];
	if (src.st && dst.st)
		std::memcpy(dst.st, src.st, (size_t)rnnoise_get_size());
	dst.pending = src.pending;
	dst.out = src.out;
	dst.dry = src.dry;
	dst.dry_pos = src.dry_pos;
	dst.up = src.up;
	dst.down = src.down;
}

void Denoiser::process(float *const *ch, int nch, int frames, float mix, bool mono)
{
	nch = std::min(nch, channels_);
	if (nch <= 0 || frames <= 0)
		return;

	float vad = 0.0f;
	const int count = mono ? 1 : nch;
	for (int i = 0; i < count; i++) {
		if (!ch_[i].st)
			continue;
		process_channel(ch_[i], ch[i], frames, mix, vad);
	}
	if (mono) {
		for (int i = 1; i < nch; i++)
			std::memcpy(ch[i], ch[0], (size_t)frames * sizeof(float));
	}
	vad_ = vad;
}

void Denoiser::process_channel(Channel &c, float *x, int frames, float mix, float &vad)
{
	if (resample_)
		c.up.process(x, frames, c.pending);
	else
		c.pending.push(x, (size_t)frames);

	float in[kFrame];
	float out[kFrame];
	float mixed[kFrame];
	while (c.pending.size() >= (size_t)kFrame) {
		const float *src = c.pending.data();
		for (int i = 0; i < kFrame; i++)
			in[i] = src[i] * 32768.0f; // RNNoise ждёт масштаб 16-битного PCM

		const float frame_vad = rnnoise_process_frame(c.st, out, in);
		vad = std::max(vad, frame_vad);

		// Смешиваем очищенный звук с исходным, задержанным на ту же величину.
		for (int i = 0; i < kFrame; i++) {
			const float dry = c.dry[c.dry_pos];
			c.dry[c.dry_pos] = src[i];
			if (++c.dry_pos == c.dry.size())
				c.dry_pos = 0;
			const float wet = out[i] * (1.0f / 32768.0f);
			mixed[i] = dry + mix * (wet - dry);
		}
		c.pending.discard(kFrame);

		if (resample_)
			c.down.process(mixed, kFrame, c.out);
		else
			c.out.push(mixed, kFrame);
	}

	c.out.pop(x, (size_t)frames);
}

} // namespace vs
