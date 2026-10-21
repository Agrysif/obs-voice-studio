#pragma once

// Связь живого блока с фильтром. Блок знает только этот интерфейс, поэтому его можно
// показать и без OBS (просмотр с «игрушечными» данными в tests/ui-preview.cpp).

#include <QString>

#include <vector>

namespace vsui {

struct LiveMeters {
	bool valid = false;
	float in_peak = -120.0f, in_rms = -120.0f;
	float out_peak = -120.0f, out_rms = -120.0f;
	float comp_gr = 0.0f, gate_db = 0.0f, vad = -1.0f;
};

class LiveLink {
public:
	virtual ~LiveLink() = default;

	// Перевод строки интерфейса по ключу без префикса «VoiceStudio.».
	virtual QString text(const char *key) = 0;

	virtual double get_double(const char *key) = 0;
	virtual bool get_bool(const char *key) = 0;
	virtual long long get_int(const char *key) = 0;
	virtual void set_double(const char *key, double v) = 0;
	// Отправить изменения в фильтр; final — пользователь отпустил мышь
	// (тогда обновляются и обычные ползунки в окне фильтра).
	virtual void commit(bool final) = 0;

	virtual bool enabled() = 0; // фильтр включён (значок «глаза»)
	virtual LiveMeters meters() = 0;
	virtual void spectrum(std::vector<float> &bands_db) = 0;
	virtual int sample_rate() = 0;
	virtual void set_active(bool active) = 0; // блок виден — нужен спектр

	// Вышла новая версия плагина: «⬆ Доступна версия …» (пусто — обновлений нет).
	virtual QString update_badge() { return QString(); }
	virtual void open_update() {}
};

} // namespace vsui
