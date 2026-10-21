// Интеграционный тест плагина в настоящем libobs — без окна OBS и без звука в колонках.
//
//   voice-studio-obs-test <voice-studio.dll> <папка data плагина> <speech.wav>
//
// Поднимает libobs в отдельном процессе, загружает плагин, создаёт тестовый
// «микрофон», который проигрывает WAV в реальном времени, вешает на него фильтр
// и нажимает элементы интерфейса так же, как это делает окно фильтров OBS.
// Запускать с OBS\bin\64bit в PATH (там лежит obs.dll).

#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QDir>

#include "settings.hpp"
#include "wav-io.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
	std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
	if (!ok)
		g_failures++;
}

// --- UI-задачи: копим и выполняем в «главном потоке» теста -------------------
std::mutex g_task_mutex;
std::deque<std::pair<obs_task_t, void *>> g_tasks;

void ui_task_handler(obs_task_t task, void *param, bool wait)
{
	if (wait) {
		task(param);
		return;
	}
	std::lock_guard<std::mutex> lock(g_task_mutex);
	g_tasks.emplace_back(task, param);
}

int run_ui_tasks()
{
	int count = 0;
	for (;;) {
		std::pair<obs_task_t, void *> t;
		{
			std::lock_guard<std::mutex> lock(g_task_mutex);
			if (g_tasks.empty())
				return count;
			t = g_tasks.front();
			g_tasks.pop_front();
		}
		t.first(t.second);
		count++;
	}
}

// --- Тестовый «микрофон» --------------------------------------------------------
Wav g_wav;

struct TestSource {
	obs_source_t *source = nullptr;
	std::thread thread;
	std::atomic<bool> running{true};
};

void *ts_create(obs_data_t *, obs_source_t *source)
{
	auto *t = new TestSource();
	t->source = source;
	t->thread = std::thread([t] {
		const uint64_t start = os_gettime_ns();
		const auto t0 = std::chrono::steady_clock::now();
		std::vector<float> buf(480);
		size_t pos = 0;
		uint64_t n = 0;
		while (t->running) {
			for (float &s : buf) {
				s = g_wav.mono[pos];
				pos = (pos + 1) % g_wav.mono.size();
			}
			obs_source_audio a = {};
			a.data[0] = reinterpret_cast<const uint8_t *>(buf.data());
			a.frames = 480;
			a.speakers = SPEAKERS_MONO;
			a.format = AUDIO_FORMAT_FLOAT;
			a.samples_per_sec = 48000;
			a.timestamp = start + n * 10000000ULL;
			obs_source_output_audio(t->source, &a);
			n++;
			std::this_thread::sleep_until(t0 + std::chrono::milliseconds(10 * n));
		}
	});
	return t;
}

void ts_destroy(void *data)
{
	auto *t = static_cast<TestSource *>(data);
	t->running = false;
	t->thread.join();
	delete t;
}

const char *ts_name(void *)
{
	return "Test microphone";
}

// --- Что пришло на выход фильтра -------------------------------------------------
struct Stats {
	std::mutex m;
	uint64_t frames = 0;
	double sq = 0.0;
	float peak = 0.0f;
	bool finite = true;

	void reset()
	{
		std::lock_guard<std::mutex> l(m);
		frames = 0;
		sq = 0.0;
		peak = 0.0f;
		finite = true;
	}
} g_stats;

void capture_cb(void *, obs_source_t *, const audio_data *audio, bool)
{
	std::lock_guard<std::mutex> l(g_stats.m);
	for (int c = 0; c < 2; c++) {
		const float *x = reinterpret_cast<const float *>(audio->data[c]);
		if (!x)
			continue;
		for (uint32_t i = 0; i < audio->frames; i++) {
			if (!std::isfinite(x[i]))
				g_stats.finite = false;
			g_stats.peak = std::max(g_stats.peak, std::fabs(x[i]));
			g_stats.sq += (double)x[i] * x[i];
		}
	}
	g_stats.frames += audio->frames;
}

std::atomic<int> g_update_props{0};
void on_update_properties(void *, calldata_t *)
{
	g_update_props++;
}

// Ждём, выполняя UI-задачи, как это делал бы главный поток OBS.
void pump(double seconds)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < end) {
		run_ui_tasks();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	run_ui_tasks();
}

bool visible(obs_properties_t *props, const char *name)
{
	obs_property_t *p = obs_properties_get(props, name);
	return p && obs_property_visible(p);
}

} // namespace

#ifdef _WIN32
// libobs ждёт пути в UTF-8, а argv в Windows приходит в ANSI-кодировке —
// путь «Плагины OBS» иначе превращается в «???????».
std::string to_utf8(const wchar_t *w)
{
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s((size_t)std::max(n - 1, 0), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}
#endif

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::printf("usage: %s <voice-studio.dll> <data dir> <speech.wav>\n", argv[0]);
		return 1;
	}
	std::string dll_path = argv[1];
	std::string data_path = argv[2];
#ifdef _WIN32
	int wargc = 0;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	if (wargv && wargc >= 4) {
		dll_path = to_utf8(wargv[1]);
		data_path = to_utf8(wargv[2]);
	}
	LocalFree(wargv);
#endif
	if (!read_wav(argv[3], g_wav) || g_wav.rate != 48000 || g_wav.mono.empty()) {
		std::printf("need a 48 kHz WAV: %s\n", argv[3]);
		return 1;
	}
	std::string panel_png = argc > 4 ? argv[4] : "voice-studio-panel.png";
#ifdef _WIN32
	{
		int n = 0;
		wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
		if (w && n > 4)
			panel_png = to_utf8(w[4]); // панель ждёт путь в UTF-8
		LocalFree(w);
	}
#endif

	// Qt нужен для проверки красивой панели; плагины Qt берём из папки OBS (текущая папка).
	QCoreApplication::addLibraryPath(QDir::currentPath());
	QApplication qt_app(argc, argv);

	check(obs_startup("ru-RU", nullptr, nullptr), "obs_startup");
	obs_set_ui_task_handler(ui_task_handler);
	obs_audio_info ai = {48000, SPEAKERS_STEREO};
	check(obs_reset_audio(&ai), "obs_reset_audio 48 kHz stereo");
	// Несуществующее устройство: мониторинг включится логически, но в колонки ничего не пойдёт.
	const bool silent_monitor = obs_set_audio_monitoring_device("Test (silent)", "voice-studio-test-no-device");

	obs_source_info ts = {};
	ts.id = "vs_test_source";
	ts.type = OBS_SOURCE_TYPE_INPUT;
	ts.output_flags = OBS_SOURCE_AUDIO;
	ts.get_name = ts_name;
	ts.create = ts_create;
	ts.destroy = ts_destroy;
	obs_register_source(&ts);

#ifdef _WIN32
	// libobs ищет зависимости плагина в папке программы; в OBS это папка obs64.exe,
	// а у теста — папка сборки. Добавляем папку OBS (текущую), чтобы нашёлся obs-frontend-api.dll.
	AddDllDirectory(QDir::toNativeSeparators(QDir::currentPath()).toStdWString().c_str());
#endif
	obs_module_t *module = nullptr;
	const int open = obs_open_module(&module, dll_path.c_str(), data_path.c_str());
	check(open == MODULE_SUCCESS, "obs_open_module(voice-studio.dll)");
	check(module && obs_init_module(module), "obs_init_module");
	if (open != MODULE_SUCCESS || !module) {
		std::printf("OBS TEST FAILED: module not loaded\n");
		return 3;
	}

	const char *display = obs_source_get_display_name("voice_studio_filter");
	std::printf("filter display name: %s\n", display ? display : "(null)");
	check(display && std::string(display).find("Голосовая студия") != std::string::npos,
	      "русское название фильтра из ru-RU.ini");

	obs_source_t *mic = obs_source_create("vs_test_source", "Mic", nullptr, nullptr);
	obs_source_t *filter = obs_source_create("voice_studio_filter", "Voice", nullptr, nullptr);
	check(mic && filter, "создание источника и фильтра");
	if (!mic || !filter) {
		std::printf("OBS TEST FAILED: cannot create sources\n");
		return 3;
	}
	obs_source_filter_add(mic, filter);
	obs_source_add_audio_capture_callback(mic, capture_cb, nullptr);
	signal_handler_connect(obs_source_get_signal_handler(filter), "update_properties", on_update_properties,
			       nullptr);
	obs_set_output_source(0, mic);

	// --- 1. Окно свойств
	obs_properties_t *props = obs_source_properties(filter);
	check(props != nullptr, "get_properties");
	check(obs_properties_get(props, "calibrate") && obs_properties_get(props, "monitor") &&
		      obs_properties_get(props, vs::key::kDenoiseAmount) && obs_properties_get(props, vs::key::kPresGain),
	      "основные элементы интерфейса на месте (вкл. внутри групп)");
	check(visible(props, "guide") && !visible(props, vs::key::kLowFreq) && !visible(props, vs::key::kGateThreshold),
	      "простой режим: подсказка видна, экспертные и порог (режим ИИ) скрыты");
	obs_property_t *pinfo = obs_properties_get(props, "preset_info");
	check(pinfo && std::string(obs_property_description(pinfo)).size() > 10, "описание выбранного стиля показано");

	obs_data_t *s = obs_source_get_settings(filter);

	// --- 2. Звук идёт и он «здоровый»
	pump(3.0);
	{
		std::lock_guard<std::mutex> l(g_stats.m);
		std::printf("out: %llu frames, peak %.3f, rms %.1f dBFS\n", (unsigned long long)g_stats.frames,
			    g_stats.peak,
			    10.0 * std::log10(std::max(1e-12, g_stats.sq / std::max<uint64_t>(1, g_stats.frames * 2))));
		check(g_stats.frames > 48000 * 2, "фильтр обрабатывает звук в аудиопотоке");
		check(g_stats.finite && g_stats.peak > 0.01f && g_stats.peak <= 0.892f, "звук без NaN и не выше потолка −1 дБ");
	}

	// --- 3. Смена стиля
	obs_data_set_string(s, vs::key::kPreset, "podcast");
	obs_property_modified(obs_properties_get(props, vs::key::kPreset), s);
	check(obs_data_get_double(s, vs::key::kLowGain) == 2.5 &&
		      std::string(obs_data_get_string(s, vs::key::kAppliedPreset)) == "podcast" &&
		      obs_data_get_bool(s, vs::key::kWarmthEnabled),
	      "стиль «Подкаст» применился к ползункам");
	obs_data_set_double(s, vs::key::kLowGain, 1.0);
	obs_property_modified(obs_properties_get(props, vs::key::kPreset), s);
	check(obs_data_get_double(s, vs::key::kLowGain) == 1.0, "повторное открытие окна не сбрасывает ручные правки");

	// --- 4. Экспертный режим и режим гейта
	obs_data_set_int(s, vs::key::kMode, 1);
	obs_property_modified(obs_properties_get(props, vs::key::kMode), s);
	check(visible(props, vs::key::kLowFreq) && !visible(props, "guide"), "экспертный режим показывает частоты");
	obs_data_set_int(s, vs::key::kGateMode, 2);
	obs_property_modified(obs_properties_get(props, vs::key::kGateMode), s);
	check(visible(props, vs::key::kGateThreshold), "режим «голос и громкость» показывает порог");
	obs_source_update(filter, nullptr);

	// --- 5. Умная настройка
	const double gain_before = obs_data_get_double(s, vs::key::kInputGain);
	obs_property_t *calib = obs_properties_get(props, "calibrate");
	check(obs_property_button_clicked(calib, filter), "кнопка «Умная настройка» нажимается");
	std::printf("status: %s\n", obs_property_description(obs_properties_get(props, "calib_status")));
	pump(12.0);
	const double gain_after = obs_data_get_double(s, vs::key::kInputGain);
	std::printf("input gain: %.1f -> %.1f dB, gate thr %.0f, comp %.0f, deess %.0f, low %.1f, pres %.1f, air %.1f\n",
		    gain_before, gain_after, obs_data_get_double(s, vs::key::kGateThreshold),
		    obs_data_get_double(s, vs::key::kCompAmount), obs_data_get_double(s, vs::key::kDeessAmount),
		    obs_data_get_double(s, vs::key::kLowGain), obs_data_get_double(s, vs::key::kPresGain),
		    obs_data_get_double(s, vs::key::kAirGain));
	check(gain_after != gain_before, "результат калибровки записан в настройки");
	check(g_update_props.load() >= 3, "окно получало обновления прогресса и результата");
	obs_properties_destroy(props);
	props = obs_source_properties(filter);
	std::printf("status: %s\n", obs_property_description(obs_properties_get(props, "calib_status")));
	check(std::string(obs_property_description(obs_properties_get(props, "calib_status"))).find("Готово") !=
		      std::string::npos,
	      "в окне показан итог «Готово»");

	// --- 6. Кнопка прослушивания (в тесте звук никуда не выводится)
	if (silent_monitor) {
		obs_property_t *mon = obs_properties_get(props, "monitor");
		obs_property_button_clicked(mon, filter);
		check(obs_source_get_monitoring_type(mic) == OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT,
		      "прослушивание включается");
		obs_property_button_clicked(mon, filter);
		check(obs_source_get_monitoring_type(mic) == OBS_MONITORING_TYPE_NONE, "и выключается");
	} else {
		std::printf("[SKIP] прослушивание: не удалось выбрать «немое» устройство, не рискуем звуком\n");
	}

	// --- 6б. Живой блок (уровни + эквалайзер) на настоящем фильтре (внутри DLL плагина)
	using PanelSelftest = const char *(*)(const char *);
	auto panel_selftest =
		reinterpret_cast<PanelSelftest>(os_dlsym(obs_get_module_lib(module), "voice_studio_debug_live_selftest"));
	check(panel_selftest != nullptr, "в плагине есть самопроверка живого блока");
	if (panel_selftest) {
		const std::string report = panel_selftest(panel_png.c_str());
		std::printf("%s", report.c_str());
		size_t pos = 0;
		while ((pos = report.find("[FAIL]", pos)) != std::string::npos) {
			g_failures++;
			pos += 6;
		}
		std::printf("panel screenshot: %s\n", panel_png.c_str());
	}

	// --- 7. Смена голоса + сравнение с оригиналом
	obs_data_set_bool(s, vs::key::kPitchEnabled, true);
	obs_data_set_double(s, vs::key::kPitchSemitones, -3.0);
	obs_data_set_double(s, vs::key::kFormantShift, -1.0);
	obs_source_update(filter, nullptr);
	g_stats.reset();
	pump(2.0);
	{
		std::lock_guard<std::mutex> l(g_stats.m);
		check(g_stats.finite && g_stats.frames > 48000 && g_stats.peak > 0.01f, "смена голоса работает в OBS");
	}
	obs_data_set_bool(s, vs::key::kBypass, true);
	obs_source_update(filter, nullptr);
	g_stats.reset();
	pump(1.0);
	{
		std::lock_guard<std::mutex> l(g_stats.m);
		check(g_stats.finite && g_stats.frames > 24000, "сравнение с оригиналом работает");
	}

	// --- Уборка
	obs_properties_destroy(props);
	obs_data_release(s);
	obs_set_output_source(0, nullptr);
	obs_source_remove_audio_capture_callback(mic, capture_cb, nullptr);
	obs_source_filter_remove(mic, filter);
	obs_source_release(filter);
	obs_source_release(mic);
	pump(0.5);
	obs_shutdown();

	std::printf("%s (%d failures)\n", g_failures ? "OBS TEST FAILED" : "OBS TEST PASSED", g_failures);
	return g_failures ? 3 : 0;
}
