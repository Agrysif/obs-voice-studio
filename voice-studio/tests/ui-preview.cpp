// Просмотр живого блока без OBS: макет окна «Фильтры» в цветах темы OBS (Yami)
// с «игрушечными» данными (синтетический голос). Блок встраивается тем же механизмом,
// что и в OBS, — через невидимую строку-метку.
//
//   voice-studio-ui-preview <папка data> show [язык]
//   voice-studio-ui-preview <папка data> shot <out.png> [normal|hover|drag|eqoff|nosignal|update|bench] [язык]
//   voice-studio-ui-preview <папка data> record <ffmpeg.exe> <out.mp4> [ru|en]   — демо-ролик 1920×1080
//
// Язык — имя файла перевода без .ini (ru-RU, en-US, kk-KZ, …), по умолчанию ru-RU.
// Запускать с OBS\bin\64bit в PATH (obs.dll, Qt) и QT_PLUGIN_PATH на ту же папку.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-data.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

#include "eq-view.hpp"
#include "live-block.hpp"
#include "presets.hpp"
#include "props-injector.hpp"
#include "settings.hpp"

#ifndef VOICE_STUDIO_UPDATE_REPO
#define VOICE_STUDIO_UPDATE_REPO ""
#endif

using namespace vs::key;

namespace {

QString g_data_dir;

class MockLink : public vsui::LiveLink {
public:
	explicit MockLink(const QString &locale)
	{
		QFile f(g_data_dir + "/locale/" + locale + ".ini");
		if (f.open(QIODevice::ReadOnly)) {
			for (const QByteArray &raw : f.readAll().split('\n')) {
				const QString line = QString::fromUtf8(raw).trimmed();
				const int eq = line.indexOf('=');
				if (eq <= 0)
					continue;
				QString v = line.mid(eq + 1).trimmed();
				if (v.startsWith('"') && v.endsWith('"'))
					v = v.mid(1, v.size() - 2);
				v.replace("\\n", "\n").replace("\\\"", "\"");
				texts_[line.left(eq).trimmed()] = v;
			}
		}
		settings_ = obs_data_create();
		set_preset("podcast");
		clock_.start();
	}
	~MockLink() override { obs_data_release(settings_); }

	bool signal = true;
	QString badge;

	void set_preset(const char *id)
	{
		vs::PresetValues pv;
		vs::find_preset(id, pv);
		vs::write_preset(settings_, pv, false);
	}

	QString tr(const char *key) { return text(key); }
	QString text(const char *key) override
	{
		auto it = texts_.find(QStringLiteral("VoiceStudio.") + key);
		return it != texts_.end() ? it->second : QString::fromUtf8(key);
	}
	double get_double(const char *k) override { return obs_data_get_double(settings_, k); }
	bool get_bool(const char *k) override { return obs_data_get_bool(settings_, k); }
	long long get_int(const char *k) override { return obs_data_get_int(settings_, k); }
	void set_double(const char *k, double v) override { obs_data_set_double(settings_, k, v); }
	void set_bool(const char *k, bool v) { obs_data_set_bool(settings_, k, v); }
	void commit(bool) override {}
	bool enabled() override { return true; }

	double envelope() const
	{
		const double t = clock_.elapsed() / 1000.0;
		const double phrase = std::sin(2.0 * 3.14159 * 0.22 * t) > -0.35 ? 1.0 : 0.0;
		const double syl = std::pow(std::max(0.0, std::sin(2.0 * 3.14159 * 3.1 * t)), 0.6);
		return phrase * (0.35 + 0.65 * syl);
	}

	vsui::LiveMeters meters() override
	{
		vsui::LiveMeters m;
		m.valid = true;
		if (!signal)
			return m;
		const double e = envelope();
		m.in_rms = (float)(-44 + 22 * e);
		m.out_rms = (float)(-30 + 14 * e);
		m.in_peak = m.in_rms + 9;
		m.out_peak = m.out_rms + 7;
		m.comp_gr = (float)(-4.5 * e);
		m.gate_db = e < 0.05 ? -20.0f : 0.0f;
		m.vad = e > 0.1 ? 0.95f : 0.04f;
		return m;
	}

	void spectrum(std::vector<float> &out) override
	{
		const double e = signal ? envelope() : 0.0;
		const int n = (int)out.size();
		for (int b = 0; b < n; b++) {
			const double f = 20.0 * std::pow(1000.0, (b + 0.5) / n);
			const double oct = std::log2(f / 400.0);
			double db = -24.0 - 4.5 * oct * oct - (f > 400 ? 3.0 * oct : 0.0);
			db += 5.0 * std::cos(2.0 * 3.14159 * f / 150.0) * (f < 2500 ? 1.0 : 0.3);
			db += 20.0 * std::log10(0.06 + e);
			out[(size_t)b] = (float)std::max(-86.0, signal ? db : -90.0);
		}
	}

	int sample_rate() override { return 48000; }
	void set_active(bool) override {}
	QString update_badge() override { return badge; }

private:
	std::map<QString, QString> texts_;
	obs_data_t *settings_;
	QElapsedTimer clock_;
};

std::shared_ptr<MockLink> g_link; // блоку в новом окне достаётся текущая связь

void run_for(int ms)
{
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

// Цвета темы OBS «Yami» (сняты со скриншота окна фильтров).
void apply_obs_theme(QApplication &app)
{
	QPalette pal;
	pal.setColor(QPalette::Window, QColor(0x1D, 0x1F, 0x26));
	pal.setColor(QPalette::WindowText, Qt::white);
	pal.setColor(QPalette::Base, QColor(0x27, 0x2A, 0x33));
	pal.setColor(QPalette::Text, Qt::white);
	pal.setColor(QPalette::Button, QColor(0x3C, 0x40, 0x4D));
	pal.setColor(QPalette::ButtonText, Qt::white);
	pal.setColor(QPalette::Highlight, QColor(0x28, 0x4C, 0xB8));
	pal.setColor(QPalette::HighlightedText, Qt::white);
	app.setPalette(pal);
	app.setStyleSheet("QListWidget { background: #272A33; border: none; border-radius: 4px; padding: 4px; }"
			  "QListWidget::item { padding: 6px; border-radius: 4px; }"
			  "QListWidget::item:selected { background: #284CB8; }"
			  "QPushButton, QComboBox { background: #3C404D; border: none; border-radius: 4px;"
			  "  padding: 6px 14px; color: white; min-height: 20px; }"
			  "QPushButton:default { background: #284CB8; }"
			  "QScrollArea { border: none; background: transparent; }"
			  "QMessageBox { background: #1D1F26; }");
}

// Надписи самого OBS вокруг нашего блока (окно, список фильтров, кнопки).
struct Chrome {
	QString title, defaults, close;
	QStringList filters;
};

Chrome chrome_for(bool russian)
{
	if (russian)
		return {QStringLiteral("Фильтры для «Микрофон»"), QStringLiteral("По умолчанию"), QStringLiteral("Закрыть"),
			{QStringLiteral("Шумоподавление"), QStringLiteral("Компрессор"), QStringLiteral("Ограничитель")}};
	return {QStringLiteral("Filters for 'Mic/Aux'"), QStringLiteral("Defaults"), QStringLiteral("Close"),
		{QStringLiteral("Noise Suppression"), QStringLiteral("Compressor"), QStringLiteral("Limiter")}};
}

struct MockWindow {
	QWidget *win = nullptr;
	QComboBox *preset = nullptr;
};

// Макет окна «Фильтры для …» с нашими настройками и строкой-меткой в начале.
MockWindow build_window(MockLink &link, const Chrome &chrome)
{
	MockWindow mw;
	auto *win = new QWidget();
	mw.win = win;
	win->setWindowTitle(chrome.title);
	auto *root = new QVBoxLayout(win);
	auto *top = new QHBoxLayout();
	root->addLayout(top);

	auto *list = new QListWidget();
	list->setFixedWidth(230);
	for (const QString &name : chrome.filters)
		list->addItem(name);
	list->addItem(link.tr("FilterName"));
	list->setCurrentRow(list->count() - 1);
	top->addWidget(list);

	auto *scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	auto *page = new QWidget();
	auto *form = new QFormLayout(page);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	form->setLabelAlignment(Qt::AlignRight);
	auto info = [](const QString &t) {
		auto *l = new QLabel(t);
		l->setWordWrap(true);
		return l;
	};
	// Так OBS показывает информационную строку без значения: только поле, без подписи.
	form->addRow(nullptr, info(vsui::make_marker(1)));
	auto *mode = new QComboBox();
	mode->addItem(link.tr("Mode.Simple"));
	form->addRow(link.tr("Mode"), mode);
	form->addRow(nullptr, info(link.tr("Guide")));
	mw.preset = new QComboBox();
	mw.preset->addItem(link.tr("Preset.Podcast"));
	form->addRow(link.tr("Preset"), mw.preset);
	form->addRow(nullptr, info(link.tr("Preset.Podcast.Tip")));
	form->addRow(nullptr, new QPushButton(link.tr("Calib.Start")));
	form->addRow(nullptr, info(link.tr("Calib.Hint")));
	form->addRow(nullptr, new QPushButton(link.tr("Monitor.On")));
	form->addRow(nullptr, new QCheckBox(link.tr("Bypass")));
	scroll->setWidget(page);
	top->addWidget(scroll, 1);

	auto *bottom = new QHBoxLayout();
	bottom->addStretch(1);
	bottom->addWidget(new QPushButton(chrome.defaults));
	bottom->addStretch(3);
	bottom->addWidget(new QPushButton(chrome.close));
	root->addLayout(bottom);
	return mw;
}

template<typename T> T *find_child(QWidget *root)
{
	for (QWidget *w : root->findChildren<QWidget *>())
		if (auto *t = dynamic_cast<T *>(w))
			return t;
	return nullptr;
}

void send_mouse(QWidget *target, QEvent::Type type, QPointF pt, Qt::MouseButtons buttons)
{
	QMouseEvent ev(type, pt, target->mapToGlobal(pt), Qt::LeftButton, buttons, Qt::NoModifier);
	QApplication::sendEvent(target, &ev);
}

// ============================================================================
// Демо-ролик: сцены рисуются в реальном времени (живой блок анимируется сам),
// кадры 1920×1080 идут в ffmpeg без сжатия.

constexpr int kW = 1920, kH = 1080, kFps = 30;
constexpr double kScale = 1.4;         // QT_SCALE_FACTOR — чёткое окно в ролике
const QSize kWinSize(840, 640);        // окно в логических точках
const QPoint kWinPos(64, 112);         // где окно на кадре (под заголовком)
constexpr int kTitleBar = 40;
const QColor kBg(0x14, 0x16, 0x1B);
const QColor kAccent(0x5B, 0x86, 0xF0);

double smooth(double x)
{
	x = std::clamp(x, 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}

QFont ui_font(int px, int weight = QFont::Normal)
{
	QFont f(QStringLiteral("Segoe UI"));
	f.setPixelSize(px);
	f.setWeight(QFont::Weight(weight));
	return f;
}

struct Texts {
	QString title, subtitle, chips;
	QString cap[5][2]; // заголовок и пояснение сцен 1–5
	QString outro, outro_note;
};

Texts texts_for(bool ru)
{
	Texts t;
	if (ru) {
		t.title = QStringLiteral("🎙 Голосовая студия");
		t.subtitle = QStringLiteral("Студийный звук микрофона для OBS Studio");
		t.chips = QStringLiteral("ИИ-шумодав  ·  Эквалайзер  ·  Компрессор  ·  Де-эссер  ·  Смена голоса");
		t.cap[0][0] = QStringLiteral("Живой блок прямо в окне фильтра");
		t.cap[0][1] = QStringLiteral("Уровни «Вход/Выход», ИИ-детектор голоса и спектр — в стиле OBS, без лишних окон.");
		t.cap[1][0] = QStringLiteral("Тяните точки эквалайзера");
		t.cap[1][1] = QStringLiteral("Кривая лежит поверх живого спектра голоса. Ползунки ниже обновляются сами.");
		t.cap[2][0] = QStringLiteral("10 стилей и ✨ Умная настройка");
		t.cap[2][1] = QStringLiteral("Подкаст, стрим, бархатный голос, рация… Умная настройка за 10 секунд подбирает всё под ваш голос.");
		t.cap[3][0] = QStringLiteral("12 языков");
		t.cap[3][1] = QStringLiteral("Английский, русский и языки СНГ: плагин говорит на языке вашего OBS.");
		t.cap[4][0] = QStringLiteral("Обновляется сам");
		t.cap[4][1] = QStringLiteral("Новые версии приходят с GitHub и проверяются цифровой подписью перед установкой.");
		t.outro = QStringLiteral("Бесплатно и с открытым кодом");
		t.outro_note = QStringLiteral("OBS Studio 32  ·  Windows 10/11");
	} else {
		t.title = QStringLiteral("🎙 Voice Studio");
		t.subtitle = QStringLiteral("Studio-grade microphone processing for OBS Studio");
		t.chips = QStringLiteral("AI noise removal  ·  EQ  ·  Compressor  ·  De-esser  ·  Voice changer");
		t.cap[0][0] = QStringLiteral("A live panel right in the filter window");
		t.cap[0][1] = QStringLiteral("Input/output levels, AI voice detection and the voice spectrum — styled like OBS itself.");
		t.cap[1][0] = QStringLiteral("Drag the EQ points");
		t.cap[1][1] = QStringLiteral("The curve sits on top of your live voice spectrum. The sliders below follow along.");
		t.cap[2][0] = QStringLiteral("10 styles + ✨ Smart setup");
		t.cap[2][1] = QStringLiteral("Podcast, streaming, deep voice, radio… Smart setup tunes everything to your voice in 10 seconds.");
		t.cap[3][0] = QStringLiteral("12 languages");
		t.cap[3][1] = QStringLiteral("English, Russian and CIS languages — the plugin follows your OBS language.");
		t.cap[4][0] = QStringLiteral("Updates itself");
		t.cap[4][1] = QStringLiteral("New versions come from GitHub and are verified with a digital signature before installing.");
		t.outro = QStringLiteral("Free and open source");
		t.outro_note = QStringLiteral("OBS Studio 32  ·  Windows 10/11");
	}
	return t;
}

struct Lang {
	const char *code;
	const char *name;
};

const Lang kLangs[] = {{"en-US", "English"},     {"be-BY", "Беларуская"}, {"uk-UA", "Українська"},
		       {"kk-KZ", "Қазақша"},     {"ky-KG", "Кыргызча"},   {"uz-UZ", "Oʻzbekcha"},
		       {"tg-TJ", "Тоҷикӣ"},      {"az-AZ", "Azərbaycanca"}, {"hy-AM", "Հայերեն"},
		       {"tt-RU", "Татарча"},     {"ba-RU", "Башҡортса"},  {"ru-RU", "Русский"}};

class Director {
public:
	Director(bool ru) : ru_(ru), texts_(texts_for(ru)), chrome_(chrome_for(ru)), locale_(ru ? "ru-RU" : "en-US")
	{
		open_window(locale_);
	}

	// Сцены: 0 заставка, 1 живой блок, 2 эквалайзер, 3 стили, 4 языки, 5 обновление, 6 финал.
	// Под музыку 120 уд/мин: смены сцен — на сильные доли, языки меняются на каждый удар.
	static constexpr double kStart[] = {0.0, 3.0, 8.0, 15.0, 20.0, 26.0, 32.0, 36.0};
	static double duration() { return kStart[7]; }

	QImage frame(double t)
	{
		int scene = 0;
		while (scene < 6 && t >= kStart[scene + 1])
			scene++;
		const double u = t - kStart[scene];
		act(scene, u);

		QImage canvas(kW, kH, QImage::Format_RGB32);
		canvas.fill(kBg);
		QPainter p(&canvas);
		p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
		const double len = kStart[scene + 1] - kStart[scene];
		if (scene == 0 || scene == 6) {
			paint_card(p, scene, u, len);
		} else {
			paint_window(p);
			paint_caption(p, scene, u);
			paint_cursor(p);
		}
		// Короткое затемнение на стыке заставки/финала с окном.
		const double edge = std::min(t - kStart[scene], kStart[scene + 1] - t);
		if ((scene == 0 && kStart[scene + 1] - t < 0.3) || (scene == 1 && u < 0.3) ||
		    (scene == 5 && kStart[6] - t < 0.3) || (scene == 6 && u < 0.3) || t < 0.3 || duration() - t < 0.4) {
			const double a = 1.0 - std::clamp(edge / 0.3, 0.0, 1.0);
			p.fillRect(canvas.rect(), QColor(0, 0, 0, (int)(a * 255)));
		}
		return canvas;
	}

private:
	void open_window(const QString &locale)
	{
		g_link = std::make_shared<MockLink>(locale);
		std::unique_ptr<QWidget> old(win_.win);
		win_ = build_window(*g_link, chrome_);
		win_.win->setAttribute(Qt::WA_DontShowOnScreen);
		win_.win->resize(kWinSize);
		win_.win->show();
		QCoreApplication::processEvents();
		QCoreApplication::processEvents();
		eq_ = find_child<vsui::EqCurveView>(win_.win);
		block_ = find_child<vsui::LiveBlock>(win_.win);
	}

	QPointF to_canvas(QWidget *w, QPointF pt) const
	{
		const QPointF in_win = w->mapTo(win_.win, pt.toPoint()) + (pt - pt.toPoint());
		return QPointF(kWinPos) + in_win * kScale;
	}

	void move_cursor(QPointF target, double x)
	{
		const double s = smooth(x);
		cursor_ = from_ + (target - from_) * s;
		cursor_on_ = true;
	}

	void act(int scene, double u)
	{
		if (scene != last_scene_) {
			last_scene_ = scene;
			from_ = cursor_;
			step_ = 0;
			if (scene == 1) {
				cursor_ = QPointF(kWinPos.x() + 700, kWinPos.y() + 720);
				cursor_on_ = false;
			}
		}
		if (scene == 2)
			act_eq(u);
		else if (scene == 3)
			act_presets(u);
		else if (scene == 4)
			act_languages(u);
		else if (scene == 5)
			act_update(u);
	}

	// Перетаскивание точки i: подвести курсор, нажать, тянуть на d, отпустить.
	void drag(int i, QPointF d, double u, double t0)
	{
		if (!eq_)
			return;
		if (u < t0 || u > t0 + 2.4)
			return;
		const double k = u - t0;
		if (k < 0.8) {
			if (step_ == 0) {
				from_ = cursor_;
				step_ = 1;
			}
			move_cursor(to_canvas(eq_, eq_->nodePoint(i)), k / 0.8);
			if (k > 0.5)
				send_mouse(eq_, QEvent::MouseMove, eq_->nodePoint(i), Qt::NoButton);
			return;
		}
		if (step_ == 1) {
			grab_ = eq_->nodePoint(i);
			send_mouse(eq_, QEvent::MouseButtonPress, grab_, Qt::LeftButton);
			step_ = 2;
		}
		if (step_ == 2) {
			const QPointF pt = grab_ + d * smooth((k - 0.8) / 1.3);
			send_mouse(eq_, QEvent::MouseMove, pt, Qt::LeftButton);
			cursor_ = to_canvas(eq_, pt);
			if (k > 2.2) {
				send_mouse(eq_, QEvent::MouseButtonRelease, pt, Qt::NoButton);
				step_ = 0;
			}
		}
	}

	void act_eq(double u)
	{
		drag(4, QPointF(18, -34), u, 0.2);  // чёткость вверх
		drag(1, QPointF(-6, -26), u, 2.6);  // глубина вверх
		drag(2, QPointF(10, 22), u, 5.0);   // гулкость вниз
	}

	void act_presets(double u)
	{
		static const char *ids[] = {"deep", "bright", "phone", "stream", "podcast"};
		static const char *keys[] = {"Preset.Deep", "Preset.Bright", "Preset.Phone", "Preset.Stream", "Preset.Podcast"};
		const int i = std::min(4, (int)(u / 1.0));
		if (i != preset_step_) {
			preset_step_ = i;
			g_link->set_preset(ids[i]);
			win_.preset->setItemText(0, g_link->tr(keys[i]));
		}
		cursor_on_ = false;
	}

	void act_languages(double u)
	{
		// Все языки по очереди, язык ролика — последним: дальше всё на нём.
		std::vector<Lang> order;
		const Lang *own = nullptr;
		for (const Lang &l : kLangs) {
			if (locale_ == l.code)
				own = &l;
			else
				order.push_back(l);
		}
		order.push_back(*own);
		const int n = (int)order.size();
		const int i = std::min(n - 1, (int)(u / (6.0 / n)));
		if (i != lang_step_) {
			lang_step_ = i;
			open_window(QString::fromUtf8(order[(size_t)i].code));
			lang_name_ = QString::fromUtf8(order[(size_t)i].name);
		}
	}

	void act_update(double u)
	{
		if (u > 0.3 && g_link->badge.isEmpty())
			g_link->badge = g_link->tr("Live.Update").arg("1.4.0");
		if (u > 0.9 && u < 1.9 && block_) {
			if (step_ == 0) {
				from_ = cursor_;
				step_ = 1;
			}
			move_cursor(to_canvas(block_, QPointF(block_->width() - 90, 62)), (u - 0.9) / 0.9);
		}
		if (u > 2.0 && !dialog_) {
			dialog_.reset(make_dialog(g_link->tr("Update.Available").arg("1.4.0", "1.3.0") + "\n\n" +
							  g_link->tr("Update.WhatsNew") + "\n" +
							  (ru_ ? QStringLiteral("• Новые стили\n• Быстрее спектр")
							       : QStringLiteral("• New styles\n• Faster spectrum")),
						  true));
			from_ = cursor_;
		}
		if (dialog_ && u > 2.6 && u < 3.6 && install_) {
			const QPointF btn = install_->mapTo(dialog_.get(), install_->rect().center());
			move_cursor(dialog_pos_ + btn * kScale, (u - 2.6) / 0.8);
		}
		if (dialog_ && u > 3.8 && !done_) {
			done_ = true;
			dialog_.reset(make_dialog(g_link->tr("Update.Done").arg("1.4.0"), false));
			g_link->badge.clear();
		}
	}

	QMessageBox *make_dialog(const QString &text, bool ask)
	{
		auto *box = new QMessageBox(QMessageBox::Information, g_link->tr("Update.Title"), text, QMessageBox::NoButton);
		install_ = nullptr;
		if (ask) {
			install_ = box->addButton(g_link->tr("Update.Install"), QMessageBox::AcceptRole);
			box->addButton(g_link->tr("Update.Later"), QMessageBox::RejectRole);
			box->addButton(g_link->tr("Update.Skip"), QMessageBox::DestructiveRole);
			box->setDefaultButton(install_);
		} else {
			box->setStandardButtons(QMessageBox::Ok);
		}
		box->setAttribute(Qt::WA_DontShowOnScreen);
		box->show();
		QCoreApplication::processEvents();
		const QSize s = box->size() * kScale;
		dialog_pos_ = QPointF(kWinPos.x() + (kWinSize.width() * kScale - s.width()) / 2,
				      kWinPos.y() + (kWinSize.height() * kScale - s.height()) / 2 - 40);
		return box;
	}

	void paint_window(QPainter &p)
	{
		const QRectF frame(kWinPos.x(), kWinPos.y() - kTitleBar, kWinSize.width() * kScale,
				   kWinSize.height() * kScale + kTitleBar);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x2A, 0x2D, 0x36));
		p.drawRoundedRect(frame.adjusted(-1, -1, 1, 1), 9, 9);
		p.setBrush(QColor(0x17, 0x19, 0x1F));
		p.drawRoundedRect(QRectF(frame.left(), frame.top(), frame.width(), kTitleBar + 10), 8, 8);
		p.setPen(QColor(0xC9, 0xCD, 0xD6));
		p.setFont(ui_font(19));
		p.drawText(QRectF(frame.left() + 18, frame.top(), frame.width() - 200, kTitleBar), Qt::AlignVCenter,
			   chrome_.title);
		p.setPen(QPen(QColor(0x8A, 0x8F, 0x9C), 1.6));
		const double cy = frame.top() + kTitleBar / 2.0, rx = frame.right() - 28;
		p.drawLine(QPointF(rx - 6, cy - 6), QPointF(rx + 6, cy + 6));
		p.drawLine(QPointF(rx - 6, cy + 6), QPointF(rx + 6, cy - 6));
		p.drawRect(QRectF(rx - 58, cy - 6, 12, 12));
		p.drawLine(QPointF(rx - 106, cy), QPointF(rx - 94, cy));

		QImage img = win_.win->grab().toImage();
		img.setDevicePixelRatio(1.0);
		p.drawImage(QPointF(kWinPos), img);

		if (dialog_) {
			p.fillRect(QRectF(kWinPos, QSizeF(kWinSize) * kScale), QColor(0, 0, 0, 110));
			QImage d = dialog_->grab().toImage();
			d.setDevicePixelRatio(1.0);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 90));
			p.drawRoundedRect(QRectF(dialog_pos_ + QPointF(0, 8), QSizeF(d.size())).adjusted(-6, -6, 6, 6), 10, 10);
			p.setBrush(QColor(0x3A, 0x3E, 0x4A));
			p.drawRoundedRect(QRectF(dialog_pos_, QSizeF(d.size())).adjusted(-1, -1, 1, 1), 6, 6);
			p.drawImage(dialog_pos_, d);
		}
	}

	void paint_caption(QPainter &p, int scene, double u)
	{
		const double x0 = kWinPos.x() + kWinSize.width() * kScale + 76, w = kW - x0 - 70;
		const double a = smooth(u / 0.45);
		const double dy = (1.0 - a) * 18.0;
		p.setOpacity(a);

		double y = 300 + dy;
		p.setPen(kAccent);
		p.setFont(ui_font(24, QFont::DemiBold));
		p.drawText(QPointF(x0, y), QStringLiteral("0%1").arg(scene));
		y += 26;

		QString head = texts_.cap[scene - 1][0];
		p.setPen(Qt::white);
		p.setFont(ui_font(54, QFont::DemiBold));
		QRectF hr = p.boundingRect(QRectF(x0, y, w, 600), Qt::TextWordWrap, head);
		p.drawText(hr, Qt::TextWordWrap, head);
		y = hr.bottom() + 22;

		if (scene == 4 && !lang_name_.isEmpty()) {
			p.setPen(kAccent.lighter(130));
			p.setFont(ui_font(46, QFont::Normal));
			p.drawText(QRectF(x0, y, w, 64), Qt::AlignLeft | Qt::AlignVCenter, lang_name_);
			y += 84;
		}

		p.setPen(QColor(0xA9, 0xAE, 0xBA));
		p.setFont(ui_font(30));
		p.drawText(QRectF(x0, y, w, 400), Qt::TextWordWrap, texts_.cap[scene - 1][1]);
		p.setOpacity(1.0);
	}

	void paint_card(QPainter &p, int scene, double u, double len)
	{
		const double a = smooth(u / 0.6) * (1.0 - smooth((u - (len - 0.5)) / 0.5));
		const double rise = (1.0 - smooth(u / 0.8)) * 24.0;
		p.setOpacity(a);
		if (scene == 0) {
			p.setPen(Qt::white);
			p.setFont(ui_font(112, QFont::DemiBold));
			p.drawText(QRectF(0, 330 + rise, kW, 160), Qt::AlignCenter, texts_.title);
			p.setPen(QColor(0xC3, 0xC7, 0xD1));
			p.setFont(ui_font(44));
			p.drawText(QRectF(0, 500 + rise, kW, 70), Qt::AlignCenter, texts_.subtitle);
			p.setPen(kAccent.lighter(125));
			p.setFont(ui_font(30));
			p.drawText(QRectF(0, 610 + rise, kW, 60), Qt::AlignCenter, texts_.chips);
		} else {
			p.setPen(Qt::white);
			p.setFont(ui_font(84, QFont::DemiBold));
			p.drawText(QRectF(0, 330 + rise, kW, 130), Qt::AlignCenter, texts_.outro);
			p.setPen(kAccent.lighter(135));
			p.setFont(ui_font(52, QFont::Medium));
			p.drawText(QRectF(0, 490 + rise, kW, 80), Qt::AlignCenter,
				   QStringLiteral("github.com/") + QStringLiteral(VOICE_STUDIO_UPDATE_REPO));
			p.setPen(QColor(0x9A, 0x9F, 0xAB));
			p.setFont(ui_font(34));
			p.drawText(QRectF(0, 610 + rise, kW, 60), Qt::AlignCenter, texts_.outro_note);
		}
		p.setOpacity(1.0);
	}

	void paint_cursor(QPainter &p)
	{
		if (!cursor_on_)
			return;
		// Обычная стрелка Windows, чуть крупнее — чтобы было видно в ролике.
		QPainterPath arrow;
		const double s = 1.5;
		const QPointF o = cursor_;
		const QPointF pts[] = {{0, 0}, {0, 17}, {4.2, 13}, {7, 19.5}, {9.6, 18.4}, {6.9, 12.2}, {12.4, 12.2}};
		arrow.moveTo(o + pts[0] * s);
		for (int i = 1; i < 7; i++)
			arrow.lineTo(o + pts[i] * s);
		arrow.closeSubpath();
		p.setPen(QPen(Qt::black, 1.6));
		p.setBrush(Qt::white);
		p.drawPath(arrow);
	}

	bool ru_;
	Texts texts_;
	Chrome chrome_;
	QString locale_;
	MockWindow win_;
	vsui::EqCurveView *eq_ = nullptr;
	vsui::LiveBlock *block_ = nullptr;
	std::unique_ptr<QMessageBox> dialog_;
	QPushButton *install_ = nullptr;
	QPointF dialog_pos_;
	bool done_ = false;

	int last_scene_ = -1, step_ = 0, preset_step_ = -1, lang_step_ = -1;
	QPointF cursor_{kW * 0.5, kH * 0.8}, from_, grab_;
	bool cursor_on_ = false;
	QString lang_name_;
};

int record(const QString &ffmpeg, const QString &out, bool ru)
{
	QProcess enc;
	enc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
	enc.start(ffmpeg, {"-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra", "-s",
			   QStringLiteral("%1x%2").arg(kW).arg(kH), "-r", QString::number(kFps), "-i", "-", "-c:v",
			   "libx264", "-preset", "slow", "-crf", "16", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
			   out});
	if (!enc.waitForStarted()) {
		std::printf("cannot start ffmpeg\n");
		return 2;
	}
	Director director(ru);
	run_for(800); // блок успел получить данные

	const int total = (int)(Director::duration() * kFps);
	QElapsedTimer clock;
	clock.start();
	for (int i = 0; i < total; i++) {
		const double t = (double)i / kFps;
		while (clock.elapsed() < t * 1000.0) {
			QCoreApplication::processEvents();
			QThread::msleep(1);
		}
		QCoreApplication::processEvents();
		const QImage f = director.frame(t);
		enc.write(reinterpret_cast<const char *>(f.constBits()), f.sizeInBytes());
		while (enc.bytesToWrite() > 256LL * 1024 * 1024)
			enc.waitForBytesWritten(1000);
	}
	enc.closeWriteChannel();
	enc.waitForFinished(-1);
	std::printf("recorded %s: %d frames, %.1f s (real %.1f s)\n", out.toLocal8Bit().constData(), total,
		    total / (double)kFps, clock.elapsed() / 1000.0);
	return enc.exitCode();
}

} // namespace

int main(int argc, char **argv)
{
	if (argc > 2 && QByteArray(argv[2]) == "record")
		qputenv("QT_SCALE_FACTOR", QByteArray::number(kScale));
	QApplication app(argc, argv);
	if (argc < 3) {
		std::printf("usage: %s <data dir> show [locale] | shot <out.png> [scenario] [locale] |"
			    " record <ffmpeg> <out.mp4> [ru|en]\n",
			    argv[0]);
		return 1;
	}
	apply_obs_theme(app);
	g_data_dir = QString::fromLocal8Bit(argv[1]);
	const QString mode = argv[2];
	vsui::install_props_injector(
		[](int, QWidget *parent) -> QWidget * { return new vsui::LiveBlock(g_link, parent); });

	if (mode == "record") {
		if (argc < 5)
			return 1;
		const bool ru = argc < 6 || QByteArray(argv[5]) != "en";
		return record(QString::fromLocal8Bit(argv[3]), QString::fromLocal8Bit(argv[4]), ru);
	}

	const QString locale = mode == "show" ? (argc > 3 ? argv[3] : "ru-RU") : (argc > 5 ? argv[5] : "ru-RU");
	g_link = std::make_shared<MockLink>(locale);
	std::unique_ptr<QWidget> win(build_window(*g_link, chrome_for(locale.startsWith("ru"))).win);
	win->resize(1000, 900);

	if (mode == "show") {
		win->show();
		return app.exec();
	}
	if (mode != "shot" || argc < 4)
		return 1;
	const QString out = QString::fromLocal8Bit(argv[3]);
	const QString scenario = argc > 4 ? argv[4] : "normal";
	if (scenario == "nosignal")
		g_link->signal = false;
	if (scenario == "eqoff")
		g_link->set_bool(kEqEnabled, false);
	if (scenario == "update")
		g_link->badge = g_link->tr("Live.Update").arg("1.4.0");

	win->setAttribute(Qt::WA_DontShowOnScreen);
	win->show();
	run_for(1500);

	if (scenario == "bench") {
		auto *block = find_child<vsui::LiveBlock>(win.get());
		if (!block)
			return 2;
		QElapsedTimer t;
		t.start();
		for (int i = 0; i < 60; i++)
			block->grab();
		std::printf("live block frame: %.2f ms -> %.1f%% of one core at 30 fps\n", t.elapsed() / 60.0,
			    t.elapsed() / 60.0 * 30.0 / 10.0);
		return 0;
	}
	if (scenario == "hover" || scenario == "drag") {
		if (auto *eq = find_child<vsui::EqCurveView>(win.get())) {
			if (scenario == "hover") {
				send_mouse(eq, QEvent::MouseMove, eq->nodePoint(4), Qt::NoButton);
			} else {
				// Тянем «Глубину» вверх: точка должна остаться под курсором и на линии.
				const QPointF start = eq->nodePoint(1);
				const QPointF target = start + QPointF(12, -28);
				send_mouse(eq, QEvent::MouseButtonPress, start, Qt::LeftButton);
				send_mouse(eq, QEvent::MouseMove, start + QPointF(6, -14), Qt::LeftButton);
				send_mouse(eq, QEvent::MouseMove, target, Qt::LeftButton);
				const QPointF now = eq->nodePoint(1);
				std::printf("drag: cursor (%.1f, %.1f) -> node (%.1f, %.1f), band gain %.1f dB, offset %.1f px\n",
					    target.x(), target.y(), now.x(), now.y(), eq->state().low_gain,
					    std::hypot(now.x() - target.x(), now.y() - target.y()));
			}
		}
		run_for(300);
	}

	const QImage img = win->grab().toImage();
	if (!img.save(out)) {
		std::printf("cannot save %s\n", argv[3]);
		return 2;
	}
	std::printf("saved %s (%dx%d)\n", argv[3], img.width(), img.height());
	return 0;
}
