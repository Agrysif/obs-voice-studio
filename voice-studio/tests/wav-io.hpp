#pragma once

// Минимальное чтение WAV для тестов (PCM 16/24 бит или float32, любое число каналов -> моно).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct Wav {
	int rate = 48000;
	std::vector<float> mono;
};

inline bool read_wav(const char *path, Wav &wav)
{
	FILE *f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::vector<uint8_t> d;
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		d.insert(d.end(), buf, buf + n);
	std::fclose(f);
	if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4))
		return false;

	int fmt = 0, channels = 0, bits = 0;
	size_t pos = 12;
	while (pos + 8 <= d.size()) {
		const uint32_t size = d[pos + 4] | (d[pos + 5] << 8) | (d[pos + 6] << 16) | ((uint32_t)d[pos + 7] << 24);
		const uint8_t *body = d.data() + pos + 8;
		if (!std::memcmp(d.data() + pos, "fmt ", 4)) {
			fmt = body[0] | (body[1] << 8);
			channels = body[2] | (body[3] << 8);
			wav.rate = (int)(body[4] | (body[5] << 8) | (body[6] << 16) | ((uint32_t)body[7] << 24));
			bits = body[14] | (body[15] << 8);
			if (fmt == 0xFFFE && size >= 26)
				fmt = body[24] | (body[25] << 8);
		} else if (!std::memcmp(d.data() + pos, "data", 4)) {
			if (channels <= 0 || bits <= 0)
				return false;
			const size_t bytes = size < d.size() - pos - 8 ? size : d.size() - pos - 8;
			const int bps = bits / 8;
			const size_t frames = bytes / (size_t)(bps * channels);
			wav.mono.resize(frames);
			for (size_t i = 0; i < frames; i++) {
				float s = 0.0f;
				for (int c = 0; c < channels; c++) {
					const uint8_t *p = body + (i * channels + c) * bps;
					if (fmt == 3 && bits == 32) {
						float v;
						std::memcpy(&v, p, 4);
						s += v;
					} else if (bits == 16) {
						s += (int16_t)(p[0] | (p[1] << 8)) / 32768.0f;
					} else if (bits == 24) {
						int32_t v = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
						s += (float)(v >> 8) / 8388608.0f;
					}
				}
				wav.mono[i] = s / (float)channels;
			}
			return true;
		}
		pos += 8 + size + (size & 1);
	}
	return false;
}
