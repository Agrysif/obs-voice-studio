#pragma once

// Общее для просмотра блока и демо-ролика: «игрушечная» связь с фильтром (синтетический
// голос), тема OBS «Yami» и макет окна «Фильтры» с нашими настройками.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

#include <obs-data.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "live-block.hpp"
#include "presets.hpp"
#include "props-injector.hpp"
#include "settings.hpp"

namespace mock {

class MockLink : public vsui::LiveLink {
public:
	explicit MockLink(const QString &locale_file)
	{
		QFile f(locale_file);
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
	// Время для синтетического голоса; по умолчанию — настоящие часы.
	std::function<double()> now = [this] { return clock_.elapsed() / 1000.0; };

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
		const double t = now();
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

// Цвета темы OBS «Yami» (сняты со скриншота окна фильтров).
inline void apply_obs_theme(QApplication &app)
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
	app.setStyleSheet(
		"QListWidget { background: #272A33; border: none; border-radius: 4px; padding: 4px; }"
		"QListWidget::item { padding: 6px; border-radius: 4px; }"
		"QListWidget::item:selected { background: #284CB8; }"
		"QPushButton, QComboBox { background: #3C404D; border: none; border-radius: 4px;"
		"  padding: 6px 14px; color: white; min-height: 20px; }"
		"QPushButton:default { background: #284CB8; }"
		"QScrollArea { border: none; background: transparent; }"
		"QMessageBox { background: #1D1F26; }"
		"QGroupBox { border: 1px solid #2E313B; border-radius: 6px; margin-top: 16px; padding-top: 8px; }"
		"QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: white; font-weight: 600; }"
		"QSlider::groove:horizontal { height: 4px; background: #3C404D; border-radius: 2px; }"
		"QSlider::sub-page:horizontal { background: #284CB8; border-radius: 2px; }"
		"QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; background: #E6E8EE; }"
		"QDoubleSpinBox { background: #272A33; border: none; border-radius: 4px; padding: 3px 6px; color: white;"
		"  min-width: 70px; }");
}

// Надписи самого OBS вокруг нашего блока (окно, список фильтров, кнопки).
struct Chrome {
	QString title, defaults, close;
	QStringList filters;
};

inline Chrome chrome_for(bool russian)
{
	if (russian)
		return {QStringLiteral("Фильтры для «Микрофон»"), QStringLiteral("По умолчанию"), QStringLiteral("Закрыть"),
			{QStringLiteral("Шумоподавление"), QStringLiteral("Компрессор"), QStringLiteral("Ограничитель")}};
	return {QStringLiteral("Filters for 'Mic/Aux'"), QStringLiteral("Defaults"), QStringLiteral("Close"),
		{QStringLiteral("Noise Suppression"), QStringLiteral("Compressor"), QStringLiteral("Limiter")}};
}

// Ползунок настроек, как в окне свойств OBS: полоска + поле с числом.
struct SliderRow {
	const char *key;
	double min, max;
	QSlider *slider;
	QDoubleSpinBox *spin;
	QWidget *row;
};

struct MockWindow {
	QWidget *win = nullptr;
	QScrollArea *scroll = nullptr;
	QComboBox *preset = nullptr;
	QPushButton *calib = nullptr;
	QPushButton *monitor = nullptr;
	std::vector<QGroupBox *> groups; // шумодав, гейт, эквалайзер, де-эссер, компрессор, тепло, голос, лимитер
	std::vector<SliderRow> sliders;

	// Ползунки показывают текущие значения настроек (как после обновления окна в OBS).
	void refresh(MockLink &link)
	{
		for (SliderRow &s : sliders) {
			const double v = link.get_double(s.key);
			const QSignalBlocker b1(s.slider), b2(s.spin);
			s.slider->setValue((int)std::lround((v - s.min) / (s.max - s.min) * 1000.0));
			s.spin->setValue(v);
		}
	}
};

// Макет окна «Фильтры для …» с нашими настройками и строкой-меткой в начале.
// full — со всеми разделами ниже (для ролика с прокруткой).
inline MockWindow build_window(MockLink &link, const Chrome &chrome, bool full = false)
{
	using namespace vs::key;
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

	mw.scroll = new QScrollArea();
	mw.scroll->setWidgetResizable(true);
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
	mw.calib = new QPushButton(link.tr("Calib.Start"));
	form->addRow(nullptr, mw.calib);
	form->addRow(nullptr, info(link.tr("Calib.Hint")));
	mw.monitor = new QPushButton(link.tr("Monitor.On"));
	form->addRow(nullptr, mw.monitor);
	form->addRow(nullptr, new QCheckBox(link.tr("Bypass")));

	if (full) {
		auto group = [&](const char *title) {
			auto *g = new QGroupBox(link.tr(title));
			g->setCheckable(true);
			g->setChecked(true);
			auto *gf = new QFormLayout(g);
			gf->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
			gf->setLabelAlignment(Qt::AlignRight);
			form->addRow(g);
			mw.groups.push_back(g);
			return gf;
		};
		auto slider = [&](QFormLayout *gf, const char *label, const char *key, double min, double max,
				  const char *unit) {
			auto *row = new QWidget();
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			auto *s = new QSlider(Qt::Horizontal);
			s->setRange(0, 1000);
			auto *spin = new QDoubleSpinBox();
			spin->setRange(min, max);
			spin->setDecimals(1);
			spin->setSuffix(link.tr(unit));
			spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
			h->addWidget(s, 1);
			h->addWidget(spin);
			gf->addRow(link.tr(label), row);
			mw.sliders.push_back({key, min, max, s, spin, row});
		};
		auto combo = [&](QFormLayout *gf, const char *label, const char *item) {
			auto *c = new QComboBox();
			c->addItem(link.tr(item));
			gf->addRow(link.tr(label), c);
		};

		slider(group("Denoise"), "Denoise.Amount", kDenoiseAmount, 0, 100, "Unit.Percent");
		QFormLayout *gate = group("Gate");
		combo(gate, "Gate.Mode", "Gate.Mode.Voice");
		slider(gate, "Gate.Range", kGateRange, -60, 0, "Unit.dB");
		QFormLayout *eq = group("Eq");
		slider(eq, "Eq.Low", kLowGain, -12, 12, "Unit.dB");
		slider(eq, "Eq.Mud", kMudGain, -12, 12, "Unit.dB");
		slider(eq, "Eq.Nasal", kNasalGain, -12, 12, "Unit.dB");
		slider(eq, "Eq.Pres", kPresGain, -12, 12, "Unit.dB");
		slider(eq, "Eq.Air", kAirGain, -12, 12, "Unit.dB");
		slider(group("Deess"), "Deess.Amount", kDeessAmount, 0, 100, "Unit.Percent");
		slider(group("Comp"), "Comp.Amount", kCompAmount, 0, 100, "Unit.Percent");
		slider(group("Warmth"), "Warmth.Amount", kWarmthAmount, 0, 100, "Unit.Percent");
		QFormLayout *voice = group("Voice");
		slider(voice, "Voice.Pitch", kPitchSemitones, -12, 12, "Unit.Semitones");
		slider(voice, "Voice.Formant", kFormantShift, -12, 12, "Unit.Semitones");
		slider(group("Limiter"), "Limiter.Ceiling", kLimiterCeiling, -12, 0, "Unit.dB");
		mw.refresh(link);
	}

	mw.scroll->setWidget(page);
	top->addWidget(mw.scroll, 1);

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

inline void send_mouse(QWidget *target, QEvent::Type type, QPointF pt, Qt::MouseButtons buttons)
{
	QMouseEvent ev(type, pt, target->mapToGlobal(pt), Qt::LeftButton, buttons, Qt::NoModifier);
	QApplication::sendEvent(target, &ev);
}

} // namespace mock
