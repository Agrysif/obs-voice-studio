#pragma once

// Общее состояние одного фильтра для аудиопотока, окна свойств OBS и красивой панели.
// Живёт в shared_ptr: панель может держать его и после удаления фильтра.

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace vs {

struct SharedState {
	// --- «Умная настройка»
	std::atomic<int> calib_request{0}; // 1 — начать, 2 — отменить
	std::atomic<bool> calibrating{false};
	std::atomic<float> calib_seconds_left{0.0f};
	std::atomic<int> calib_generation{0}; // растёт после каждого результата

	void set_status(const std::string &text, int type)
	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		status_ = text;
		status_type_ = type;
	}
	std::string get_status(int &type)
	{
		std::lock_guard<std::mutex> lock(status_mutex_);
		type = status_type_;
		return status_;
	}

	// --- Индикаторы (пишет аудиопоток, читает интерфейс)
	std::atomic<float> in_peak{-120.0f};  // пик с прошлого чтения
	std::atomic<float> out_peak{-120.0f}; // пик с прошлого чтения
	std::atomic<float> in_rms{-120.0f};
	std::atomic<float> out_rms{-120.0f};
	std::atomic<float> comp_gr{0.0f};
	std::atomic<float> gate_db{0.0f};
	std::atomic<float> vad{-1.0f};
	std::atomic<int> sample_rate{48000};

	static void raise_max(std::atomic<float> &a, float v)
	{
		float cur = a.load(std::memory_order_relaxed);
		while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
		}
	}

	// --- Последние отсчёты выхода для спектра (только пока открыта панель)
	static constexpr int kRing = 4096;
	std::atomic<int> ui_listeners{0};

	void push_ring(const float *x, int n)
	{
		std::unique_lock<std::mutex> lock(ring_mutex_, std::try_to_lock);
		if (!lock.owns_lock())
			return; // аудиопоток никогда не ждёт интерфейс
		for (int i = 0; i < n; i++) {
			ring_[ring_pos_] = x[i];
			ring_pos_ = (ring_pos_ + 1) % kRing;
		}
	}

	void copy_ring(float *out, int n)
	{
		std::lock_guard<std::mutex> lock(ring_mutex_);
		int pos = (ring_pos_ - n + kRing * 2) % kRing;
		for (int i = 0; i < n; i++) {
			out[i] = ring_[pos];
			pos = (pos + 1) % kRing;
		}
	}

private:
	std::mutex status_mutex_;
	std::string status_;
	int status_type_ = 0;

	std::mutex ring_mutex_;
	float ring_[kRing] = {};
	int ring_pos_ = 0;
};

} // namespace vs
