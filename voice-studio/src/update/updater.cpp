#include "updater.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QAction>
#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "public-key.hpp"
#include "update-core.hpp"

#ifndef VOICE_STUDIO_UPDATE_REPO
#define VOICE_STUDIO_UPDATE_REPO ""
#endif

#define TXT(k) QString::fromUtf8(obs_module_text("VoiceStudio." k))

namespace vsup {

namespace {

constexpr const char *kRepo = VOICE_STUDIO_UPDATE_REPO;
constexpr int64_t kCheckInterval = 20 * 3600; // автоматически — не чаще раза в ~сутки

struct CheckResult {
	bool ok = false;
	bool interactive = false;
	std::string error; // "network", "signature", "format"
	Manifest manifest;
};

struct InstallResult {
	bool ok = false;
	std::string error; // "network", "signature", "rights", "format"
	std::string version;
};

std::mutex g_mutex;
std::thread g_worker;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_shutting_down{false};
bool g_initialized = false;

// Дальше — только из UI-потока.
Manifest g_available; // найденная новая версия
bool g_has_update = false;
bool g_installed = false; // уже поставили — ждём перезапуска OBS
bool g_auto = true;
int64_t g_last_check = 0;
std::string g_skip;
QProgressDialog *g_progress = nullptr;

bool enabled()
{
	return kRepo[0] != '\0';
}

std::string user_agent()
{
	return std::string("VoiceStudio-Updater/") + VOICE_STUDIO_VERSION + " (OBS " + obs_get_version_string() + ")";
}

std::string base_url()
{
	return std::string("https://github.com/") + kRepo;
}

QString page_url()
{
	const std::string p = g_available.page.empty() ? base_url() + "/releases/latest" : g_available.page;
	return QString::fromStdString(p);
}

QWidget *main_window()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

// --- Настройки: plugin_config/voice-studio/updater.json
void load_config()
{
	char *path = obs_module_config_path("updater.json");
	if (!path)
		return;
	if (obs_data_t *d = obs_data_create_from_json_file_safe(path, "bak")) {
		obs_data_set_default_bool(d, "auto", true);
		g_auto = obs_data_get_bool(d, "auto");
		g_last_check = obs_data_get_int(d, "last_check");
		g_skip = obs_data_get_string(d, "skip_version");
		obs_data_release(d);
	}
	bfree(path);
}

void save_config()
{
	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path("updater.json");
	if (dir && path) {
		os_mkdirs(dir);
		obs_data_t *d = obs_data_create();
		obs_data_set_bool(d, "auto", g_auto);
		obs_data_set_int(d, "last_check", g_last_check);
		obs_data_set_string(d, "skip_version", g_skip.c_str());
		obs_data_save_json_safe(d, path, "tmp", "bak");
		obs_data_release(d);
	}
	bfree(dir);
	bfree(path);
}

// Папка плагина: …/voice-studio (сама библиотека лежит в …/voice-studio/bin/64bit).
std::wstring plugin_root()
{
	const char *bin = obs_get_module_binary_path(obs_current_module());
	if (!bin)
		return {};
	const std::filesystem::path dll(utf8_to_wide(bin));
	const std::filesystem::path dir64 = dll.parent_path();
	if (dir64.filename() != L"64bit" || dir64.parent_path().filename() != L"bin")
		return {};
	return dir64.parent_path().parent_path().wstring();
}

// Для языков СНГ описание версии — по-русски, для остальных — по-английски.
bool notes_in_russian()
{
	static const char *cis[] = {"ru", "be", "uk", "kk", "ky", "tg", "uz", "az", "hy", "tt", "ba", "kaa"};
	const std::string loc = obs_get_locale() ? obs_get_locale() : "";
	for (const char *c : cis) {
		const size_t n = std::strlen(c);
		if (loc.compare(0, n, c) == 0 && (loc.size() == n || loc[n] == '-'))
			return true;
	}
	return false;
}

QString error_text(const std::string &code)
{
	if (code == "signature")
		return TXT("Update.ErrSignature");
	if (code == "rights")
		return TXT("Update.ErrRights");
	if (code == "format")
		return TXT("Update.ErrFormat");
	return TXT("Update.ErrNetwork");
}

bool obs_compatible(const Manifest &m)
{
	if (!m.obs_min.empty() && compare_versions(obs_get_version_string(), m.obs_min) < 0)
		return false;
	const int major = (int)(obs_get_version() >> 24);
	return m.obs_max_major == 0 || major <= m.obs_max_major;
}

// --- Фоновый поток: проверка
CheckResult do_check(bool interactive)
{
	CheckResult r;
	r.interactive = interactive;
	const std::string ua = user_agent();
	const HttpResult text =
		http_get(base_url() + "/releases/latest/download/update.txt", 64 * 1024, ua, &g_shutting_down);
	const HttpResult sig = text.ok ? http_get(base_url() + "/releases/latest/download/update.txt.sig", 4096, ua,
						  &g_shutting_down)
				       : HttpResult();
	if (!text.ok || !sig.ok) {
		r.error = "network";
		return r;
	}
	std::vector<uint8_t> raw;
	if (!base64_decode(sig.body, raw) || raw.size() != 64 ||
	    !verify_signature(text.body, raw.data(), kUpdatePublicKey)) {
		r.error = "signature";
		return r;
	}
	if (!parse_manifest(text.body, r.manifest) || r.manifest.name != "voice-studio") {
		r.error = "format";
		return r;
	}
	r.ok = true;
	return r;
}

// --- Фоновый поток: скачивание и установка
InstallResult do_install(const Manifest &m)
{
	InstallResult r;
	r.version = m.version;
	const std::wstring root = plugin_root();
	if (root.empty()) {
		r.error = "rights";
		return r;
	}
	const std::string url = base_url() + "/releases/download/v" + m.version + "/" + m.bundle;
	const HttpResult bin = http_get(url, 128 * 1024 * 1024, user_agent(), &g_shutting_down);
	if (!bin.ok) {
		r.error = "network";
		return r;
	}
	uint8_t hash[32];
	if (bin.body.size() != m.bundle_size || !sha256(bin.body.data(), bin.body.size(), hash) ||
	    to_hex(hash, 32) != m.bundle_sha256) {
		r.error = "signature"; // размер или хеш не совпали с подписанным описанием
		return r;
	}
	std::vector<BundleFile> files;
	if (!read_bundle(bin.body, files)) {
		r.error = "format";
		return r;
	}
	r.error = apply_bundle(root, files);
	r.ok = r.error.empty();
	return r;
}

void run_in_background(std::function<void()> job)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_worker.joinable())
		g_worker.join();
	g_worker = std::thread(std::move(job));
}

void stop_worker()
{
	g_shutting_down = true;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_worker.joinable())
		g_worker.join();
}

// --- UI-поток
void show_update_dialog();

void on_check_done(void *param)
{
	std::unique_ptr<CheckResult> r(static_cast<CheckResult *>(param));
	g_busy = false;
	if (g_shutting_down)
		return;
	if (!r->ok) {
		blog(LOG_WARNING, "[voice-studio] update check failed: %s", r->error.c_str());
		if (r->interactive)
			QMessageBox::warning(main_window(), TXT("Update.Title"),
					     TXT("Update.CheckFailed").arg(error_text(r->error)));
		return;
	}
	g_last_check = (int64_t)time(nullptr);
	save_config();
	const Manifest &m = r->manifest;
	blog(LOG_INFO, "[voice-studio] update check: latest %s, current %s", m.version.c_str(), VOICE_STUDIO_VERSION);
	if (compare_versions(m.version, VOICE_STUDIO_VERSION) <= 0) {
		g_has_update = false;
		if (r->interactive)
			QMessageBox::information(main_window(), TXT("Update.Title"),
						 TXT("Update.UpToDate").arg(VOICE_STUDIO_VERSION));
		return;
	}
	if (!obs_compatible(m)) {
		if (r->interactive)
			QMessageBox::information(main_window(), TXT("Update.Title"),
						 TXT("Update.ObsTooOld")
							 .arg(QString::fromStdString(m.version),
							      QString::fromStdString(m.obs_min)));
		return;
	}
	g_available = m;
	g_has_update = true;
	// Сами предлагаем один раз на версию; после «Пропустить» — только по запросу из меню.
	if (r->interactive || m.version != g_skip)
		show_update_dialog();
}

void on_install_done(void *param)
{
	std::unique_ptr<InstallResult> r(static_cast<InstallResult *>(param));
	g_busy = false;
	if (g_progress) {
		g_progress->close();
		g_progress->deleteLater();
		g_progress = nullptr;
	}
	if (g_shutting_down)
		return;
	if (r->ok) {
		g_installed = true;
		g_has_update = false;
		blog(LOG_INFO, "[voice-studio] update %s installed, restart OBS to apply", r->version.c_str());
		QMessageBox::information(main_window(), TXT("Update.Title"),
					 TXT("Update.Done").arg(QString::fromStdString(r->version)));
		return;
	}
	blog(LOG_WARNING, "[voice-studio] update install failed: %s", r->error.c_str());
	QMessageBox box(QMessageBox::Warning, TXT("Update.Title"), TXT("Update.Failed").arg(error_text(r->error)),
			QMessageBox::Close, main_window());
	QPushButton *open = box.addButton(TXT("Update.OpenPage"), QMessageBox::AcceptRole);
	box.exec();
	if (box.clickedButton() == open)
		QDesktopServices::openUrl(QUrl(page_url()));
}

void start_install()
{
	if (g_busy.exchange(true))
		return;
	g_progress = new QProgressDialog(TXT("Update.Downloading"), QString(), 0, 0, main_window());
	g_progress->setWindowTitle(TXT("Update.Title"));
	g_progress->setCancelButton(nullptr);
	g_progress->setMinimumDuration(0);
	g_progress->show();
	const Manifest m = g_available;
	run_in_background(
		[m] { obs_queue_task(OBS_TASK_UI, on_install_done, new InstallResult(do_install(m)), false); });
}

void show_update_dialog()
{
	if (!g_has_update || g_installed || g_busy)
		return;
	const Manifest m = g_available;
	QString text = TXT("Update.Available").arg(QString::fromStdString(m.version), VOICE_STUDIO_VERSION);
	const std::string &notes = notes_in_russian() ? m.notes_ru : m.notes_en;
	if (!notes.empty())
		text += "\n\n" + TXT("Update.WhatsNew") + "\n" + QString::fromStdString(notes);

	QMessageBox box(QMessageBox::Information, TXT("Update.Title"), text, QMessageBox::NoButton, main_window());
	QPushButton *install = box.addButton(TXT("Update.Install"), QMessageBox::AcceptRole);
	box.addButton(TXT("Update.Later"), QMessageBox::RejectRole);
	QPushButton *skip = box.addButton(TXT("Update.Skip"), QMessageBox::DestructiveRole);
	box.setDefaultButton(install);
	box.exec();
	if (g_shutting_down)
		return;
	if (box.clickedButton() == install) {
		start_install();
	} else if (box.clickedButton() == skip) {
		g_skip = m.version;
		save_config();
	}
}

void check(bool interactive)
{
	if (!enabled() || g_shutting_down || g_busy.exchange(true))
		return;
	run_in_background([interactive] {
		obs_queue_task(OBS_TASK_UI, on_check_done, new CheckResult(do_check(interactive)), false);
	});
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		const int64_t now = (int64_t)time(nullptr);
		if (g_auto && enabled() && now - g_last_check > kCheckInterval)
			QTimer::singleShot(8000, main_window(), [] { check(false); });
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		stop_worker(); // OBS закрывается — не ждём сеть дольше, чем нужно
	}
}

} // namespace

void init_updater()
{
	if (!obs_frontend_get_main_window())
		return;
	g_initialized = true;
	load_config();
	const std::wstring root = plugin_root();
	if (!root.empty())
		cleanup_old_files(root); // остатки прошлого обновления

	auto *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("VoiceStudio.Update.Menu")));
	if (action && enabled()) {
		auto *menu = new QMenu(main_window());
		QObject::connect(menu->addAction(TXT("Update.MenuCheck")), &QAction::triggered, [] { check(true); });
		QAction *auto_check = menu->addAction(TXT("Update.MenuAuto"));
		auto_check->setCheckable(true);
		auto_check->setChecked(g_auto);
		QObject::connect(auto_check, &QAction::toggled, [](bool on) {
			g_auto = on;
			save_config();
		});
		menu->addSeparator();
		QObject::connect(menu->addAction(TXT("Update.MenuPage")), &QAction::triggered,
				 [] { QDesktopServices::openUrl(QUrl(QString::fromStdString(base_url()))); });
		action->setMenu(menu);
	} else if (action) {
		action->setVisible(false); // сборка без адреса обновлений (для разработки)
	}
	obs_frontend_add_event_callback(frontend_event, nullptr);
}

void shutdown_updater()
{
	stop_worker();
	if (g_initialized)
		obs_frontend_remove_event_callback(frontend_event, nullptr);
	g_initialized = false;
}

QString update_badge()
{
	if (!g_has_update || g_installed || g_available.version == g_skip)
		return QString();
	return TXT("Live.Update").arg(QString::fromStdString(g_available.version));
}

void open_update_dialog()
{
	show_update_dialog();
}

} // namespace vsup
