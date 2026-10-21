#include "live-block.hpp"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

#include "eq-view.hpp"
#include "settings.hpp"
#include "style.hpp"

using namespace vs::key;

namespace vsui {

namespace {
constexpr int kRowH = 20;   // строка индикатора
constexpr int kGap = 6;
constexpr int kBottom = 12; // отступ до обычных настроек
constexpr int kSpectrumBands = 96;

// Шкала индикатора, как у микшера OBS: −60…0 дБ, жёлтая зона с −20, красная с −9.
constexpr double kMinDb = -60.0;
constexpr double kWarnDb = -20.0;
constexpr double kHotDb = -9.0;
} // namespace

LiveBlock::LiveBlock(std::shared_ptr<LiveLink> link, QWidget *parent) : QWidget(parent), link_(std::move(link))
{
	spectrum_.assign(kSpectrumBands, -90.0f);
	hint_ = link_->text("Live.EqHint");

	eq_ = new EqCurveView(this);
	eq_->setTexts({link_->text("Live.Hpf"), link_->text("Live.Low"), link_->text("Live.Mud"),
		       link_->text("Live.Nasal"), link_->text("Live.Pres"), link_->text("Live.Air")},
		      link_->text("Unit.Hz"), link_->text("Live.kHz"), link_->text("Unit.dB"), link_->text("Live.EqOff"));
	eq_->onChanged = [this](const char *key, double v, bool final) {
		link_->set_double(key, v);
		link_->commit(final);
	};

	// 60 кадров в секунду — индикаторы и спектр движутся плавно.
	timer_.setInterval(16);
	timer_.setTimerType(Qt::PreciseTimer);
	connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
	clock_.start();
	setMouseTracking(true); // рука над надписью об обновлении
	sync();
}

LiveBlock::~LiveBlock()
{
	if (active_)
		link_->set_active(false);
}

int LiveBlock::meters_height() const
{
	return kRowH * 3 + kGap * 2;
}

int LiveBlock::eq_height(int w) const
{
	return std::clamp(w * 30 / 100, 150, 220);
}

int LiveBlock::hint_height(int w) const
{
	const QFontMetrics fm(style::font(this, 0.85));
	return fm.boundingRect(QRect(0, 0, std::max(40, w), 1000), Qt::TextWordWrap, hint_).height();
}

int LiveBlock::heightForWidth(int w) const
{
	return meters_height() + kGap * 2 + eq_height(w) + 4 + hint_height(w) + kBottom;
}

void LiveBlock::resizeEvent(QResizeEvent *)
{
	eq_->setGeometry(0, meters_height() + kGap * 2, width(), eq_height(width()));
}

void LiveBlock::showEvent(QShowEvent *)
{
	if (!active_) {
		link_->set_active(true);
		active_ = true;
	}
	clock_.restart();
	if (!manual_)
		timer_.start();
}

void LiveBlock::hideEvent(QHideEvent *)
{
	timer_.stop();
	if (active_) {
		link_->set_active(false);
		active_ = false;
	}
}

void LiveBlock::sync()
{
	badge_ = link_->update_badge();
	if (eq_->interacting())
		return;
	EqState s;
	s.eq_on = link_->get_bool(kEqEnabled);
	s.hpf_on = link_->get_bool(kHpfEnabled);
	s.hpf_freq = link_->get_double(kHpfFreq);
	s.hpf_slope = (int)link_->get_int(kHpfSlope);
	s.low_gain = link_->get_double(kLowGain);
	s.low_freq = link_->get_double(kLowFreq);
	s.mud_gain = link_->get_double(kMudGain);
	s.mud_freq = link_->get_double(kMudFreq);
	s.mud_q = link_->get_double(kMudQ);
	s.nasal_gain = link_->get_double(kNasalGain);
	s.nasal_freq = link_->get_double(kNasalFreq);
	s.nasal_q = link_->get_double(kNasalQ);
	s.pres_gain = link_->get_double(kPresGain);
	s.pres_freq = link_->get_double(kPresFreq);
	s.pres_q = link_->get_double(kPresQ);
	s.air_gain = link_->get_double(kAirGain);
	s.air_freq = link_->get_double(kAirFreq);
	s.lpf_freq = link_->get_double(kLpfFreq);
	eq_->setState(s);
	eq_->setSampleRate(link_->sample_rate());
	enabled_ = link_->enabled();
}

void LiveBlock::update_bar(Bar &b, double rms, double peak, double dt)
{
	// Быстрый подъём, плавный спад — как у индикаторов микшера.
	b.rms = rms > b.rms ? rms : std::max(rms, b.rms - 24.0 * dt);
	b.peak = peak > b.peak ? peak : std::max(peak, b.peak - 30.0 * dt);
	if (peak >= b.hold) {
		b.hold = peak;
		b.hold_t = 1.2;
	} else if ((b.hold_t -= dt) < 0.0) {
		b.hold = std::max(peak, b.hold - 18.0 * dt);
	}
}

void LiveBlock::tick()
{
	advance(std::clamp(clock_.restart() / 1000.0, 0.001, 0.1));
}

void LiveBlock::step(double dt)
{
	advance(dt);
	sync(); // в ролике настройки меняются плавно — кривая должна идти следом каждый кадр
	update();
}

void LiveBlock::advance(double dt)
{
	ticks_++;

	const LiveMeters m = link_->meters();
	signal_ = m.valid && m.in_peak > -100.0f;
	update_bar(in_, m.in_rms, m.in_peak, dt);
	update_bar(out_, m.out_rms, m.out_peak, dt);
	gr_ += (m.comp_gr - gr_) * std::min(1.0, dt * 10.0);
	gate_ += (m.gate_db - gate_) * std::min(1.0, dt * 10.0);
	vad_ += ((m.vad >= 0.0f ? m.vad : 0.0) - vad_) * std::min(1.0, dt * 6.0);

	link_->spectrum(spectrum_);
	eq_->setSpectrum(spectrum_, dt);
	if (ticks_ % 20 == 0)
		sync(); // ползунки ниже могли поменять эквалайзер
	update(0, 0, width(), meters_height());
}

void LiveBlock::paint_bar(QPainter &p, const QRectF &row, const Bar &b, const QString &label, double label_w)
{
	const QPalette &pal = palette();
	const double value_w = 64.0;
	p.setPen(style::text(pal, 200));
	p.drawText(QRectF(row.left(), row.top(), label_w, row.height()), Qt::AlignVCenter | Qt::AlignLeft, label);

	const QRectF bar(row.left() + label_w, row.center().y() - 4.0, row.width() - label_w - value_w, 8.0);
	p.setPen(Qt::NoPen);
	p.setBrush(style::well(pal));
	p.drawRoundedRect(bar, 2, 2);

	auto x_of = [&](double db) {
		return bar.left() + std::clamp((db - kMinDb) / -kMinDb, 0.0, 1.0) * bar.width();
	};
	// Три сплошные зоны, как у микшера OBS. Пик — та же зона, но прозрачнее.
	auto fill = [&](double db, int alpha) {
		const double x = x_of(db);
		const double xw = x_of(kWarnDb), xh = x_of(kHotDb);
		auto seg = [&](double a, double c, QColor col) {
			if (c <= a)
				return;
			col.setAlpha(alpha);
			p.fillRect(QRectF(a, bar.top(), c - a, bar.height()), col);
		};
		seg(bar.left(), std::min(x, xw), style::meter_ok());
		seg(xw, std::min(x, xh), style::meter_warn());
		seg(xh, x, style::meter_hot());
	};
	fill(b.peak, 90);
	fill(b.rms, 255);
	if (b.hold > kMinDb + 1.0)
		p.fillRect(QRectF(x_of(b.hold) - 1.0, bar.top(), 2.0, bar.height()), style::text(pal, 230));

	p.setPen(style::text(pal, 170));
	const QString value = b.rms < -70.0 ? QStringLiteral("—")
					    : QString::number((int)std::lround(b.rms)) + link_->text("Unit.dB");
	p.drawText(QRectF(bar.right(), row.top(), value_w, row.height()), Qt::AlignVCenter | Qt::AlignRight, value);
}

void LiveBlock::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QPalette &pal = palette();
	p.setFont(style::font(this));
	const QFontMetrics fm(p.font());
	const QString in_label = link_->text("Live.In"), out_label = link_->text("Live.Out");
	const double label_w = std::max(fm.horizontalAdvance(in_label), fm.horizontalAdvance(out_label)) + 14.0;

	paint_bar(p, QRectF(0, 0, width(), kRowH), in_, in_label, label_w);
	paint_bar(p, QRectF(0, kRowH + kGap, width(), kRowH), out_, out_label, label_w);

	// Строка состояния: «● ИИ слышит голос · сжатие −3 дБ · пауза приглушена».
	const QRectF st(label_w, (kRowH + kGap) * 2, width() - label_w, kRowH);
	p.setFont(style::font(this, 0.9));
	const QFontMetrics sfm(p.font());
	QString state;
	QColor dot = style::text(pal, 90);
	if (!enabled_) {
		state = link_->text("Live.Off");
	} else if (!signal_) {
		state = link_->text("Live.NoSignal");
	} else if (vad_ > 0.5) {
		state = link_->text("Live.Voice");
		dot = style::meter_ok();
	} else {
		state = link_->text("Live.Silence");
	}
	// Справа — «⬆ Доступна версия …», если вышло обновление плагина.
	double badge_w = 0.0;
	badge_rect_ = QRectF();
	if (!badge_.isEmpty()) {
		badge_w = std::min<double>(sfm.horizontalAdvance(badge_), st.width() * 0.62);
		badge_rect_ = QRectF(st.right() - badge_w, st.top(), badge_w, st.height());
		QColor link_color = style::accent(pal);
		if (style::is_dark(pal))
			link_color = link_color.lighter(170);
		p.setPen(link_color);
		p.drawText(badge_rect_, Qt::AlignVCenter | Qt::AlignRight,
			   sfm.elidedText(badge_, Qt::ElideRight, (int)badge_w));
		badge_w += 12.0;
	}
	p.setPen(Qt::NoPen);
	p.setBrush(dot);
	p.drawEllipse(QPointF(st.left() + 4, st.center().y()), 3.5, 3.5);
	QString line = state;
	if (enabled_ && signal_) {
		if (gr_ < -0.5)
			line += "  ·  " + QString(link_->text("Live.Squeeze")).arg(QString::number(gr_, 'f', 1));
		if (gate_ < -3.0)
			line += "  ·  " + link_->text("Live.GatePause");
	}
	p.setPen(style::text(pal, 170));
	p.drawText(st.adjusted(14, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
		   sfm.elidedText(line, Qt::ElideRight, (int)(st.width() - 14 - badge_w)));

	// Подсказка под графиком и разделитель перед обычными настройками.
	const int eq_bottom = eq_->geometry().bottom() + 1;
	p.setFont(style::font(this, 0.85));
	p.setPen(style::text(pal, 120));
	p.drawText(QRectF(0, eq_bottom + 4, width(), hint_height(width())), Qt::TextWordWrap | Qt::AlignLeft, hint_);
	p.setPen(QPen(style::text(pal, 30), 1.0));
	p.drawLine(QPointF(0, height() - 0.5), QPointF(width(), height() - 0.5));
}

void LiveBlock::mouseMoveEvent(QMouseEvent *e)
{
	if (badge_rect_.contains(e->position()))
		setCursor(Qt::PointingHandCursor);
	else
		unsetCursor();
}

void LiveBlock::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !badge_rect_.contains(e->position()))
		return;
	// Окно обновления — уже после выхода из обработчика: пока оно открыто, OBS может
	// перестроить окно свойств и удалить этот блок.
	std::shared_ptr<LiveLink> link = link_;
	QTimer::singleShot(0, QCoreApplication::instance(), [link] { link->open_update(); });
}

} // namespace vsui
