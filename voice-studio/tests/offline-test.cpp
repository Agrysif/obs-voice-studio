// Офлайн-проверка цепочки обработки без OBS.
//
//   voice-studio-test process <in.wav> <out.wav> [noise_db] [pitch] [formant]
//       — прогнать WAV через полную цепочку (по умолчанию «Естественный» стиль)
//   voice-studio-test calibrate <in.wav> [noise_db]
//       — «Умная настройка» на записи: что измерено и что предложено
//   voice-studio-test latency <in.wav>
//       — проверить, что задержка «сухого» и очищенного сигнала совпадает
//   voice-studio-test bench <in.wav>
//       — нагрузка на процессор для разных режимов
//
// noise_db — добавить к записи шум вентилятора с таким уровнем (дБFS), например -40.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "voice-chain.hpp"
#include "wav-io.hpp"

namespace {

bool write_wav(const char *path, int rate, const std::vector<float> &l, const std::vector<float> &r)
{
	FILE *f = std::fopen(path, "wb");
	if (!f)
		return false;
	const uint32_t frames = (uint32_t)l.size();
	const uint32_t data_bytes = frames * 2 * 2;
	auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f);
	u32(36 + data_bytes);
	std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16);
	u16(1);
	u16(2);
	u32((uint32_t)rate);
	u32((uint32_t)rate * 4);
	u16(4);
	u16(16);
	std::fwrite("data", 1, 4, f);
	u32(data_bytes);
	for (uint32_t i = 0; i < frames; i++) {
		int16_t s[2] = {(int16_t)vs::clampf(l[i] * 32767.0f, -32768.0f, 32767.0f),
				(int16_t)vs::clampf(r[i] * 32767.0f, -32768.0f, 32767.0f)};
		std::fwrite(s, 2, 2, f);
	}
	std::fclose(f);
	return true;
}

// Шум «вентилятора»: розовый шум + гул 50 Гц.
void add_noise(std::vector<float> &x, int rate, float noise_db)
{
	if (noise_db <= -120.0f)
		return;
	std::mt19937 rng(12345);
	std::normal_distribution<float> nd(0.0f, 1.0f);
	float b0 = 0, b1 = 0, b2 = 0;
	std::vector<float> n(x.size());
	double sq = 0.0;
	for (size_t i = 0; i < x.size(); i++) {
		const float w = nd(rng);
		b0 = 0.99765f * b0 + w * 0.0990460f;
		b1 = 0.96300f * b1 + w * 0.2965164f;
		b2 = 0.57000f * b2 + w * 1.0526913f;
		float v = b0 + b1 + b2 + w * 0.1848f;
		v += 0.6f * std::sin(2.0f * vs::kPi * 50.0f * (float)i / (float)rate);
		n[i] = v;
		sq += (double)v * v;
	}
	const float scale = vs::db_to_lin(noise_db) / (float)std::sqrt(sq / x.size());
	for (size_t i = 0; i < x.size(); i++)
		x[i] += n[i] * scale;
}

struct RunResult {
	std::vector<float> l, r;
	double seconds = 0.0;
	bool finite = true;
	float peak = 0.0f;
};

RunResult run(vs::VoiceChain &chain, const std::vector<float> &in, int rate, int block = 480, bool stereo_diff = false)
{
	RunResult res;
	res.l.resize(in.size());
	res.r.resize(in.size());
	std::vector<float> a(block), b(block);
	const auto t0 = std::chrono::steady_clock::now();
	for (size_t pos = 0; pos < in.size(); pos += block) {
		const int n = (int)std::min<size_t>(block, in.size() - pos);
		for (int i = 0; i < n; i++) {
			a[i] = in[pos + i];
			b[i] = stereo_diff ? in[pos + i] * 0.8f : in[pos + i];
		}
		float *planes[2] = {a.data(), b.data()};
		{
			vs::DenormalGuard g;
			chain.process(planes, 2, n, false);
		}
		for (int i = 0; i < n; i++) {
			res.l[pos + i] = a[i];
			res.r[pos + i] = b[i];
			if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
				res.finite = false;
			res.peak = std::max(res.peak, std::max(std::fabs(a[i]), std::fabs(b[i])));
		}
	}
	res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	(void)rate;
	return res;
}

// Сдвиг, при котором out лучше всего совпадает с in.
int best_lag(const std::vector<float> &in, const std::vector<float> &out, int max_lag)
{
	int best = 0;
	double best_v = -1e300;
	const size_t start = (size_t)max_lag;
	const size_t len = std::min<size_t>(in.size(), out.size()) - (size_t)max_lag * 2;
	for (int lag = 0; lag <= max_lag; lag++) {
		double s = 0.0, e = 0.0;
		for (size_t i = start; i < start + len; i++) {
			s += (double)in[i] * out[i + lag];
			e += (double)out[i + lag] * out[i + lag];
		}
		const double v = e > 0 ? s / std::sqrt(e) : 0.0;
		if (v > best_v) {
			best_v = v;
			best = lag;
		}
	}
	return best;
}

vs::ChainParams minimal_params()
{
	vs::ChainParams p;
	p.eq_on = false;
	p.gate_on = false;
	p.deess_on = false;
	p.comp_on = false;
	p.warmth_on = false;
	p.pitch_on = false;
	p.limiter_on = false;
	return p;
}

void print_calibration(const vs::CalibrationResult &r)
{
	std::printf("status=%d voice=%.1f dB noise=%.1f dB snr=%.1f peak=%.1f dyn=%.1f sib=%.1f clip=%d\n", r.status,
		    r.voice_db, r.noise_db, r.snr_db, r.peak_db, r.dynamics_db, r.sibilance_db, r.clipping);
	std::printf("frames: voice=%d noise=%d\n", r.voice_frames, r.noise_frames);
	std::printf("-> gain=%+.1f denoise=%.0f gate_thr=%.0f comp=%.0f deess=%.0f hpf=%.0f\n", r.input_gain_db,
		    r.denoise_amount, r.gate_threshold_db, r.comp_amount, r.deess_amount, r.hpf_freq);
	std::printf("-> eq(valid=%d): low=%+.1f mud=%+.1f nasal=%+.1f pres=%+.1f air=%+.1f\n", r.eq_valid, r.eq_low,
		    r.eq_mud, r.eq_nasal, r.eq_pres, r.eq_air);
}

// Частота основного тона по переходам через ноль.
float zero_cross_freq(const std::vector<float> &x, int rate, size_t from)
{
	int crossings = 0;
	size_t first = 0, last = 0;
	for (size_t i = from + 1; i < x.size(); i++) {
		if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
			if (!crossings)
				first = i;
			last = i;
			crossings++;
		}
	}
	return crossings > 1 ? (float)(crossings - 1) * rate / (float)(last - first) : 0.0f;
}

int selftest()
{
	int failures = 0;
	auto check = [&](bool ok, const char *what, double value) {
		std::printf("[%s] %s: %.2f\n", ok ? " OK " : "FAIL", what, value);
		if (!ok)
			failures++;
	};
	const int rate = 48000;
	std::vector<float> sine(rate * 3);
	for (size_t i = 0; i < sine.size(); i++)
		sine[i] = 0.3f * std::sin(2.0f * vs::kPi * 440.0f * (float)i / rate);

	// Смена высоты: +12 и -12 полутонов (форманты едут вместе с тоном).
	for (float st : {12.0f, -12.0f, 3.0f}) {
		vs::ChainParams p = minimal_params();
		p.denoise_on = false;
		p.pitch_on = true;
		p.pitch_semitones = st;
		p.formant_preserve = false;
		vs::VoiceChain chain;
		chain.set_params(p);
		chain.configure(rate, 2);
		RunResult r = run(chain, sine, rate, 480);
		const float f = zero_cross_freq(r.l, rate, rate);
		const float expect = 440.0f * std::pow(2.0f, st / 12.0f);
		char what[96];
		std::snprintf(what, sizeof(what), "pitch %+.0f st: frequency (expect %.0f Hz)", st, expect);
		check(std::fabs(f - expect) < expect * 0.01f, what, f);
	}

	// Лимитер: синус +6 дБFS не должен превысить потолок -1 дБFS.
	{
		std::vector<float> loud(sine.size());
		for (size_t i = 0; i < loud.size(); i++)
			loud[i] = sine[i] / 0.3f * 2.0f;
		vs::ChainParams p = minimal_params();
		p.denoise_on = false;
		p.limiter_on = true;
		p.limiter_ceiling_db = -1.0f;
		vs::VoiceChain chain;
		chain.set_params(p);
		chain.configure(rate, 2);
		RunResult r = run(chain, loud, rate, 480);
		check(r.peak <= vs::db_to_lin(-1.0f) + 1e-4f, "limiter peak, dBFS (<= -1)", vs::lin_to_db(r.peak));
	}

	// Эквалайзер: +6 дБ «Чёткости» на 4 кГц.
	{
		std::vector<float> tone(rate * 2);
		for (size_t i = 0; i < tone.size(); i++)
			tone[i] = 0.1f * std::sin(2.0f * vs::kPi * 4000.0f * (float)i / rate);
		vs::ChainParams p = minimal_params();
		p.denoise_on = false;
		p.eq_on = true;
		p.hpf_on = false;
		p.mud_gain = p.pres_gain = p.air_gain = 0.0f;
		p.pres_gain = 6.0f;
		vs::VoiceChain chain;
		chain.set_params(p);
		chain.configure(rate, 2);
		RunResult r = run(chain, tone, rate, 480);
		check(std::fabs(vs::lin_to_db(r.peak / 0.1f) - 6.0f) < 0.3f, "EQ presence +6 dB at 4 kHz, dB",
		      vs::lin_to_db(r.peak / 0.1f));
	}

	// Тишина на входе — тишина на выходе, без NaN.
	{
		std::vector<float> silence(rate * 2, 0.0f);
		vs::ChainParams p;
		p.pitch_on = true;
		p.pitch_semitones = -2.0f;
		p.warmth_on = true;
		vs::VoiceChain chain;
		chain.set_params(p);
		chain.configure(rate, 2);
		RunResult r = run(chain, silence, rate, 1024);
		check(r.finite && r.peak < 1e-4f, "silence in -> peak out", r.peak);
	}

	std::printf("%s\n", failures ? "SELFTEST FAILED" : "SELFTEST PASSED");
	return failures ? 3 : 0;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc >= 2 && std::string(argv[1]) == "selftest")
		return selftest();
	if (argc < 3) {
		std::printf("usage: %s selftest | process|calibrate|latency|bench <in.wav> ...\n", argv[0]);
		return 1;
	}
	const std::string mode = argv[1];
	Wav wav;
	if (!read_wav(argv[2], wav)) {
		std::printf("cannot read %s\n", argv[2]);
		return 1;
	}
	std::printf("input: %s, %d Hz, %.1f s\n", argv[2], wav.rate, wav.mono.size() / (double)wav.rate);

	if (mode == "process") {
		if (argc < 4)
			return 1;
		const float noise_db = argc > 4 ? (float)std::atof(argv[4]) : -200.0f;
		const std::vector<float> clean = wav.mono;
		add_noise(wav.mono, wav.rate, noise_db);
		vs::ChainParams p;
		if (argc > 5) {
			p.pitch_on = true;
			p.pitch_semitones = (float)std::atof(argv[5]);
			p.formant_semitones = argc > 6 ? (float)std::atof(argv[6]) : 0.0f;
		}
		vs::VoiceChain chain;
		chain.set_params(p);
		chain.configure(wav.rate, 2);
		RunResult r = run(chain, wav.mono, wav.rate, 1024);
		std::printf("finite=%d peak=%.3f (%.1f dBFS) latency=%d samples, realtime x%.0f\n", r.finite, r.peak,
			    vs::lin_to_db(r.peak), chain.latency(), (wav.mono.size() / (double)wav.rate) / r.seconds);

		// Шум в паузах и громкость речи до/после (паузы определяем по чистой записи).
		const int lat = chain.latency();
		const int fl = wav.rate / 100;
		double pin = 0, pout = 0, sin_ = 0, sout = 0;
		int np = 0, ns = 0;
		for (size_t pos = 0; pos + fl + lat < clean.size(); pos += fl) {
			double c = 0, a = 0, b = 0;
			for (int i = 0; i < fl; i++) {
				c += (double)clean[pos + i] * clean[pos + i];
				a += (double)wav.mono[pos + i] * wav.mono[pos + i];
				b += (double)r.l[pos + i + lat] * r.l[pos + i + lat];
			}
			const float cdb = vs::power_to_db(c / fl);
			if (cdb < -70.0f) {
				pin += a / fl;
				pout += b / fl;
				np++;
			} else if (cdb > -35.0f) {
				sin_ += a / fl;
				sout += b / fl;
				ns++;
			}
		}
		if (np && ns)
			std::printf("noise in pauses: %.1f -> %.1f dB; speech: %.1f -> %.1f dB (%d pause / %d speech frames)\n",
				    vs::power_to_db(pin / np), vs::power_to_db(pout / np), vs::power_to_db(sin_ / ns),
				    vs::power_to_db(sout / ns), np, ns);
		std::vector<float> noisy = wav.mono;
		write_wav(argv[3], wav.rate, r.l, r.r);
		const std::string orig = std::string(argv[3]) + ".input.wav";
		write_wav(orig.c_str(), wav.rate, noisy, noisy);
		return r.finite ? 0 : 2;
	}

	if (mode == "calibrate") {
		const float noise_db = argc > 3 ? (float)std::atof(argv[3]) : -200.0f;
		add_noise(wav.mono, wav.rate, noise_db);
		vs::VoiceChain chain;
		chain.set_params(vs::ChainParams());
		chain.configure(wav.rate, 2);
		const float secs = std::min(10.0f, (float)(wav.mono.size() / (double)wav.rate) - 0.5f);
		chain.start_calibration(secs);
		std::vector<float> a(480), b(480);
		bool done = false;
		for (size_t pos = 0; pos + 480 <= wav.mono.size() && !done; pos += 480) {
			std::copy(wav.mono.begin() + pos, wav.mono.begin() + pos + 480, a.begin());
			b = a;
			float *planes[2] = {a.data(), b.data()};
			chain.process(planes, 2, 480, false);
			done = chain.take_calibration_finished();
		}
		std::printf("finished=%d\n", done);
		print_calibration(chain.calibration_result());
		return done ? 0 : 2;
	}

	if (mode == "latency") {
		for (int rate_case = 0; rate_case < 2; rate_case++) {
			std::vector<float> in = wav.mono;
			int rate = wav.rate;
			if (rate_case == 1) {
				// Проверяем путь с передискретизацией: «переименовываем» частоту в 44.1 кГц.
				rate = 44100;
			}
			for (float mix : {0.0f, 1.0f}) {
				vs::ChainParams p = minimal_params();
				p.denoise_amount = mix * 100.0f;
				vs::VoiceChain chain;
				chain.set_params(p);
				chain.configure(rate, 2);
				RunResult r = run(chain, in, rate, 441);
				const int lag = best_lag(in, r.l, 4000);
				std::printf("rate=%d mix=%.0f: measured lag=%d, reported latency=%d\n", rate, mix, lag,
					    chain.latency());
			}
			// Сравнение с оригиналом должно совпадать по времени с обработкой.
			vs::ChainParams p = minimal_params();
			p.bypass = true;
			vs::VoiceChain chain;
			chain.set_params(p);
			chain.configure(rate, 2);
			RunResult r = run(chain, in, rate, 441);
			std::printf("rate=%d bypass: measured lag=%d, reported latency=%d\n", rate, best_lag(in, r.l, 4000),
				    chain.latency());
		}
		// Смена голоса: задержка
		vs::ChainParams p = minimal_params();
		p.denoise_on = false;
		p.pitch_on = true;
		p.formant_semitones = 0.05f; // почти тождественное преобразование
		p.pitch_semitones = 0.0f;
		vs::VoiceChain chain;
		chain.set_params(p);
		chain.configure(wav.rate, 2);
		RunResult r = run(chain, wav.mono, wav.rate, 480);
		std::printf("pitch: measured lag=%d, reported latency=%d\n", best_lag(wav.mono, r.l, 6000), chain.latency());
		return 0;
	}

	if (mode == "bench") {
		struct Case {
			const char *name;
			vs::ChainParams p;
			bool stereo_diff;
		};
		std::vector<Case> cases;
		cases.push_back({"natural (mono mic)", vs::ChainParams(), false});
		cases.push_back({"natural (true stereo)", vs::ChainParams(), true});
		vs::ChainParams pitch;
		pitch.pitch_on = true;
		pitch.pitch_semitones = -3.0f;
		cases.push_back({"natural + pitch -3", pitch, false});
		vs::ChainParams all = pitch;
		all.warmth_on = true;
		all.formant_semitones = -2.0f;
		cases.push_back({"everything, true stereo", all, true});
		const double audio_sec = wav.mono.size() / (double)wav.rate;
		for (Case &c : cases) {
			vs::VoiceChain chain;
			chain.set_params(c.p);
			chain.configure(wav.rate, 2);
			RunResult r = run(chain, wav.mono, wav.rate, 480, c.stereo_diff);
			std::printf("%-26s CPU %.2f%% of one core (finite=%d)\n", c.name, 100.0 * r.seconds / audio_sec,
				    r.finite);
		}
		return 0;
	}

	std::printf("unknown mode\n");
	return 1;
}
