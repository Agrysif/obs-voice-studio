#include "eq-view.hpp"

#include <QEvent>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <complex>

#include "biquad.hpp"
#include "settings.hpp"
#include "style.hpp"

namespace vsui {

namespace {

constexpr double kFMin = 20.0;
constexpr double kFMax = 20000.0;
constexpr double kDbRange = 15.0; // видимый диапазон ±15 дБ
constexpr int kNodes = 6;

enum NodeKind { Hpf, Shelf, Peak };

struct NodeDef {
	NodeKind kind;
	const char *gain_key;
	const char *freq_key;
	const char *q_key;
	double fmin, fmax;
};

const NodeDef kDefs[kNodes] = {
	{Hpf, nullptr, vs::key::kHpfFreq, nullptr, 20, 500},
	{Shelf, vs::key::kLowGain, vs::key::kLowFreq, nullptr, 40, 400},
	{Peak, vs::key::kMudGain, vs::key::kMudFreq, vs::key::kMudQ, 150, 800},
	{Peak, vs::key::kNasalGain, vs::key::kNasalFreq, vs::key::kNasalQ, 500, 2500},
	{Peak, vs::key::kPresGain, vs::key::kPresFreq, vs::key::kPresQ, 1500, 8000},
	{Shelf, vs::key::kAirGain, vs::key::kAirFreq, nullptr, 5000, 16000},
};

double mag_db(const vs::BiquadCoefs &c, double f, double fs)
{
	if (c.is_identity())
		return 0.0;
	const double w = 2.0 * 3.141592653589793 * f / fs;
	const std::complex<double> z1 = std::polar(1.0, -w);
	const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z1 * z1;
	const std::complex<double> den = 1.0 + c.a1 * z1 + c.a2 * z1 * z1;
	return 20.0 * std::log10(std::max(1e-9, std::abs(num) / std::abs(den)));
}

bool same_state(const EqState &a, const EqState &b)
{
	return a.eq_on == b.eq_on && a.hpf_on == b.hpf_on && a.hpf_freq == b.hpf_freq && a.hpf_slope == b.hpf_slope &&
	       a.low_gain == b.low_gain && a.low_freq == b.low_freq && a.mud_gain == b.mud_gain &&
	       a.mud_freq == b.mud_freq && a.mud_q == b.mud_q && a.nasal_gain == b.nasal_gain &&
	       a.nasal_freq == b.nasal_freq && a.nasal_q == b.nasal_q && a.pres_gain == b.pres_gain &&
	       a.pres_freq == b.pres_freq && a.pres_q == b.pres_q && a.air_gain == b.air_gain &&
	       a.air_freq == b.air_freq && a.lpf_freq == b.lpf_freq;
}

} // namespace

EqCurveView::EqCurveView(QWidget *parent) : QWidget(parent)
{
	setMouseTracking(true);
	setMinimumHeight(140);
}

void EqCurveView::setState(const EqState &s)
{
	if (drag_ >= 0 || same_state(s, s_))
		return;
	s_ = s;
	mark_dirty();
	update();
}

void EqCurveView::setSampleRate(int sr)
{
	sr = sr > 8000 ? sr : 48000;
	if (sr != sample_rate_) {
		sample_rate_ = sr;
		mark_dirty();
	}
}

void EqCurveView::setSpectrum(const std::vector<float> &db, double dt)
{
	const size_t n = db.size();
	if (spectrum_.size() != n)
		spectrum_.assign(n, -90.0f);
	// Сглаживаем соседние полосы (окно 1-2-3-2-1), чтобы не было «иголок».
	std::vector<float> smooth(n);
	for (size_t i = 0; i < n; i++) {
		double s = 0.0, w = 0.0;
		for (int k = -2; k <= 2; k++) {
			const long j = (long)i + k;
			if (j < 0 || j >= (long)n)
				continue;
			const double wk = 3 - std::abs(k);
			s += db[(size_t)j] * wk;
			w += wk;
		}
		smooth[i] = (float)(s / w);
	}
	// Плавный подъём и ещё более плавное затухание — «волна», а не мерцание.
	const float up = (float)std::min(1.0, dt * 14.0);
	const float down = (float)std::min(1.0, dt * 5.0);
	for (size_t i = 0; i < n; i++) {
		const float k = smooth[i] > spectrum_[i] ? up : down;
		spectrum_[i] += (smooth[i] - spectrum_[i]) * k;
	}
	update(plot().toAlignedRect());
}

void EqCurveView::setTexts(const QStringList &node_names, const QString &hz, const QString &khz, const QString &db,
			   const QString &off_hint)
{
	names_ = node_names;
	hz_ = hz;
	khz_ = khz;
	db_ = db;
	off_hint_ = off_hint;
	grid_ = QImage(); // подписи сетки зависят от единиц
}

QRectF EqCurveView::plot() const
{
	return QRectF(rect()).adjusted(1, 1, -1, -18);
}

double EqCurveView::x_of(double f) const
{
	const QRectF r = plot().adjusted(8, 0, -8, 0);
	return r.left() + std::log(f / kFMin) / std::log(kFMax / kFMin) * r.width();
}

double EqCurveView::f_of(double x) const
{
	const QRectF r = plot().adjusted(8, 0, -8, 0);
	return kFMin * std::pow(kFMax / kFMin, std::clamp((x - r.left()) / r.width(), 0.0, 1.0));
}

double EqCurveView::y_of(double db) const
{
	const QRectF r = plot().adjusted(0, 8, 0, -8);
	return r.center().y() - db / kDbRange * r.height() / 2.0;
}

double EqCurveView::db_of(double y) const
{
	const QRectF r = plot().adjusted(0, 8, 0, -8);
	return -(y - r.center().y()) / (r.height() / 2.0) * kDbRange;
}

double EqCurveView::response_db(const EqState &st, double f) const
{
	if (!st.eq_on)
		return 0.0;
	const double fs = sample_rate_;
	double t = 0.0;
	if (st.hpf_on) {
		if (st.hpf_slope >= 24) {
			t += mag_db(vs::biquad::high_pass(fs, st.hpf_freq, vs::biquad::kButter4Q1), f, fs);
			t += mag_db(vs::biquad::high_pass(fs, st.hpf_freq, vs::biquad::kButter4Q2), f, fs);
		} else {
			t += mag_db(vs::biquad::high_pass(fs, st.hpf_freq, vs::biquad::kButter2Q), f, fs);
		}
	}
	t += mag_db(vs::biquad::low_shelf(fs, st.low_freq, st.low_gain), f, fs);
	t += mag_db(vs::biquad::peaking(fs, st.mud_freq, st.mud_q, st.mud_gain), f, fs);
	t += mag_db(vs::biquad::peaking(fs, st.nasal_freq, st.nasal_q, st.nasal_gain), f, fs);
	t += mag_db(vs::biquad::peaking(fs, st.pres_freq, st.pres_q, st.pres_gain), f, fs);
	t += mag_db(vs::biquad::high_shelf(fs, st.air_freq, st.air_gain), f, fs);
	if (st.lpf_freq < 19500.0 && st.lpf_freq < fs * 0.45) {
		t += mag_db(vs::biquad::low_pass(fs, st.lpf_freq, vs::biquad::kButter4Q1), f, fs);
		t += mag_db(vs::biquad::low_pass(fs, st.lpf_freq, vs::biquad::kButter4Q2), f, fs);
	}
	return t;
}

void EqCurveView::apply_band(EqState &st, int i, double freq, double gain)
{
	switch (i) {
	case 0:
		st.hpf_freq = freq;
		break;
	case 1:
		st.low_freq = freq;
		st.low_gain = gain;
		break;
	case 2:
		st.mud_freq = freq;
		st.mud_gain = gain;
		break;
	case 3:
		st.nasal_freq = freq;
		st.nasal_gain = gain;
		break;
	case 4:
		st.pres_freq = freq;
		st.pres_gain = gain;
		break;
	default:
		st.air_freq = freq;
		st.air_gain = gain;
		break;
	}
}

double EqCurveView::node_freq(int i) const
{
	switch (i) {
	case 0:
		return s_.hpf_freq;
	case 1:
		return s_.low_freq;
	case 2:
		return s_.mud_freq;
	case 3:
		return s_.nasal_freq;
	case 4:
		return s_.pres_freq;
	default:
		return s_.air_freq;
	}
}

// Усиление самой полосы (то, что стоит на ползунке ниже).
double EqCurveView::band_gain(int i) const
{
	switch (i) {
	case 0:
		return 0.0;
	case 1:
		return s_.low_gain;
	case 2:
		return s_.mud_gain;
	case 3:
		return s_.nasal_gain;
	case 4:
		return s_.pres_gain;
	default:
		return s_.air_gain;
	}
}

double EqCurveView::node_q(int i) const
{
	switch (i) {
	case 2:
		return s_.mud_q;
	case 3:
		return s_.nasal_q;
	case 4:
		return s_.pres_q;
	default:
		return 0.0;
	}
}

// Где проходит итоговая кривая на частоте точки — точка всегда сидит на линии.
// (У «полки» на своей частоте лишь половина усиления, и соседние полосы тоже складываются.)
double EqCurveView::node_level(int i) const
{
	if (nodes_dirty_) {
		for (int k = 0; k < kNodes; k++)
			node_db_[k] = response_db(s_, node_freq(k));
		nodes_dirty_ = false;
	}
	return node_db_[i];
}

QPointF EqCurveView::node_pos(int i) const
{
	const double g = std::clamp(node_level(i), -kDbRange + 1.0, kDbRange - 1.0);
	return QPointF(x_of(node_freq(i)), y_of(g));
}

double EqCurveView::snap_freq(int i, double freq) const
{
	const NodeDef &d = kDefs[i];
	freq = std::clamp(freq, d.fmin, d.fmax);
	return freq < 1000.0 ? std::round(freq / 5.0) * 5.0 : std::round(freq / 50.0) * 50.0;
}

// Подбирает усиление полосы так, чтобы итоговая кривая на её частоте прошла через курсор.
double EqCurveView::solve_gain(int i, double freq, double target_db) const
{
	EqState t = s_;
	auto level = [&](double g) {
		apply_band(t, i, freq, g);
		return response_db(t, freq);
	};
	double lo = -12.0, hi = 12.0;
	if (target_db <= level(lo))
		return lo;
	if (target_db >= level(hi))
		return hi;
	for (int k = 0; k < 22; k++) {
		const double mid = 0.5 * (lo + hi);
		(level(mid) < target_db ? lo : hi) = mid;
	}
	return 0.5 * (lo + hi);
}

int EqCurveView::hit(const QPointF &pos) const
{
	int best = -1;
	double best_d = 14.0;
	for (int i = 0; i < kNodes; i++) {
		if (i == 0 && !s_.hpf_on)
			continue;
		const QPointF d = node_pos(i) - pos;
		const double dist = std::hypot(d.x(), d.y());
		if (dist < best_d) {
			best_d = dist;
			best = i;
		}
	}
	return best;
}

void EqCurveView::set_node(int i, double freq, double gain, bool final)
{
	const NodeDef &d = kDefs[i];
	freq = snap_freq(i, freq);
	gain = std::round(std::clamp(gain, -12.0, 12.0) * 10.0) / 10.0;
	apply_band(s_, i, freq, gain);
	mark_dirty();
	update();
	if (!onChanged)
		return;
	// Колбэк может привести к перестройке окна OBS и удалению этого виджета:
	// держим копию колбэка и проверяем, жив ли виджет, перед вторым вызовом.
	const QPointer<EqCurveView> self(this);
	const auto cb = onChanged;
	cb(d.freq_key, freq, final);
	if (self && d.gain_key)
		cb(d.gain_key, gain, final);
}
QString EqCurveView::freq_text(double f) const
{
	if (f >= 1000.0)
		return QString::number(f / 1000.0, 'f', f >= 10000.0 ? 1 : 2).replace(QRegularExpression("\\.?0+$"), "") +
		       khz_;
	return QString::number((int)std::lround(f)) + hz_;
}

// Сетка и кривая меняются редко — рисуем их в отдельные картинки и только накладываем.
void EqCurveView::rebuild_layers()
{
	const double dpr = devicePixelRatioF();
	const QSize phys((int)std::ceil(width() * dpr), (int)std::ceil(height() * dpr));
	const QRectF r = plot();
	const double zero = y_of(0.0);

	if (grid_.size() != phys || grid_dpr_ != dpr) {
		grid_ = QImage(phys, QImage::Format_ARGB32_Premultiplied);
		grid_.setDevicePixelRatio(dpr);
		grid_.fill(Qt::transparent);
		grid_dpr_ = dpr;
		mark_dirty();
		QPainter p(&grid_);
		p.setRenderHint(QPainter::Antialiasing);
		paint_grid(p, r, zero);
	}
	if (!curve_dirty_)
		return;
	curve_dirty_ = false;
	curve_ = QImage(phys, QImage::Format_ARGB32_Premultiplied);
	curve_.setDevicePixelRatio(dpr);
	curve_.fill(Qt::transparent);
	QPainter p(&curve_);
	p.setRenderHint(QPainter::Antialiasing);
	paint_curve(p, r, zero);
}

void EqCurveView::paint_grid(QPainter &p, const QRectF &r, double zero)
{
	const QPalette &pal = palette();
	p.setPen(QPen(style::text(pal, 40), 1.0));
	p.setBrush(style::well(pal));
	p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

	p.setFont(style::font(this, 0.85));
	const double grid_f[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
	for (double f : grid_f) {
		const bool major = f == 100 || f == 1000 || f == 10000;
		const double x = x_of(f);
		p.setPen(QPen(style::text(pal, major ? 34 : 16), 1.0));
		p.drawLine(QPointF(x, r.top() + 1), QPointF(x, r.bottom() - 1));
		if (major) {
			p.setPen(style::text(pal, 130));
			p.drawText(QRectF(x - 30, r.bottom() + 2, 60, 16), Qt::AlignCenter,
				   f >= 1000 ? QString::number((int)(f / 1000)) + khz_ : QString::number((int)f) + hz_);
		}
	}
	for (double g : {-12.0, -6.0, 6.0, 12.0}) {
		p.setPen(QPen(style::text(pal, 14), 1.0));
		p.drawLine(QPointF(r.left() + 1, y_of(g)), QPointF(r.right() - 1, y_of(g)));
	}
	p.setPen(QPen(style::text(pal, 60), 1.0));
	p.drawLine(QPointF(r.left() + 1, zero), QPointF(r.right() - 1, zero));
}

void EqCurveView::paint_curve(QPainter &p, const QRectF &r, double zero)
{
	const QPalette &pal = palette();
	QPainterPath curve;
	const int pts = std::max(64, (int)r.width() / 3);
	for (int i = 0; i <= pts; i++) {
		const double x = r.left() + r.width() * i / pts;
		const double y = y_of(std::clamp(response_db(s_, f_of(x)), -kDbRange - 3, kDbRange + 3));
		if (i == 0)
			curve.moveTo(x, y);
		else
			curve.lineTo(x, y);
	}
	(void)zero;
	p.setClipRect(r.adjusted(1, 1, -1, -1));
	p.setBrush(Qt::NoBrush);
	QColor line = s_.eq_on ? style::accent(pal) : style::text(pal, 90);
	if (s_.eq_on && style::is_dark(pal))
		line = line.lighter(140); // синий OBS на тёмном фоне иначе теряется
	p.setPen(QPen(line, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.drawPath(curve);
}

void EqCurveView::paintEvent(QPaintEvent *)
{
	rebuild_layers();
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF r = plot();
	p.drawImage(QPoint(0, 0), grid_);

	// Живой спектр голоса — нейтральной серой заливкой, плавной кривой через точки полос.
	if (!spectrum_.empty()) {
		const QRectF inner = r.adjusted(1, 1, -1, -1);
		const int n = (int)spectrum_.size();
		std::vector<QPointF> pts((size_t)n);
		for (int i = 0; i < n; i++) {
			const double x = inner.left() + (i + 0.5) / n * inner.width();
			const double level = std::clamp((spectrum_[(size_t)i] + 90.0) / 72.0, 0.0, 1.0);
			pts[(size_t)i] = QPointF(x, inner.bottom() - level * inner.height() * 0.9);
		}
		QPainterPath sp;
		sp.moveTo(inner.left(), inner.bottom());
		sp.lineTo(inner.left(), pts[0].y());
		sp.lineTo(pts[0]);
		// Квадратичные кривые через середины отрезков — без изломов.
		for (int i = 1; i < n; i++)
			sp.quadTo(pts[(size_t)i - 1], (pts[(size_t)i - 1] + pts[(size_t)i]) / 2.0);
		sp.lineTo(pts[(size_t)n - 1]);
		sp.lineTo(inner.right(), pts[(size_t)n - 1].y());
		sp.lineTo(inner.right(), inner.bottom());
		sp.closeSubpath();
		p.save();
		p.setClipRect(inner);
		p.fillPath(sp, style::text(palette(), 26));
		p.restore();
	}

	p.drawImage(QPoint(0, 0), curve_);
	paint_nodes(p);
}

void EqCurveView::paint_nodes(QPainter &p)
{
	const QPalette &pal = palette();
	const QRectF r = plot();
	QColor accent = style::accent(pal);
	if (style::is_dark(pal))
		accent = accent.lighter(140);

	for (int i = 0; i < kNodes; i++) {
		if (i == 0 && !s_.hpf_on)
			continue;
		const QPointF c = node_pos(i);
		const bool hot = i == hover_ || i == drag_;
		const double rad = hot ? 6.5 : 5.0;
		p.setPen(QPen(hot ? style::text(pal) : style::text(pal, s_.eq_on ? 220 : 110), 1.5));
		p.setBrush(hot ? accent : style::well(pal));
		p.drawEllipse(c, rad, rad);
	}

	// Подсказка над точкой
	const int tip = drag_ >= 0 ? drag_ : hover_;
	if (tip >= 0 && tip < names_.size()) {
		QString t = names_[tip] + "  " + freq_text(node_freq(tip));
		if (tip > 0) {
			const double g = band_gain(tip);
			t += "  " + QString(g > 0 ? "+" : "") + QString::number(g, 'f', 1) + db_;
		}
		if (node_q(tip) > 0.0)
			t += "  Q " + QString::number(node_q(tip), 'f', 2);
		p.setFont(style::font(this, 0.9));
		const QFontMetricsF fm(p.font());
		const double tw = fm.horizontalAdvance(t) + 16;
		const double th = fm.height() + 8;
		const QPointF c = node_pos(tip);
		QRectF b(c.x() - tw / 2, c.y() - th - 12, tw, th);
		if (b.top() < r.top() + 2)
			b.moveTop(c.y() + 12);
		b.moveLeft(std::clamp(b.left(), r.left() + 2, r.right() - tw - 2));
		p.setPen(QPen(style::text(pal, 60), 1.0));
		p.setBrush(pal.color(QPalette::Window));
		p.drawRoundedRect(b, 4, 4);
		p.setPen(style::text(pal));
		p.drawText(b, Qt::AlignCenter, t);
	}

	if (!s_.eq_on && !off_hint_.isEmpty()) {
		p.setFont(style::font(this, 0.95));
		p.setPen(style::text(pal, 170));
		p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, off_hint_);
	}
}

void EqCurveView::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !s_.eq_on)
		return;
	drag_ = hit(e->position());
	if (drag_ >= 0)
		setCursor(Qt::ClosedHandCursor);
	update();
}

void EqCurveView::mouseMoveEvent(QMouseEvent *e)
{
	if (drag_ >= 0) {
		// Точка идёт ровно за курсором: подбираем усиление полосы так,
		// чтобы итоговая кривая прошла через положение мыши.
		const double freq = snap_freq(drag_, f_of(e->position().x()));
		const double gain = drag_ == 0 ? 0.0 : solve_gain(drag_, freq, db_of(e->position().y()));
		set_node(drag_, freq, gain, false);
		return;
	}
	const int h = s_.eq_on ? hit(e->position()) : -1;
	if (h != hover_) {
		hover_ = h;
		setCursor(h >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
		update();
	}
}

void EqCurveView::mouseReleaseEvent(QMouseEvent *e)
{
	if (drag_ < 0 || e->button() != Qt::LeftButton)
		return;
	const int i = drag_;
	drag_ = -1;
	setCursor(hover_ >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
	set_node(i, node_freq(i), band_gain(i), true);
}

void EqCurveView::mouseDoubleClickEvent(QMouseEvent *e)
{
	const int i = hit(e->position());
	if (i > 0 && s_.eq_on)
		set_node(i, node_freq(i), 0.0, true);
}

void EqCurveView::wheelEvent(QWheelEvent *e)
{
	const int i = hit(e->position());
	if (i < 0 || !kDefs[i].q_key || !s_.eq_on) {
		e->ignore(); // прокрутка окна
		return;
	}
	const double steps = e->angleDelta().y() / 120.0;
	const double q = std::clamp(node_q(i) * std::pow(1.15, steps), 0.3, 4.0);
	switch (i) {
	case 2:
		s_.mud_q = q;
		break;
	case 3:
		s_.nasal_q = q;
		break;
	default:
		s_.pres_q = q;
		break;
	}
	mark_dirty();
	update();
	e->accept();
	if (onChanged) {
		const auto cb = onChanged; // последнее действие: виджет может быть удалён внутри
		cb(kDefs[i].q_key, std::round(q * 20.0) / 20.0, true);
	}
}

void EqCurveView::leaveEvent(QEvent *)
{
	if (drag_ < 0 && hover_ >= 0) {
		hover_ = -1;
		update();
	}
}

void EqCurveView::changeEvent(QEvent *e)
{
	// Сменилась тема OBS или шрифт — перерисовываем сетку и кривую.
	if (e->type() == QEvent::PaletteChange || e->type() == QEvent::FontChange) {
		grid_ = QImage();
		mark_dirty();
	}
	QWidget::changeEvent(e);
}

} // namespace vsui
