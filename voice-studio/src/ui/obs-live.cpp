// Живой блок в окне фильтра OBS: связь с настоящим фильтром и встраивание.

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QEventLoop>
#include <QFormLayout>
#include <QImage>
#include <QLabel>
#include <QTimer>

#include <algorithm>
#include <map>
#include <mutex>

#include "live-block.hpp"
#include "obs-live.hpp"
#include "props-injector.hpp"
#include "settings.hpp"
#include "spectrum.hpp"
#include "update/updater.hpp"
#include "voice-filter.hpp"

using namespace vs::key;

namespace vsui {

namespace {

struct SourceRef {
	obs_source_t *s = nullptr;
	explicit SourceRef(obs_source_t *src) : s(src) {}
	~SourceRef() { obs_source_release(s); }
	SourceRef(const SourceRef &) = delete;
	SourceRef &operator=(const SourceRef &) = delete;
	explicit operator bool() const { return s != nullptr; }
};

struct DataRef {
	obs_data_t *d;
	explicit DataRef(obs_data_t *data) : d(data) {}
	~DataRef() { obs_data_release(d); }
	DataRef(const DataRef &) = delete;
	DataRef &operator=(const DataRef &) = delete;
};

class ObsFilterLink : public LiveLink {
public:
	explicit ObsFilterLink(obs_source_t *filter)
		: weak_(obs_source_get_weak_source(filter)),
		  shared_(vs::find_shared_state(filter))
	{
	}
	~ObsFilterLink() override
	{
		set_active(false);
		obs_weak_source_release(weak_);
	}

	QString text(const char *key) override
	{
		const std::string k = std::string("VoiceStudio.") + key;
		return QString::fromUtf8(obs_module_text(k.c_str()));
	}

	double get_double(const char *key) override
	{
		SourceRef f(strong());
		if (!f)
			return 0.0;
		DataRef s(obs_source_get_settings(f.s));
		return obs_data_get_double(s.d, key);
	}
	bool get_bool(const char *key) override
	{
		SourceRef f(strong());
		if (!f)
			return false;
		DataRef s(obs_source_get_settings(f.s));
		return obs_data_get_bool(s.d, key);
	}
	long long get_int(const char *key) override
	{
		SourceRef f(strong());
		if (!f)
			return 0;
		DataRef s(obs_source_get_settings(f.s));
		return obs_data_get_int(s.d, key);
	}
	void set_double(const char *key, double v) override
	{
		SourceRef f(strong());
		if (!f)
			return;
		DataRef s(obs_source_get_settings(f.s));
		obs_data_set_double(s.d, key, v);
	}

	void commit(bool final) override
	{
		SourceRef f(strong());
		if (!f)
			return;
		obs_source_update(f.s, nullptr);
		// Отпустили мышь — обычные ползунки ниже должны показать новые значения.
		// Важно: OBS перестраивает окно свойств синхронно и сразу удаляет старые виджеты,
		// включая этот блок. Поэтому просим обновить окно только после того, как
		// обработчик мыши полностью закончит работу (иначе — обращение к удалённому объекту).
		if (final && !refresh_pending_->exchange(true)) {
			obs_weak_source_t *w = obs_source_get_weak_source(f.s);
			auto pending = refresh_pending_;
			QTimer::singleShot(0, qApp, [w, pending] {
				pending->store(false);
				if (obs_source_t *s = obs_weak_source_get_source(w)) {
					obs_source_update_properties(s);
					obs_source_release(s);
				}
				obs_weak_source_release(w);
			});
		}
	}

	bool enabled() override
	{
		SourceRef f(strong());
		return f && obs_source_enabled(f.s);
	}

	LiveMeters meters() override
	{
		LiveMeters m;
		if (!shared_)
			return m;
		vs::SharedState &sh = *shared_;
		m.valid = true;
		m.in_peak = sh.in_peak.exchange(-120.0f);
		m.out_peak = sh.out_peak.exchange(-120.0f);
		m.in_rms = sh.in_rms.load();
		m.out_rms = sh.out_rms.load();
		m.comp_gr = sh.comp_gr.load();
		m.gate_db = sh.gate_db.load();
		m.vad = sh.vad.load();
		if (m.in_peak <= -119.0f) {
			// Звук не приходит — не показываем старые уровни.
			m.in_rms = m.out_rms = -120.0f;
			m.comp_gr = m.gate_db = 0.0f;
			m.vad = -1.0f;
		}
		return m;
	}

	void spectrum(std::vector<float> &bands_db) override
	{
		if (!shared_) {
			std::fill(bands_db.begin(), bands_db.end(), -90.0f);
			return;
		}
		float buf[SpectrumAnalyzer::kSize];
		shared_->copy_ring(buf, SpectrumAnalyzer::kSize);
		analyzer_.compute(buf, shared_->sample_rate.load(), bands_db);
	}

	int sample_rate() override { return shared_ ? shared_->sample_rate.load() : 48000; }

	void set_active(bool active) override
	{
		if (active == active_ || !shared_)
			return;
		active_ = active;
		shared_->ui_listeners += active ? 1 : -1;
	}

	QString update_badge() override { return vsup::update_badge(); }
	void open_update() override { vsup::open_update_dialog(); }

private:
	obs_source_t *strong() const { return weak_ ? obs_weak_source_get_source(weak_) : nullptr; }

	obs_weak_source_t *weak_;
	std::shared_ptr<vs::SharedState> shared_;
	bool active_ = false;
	// Переживает сам блок: обновление окна выполняется уже после его удаления.
	std::shared_ptr<std::atomic<bool>> refresh_pending_ = std::make_shared<std::atomic<bool>>(false);
	SpectrumAnalyzer analyzer_;
};

// Номера меток -> фильтры. Окно свойств пересоздаётся часто, старые номера вытесняются.
std::mutex g_tokens_mutex;
std::map<int, obs_weak_source_t *> g_tokens;
int g_next_token = 1;
constexpr size_t kMaxTokens = 64;

int register_token(obs_source_t *filter)
{
	std::lock_guard<std::mutex> lock(g_tokens_mutex);
	const int token = g_next_token;
	g_next_token = g_next_token % 60000 + 1;
	if (auto it = g_tokens.find(token); it != g_tokens.end()) {
		obs_weak_source_release(it->second);
		g_tokens.erase(it);
	}
	g_tokens[token] = obs_source_get_weak_source(filter);
	while (g_tokens.size() > kMaxTokens) {
		obs_weak_source_release(g_tokens.begin()->second);
		g_tokens.erase(g_tokens.begin());
	}
	return token;
}

obs_source_t *source_for_token(int token)
{
	std::lock_guard<std::mutex> lock(g_tokens_mutex);
	auto it = g_tokens.find(token);
	return it == g_tokens.end() ? nullptr : obs_weak_source_get_source(it->second);
}

void release_tokens()
{
	std::lock_guard<std::mutex> lock(g_tokens_mutex);
	for (auto &it : g_tokens)
		obs_weak_source_release(it.second);
	g_tokens.clear();
}

std::string live_marker(obs_source_t *filter)
{
	return make_marker(register_token(filter)).toStdString();
}

QWidget *create_block(int token, QWidget *parent)
{
	obs_source_t *filter = source_for_token(token);
	if (!filter)
		return nullptr;
	auto link = std::make_shared<ObsFilterLink>(filter);
	obs_source_release(filter);
	return new LiveBlock(link, parent);
}

bool g_registered = false;

} // namespace

void register_live_block()
{
	// Только в настоящем окне OBS (в тестах на чистом libobs интерфейса нет).
	if (!qApp || !obs_frontend_get_main_window())
		return;
	install_props_injector(create_block);
	vs::set_live_marker_hook(live_marker);
	g_registered = true;
}

void unregister_live_block()
{
	vs::set_live_marker_hook(nullptr);
	if (g_registered)
		uninstall_props_injector();
	release_tokens();
	g_registered = false;
}

} // namespace vsui

// Самопроверка живого блока на настоящем фильтре — для теста на чистом libobs
// (tests/obs-host-test.cpp). Возвращает отчёт строками «[ OK ] …» / «[FAIL] …».
extern "C" __declspec(dllexport) const char *voice_studio_debug_live_selftest(const char *png_path)
{
	using namespace vsui;
	static std::string report;
	report.clear();
	auto check = [](bool ok, const char *what) {
		report += ok ? "[ OK ] " : "[FAIL] ";
		report += what;
		report += "\n";
	};
	auto wait = [](int ms) {
		QEventLoop loop;
		QTimer::singleShot(ms, &loop, &QEventLoop::quit);
		loop.exec();
	};
	if (!qApp) {
		check(false, "живой блок: нет QApplication");
		return report.c_str();
	}

	std::vector<vs::FilterInstance> list = vs::list_filter_instances();
	check(list.size() == 1, "живой блок: фильтр найден");
	if (list.empty())
		return report.c_str();
	obs_source_t *filter = list.front().filter;

	{
		ObsFilterLink link(filter);
		link.set_active(true);
		// В тестовой записи есть паузы с цифровой тишиной — ждём, пока попадём на речь.
		bool meters_ok = false, spectrum_ok = false;
		std::vector<float> spec(96, -90.0f);
		for (int i = 0; i < 30 && !(meters_ok && spectrum_ok); i++) {
			wait(100);
			const LiveMeters m = link.meters();
			meters_ok |= m.valid && m.in_rms > -80.0f && m.out_rms > -80.0f;
			link.spectrum(spec);
			spectrum_ok |= *std::max_element(spec.begin(), spec.end()) > -70.0f;
		}
		check(meters_ok, "живой блок: индикаторы получают звук");
		check(spectrum_ok, "живой блок: спектр голоса строится");
		link.set_double(kPresGain, 5.5);
		link.commit(true);
		check(link.get_double(kPresGain) == 5.5, "живой блок: точка эквалайзера пишет в фильтр");
	}

	// Как в OBS: окно свойств — форма, в которой есть строка с меткой.
	install_props_injector(create_block);
	QWidget page;
	page.setAttribute(Qt::WA_DontShowOnScreen);
	auto *form = new QFormLayout(&page);
	form->addRow(new QLabel(QString::fromStdString(live_marker(filter))));
	form->addRow(new QLabel("Режим настроек"), new QLabel("…"));
	page.resize(560, 520);
	page.show();
	wait(100);

	LiveBlock *block = nullptr;
	for (QWidget *w : page.findChildren<QWidget *>())
		if (auto *b = dynamic_cast<LiveBlock *>(w))
			block = b;
	check(block != nullptr, "живой блок: встроился на место метки");
	check(form->rowCount() == 3 && !form->isRowVisible(1), "живой блок: строка-метка скрыта");

	wait(2000);
	const QImage img = page.grab().toImage();
	check(!img.isNull() && img.save(QString::fromUtf8(png_path)), "живой блок: скриншот сохранён");

	page.hide();
	uninstall_props_injector();
	for (vs::FilterInstance &fi : list)
		obs_source_release(fi.filter);
	return report.c_str();
}
