// Демо-ролик «Голосовой студии» под музыку (123 уд/мин, 35,5 с): настоящий живой блок
// и окно настроек в 3D, сцена трансляции со зрителями и чатом.
// Стиль — киберспортивный плакат: чёрный, сигнальный оранжевый и фиолетовый, осколки
// и клетки на фоне, узкий наклонный шрифт, моноширинные подписи, векторные иконки,
// без свечений. Кадры считаются по виртуальному времени (30 к/с), поэтому тяжёлые
// сцены не дают рывков; готовые кадры уходят в ffmpeg вместе с музыкой.
//
//   voice-studio-demo-video <data> <ffmpeg.exe> <music> <out.mp4> ru|en
//   voice-studio-demo-video <data> stills <папка> ru|en 1.0,8.5,…    — отдельные кадры для проверки
//
// Запускать с OBS\bin\64bit в PATH (obs.dll, Qt) и QT_PLUGIN_PATH на ту же папку.

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScrollBar>
#include <QSvgRenderer>

#include <cstdio>
#include <random>

#include "eq-view.hpp"
#include "preview-mock.hpp"
#include "style.hpp"

#ifndef VOICE_STUDIO_UPDATE_REPO
#define VOICE_STUDIO_UPDATE_REPO ""
#endif

using namespace vs::key;
using namespace mock;

namespace {

// ---------------------------------------------------------------------------
// Кадр, время, музыка

constexpr int kW = 1920, kH = 1080, kFps = 30;
constexpr double kDpr = 2.0;     // окна рисуются в двойном разрешении — можно приближать
const QSize kWin(880, 700);      // окно OBS в логических точках
constexpr double kTitleH = 36.0; // заголовок окна на карточке
const QSizeF kCard(880, 736);    // карточка окна (с заголовком), логические точки
constexpr double kCardScale = 1.18;

constexpr double kB0 = -0.028, kBeat = 60.0 / 123.0; // сетка долей трека
double bt(double k)
{
	return kB0 + k * kBeat;
}
const double T_STREAM = bt(8), T_GROOVE = bt(16), T_RISE = bt(26), T_CLICK = bt(27), T_DROP = bt(32),
	     T_AFTER = bt(34), T_FEAT = bt(40), T_LANG = bt(48), T_BREAK = bt(56), T_DROP2 = bt(64),
	     T_STAB1 = bt(70), T_STAB2 = bt(71), T_END = 35.5;

double clamp01(double x)
{
	return std::clamp(x, 0.0, 1.0);
}
double lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}
double ramp(double t, double a, double b)
{
	return clamp01((t - a) / (b - a));
}
double smooth(double x)
{
	x = clamp01(x);
	return x * x * (3 - 2 * x);
}
double ease_out(double x)
{
	x = clamp01(x);
	return 1 - std::pow(1 - x, 3);
}
double ease_in(double x)
{
	x = clamp01(x);
	return x * x * x;
}
double ease_in_out(double x)
{
	x = clamp01(x);
	return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}
double ease_out_back(double x)
{
	x = clamp01(x);
	const double c1 = 1.70158, c3 = c1 + 1;
	return 1 + c3 * std::pow(x - 1, 3) + c1 * std::pow(x - 1, 2);
}
double ease_out_expo(double x)
{
	x = clamp01(x);
	return x >= 1 ? 1 : 1 - std::pow(2, -10 * x);
}
double since_beat(double t)
{
	const double ph = std::fmod(t - kB0, kBeat);
	return ph < 0 ? ph + kBeat : ph;
}
int beat_index(double t)
{
	return (int)std::floor((t - kB0) / kBeat);
}
// Есть ли в этот момент бочка (во вступлении — тише, в паузах перед дропами — нет).
double kick(double t)
{
	if (t < T_GROOVE)
		return 0.5;
	if ((t > bt(26) && t < T_DROP) || (t > bt(56.5) && t < T_DROP2) || t > bt(71.5))
		return 0.0;
	return 1.0;
}
double pulse(double t, double decay = 9.0)
{
	return kick(t) * std::exp(-since_beat(t) * decay);
}

double hash01(int i)
{
	uint32_t x = (uint32_t)i * 2654435761u;
	x ^= x >> 13;
	x *= 0x5bd1e995u;
	x ^= x >> 15;
	return (x & 0xFFFFFF) / double(0xFFFFFF);
}

namespace col {
const QColor bg(0x0A, 0x0C, 0x12), panel(0x12, 0x15, 0x1E), panel2(0x1A, 0x1E, 0x29), line(0x2A, 0x30, 0x40);
// Один цвет — синий плагина (как акцент в окне OBS и в README) и его оттенки.
const QColor accent(0x2F, 0x6F, 0xF0), accent_lt(0x8D, 0xB2, 0xFF), accent_dk(0x17, 0x3A, 0x8C);
const QColor white(0xF4, 0xF4, 0xF6), gray(0x9A, 0x9A, 0xA6), dim(0x5E, 0x5E, 0x68);
} // namespace col

// ---------------------------------------------------------------------------
// Шрифты

QFont display_font(double px)
{
	QFont f(QStringLiteral("Bahnschrift Condensed"));
	f.setWeight(QFont::Bold);
	f.setPixelSize((int)std::lround(px));
	return f;
}

QFont mono_font(double px, int weight = QFont::DemiBold)
{
	QFont f(QStringLiteral("Cascadia Mono"));
	f.setWeight(QFont::Weight(weight));
	f.setPixelSize((int)std::lround(px));
	return f;
}

QFont body_font(double px, int weight = QFont::Normal)
{
	QFont f(QStringLiteral("Bahnschrift"));
	f.setWeight(QFont::Weight(weight));
	f.setPixelSize((int)std::lround(px));
	return f;
}

// ---------------------------------------------------------------------------
// Векторные иконки (24×24, линии) — вместо эмодзи

const std::map<QString, QString> kIcons = {
	{"mic", R"(<rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 11a7 7 0 0 0 14 0"/><path d="M12 18v4"/><path d="M8 22h8"/>)"},
	{"noise", R"(<path d="M2 12h2l1-4 1 8 1-10 1 11 1-6 1 4 1-8 1 10 1-5 1 3 1-9 1 11 1-5 1 2h2"/>)"},
	{"wave", R"(<path d="M2 12c1.5 0 1.5-5 3-5s1.5 10 3 10 1.5-14 3-14 1.5 18 3 18 1.5-14 3-14 1.5 10 3 10 1.5-5 2-5"/>)"},
	{"sparkle", R"(<path d="M11 3l1.9 5.6L18.5 10.5l-5.6 1.9L11 18l-1.9-5.6L3.5 10.5l5.6-1.9z"/><path d="M19 14.5l.8 2.2 2.2.8-2.2.8-.8 2.2-.8-2.2-2.2-.8 2.2-.8z"/>)"},
	{"check", R"(<path d="M4 12.5l5 5L20 6.5"/>)"},
	{"shield-check", R"(<path d="M12 2l8 3v6c0 5-3.5 9-8 11-4.5-2-8-6-8-11V5z"/><path d="M8.5 12l2.5 2.5 4.5-5"/>)"},
	{"lock", R"(<rect x="4" y="10" width="16" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/>)"},
	{"eye", R"(<path d="M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>)"},
	{"user-plus", R"(<circle cx="9" cy="8" r="4"/><path d="M2 21v-1a7 7 0 0 1 14 0v1"/><path d="M19 8v6M16 11h6"/>)"},
	{"user-minus", R"(<circle cx="9" cy="8" r="4"/><path d="M2 21v-1a7 7 0 0 1 14 0v1"/><path d="M16 11h6"/>)"},
	{"heart", R"(<path d="M12 21s-7.5-4.6-9.5-9.3C1 8.2 3.2 4.5 6.8 4.5c2.1 0 3.6 1.1 5.2 3 1.6-1.9 3.1-3 5.2-3 3.6 0 5.8 3.7 4.3 7.2C19.5 16.4 12 21 12 21z"/>)"},
	{"download", R"(<path d="M12 3v12"/><path d="M7 10l5 5 5-5"/><path d="M4 21h16"/>)"},
	{"globe", R"(<circle cx="12" cy="12" r="10"/><path d="M2 12h20"/><path d="M12 2a15 15 0 0 1 0 20a15 15 0 0 1 0-20z"/>)"},
	{"refresh", R"(<path d="M21 12a9 9 0 0 1-15.5 6.2L3 16"/><path d="M3 21v-5h5"/><path d="M3 12a9 9 0 0 1 15.5-6.2L21 8"/><path d="M21 3v5h-5"/>)"},
	{"chip", R"(<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/><path d="M9 12h1.2l1-2.2 1.6 4.4 1-2.2H15"/>)"},
	{"gate", R"(<path d="M4 21V5M20 21V5"/><path d="M4 9h16M4 16h16"/><path d="M9 9v7M15 9v7"/>)"},
	{"sliders", R"(<path d="M6 3v18M12 3v18M18 3v18"/><circle cx="6" cy="15" r="2.4" fill="#0D0D0F"/><circle cx="12" cy="8" r="2.4" fill="#0D0D0F"/><circle cx="18" cy="14" r="2.4" fill="#0D0D0F"/>)"},
	{"deesser", R"(<path d="M16 7.5c-.8-1.6-2.3-2.5-4.2-2.5-2.4 0-4.1 1.4-4.1 3.3 0 4.5 8.6 2.6 8.6 7.3 0 2-1.9 3.4-4.4 3.4-2.1 0-3.7-1-4.4-2.7"/><path d="M20 4L4 20"/>)"},
	{"compress", R"(<path d="M4 4l5 5M9 5v4H5"/><path d="M20 20l-5-5M15 19v-4h4"/><path d="M20 4l-5 5M15 5v4h4"/><path d="M4 20l5-5M5 15h4v4"/>)"},
	{"flame", R"(<path d="M12 22c4 0 7-2.8 7-7 0-4.5-4-6.5-4-11-2.5 1.5-4.5 4-4.5 6.5-1.3-.8-2-2-2.2-3.5C6.5 9 5 11.6 5 15c0 4.2 3 7 7 7z"/>)"},
	{"mask", R"(<path d="M3 7c3-1.5 6-2 9-2s6 .5 9 2c0 7-3.5 12-9 12S3 14 3 7z"/><path d="M6.8 10.8c1-.8 2.3-.8 3.2 0M14 10.8c.9-.8 2.2-.8 3.2 0"/><path d="M9 15c1.8 1.2 4.2 1.2 6 0"/>)"},
	{"shield", R"(<path d="M12 2l8 3v6c0 5-3.5 9-8 11-4.5-2-8-6-8-11V5z"/><path d="M8 12h8"/>)"},
	{"arrow-up", R"(<path d="M12 19V5"/><path d="M5 12l7-7 7 7"/>)"},
	{"chat", R"(<path d="M21 12a8 8 0 0 1-11.6 7.1L3 21l1.9-5.4A8 8 0 1 1 21 12z"/>)"},
	{"x", R"(<path d="M6 6l12 12M18 6L6 18"/>)"},
	{"play", R"(<path d="M7 4l13 8-13 8z"/>)"},
	{"search", R"(<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>)"},
	{"bell", R"(<path d="M6 8a6 6 0 0 1 12 0c0 7 3 9 3 9H3s3-2 3-9"/><path d="M10.3 21a1.9 1.9 0 0 0 3.4 0"/>)"},
	{"chevrons", R"(<path d="M11 17l-5-5 5-5"/><path d="M18 17l-5-5 5-5"/>)"},
	{"users", R"(<circle cx="9" cy="7" r="4"/><path d="M3 21v-2a4 4 0 0 1 4-4h4a4 4 0 0 1 4 4v2"/><path d="M16 3.1a4 4 0 0 1 0 7.8M21 21v-2a4 4 0 0 0-3-3.9"/>)"},
	{"star", R"(<path d="M12 2l3.1 6.3 6.9 1-5 4.9 1.2 6.8L12 17.8 5.8 21l1.2-6.8-5-4.9 6.9-1z"/>)"},
	{"user", R"(<circle cx="12" cy="8" r="4"/><path d="M4 21v-1a8 8 0 0 1 16 0v1"/>)"},
	{"gem", R"(<path d="M6 3h12l4 6-10 12L2 9z"/><path d="M2 9h20"/><path d="M12 21L8 9l4-6 4 6z"/>)"},
};

std::map<QString, QImage> g_icon_cache;

const QImage &icon_image(const QString &name, QColor color, int px, double stroke = 2.0)
{
	const QString key = QStringLiteral("%1|%2|%3|%4").arg(name).arg(color.name(QColor::HexArgb)).arg(px).arg(stroke);
	auto it = g_icon_cache.find(key);
	if (it != g_icon_cache.end())
		return it->second;
	const QString svg = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" "
					   "stroke=\"%1\" stroke-width=\"%2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%3</svg>")
				    .arg(color.name(QColor::HexRgb))
				    .arg(stroke)
				    .arg(kIcons.at(name));
	QSvgRenderer r(svg.toUtf8());
	QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);
	r.render(&p, QRectF(0, 0, px, px));
	p.end();
	return g_icon_cache.emplace(key, img).first->second;
}

void draw_icon(QPainter &p, const QString &name, QPointF center, double size, QColor color, double stroke = 2.0)
{
	const QImage &img = icon_image(name, color, (int)std::ceil(size), stroke);
	p.drawImage(QRectF(center.x() - size / 2, center.y() - size / 2, size, size), img);
}

// ---------------------------------------------------------------------------
// Текст: крупное наклонное слово, моноширинная строка-тег, подписи

// Крупное узкое наклонное слово, выезжает с открытием слева. block — плашка за словом.
void big_text(QPainter &p, const QString &text, QPointF base, double px, double local, double out, QColor color,
	      Qt::Alignment align = Qt::AlignLeft, QColor block = QColor(), double skew = -0.2)
{
	if (local <= 0 || out >= 1)
		return;
	const QFont f = display_font(px);
	const QFontMetricsF fm(f);
	const double w = fm.horizontalAdvance(text);
	double x = base.x();
	if (align & Qt::AlignHCenter)
		x -= w / 2;
	else if (align & Qt::AlignRight)
		x -= w;
	const double k = ease_out(local / 0.32), ko = ease_in(out);
	p.save();
	p.translate(x + (1 - k) * -px * 0.6 + ko * px * 0.8, base.y());
	p.shear(skew, 0);
	const double cap = fm.capHeight();
	if (block.isValid())
		p.fillRect(QRectF(-px * 0.12, -cap - px * 0.12, (w + px * 0.24) * k, cap + px * 0.24), block);
	p.setClipRect(QRectF(-px * 0.3, -fm.ascent() - 10, (w + px * 0.6) * k, fm.height() + 20));
	p.setOpacity(p.opacity() * (1 - ko));
	p.setFont(f);
	p.setPen(color);
	p.drawText(QPointF(0, 0), text);
	p.restore();
}

// «[ТЕГ]: текст_» — как строка консоли, текст печатается, курсор мигает.
void tag_line(QPainter &p, QPointF pos, const QString &prefix, const QString &text, double local, double out = 0,
	      double px = 30)
{
	if (local <= 0 || out >= 1)
		return;
	const QFont f = mono_font(px);
	const QFontMetricsF fm(f);
	const QString head = QStringLiteral("[%1]:").arg(prefix);
	const int n = std::clamp((int)((local - 0.15) * 34), 0, (int)text.size());
	const QString shown = text.left(n);
	const double full_w = fm.horizontalAdvance(head + QStringLiteral(" ") + text + QStringLiteral("_")) + px * 1.2;
	const double h = fm.height() + px * 0.55;
	const double k = ease_out(local / 0.22);
	p.save();
	p.setOpacity(p.opacity() * (1 - ease_in(out)));
	p.fillRect(QRectF(pos.x(), pos.y(), full_w * k, h), QColor(0x0D, 0x0D, 0x0F, 240));
	p.fillRect(QRectF(pos.x(), pos.y(), 5, h), col::accent);
	p.setClipRect(QRectF(pos.x(), pos.y(), full_w * k, h));
	p.setFont(f);
	const double base = pos.y() + h / 2 + fm.capHeight() / 2;
	double x = pos.x() + px * 0.6;
	p.setPen(col::accent);
	p.drawText(QPointF(x, base), head);
	x += fm.horizontalAdvance(head + QStringLiteral(" "));
	p.setPen(col::white);
	p.drawText(QPointF(x, base), shown);
	x += fm.horizontalAdvance(shown);
	if (n < text.size() || std::fmod(local, 0.66) < 0.38) {
		p.setPen(col::accent);
		p.drawText(QPointF(x, base), QStringLiteral("_"));
	}
	p.restore();
}

// Шапка раздела вверху слева, как заголовок окна консоли.
void hud(QPainter &p, const QString &section, int index, int total, double local, double out)
{
	if (local <= 0 || out >= 1)
		return;
	const QFont f = mono_font(22);
	const QFontMetricsF fm(f);
	const double k = ease_out(local / 0.3);
	const QRectF r(56, 44, 640 * k, 52);
	p.save();
	p.setOpacity(p.opacity() * (1 - ease_in(out)));
	p.fillRect(r, col::panel);
	p.setPen(QPen(col::line, 1.5));
	p.drawRect(r);
	p.setClipRect(r);
	p.setFont(f);
	const double base = r.center().y() + fm.capHeight() / 2;
	p.setPen(col::white);
	p.drawText(QPointF(r.left() + 22, base), QStringLiteral("VOICE STUDIO"));
	p.setPen(col::accent);
	const double x2 = r.left() + 22 + fm.horizontalAdvance(QStringLiteral("VOICE STUDIO  "));
	p.drawText(QPointF(x2, base), QStringLiteral(">>"));
	p.setPen(col::gray);
	p.drawText(QPointF(x2 + fm.horizontalAdvance(QStringLiteral(">>  ")), base), section);
	const QString idx = QStringLiteral("[ %1/%2 ]").arg(index, 2, 10, QChar('0')).arg(total, 2, 10, QChar('0'));
	p.drawText(QPointF(r.left() + 640 - 22 - fm.horizontalAdvance(idx), base), idx);
	p.restore();
}

// Моноширинная плашка (кнопка, значок, подпись).
QRectF chip(QPainter &p, QPointF center, const QString &text, double px, QColor bg, QColor fg, double scale = 1,
	    double opacity = 1, const QString &icon = QString(), QColor border = QColor())
{
	if (opacity <= 0)
		return QRectF();
	const QFont f = mono_font(px * scale);
	const QFontMetricsF fm(f);
	const double isz = icon.isEmpty() ? 0 : px * 1.15 * scale;
	const double gap = icon.isEmpty() ? 0 : px * 0.45 * scale;
	const QSizeF s(fm.horizontalAdvance(text) + isz + gap + px * 1.3 * scale, fm.height() + px * 0.7 * scale);
	const QRectF r(center - QPointF(s.width() / 2, s.height() / 2), s);
	p.save();
	p.setOpacity(p.opacity() * opacity);
	p.fillRect(r, bg);
	if (border.isValid()) {
		p.setPen(QPen(border, 2));
		p.setBrush(Qt::NoBrush);
		p.drawRect(r.adjusted(1, 1, -1, -1));
	}
	double x = r.left() + px * 0.65 * scale;
	if (!icon.isEmpty()) {
		draw_icon(p, icon, QPointF(x + isz / 2, r.center().y()), isz, fg, 2.2);
		x += isz + gap;
	}
	p.setFont(f);
	p.setPen(fg);
	p.drawText(QPointF(x, r.center().y() + fm.capHeight() / 2), text);
	p.restore();
	return r;
}

QString group_digits(long long n)
{
	QString s = QString::number(n);
	for (int i = s.size() - 3; i > 0; i -= 3)
		s.insert(i, QChar(0x2009));
	return s;
}

// Клетчатая заливка (как «шахматка» на референсах).
void checker(QPainter &p, const QRectF &r, double cell, QColor a, QColor b)
{
	p.fillRect(r, a);
	p.save();
	p.setClipRect(r);
	for (int y = 0; y * cell < r.height(); y++)
		for (int x = 0; x * cell < r.width(); x++)
			if ((x + y) % 2)
				p.fillRect(QRectF(r.left() + x * cell, r.top() + y * cell, cell, cell), b);
	p.restore();
}

// ---------------------------------------------------------------------------
// 3D-карточки: плоская картинка, повёрнутая в пространстве, с перспективой

struct Pose {
	double cx = kW / 2.0, cy = kH / 2.0, z = 0, rx = 0, ry = 0, rz = 0, scale = 1, opacity = 1;
};

Pose mix(const Pose &a, const Pose &b, double t)
{
	Pose r;
	r.cx = lerp(a.cx, b.cx, t);
	r.cy = lerp(a.cy, b.cy, t);
	r.z = lerp(a.z, b.z, t);
	r.rx = lerp(a.rx, b.rx, t);
	r.ry = lerp(a.ry, b.ry, t);
	r.rz = lerp(a.rz, b.rz, t);
	r.scale = lerp(a.scale, b.scale, t);
	r.opacity = lerp(a.opacity, b.opacity, t);
	return r;
}

constexpr double kFocal = 1700;

// rx < 0 — верх дальше от зрителя (как экран, на который смотрят чуть сверху).
QPointF project_point(double x, double y, const Pose &p)
{
	const double rz = p.rz * M_PI / 180, rx = p.rx * M_PI / 180, ry = p.ry * M_PI / 180;
	const double x1 = x * std::cos(rz) - y * std::sin(rz), y1 = x * std::sin(rz) + y * std::cos(rz);
	const double y2 = y1 * std::cos(rx), z2 = y1 * std::sin(rx);
	const double x3 = x1 * std::cos(ry) + z2 * std::sin(ry), z3 = -x1 * std::sin(ry) + z2 * std::cos(ry);
	const double k = kFocal / std::max(200.0, kFocal + z3 + p.z);
	return QPointF(p.cx + x3 * k, p.cy + y2 * k);
}

QPolygonF project_quad(QSizeF disp, const Pose &p)
{
	const double hw = disp.width() * p.scale / 2, hh = disp.height() * p.scale / 2;
	return QPolygonF({project_point(-hw, -hh, p), project_point(hw, -hh, p), project_point(hw, hh, p),
			  project_point(-hw, hh, p)});
}

QPolygonF rect4(QSizeF s)
{
	return QPolygonF({QPointF(0, 0), QPointF(s.width(), 0), QPointF(s.width(), s.height()), QPointF(0, s.height())});
}

QTransform card_transform(QSizeF img, QSizeF disp, const Pose &p)
{
	QTransform t;
	QTransform::quadToQuad(rect4(img), project_quad(disp, p), t);
	return t;
}

// Карточка: плоская «подложка» со сдвигом (без размытия), картинка и тонкая рамка.
QTransform draw_card(QPainter &p, const QImage &img, QSizeF disp, const Pose &pose, QColor border = col::line,
		     QColor plate = QColor(0, 0, 0, 170), double plate_off = 18)
{
	if (pose.opacity <= 0.003)
		return QTransform();
	p.save();
	p.setOpacity(p.opacity() * pose.opacity);
	if (plate.alpha() > 0) {
		Pose sp = pose;
		sp.cx += plate_off * pose.scale;
		sp.cy += plate_off * pose.scale;
		p.setPen(Qt::NoPen);
		p.setBrush(plate);
		p.drawPolygon(project_quad(disp, sp));
	}
	const QTransform t = card_transform(img.size(), disp, pose);
	p.setTransform(t, true);
	p.drawImage(0, 0, img);
	p.setTransform(QTransform());
	if (border.alpha() > 0) {
		p.setPen(QPen(border, 2.5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
		p.setBrush(Qt::NoBrush);
		p.drawPolygon(project_quad(disp, pose));
	}
	p.restore();
	return t;
}

// ---------------------------------------------------------------------------
// Частицы: плоские осколки, квадраты, клетки и иконки

struct Particle {
	QPointF p, v;
	double age = 0, life = 1, size = 10, angle = 0, spin = 0;
	QColor color;
	int kind = 0; // 0 квадрат, 1 треугольник-осколок, 2 иконка, 3 клетка 2×2
	QString icon;
	double gravity = 0, drag = 0.0;
};

class Particles {
public:
	std::vector<Particle> list;
	std::mt19937 rng{20261021};

	double rnd(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }

	void burst(QPointF at, int n, double speed_min, double speed_max, std::vector<QColor> colors, int kind = 1,
		   double life = 0.9, double size = 14, double gravity = 600)
	{
		for (int i = 0; i < n; i++) {
			Particle q;
			const double a = rnd(0, 2 * M_PI), s = rnd(speed_min, speed_max);
			q.p = at;
			q.v = QPointF(std::cos(a) * s, std::sin(a) * s);
			q.life = life * rnd(0.6, 1.2);
			q.size = size * rnd(0.5, 1.4);
			q.color = colors[(size_t)(i % colors.size())];
			static const int mixed[] = {1, 0, 3}; // осколок, квадрат, клетка
			q.kind = kind == 4 ? mixed[i % 3] : kind;
			q.gravity = gravity;
			q.drag = 1.8;
			q.angle = rnd(0, 360);
			q.spin = rnd(-540, 540);
			list.push_back(q);
		}
	}

	void icons(QPointF at, int n, const QStringList &names, std::vector<QColor> colors, double spread, double rise,
		   double life = 1.4, double size = 40)
	{
		for (int i = 0; i < n; i++) {
			Particle q;
			q.p = at + QPointF(rnd(-spread, spread), rnd(-10, 10));
			q.v = QPointF(rnd(-50, 50), -rise * rnd(0.7, 1.3));
			q.life = life * rnd(0.8, 1.2);
			q.size = size * rnd(0.8, 1.2);
			q.kind = 2;
			q.icon = names[(int)rnd(0, names.size() - 0.001)];
			q.color = colors[(size_t)(i % colors.size())];
			q.gravity = rise < 0 ? 300 : -30;
			q.drag = 0.4;
			list.push_back(q);
		}
	}

	// Квадраты, которые стягиваются к точке (зарядка перед дропом).
	void implode(QPointF to, int n, double radius, QColor c)
	{
		for (int i = 0; i < n; i++) {
			Particle q;
			const double a = rnd(0, 2 * M_PI), r = radius * rnd(0.7, 1.2);
			q.p = to + QPointF(std::cos(a) * r, std::sin(a) * r);
			q.life = rnd(0.45, 0.7);
			q.v = (to - q.p) / q.life;
			q.size = rnd(6, 13);
			q.color = c;
			q.kind = 0;
			q.angle = rnd(0, 90);
			list.push_back(q);
		}
	}

	void update(double dt)
	{
		for (Particle &q : list) {
			q.age += dt;
			q.v *= std::exp(-q.drag * dt);
			q.v.ry() += q.gravity * dt;
			q.p += q.v * dt;
			q.angle += q.spin * dt;
		}
		list.erase(std::remove_if(list.begin(), list.end(), [](const Particle &q) { return q.age >= q.life; }),
			   list.end());
	}

	void draw(QPainter &p)
	{
		for (const Particle &q : list) {
			const double k = q.age / q.life;
			const double s = q.size * (1 - 0.6 * k * k);
			p.save();
			p.setOpacity(q.kind == 2 ? std::min(1.0, (1 - k) * 2) * std::min(1.0, q.age * 8) : 1.0);
			p.translate(q.p);
			p.rotate(q.angle);
			p.setPen(Qt::NoPen);
			if (q.kind == 0) {
				p.fillRect(QRectF(-s / 2, -s / 2, s, s), q.color);
			} else if (q.kind == 1) {
				p.setBrush(q.color);
				p.drawPolygon(QPolygonF({QPointF(-s * 0.6, s * 0.45), QPointF(s * 0.7, s * 0.2), QPointF(-s * 0.1, -s * 0.7)}));
			} else if (q.kind == 3) {
				const double c = s / 2;
				p.fillRect(QRectF(-c, -c, c, c), q.color);
				p.fillRect(QRectF(0, 0, c, c), q.color);
				p.fillRect(QRectF(0, -c, c, c), col::white);
				p.fillRect(QRectF(-c, 0, c, c), col::white);
			} else {
				p.rotate(-q.angle);
				draw_icon(p, q.icon, QPointF(0, 0), s, q.color, 2.4);
			}
			p.restore();
		}
	}
};

// ---------------------------------------------------------------------------
// Тексты ролика

struct Msg {
	QString user, text;
	QColor color;
};
struct Feature {
	QString icon, title, desc, value;
};
struct Script {
	QString title, subtitle;
	QString w_intro, w_before, w_groove, w_rise, w_done, w_after, w_feat, w_lang, w_upd, w_fin;
	QString tag_intro, tag_before, tag_groove, tag_rise, tag_after, tag_feat, tag_lang, tag_upd, tag_fin;
	QString p_obs, p_chat, p_filter, p_setup, p_live, p_lang, p_github;
	QString sec_intro, sec_stream, sec_live, sec_setup, sec_feat, sec_lang, sec_upd;
	QString tag_before_badge, tag_after_badge, channel, stream_title, viewers_word, follow, chat_head;
	std::vector<Msg> chat_before, chat_after;
	QString call_levels, call_ai, call_spectrum, call_drag, listening;
	std::vector<Feature> features;
	QString upd_sig, upd_sha, upd_ready, upd_head;
	QString fin_cta;
};

Script script_for(bool ru)
{
	const QColor u1 = col::accent, u2 = col::accent_lt, u3 = col::accent_lt, u4 = col::white, u5 = QColor(0xFF, 0xB1, 0x99),
		     u6 = QColor(0xC9, 0xA8, 0xFF);
	Script s;
	if (ru) {
		s.title = QStringLiteral("ГОЛОСОВАЯ СТУДИЯ");
		s.subtitle = QStringLiteral("плагин для OBS Studio");
		s.w_intro = QStringLiteral("ЗВУЧИ");
		s.w_before = QStringLiteral("ШУМ?");
		s.w_groove = QStringLiteral("ВСЁ НА ВИДУ");
		s.w_rise = QStringLiteral("10 СЕКУНД");
		s.w_done = QStringLiteral("ГОТОВО");
		s.w_after = QStringLiteral("ЧИСТО");
		s.w_feat = QStringLiteral("ВСЁ В ОДНОМ");
		s.w_lang = QStringLiteral("12 ЯЗЫКОВ");
		s.w_upd = QStringLiteral("ОБНОВЛЯЕТСЯ САМ");
		s.w_fin = QStringLiteral("ЗВУЧИ ЧИСТО");
		s.p_obs = QStringLiteral("OBS");
		s.p_chat = QStringLiteral("ЧАТ");
		s.p_filter = QStringLiteral("ФИЛЬТР");
		s.p_setup = QStringLiteral("НАСТРОЙКА");
		s.p_live = QStringLiteral("ЭФИР");
		s.p_lang = QStringLiteral("ЯЗЫК");
		s.p_github = QStringLiteral("GITHUB");
		s.tag_intro = QStringLiteral("студийный звук любого микрофона");
		s.tag_before = QStringLiteral("шум, гул, треск — зрители уходят");
		s.tag_groove = QStringLiteral("уровни и спектр прямо в окне фильтра");
		s.tag_rise = QStringLiteral("слушаю ваш голос");
		s.tag_after = QStringLiteral("+2 826 зрителей");
		s.tag_feat = QStringLiteral("8 инструментов в одном фильтре");
		s.tag_lang = QStringLiteral("говорит на языке вашего OBS");
		s.tag_upd = QStringLiteral("каждая версия с цифровой подписью");
		s.tag_fin = QStringLiteral("agrysif/obs-voice-studio");
		s.sec_intro = QStringLiteral("PLUGIN");
		s.sec_stream = QStringLiteral("STREAM");
		s.sec_live = QStringLiteral("LIVE BLOCK");
		s.sec_setup = QStringLiteral("SMART SETUP");
		s.sec_feat = QStringLiteral("FEATURES");
		s.sec_lang = QStringLiteral("LANGUAGES");
		s.sec_upd = QStringLiteral("UPDATE");
		s.tag_before_badge = QStringLiteral("БЕЗ ПЛАГИНА");
		s.tag_after_badge = QStringLiteral("ГОЛОСОВАЯ СТУДИЯ");
		s.channel = QStringLiteral("твой_канал");
		s.stream_title = QStringLiteral("Вечерний стрим: болтаем и играем");
		s.viewers_word = QStringLiteral("ЗРИТЕЛЕЙ");
		s.follow = QStringLiteral("Отслеживать");
		s.chat_head = QStringLiteral("ЧАТ ТРАНСЛЯЦИИ");
		s.chat_before = {{"kirill_88", "что за шум??", u1},  {"nastya_k", "микро трещит", u2},
				 {"max_play", "тебя плохо слышно", u3}, {"zloy_kot", "выключи вентилятор пж", u4},
				 {"lena_s", "ушла, уши болят", u5}};
		s.chat_after = {{"dima_pro", "ОГО звук", u1},        {"nastya_k", "как на радио!", u2},
				{"max_play", "что за микро??", u3},  {"kirill_88", "студия", u4},
				{"vasya_tv", "+ подписка", u5},       {"anon228", "голос огонь", u6},
				{"lena_s", "вернулась ради звука", u5}, {"pro100", "скинь настройки!", u3},
				{"katya", "<3 <3 <3", u2},           {"denis", "наконец-то чисто", u1},
				{"olya_m", "кайф слушать", u4},      {"tim", "лучший звук на стриме", u6}};
		s.call_levels = QStringLiteral("УРОВНИ ВХОД / ВЫХОД");
		s.call_ai = QStringLiteral("ИИ СЛЫШИТ ГОЛОС");
		s.call_spectrum = QStringLiteral("СПЕКТР ВАШЕГО ГОЛОСА");
		s.call_drag = QStringLiteral("ТЯНИТЕ ТОЧКИ МЫШЬЮ");
		s.listening = QStringLiteral("СЛУШАЮ");
		s.features = {{"chip", "ИИ-ШУМОДАВ", "убирает гул, улицу и клавиатуру", "100%"},
			      {"gate", "ГЕЙТ ПО ГОЛОСУ", "тишина в паузах", "ИИ"},
			      {"sliders", "ЭКВАЛАЙЗЕР", "глубина, чёткость, воздух", "5 ПОЛОС"},
			      {"deesser", "ДЕ-ЭССЕР", "мягкие «с» и «ш»", "С/Ш"},
			      {"compress", "КОМПРЕССОР", "ровная громкость", "1 РУЧКА"},
			      {"flame", "ТЕПЛО", "ламповый бархат", "TUBE"},
			      {"mask", "СМЕНА ГОЛОСА", "без эффекта бурундука", "±12"},
			      {"shield", "ЛИМИТЕР", "не хрипит на крике", "−1 дБ"}};
		s.upd_head = QStringLiteral("ОБНОВЛЕНИЕ");
		s.upd_sig = QStringLiteral("ПОДПИСЬ ECDSA P-256");
		s.upd_sha = QStringLiteral("ЦЕЛОСТНОСТЬ SHA-256");
		s.upd_ready = QStringLiteral("ГОТОВО — ВКЛЮЧИТСЯ ПОСЛЕ ПЕРЕЗАПУСКА OBS");
		s.fin_cta = QStringLiteral("СКАЧАТЬ БЕСПЛАТНО");
	} else {
		s.title = QStringLiteral("VOICE STUDIO");
		s.subtitle = QStringLiteral("a plugin for OBS Studio");
		s.w_intro = QStringLiteral("SOUND");
		s.w_before = QStringLiteral("NOISE?");
		s.w_groove = QStringLiteral("SEE IT LIVE");
		s.w_rise = QStringLiteral("10 SECONDS");
		s.w_done = QStringLiteral("DONE");
		s.w_after = QStringLiteral("CLEAN");
		s.w_feat = QStringLiteral("ALL IN ONE");
		s.w_lang = QStringLiteral("12 LANGUAGES");
		s.w_upd = QStringLiteral("SELF-UPDATING");
		s.w_fin = QStringLiteral("SOUND CLEAN");
		s.p_obs = QStringLiteral("OBS");
		s.p_chat = QStringLiteral("CHAT");
		s.p_filter = QStringLiteral("FILTER");
		s.p_setup = QStringLiteral("SETUP");
		s.p_live = QStringLiteral("LIVE");
		s.p_lang = QStringLiteral("LANG");
		s.p_github = QStringLiteral("GITHUB");
		s.tag_intro = QStringLiteral("studio sound from any microphone");
		s.tag_before = QStringLiteral("noise, hum, crackle — viewers leave");
		s.tag_groove = QStringLiteral("levels and spectrum in the filter window");
		s.tag_rise = QStringLiteral("listening to your voice");
		s.tag_after = QStringLiteral("+2,826 viewers");
		s.tag_feat = QStringLiteral("8 tools in one filter");
		s.tag_lang = QStringLiteral("follows your OBS language");
		s.tag_upd = QStringLiteral("every version is digitally signed");
		s.tag_fin = QStringLiteral("agrysif/obs-voice-studio");
		s.sec_intro = QStringLiteral("PLUGIN");
		s.sec_stream = QStringLiteral("STREAM");
		s.sec_live = QStringLiteral("LIVE BLOCK");
		s.sec_setup = QStringLiteral("SMART SETUP");
		s.sec_feat = QStringLiteral("FEATURES");
		s.sec_lang = QStringLiteral("LANGUAGES");
		s.sec_upd = QStringLiteral("UPDATE");
		s.tag_before_badge = QStringLiteral("NO PLUGIN");
		s.tag_after_badge = QStringLiteral("VOICE STUDIO");
		s.channel = QStringLiteral("your_channel");
		s.stream_title = QStringLiteral("Late night stream: chatting & gaming");
		s.viewers_word = QStringLiteral("VIEWERS");
		s.follow = QStringLiteral("Follow");
		s.chat_head = QStringLiteral("STREAM CHAT");
		s.chat_before = {{"kevin_88", "what's that noise??", u1}, {"nastya_k", "mic is crackling", u2},
				 {"max_play", "can barely hear you", u3}, {"grumpy_cat", "turn off the fan pls", u4},
				 {"lena_s", "leaving, my ears hurt", u5}};
		s.chat_after = {{"dima_pro", "WOW the audio", u1},       {"nastya_k", "sounds like radio!", u2},
				{"max_play", "what mic is that??", u3},  {"kevin_88", "studio quality", u4},
				{"sam_tv", "just followed", u5},         {"anon228", "voice is fire", u6},
				{"lena_s", "came back for the sound", u5}, {"pro100", "share your settings!", u3},
				{"katya", "<3 <3 <3", u2},              {"denis", "finally crystal clear", u1},
				{"olya_m", "so nice to listen", u4},     {"tim", "best audio on stream", u6}};
		s.call_levels = QStringLiteral("INPUT / OUTPUT LEVELS");
		s.call_ai = QStringLiteral("AI HEARS YOUR VOICE");
		s.call_spectrum = QStringLiteral("YOUR VOICE SPECTRUM");
		s.call_drag = QStringLiteral("DRAG THE POINTS");
		s.listening = QStringLiteral("LISTENING");
		s.features = {{"chip", "AI DENOISE", "removes hum, street and keys", "100%"},
			      {"gate", "VOICE GATE", "silent pauses", "AI"},
			      {"sliders", "EQUALIZER", "depth, clarity, air", "5 BANDS"},
			      {"deesser", "DE-ESSER", "smooth S and SH", "S/SH"},
			      {"compress", "COMPRESSOR", "even loudness", "1 KNOB"},
			      {"flame", "WARMTH", "tube-style velvet", "TUBE"},
			      {"mask", "VOICE CHANGER", "no chipmunk effect", "±12"},
			      {"shield", "LIMITER", "no clipping on shouts", "−1 dB"}};
		s.upd_head = QStringLiteral("UPDATE");
		s.upd_sig = QStringLiteral("SIGNATURE ECDSA P-256");
		s.upd_sha = QStringLiteral("INTEGRITY SHA-256");
		s.upd_ready = QStringLiteral("READY — ACTIVE AFTER OBS RESTART");
		s.fin_cta = QStringLiteral("DOWNLOAD FREE");
	}
	return s;
}

struct Lang {
	const char *code, *name;
};
const Lang kLangs[] = {{"en-US", "ENGLISH"},  {"be-BY", "БЕЛАРУСКАЯ"},   {"uk-UA", "УКРАЇНСЬКА"},
		       {"kk-KZ", "ҚАЗАҚША"},  {"ky-KG", "КЫРГЫЗЧА"},     {"uz-UZ", "OʻZBEKCHA"},
		       {"tg-TJ", "ТОҶИКӢ"},   {"az-AZ", "AZƏRBAYCANCA"}, {"hy-AM", "ՀԱՅԵՐԵՆ"},
		       {"tt-RU", "ТАТАРЧА"},  {"ba-RU", "БАШҠОРТСА"},    {"ru-RU", "РУССКИЙ"}};

// ---------------------------------------------------------------------------
// Трансляция в духе Twitch: тёмная тема, фиолетовые кнопки, красный счётчик зрителей,
// чат справа. Узнаваемо, но без чужих логотипов и названий.

struct ChatLine {
	Msg msg;
	double at;
};

namespace tw {
const QColor bg(0x0E, 0x0E, 0x10), panel(0x18, 0x18, 0x1B), panel2(0x1F, 0x1F, 0x23), line(0x2F, 0x2F, 0x35);
const QColor purple(0x91, 0x46, 0xFF), live(0xEB, 0x04, 0x00), text(0xEF, 0xEF, 0xF1), sub(0xAD, 0xAD, 0xB8);
const QColor names[] = {QColor(0xFF, 0x7F, 0x50), QColor(0x1E, 0x90, 0xFF), QColor(0x9A, 0xCD, 0x32), QColor(0xFF, 0x69, 0xB4),
			QColor(0x00, 0xC8, 0x7A), QColor(0xDA, 0xA5, 0x20), QColor(0xB0, 0x7C, 0xFF), QColor(0x5F, 0x9E, 0xA0)};

QFont ui(double px, int weight = QFont::Normal)
{
	QFont f(QStringLiteral("Segoe UI"));
	f.setPixelSize((int)std::lround(px));
	f.setWeight(QFont::Weight(weight));
	return f;
}
} // namespace tw

class StreamView {
public:
	static constexpr int TW = 1600, TH = 900;
	static constexpr double PX = 50, PY = 50, PW = 1210, PH = 681; // плеер

	explicit StreamView(const Script &s, bool ru) : s_(s), ru_(ru)
	{
		// «Игра»: плоский пейзаж в синих тонах
		game_ = QImage((int)PW, (int)PH, QImage::Format_ARGB32_Premultiplied);
		QPainter p(&game_);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(game_.rect(), QColor(0x14, 0x22, 0x44));
		for (int i = 0; i < 6; i++)
			p.fillRect(QRectF(0, i * 52, PW, 52), QColor(0x14 + i * 4, 0x22 + i * 5, 0x44 + i * 8));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0xE8, 0xEE, 0xFF));
		p.drawEllipse(QPointF(860, 210), 70, 70);
		for (int layer = 0; layer < 3; layer++) {
			QPainterPath m;
			m.moveTo(0, PH);
			for (int x = 0; x <= (int)PW; x += 70)
				m.lineTo(x, 300 + layer * 60 + 90 * hash01(x * 3 + layer * 101) * (1.0 - layer * 0.25));
			m.lineTo(PW, PH);
			m.closeSubpath();
			p.setBrush(QColor(0x1E + layer * 4, 0x33 + layer * 6, 0x66 - layer * 10));
			p.drawPath(m);
		}
		p.setBrush(QColor(0x0F, 0x18, 0x30));
		p.drawRect(QRectF(0, 500, PW, PH - 500));
		p.setPen(QPen(QColor(0x2F, 0x6F, 0xF0, 160), 2));
		for (int i = -14; i <= 14; i++)
			p.drawLine(QPointF(PW / 2 + i * 30, 500), QPointF(PW / 2 + i * 190, PH));
		for (int i = 1; i < 7; i++) {
			const double y = 500 + (PH - 500) * std::pow(i / 7.0, 1.8);
			p.drawLine(QPointF(0, y), QPointF(PW, y));
		}
		p.end();

		cam_ = QImage(200, 200, QImage::Format_ARGB32_Premultiplied);
		QPainter c(&cam_);
		c.setRenderHint(QPainter::Antialiasing);
		c.fillRect(cam_.rect(), QColor(0x22, 0x24, 0x2C));
		c.setPen(Qt::NoPen);
		c.setBrush(QColor(0xE6, 0xE8, 0xEE));
		c.drawEllipse(QPointF(100, 84), 37, 42);
		c.setBrush(col::accent);
		c.drawRoundedRect(QRectF(33, 136, 134, 100), 50, 50);
		c.setPen(QPen(QColor(0x15, 0x15, 0x18), 9, Qt::SolidLine, Qt::FlatCap));
		c.setBrush(Qt::NoBrush);
		c.drawArc(QRectF(54, 36, 92, 96), 20 * 16, 140 * 16);
		c.setPen(Qt::NoPen);
		c.setBrush(QColor(0x15, 0x15, 0x18));
		c.drawRoundedRect(QRectF(48, 80, 14, 30), 4, 4);
		c.drawRoundedRect(QRectF(138, 80, 14, 30), 4, 4);
		c.setPen(QPen(QColor(0x15, 0x15, 0x18), 5, Qt::SolidLine, Qt::RoundCap));
		c.drawLine(QPointF(57, 108), QPointF(84, 126));
		c.end();
	}

	// Речь стримера: огибающая громкости.
	static double speech(double t)
	{
		const double phrase = std::sin(2 * M_PI * 0.31 * t) > -0.5 ? 1.0 : 0.15;
		const double syl = std::pow(std::max(0.0, std::sin(2 * M_PI * 3.3 * t)), 0.5);
		return phrase * (0.25 + 0.75 * syl);
	}

	QImage render(double t, bool clean, double viewers, const std::vector<ChatLine> &chat, double follow_pop)
	{
		QImage img(TW, TH, QImage::Format_ARGB32_Premultiplied);
		img.fill(tw::bg);
		QPainter p(&img);
		p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
		top_bar(p);
		side_bar(p);
		player(p, t, clean);
		info(p, t, clean, viewers, follow_pop);
		chat_panel(p, t, chat);
		p.end();
		return img;
	}

	// Где на текстуре счётчик зрителей и кнопка «Отслеживать» (для частиц).
	static QPointF viewers_pos() { return QPointF(1150, 852); }
	static QPointF follow_pos() { return QPointF(1170, 778); }

private:
	void top_bar(QPainter &p)
	{
		p.fillRect(QRectF(0, 0, TW, 50), tw::panel);
		p.fillRect(QRectF(0, 49, TW, 1), QColor(0, 0, 0));
		// Нейтральный значок площадки (не логотип)
		p.setPen(Qt::NoPen);
		p.setBrush(tw::purple);
		p.drawRoundedRect(QRectF(14, 11, 28, 28), 6, 6);
		draw_icon(p, "play", QPointF(28, 25), 16, Qt::white, 2.4);
		p.setFont(tw::ui(17, QFont::DemiBold));
		p.setPen(tw::text);
		p.drawText(QPointF(64, 31), ru_ ? QStringLiteral("Отслеживаемое") : QStringLiteral("Following"));
		p.drawText(QPointF(ru_ ? 204 : 166, 31), ru_ ? QStringLiteral("Просмотр") : QStringLiteral("Browse"));
		const QRectF search(560, 9, 480, 32);
		p.setBrush(tw::panel2);
		p.setPen(QPen(QColor(0x3A, 0x3A, 0x3D), 1.5));
		p.drawRoundedRect(search, 6, 6);
		p.fillRect(QRectF(search.right() - 36, search.top() + 1, 35, search.height() - 2), QColor(0x2F, 0x2F, 0x35));
		draw_icon(p, "search", QPointF(search.right() - 18, search.center().y()), 18, tw::sub, 2.2);
		p.setFont(tw::ui(15));
		p.setPen(tw::sub);
		p.drawText(QPointF(search.left() + 12, search.center().y() + 5), ru_ ? QStringLiteral("Поиск") : QStringLiteral("Search"));
		p.setPen(Qt::NoPen);
		p.setBrush(col::accent);
		p.drawEllipse(QPointF(TW - 30, 25), 15, 15);
		draw_icon(p, "bell", QPointF(TW - 76, 25), 22, tw::text, 2.0);
	}

	void side_bar(QPainter &p)
	{
		p.fillRect(QRectF(0, 50, PX, TH - 50), tw::panel);
		const QColor av[] = {QColor(0x4B, 0x7B, 0xEC), QColor(0xE0, 0x6C, 0x75), QColor(0x56, 0xB6, 0xC2), QColor(0xC6, 0x78, 0xDD),
				     QColor(0x98, 0xC3, 0x79), QColor(0xE5, 0xC0, 0x7B), QColor(0x61, 0xAF, 0xEF)};
		draw_icon(p, "chevrons", QPointF(PX / 2, 72), 20, tw::sub, 2.2);
		for (int i = 0; i < 7; i++) {
			p.setPen(Qt::NoPen);
			p.setBrush(av[i]);
			p.drawEllipse(QPointF(PX / 2, 110 + i * 46), 15, 15);
		}
	}

	void player(QPainter &p, double t, bool clean)
	{
		const QRectF pr(PX, PY, PW, PH);
		p.save();
		p.setClipRect(pr);
		p.drawImage(QPointF(PX - 8 + 8 * std::sin(t * 0.6), PY), game_);
		// Помехи без плагина: «битые» полосы и шум
		if (!clean) {
			for (int i = 0; i < 5; i++) {
				const int seed = (int)(t * 14) * 31 + i;
				if (hash01(seed) > 0.45) {
					const double y = hash01(seed + 5) * (PH - 40), h = 4 + hash01(seed + 9) * 22;
					p.drawImage(QRectF(PX + 18 * (hash01(seed + 3) - 0.5), PY + y, PW, h), game_, QRectF(0, y, PW, h));
					p.fillRect(QRectF(PX, PY + y, PW, 2), QColor(255, 255, 255, 70));
				}
			}
			for (int i = 0; i < 900; i++) {
				const int seed = i * 97 + (int)(t * 30) * 7919;
				p.setPen(QColor(255, 255, 255, (int)(90 * hash01(seed))));
				p.drawPoint(QPointF(PX + hash01(seed + 1) * PW, PY + hash01(seed + 2) * PH));
			}
		}
		// Наложения стримера из OBS: камера, звук, плашка
		const QRectF cam(PX + 22, PY + PH - 22 - 200, 200, 200);
		p.fillRect(cam.adjusted(-4, -4, 4, 4), clean ? col::accent : QColor(0x55, 0x55, 0x60));
		p.drawImage(cam.topLeft(), cam_);
		const double env = speech(t);
		const QRectF wave(PX + 250, PY + PH - 86, 600, 64);
		p.fillRect(wave, QColor(0x0D, 0x0D, 0x0F, 225));
		p.fillRect(QRectF(wave.left(), wave.top(), 5, wave.height()), clean ? col::accent : QColor(0x77, 0x77, 0x82));
		draw_icon(p, clean ? "mic" : "noise", QPointF(wave.left() + 36, wave.center().y()), 30, clean ? col::accent_lt : tw::sub, 2.2);
		QPainterPath wpath;
		const double x0 = wave.left() + 66, x1 = wave.right() - 16;
		for (int i = 0; i <= 130; i++) {
			const double x = x0 + i * (x1 - x0) / 130;
			const double e = speech(t - (130 - i) * 0.012);
			double y = std::sin(i * 0.9 + t * 30) * e * 22;
			if (!clean)
				y = y * 0.7 + (hash01(i * 31 + (int)(t * 30) * 17) - 0.5) * 30 + (hash01(i + (int)(t * 30)) > 0.92 ? 24 : 0);
			const QPointF pt(x, wave.center().y() + y);
			if (i == 0)
				wpath.moveTo(pt);
			else
				wpath.lineTo(pt);
		}
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(clean ? Qt::white : tw::sub, 3));
		p.drawPath(wpath);
		(void)env;
		{
			const QString tag = clean ? s_.tag_after_badge : s_.tag_before_badge;
			const QFont f = mono_font(20);
			const double w = QFontMetricsF(f).horizontalAdvance(tag) + 20 * 1.15 + 20 * 0.45 + 20 * 1.3;
			chip(p, QPointF(PX + PW - 20 - w / 2, PY + 34), tag, 20, clean ? col::accent : QColor(0x0D, 0x0D, 0x0F, 220),
			     clean ? Qt::white : tw::sub, 1, 1, clean ? "mic" : "x", clean ? QColor() : QColor(0x55, 0x55, 0x60));
		}
		p.restore();
	}

	void info(QPainter &p, double t, bool clean, double viewers, double follow_pop)
	{
		const double y0 = PY + PH;
		p.fillRect(QRectF(PX, y0, PW, TH - y0), tw::bg);
		// Аватар с красным кольцом «в эфире»
		const QPointF av(PX + 48, y0 + 62);
		p.setPen(QPen(tw::live, 4));
		p.setBrush(col::accent);
		p.drawEllipse(av, 34, 34);
		draw_icon(p, "mic", av, 32, Qt::white, 2.4);
		const QString live = ru_ ? QStringLiteral("В ЭФИРЕ") : QStringLiteral("LIVE");
		p.setFont(tw::ui(13, QFont::Bold));
		const double lw = QFontMetricsF(p.font()).horizontalAdvance(live) + 12;
		p.setPen(Qt::NoPen);
		p.setBrush(tw::live);
		p.drawRoundedRect(QRectF(av.x() - lw / 2, av.y() + 26, lw, 20), 4, 4);
		p.setPen(Qt::white);
		p.drawText(QRectF(av.x() - lw / 2, av.y() + 26, lw, 20), Qt::AlignCenter, live);
		// Название, заголовок, категория, теги
		const double x = PX + 104;
		p.setFont(tw::ui(22, QFont::Bold));
		p.setPen(tw::text);
		p.drawText(QPointF(x, y0 + 40), s_.channel);
		p.setFont(tw::ui(17, QFont::DemiBold));
		p.drawText(QPointF(x, y0 + 68), s_.stream_title);
		p.setFont(tw::ui(16));
		p.setPen(QColor(0xBF, 0x94, 0xFF));
		p.drawText(QPointF(x, y0 + 94), ru_ ? QStringLiteral("Общение") : QStringLiteral("Just Chatting"));
		double tx = x;
		for (const QString &tag : ru_ ? QStringList{"Русский", "Подкаст", "ГолосоваяСтудия"} : QStringList{"English", "Podcast", "VoiceStudio"}) {
			p.setFont(tw::ui(13, QFont::DemiBold));
			const double w = QFontMetricsF(p.font()).horizontalAdvance(tag) + 18;
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0x2F, 0x2F, 0x35));
			p.drawRoundedRect(QRectF(tx, y0 + 106, w, 24), 12, 12);
			p.setPen(QColor(0xDE, 0xDE, 0xE3));
			p.drawText(QRectF(tx, y0 + 106, w, 24), Qt::AlignCenter, tag);
			tx += w + 8;
		}
		// Кнопки «Отслеживать» и «Подписаться»
		const double k = 1 + 0.08 * follow_pop;
		const QRectF fb(PX + PW - 20 - 180 * k, y0 + 14, 180 * k, 40);
		p.setPen(Qt::NoPen);
		p.setBrush(tw::purple);
		p.drawRoundedRect(fb, 6, 6);
		draw_icon(p, "heart", QPointF(fb.left() + 26, fb.center().y()), 20, Qt::white, 2.4);
		p.setFont(tw::ui(17, QFont::DemiBold));
		p.setPen(Qt::white);
		p.drawText(fb.adjusted(40, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, s_.follow);
		const QRectF sb(fb.left() - 10 - 190, y0 + 14, 190, 40);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x2F, 0x2F, 0x35));
		p.drawRoundedRect(sb, 6, 6);
		draw_icon(p, "star", QPointF(sb.left() + 24, sb.center().y()), 20, tw::text, 2.2);
		p.setPen(tw::text);
		p.drawText(sb.adjusted(40, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, ru_ ? QStringLiteral("Подписаться") : QStringLiteral("Subscribe"));
		// Зрители — красным, как у площадки; время эфира
		p.setFont(tw::ui(30, QFont::Bold));
		const QString vt = group_digits((long long)viewers);
		const double vw = QFontMetricsF(p.font()).horizontalAdvance(vt);
		p.setPen(clean ? QColor(0xFF, 0x6B, 0x6B) : QColor(0xFF, 0x8A, 0x8A));
		p.drawText(QPointF(PX + PW - 20 - vw, y0 + 112), vt);
		draw_icon(p, "user", QPointF(PX + PW - 20 - vw - 22, y0 + 102), 26, QColor(0xFF, 0x6B, 0x6B), 2.4);
		p.setFont(tw::ui(16, QFont::DemiBold));
		p.setPen(tw::sub);
		const int up = 4980 + (int)t;
		const QString uptime = QStringLiteral("%1:%2:%3").arg(up / 3600).arg((up / 60) % 60, 2, 10, QChar('0')).arg(up % 60, 2, 10, QChar('0'));
		p.drawText(QPointF(PX + PW - 20 - vw - 54 - QFontMetricsF(p.font()).horizontalAdvance(uptime), y0 + 108), uptime);
	}

	// Перенос сообщения: первая строка — после имени, остальные — с начала.
	static QStringList wrap(const QString &text, const QFontMetricsF &fm, double first, double full)
	{
		QStringList lines;
		QString cur;
		double width = first;
		for (const QString &w : text.split(' ')) {
			const QString cand = cur.isEmpty() ? w : cur + ' ' + w;
			if (fm.horizontalAdvance(cand) > width && !cur.isEmpty()) {
				lines << cur;
				cur = w;
				width = full;
			} else {
				cur = cand;
			}
		}
		if (!cur.isEmpty())
			lines << cur;
		return lines;
	}

	void chat_panel(QPainter &p, double t, const std::vector<ChatLine> &chat)
	{
		const double cx = PX + PW, cw = TW - cx;
		p.fillRect(QRectF(cx, PY, cw, TH - PY), tw::panel);
		p.fillRect(QRectF(cx, PY, 1, TH - PY), QColor(0, 0, 0));
		p.setFont(tw::ui(15, QFont::DemiBold));
		p.setPen(tw::text);
		p.drawText(QRectF(cx, PY, cw, 50), Qt::AlignCenter, s_.chat_head);
		draw_icon(p, "chevrons", QPointF(cx + 22, PY + 25), 18, tw::sub, 2.2);
		draw_icon(p, "users", QPointF(TW - 22, PY + 25), 20, tw::sub, 2.2);
		p.fillRect(QRectF(cx, PY + 50, cw, 1), tw::line);

		const QFont nf = tw::ui(17, QFont::Bold), mf = tw::ui(17);
		const QFontMetricsF nfm(nf), mfm(mf);
		double y = TH - 96;
		for (auto it = chat.rbegin(); it != chat.rend() && y > PY + 70; ++it) {
			const double age = t - it->at;
			if (age < 0)
				continue;
			const double k = ease_out(age / 0.2);
			const int ci = (int)(hash01(qHash(it->msg.user) & 0xFFFF) * 8) % 8;
			const QString name = it->msg.user;
			const double badge = (qHash(name) % 3 == 0) ? 22 : 0;
			const double name_w = badge + nfm.horizontalAdvance(name + ": ");
			const QStringList lines = wrap(it->msg.text, mfm, cw - 30 - name_w, cw - 30);
			const double h = lines.size() * 24 + 6;
			y -= h;
			p.save();
			p.setOpacity(k);
			p.translate((1 - k) * 30, 0);
			const double x = cx + 14;
			if (badge > 0) {
				p.setPen(Qt::NoPen);
				p.setBrush(tw::purple);
				p.drawRoundedRect(QRectF(x, y + 4, 18, 18), 3, 3);
				draw_icon(p, "star", QPointF(x + 9, y + 13), 12, Qt::white, 2.6);
			}
			p.setFont(nf);
			p.setPen(tw::names[ci]);
			p.drawText(QPointF(x + badge, y + 19), name);
			p.setPen(tw::text);
			p.drawText(QPointF(x + badge + nfm.horizontalAdvance(name), y + 19), QStringLiteral(":"));
			p.setFont(mf);
			for (int i = 0; i < lines.size(); i++)
				p.drawText(QPointF(i == 0 ? x + name_w : x, y + 19 + i * 24), lines[i]);
			p.restore();
			y -= 8;
		}
		// Поле ввода и кнопка «Чат»
		const QRectF input(cx + 12, TH - 80, cw - 24, 36);
		p.setPen(Qt::NoPen);
		p.setBrush(tw::panel2);
		p.drawRoundedRect(input, 6, 6);
		p.setFont(tw::ui(15));
		p.setPen(tw::sub);
		p.drawText(input.adjusted(12, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
			   ru_ ? QStringLiteral("Отправить сообщение") : QStringLiteral("Send a message"));
		const QRectF send(TW - 12 - 64, TH - 38, 64, 30);
		p.setBrush(tw::purple);
		p.drawRoundedRect(send, 5, 5);
		p.setFont(tw::ui(15, QFont::DemiBold));
		p.setPen(Qt::white);
		p.drawText(send, Qt::AlignCenter, ru_ ? QStringLiteral("Чат") : QStringLiteral("Chat"));
		draw_icon(p, "gem", QPointF(cx + 26, TH - 23), 20, tw::sub, 2.2);
	}

	const Script &s_;
	bool ru_;
	QImage game_, cam_;
};

// ---------------------------------------------------------------------------
// Режиссёр: состояние всех сцен, обновление по кадрам и отрисовка

QString g_data;
double g_vt = 0; // виртуальное время ролика

std::shared_ptr<MockLink> make_link(const QString &locale)
{
	auto link = std::make_shared<MockLink>(g_data + "/locale/" + locale + ".ini");
	link->now = [] { return g_vt; };
	return link;
}

std::shared_ptr<MockLink> g_factory_link;

class Director {
public:
	Director(bool ru) : ru_(ru), s_(script_for(ru)), chrome_(chrome_for(ru)), stream_(s_, ru)
	{
		locale_ = ru ? "ru-RU" : "en-US";
		build_languages();
		link_ = make_link(locale_);
		link_->set_preset("natural");
		g_factory_link = link_;
		win_ = build_window(*link_, chrome_, true);
		open(win_.win);
		block_ = find_child<vsui::LiveBlock>(win_.win);
		eq_ = find_child<vsui::EqCurveView>(win_.win);
		build_dialog();
		build_features();
		for (int i = 0; i < 45; i++)
			step_ui(1.0 / kFps);
	}

	void frame(QImage &canvas, double t, double dt)
	{
		t_ = t;
		g_vt = t;
		update(t, dt);
		parts_.update(dt);

		QPainter p(&canvas);
		p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
		// Удар камеры на дропах, дрожь перед дропом.
		const double shake = 5 * ease_in(ramp(t, T_CLICK, T_DROP)) * (t < T_DROP ? 1 : 0) +
				     16 * std::exp(-(t - T_DROP) * 7) * (t >= T_DROP ? 1 : 0) +
				     14 * std::exp(-(t - T_DROP2) * 7) * (t >= T_DROP2 ? 1 : 0);
		if (shake > 0.2)
			p.translate((hash01((int)(t * 60)) - 0.5) * shake, (hash01((int)(t * 60) + 99) - 0.5) * shake);
		background(p, t);

		if (t < T_STREAM + 0.25)
			scene_intro(p, t);
		if (t >= T_STREAM - 0.05 && t < T_GROOVE + 0.25)
			scene_before(p, t);
		if (t >= T_GROOVE - 0.05 && t < T_AFTER + 0.25)
			scene_window(p, t);
		if (t >= T_AFTER - 0.05 && t < T_FEAT + 0.25)
			scene_after(p, t);
		if (t >= T_FEAT - 0.05 && t < T_LANG + 0.25)
			scene_features(p, t);
		if (t >= T_LANG - 0.05 && t < T_BREAK + 0.25)
			scene_languages(p, t);
		if (t >= T_BREAK - 0.05 && t < T_DROP2 + 0.25)
			scene_update(p, t);
		if (t >= T_DROP2 - 0.05)
			scene_finale(p, t);

		parts_.draw(p);
		draw_ripples(p, t);
		if (cursor_on_)
			draw_cursor(p, cursor_);

		p.resetTransform();
		for (double tw : {T_STREAM, T_GROOVE, T_AFTER, T_FEAT, T_LANG, T_BREAK, T_DROP2})
			wipe(p, t, tw, beat_index(tw) % 2 == 0);
		// Короткие цветные вспышки на ударах (вместо свечения).
		const double flash = 0.55 * std::exp(-(t - T_DROP) * 14) * (t >= T_DROP) +
				     0.5 * std::exp(-(t - T_DROP2) * 14) * (t >= T_DROP2) +
				     0.35 * std::exp(-(t - T_STAB1) * 16) * (t >= T_STAB1) +
				     0.35 * std::exp(-(t - T_STAB2) * 16) * (t >= T_STAB2);
		if (flash > 0.01)
			p.fillRect(canvas.rect(), QColor(col::accent.red(), col::accent.green(), col::accent.blue(), (int)(255 * clamp01(flash))));
		const double black = std::max(1 - ramp(t, 0, 0.35), ramp(t, T_END - 0.4, T_END - 0.05));
		if (black > 0)
			p.fillRect(canvas.rect(), QColor(0, 0, 0, (int)(255 * black)));
	}

private:
	// ----- окна OBS
	void open(QWidget *w)
	{
		w->setAttribute(Qt::WA_DontShowOnScreen);
		w->resize(kWin);
		w->show();
		QCoreApplication::processEvents();
		QCoreApplication::processEvents();
		if (auto *b = find_child<vsui::LiveBlock>(w))
			b->setManualClock(true);
	}

	void step_ui(double dt)
	{
		if (block_)
			block_->step(dt);
		win_.refresh(*link_);
		QCoreApplication::processEvents();
	}

	QImage card_image(QWidget *w, double scale = 1.0)
	{
		QImage grab = w->grab().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
		grab.setDevicePixelRatio(1);
		const double d = kDpr * scale;
		QImage card((int)(kCard.width() * d), (int)(kCard.height() * d), QImage::Format_ARGB32_Premultiplied);
		card.fill(QColor(0x1D, 0x1F, 0x26));
		QPainter p(&card);
		p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
		p.fillRect(QRectF(0, 0, card.width(), kTitleH * d), QColor(0x15, 0x17, 0x1D));
		p.setFont(body_font(14 * d));
		p.setPen(QColor(0xC9, 0xCD, 0xD6));
		p.drawText(QRectF(14 * d, 0, card.width() * 0.7, kTitleH * d), Qt::AlignVCenter, chrome_.title);
		p.setPen(QPen(QColor(0x8A, 0x8F, 0x9C), 1.3 * d));
		const double cy = kTitleH * d / 2, rx = card.width() - 22 * d;
		p.drawLine(QPointF(rx - 5 * d, cy - 5 * d), QPointF(rx + 5 * d, cy + 5 * d));
		p.drawLine(QPointF(rx - 5 * d, cy + 5 * d), QPointF(rx + 5 * d, cy - 5 * d));
		p.drawRect(QRectF(rx - 45 * d, cy - 5 * d, 10 * d, 10 * d));
		p.drawLine(QPointF(rx - 82 * d, cy), QPointF(rx - 72 * d, cy));
		p.drawImage(QRectF(0, kTitleH * d, kWin.width() * d, kWin.height() * d), grab);
		return card;
	}

	QRectF card_rect(QWidget *w) const
	{
		const QPoint tl = w->mapTo(win_.win, QPoint(0, 0));
		return QRectF(tl.x(), tl.y() + kTitleH, w->width(), w->height());
	}

	// Камера смотрит на точку карточки (логические координаты) с приближением zoom.
	static Pose focus(QPointF f, double zoom, double rx = 0, double ry = 0, QPointF screen = QPointF(kW / 2.0, kH / 2.0))
	{
		Pose p;
		p.scale = zoom;
		p.rx = rx;
		p.ry = ry;
		p.cx = screen.x() + (kCard.width() / 2 - f.x()) * kCardScale * zoom;
		p.cy = screen.y() + (kCard.height() / 2 - f.y()) * kCardScale * zoom;
		return p;
	}

	QPolygonF map_rect(const QRectF &r) const
	{
		QPolygonF q;
		for (QPointF c : {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()})
			q << card_tx_.map(c * kDpr);
		return q;
	}
	QPointF map_pt(QPointF pt) const { return card_tx_.map(pt * kDpr); }

	QTransform draw_window(QPainter &p, const Pose &pose, QColor border = col::accent)
	{
		const QImage img = card_image(win_.win);
		card_tx_ = draw_card(p, img, kCard * kCardScale, pose, border, QColor(0, 0, 0, 190), 22);
		return card_tx_;
	}

	// ----- заготовки: языки, окно обновления, карточки функций
	void build_languages()
	{
		for (const Lang &l : kLangs)
			if (locale_ != l.code)
				lang_order_.push_back(l);
		for (const Lang &l : kLangs)
			if (locale_ == l.code)
				lang_order_.push_back(l);
		for (const Lang &l : lang_order_) {
			auto link = make_link(l.code);
			g_factory_link = link;
			MockWindow mw = build_window(*link, chrome_, false);
			open(mw.win);
			auto *b = find_child<vsui::LiveBlock>(mw.win);
			for (int i = 0; i < 40; i++) {
				g_vt += 1.0 / kFps;
				if (b)
					b->step(1.0 / kFps);
			}
			QCoreApplication::processEvents();
			lang_cards_.push_back(card_image(mw.win, 0.5));
			delete mw.win;
		}
		g_vt = 0;
	}

	void build_dialog()
	{
		QMessageBox box(QMessageBox::Information, link_->tr("Update.Title"),
				link_->tr("Update.Available").arg("1.4.0", "1.3.0") + "\n\n" + link_->tr("Update.WhatsNew") + "\n" +
					(ru_ ? QStringLiteral("• Новые стили\n• Быстрее спектр") : QStringLiteral("• New styles\n• Faster spectrum")),
				QMessageBox::NoButton);
		QPushButton *install = box.addButton(link_->tr("Update.Install"), QMessageBox::AcceptRole);
		box.addButton(link_->tr("Update.Later"), QMessageBox::RejectRole);
		box.addButton(link_->tr("Update.Skip"), QMessageBox::DestructiveRole);
		box.setDefaultButton(install);
		box.setAttribute(Qt::WA_DontShowOnScreen);
		box.show();
		QCoreApplication::processEvents();
		dialog_ = box.grab().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
		dialog_.setDevicePixelRatio(1);
		dialog_size_ = QSizeF(box.size());
		install_center_ = QPointF(install->mapTo(&box, install->rect().center()));
	}

	// Карточка функции в стиле «карточки героя»: картинка с клеткой и иконкой + подпись.
	void build_features()
	{
		for (size_t i = 0; i < s_.features.size(); i++) {
			const Feature &f = s_.features[i];
			QImage img(1040, 1080, QImage::Format_ARGB32_Premultiplied);
			img.fill(col::panel);
			QPainter p(&img);
			p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
			const bool violet = i % 2;
			const QRectF art(0, 0, 1040, 760);
			checker(p, art, 65, violet ? col::accent_dk : col::accent, violet ? col::accent_lt : col::accent_lt);
			// Осколок поверх клетки
			p.setPen(Qt::NoPen);
			p.setBrush(col::bg);
			p.drawPolygon(QPolygonF({QPointF(0, 520), QPointF(1040, 330), QPointF(1040, 760), QPointF(0, 760)}));
			p.fillRect(QRectF(300, 160, 440, 440), col::bg);
			draw_icon(p, f.icon, QPointF(520, 380), 300, col::white, 1.8);
			p.setFont(mono_font(40));
			p.setPen(col::white);
			p.drawText(QPointF(40, 80), QStringLiteral("[ %1/%2 ]").arg(i + 1, 2, 10, QChar('0')).arg(s_.features.size(), 2, 10, QChar('0')));
			// Подпись
			p.fillRect(QRectF(0, 760, 1040, 320), col::panel);
			p.fillRect(QRectF(0, 760, 1040, 6), violet ? col::accent_dk : col::accent);
			p.setFont(display_font(120));
			p.setPen(col::white);
			p.drawText(QPointF(48, 900), f.title);
			p.setFont(body_font(44));
			p.setPen(col::gray);
			p.drawText(QPointF(52, 970), f.desc);
			chip(p, QPointF(110, 1032), QStringLiteral("FILTER"), 24, col::panel2, col::gray, 1, 1, QString(), col::line);
			p.setFont(mono_font(64, QFont::Bold));
			p.setPen(violet ? col::accent_lt : col::accent);
			const double vw = QFontMetricsF(mono_font(64, QFont::Bold)).horizontalAdvance(f.value);
			p.drawText(QPointF(1000 - vw, 1040), f.value);
			p.setPen(QPen(violet ? col::accent_dk : col::accent, 6));
			p.setBrush(Qt::NoBrush);
			p.drawRect(QRectF(3, 3, 1034, 1074));
			feature_cards_.push_back(img);
		}
	}

	// ----- курсор, клики, подсветка
	void move_cursor(QPointF from, QPointF to, double k)
	{
		cursor_on_ = true;
		cursor_ = from + (to - from) * smooth(k);
	}

	void click(QPointF at, QColor c = col::accent)
	{
		ripples_.push_back({at, t_, c});
		parts_.burst(at, 14, 160, 520, {c, col::white}, 0, 0.45, 9, 0);
	}

	void draw_cursor(QPainter &p, QPointF o)
	{
		QPainterPath arrow;
		const double s = 1.9;
		const QPointF pts[] = {{0, 0}, {0, 17}, {4.2, 13}, {7, 19.5}, {9.6, 18.4}, {6.9, 12.2}, {12.4, 12.2}};
		arrow.moveTo(o + pts[0] * s);
		for (int i = 1; i < 7; i++)
			arrow.lineTo(o + pts[i] * s);
		arrow.closeSubpath();
		p.setPen(QPen(Qt::black, 2));
		p.setBrush(Qt::white);
		p.drawPath(arrow);
	}

	struct Ripple {
		QPointF at;
		double t0;
		QColor c;
	};

	// Клик: квадратная «рамка», которая расходится и тает.
	void draw_ripples(QPainter &p, double t)
	{
		for (const Ripple &r : ripples_) {
			const double k = (t - r.t0) / 0.45;
			if (k < 0 || k > 1)
				continue;
			QColor c = r.c;
			c.setAlphaF(1 - k);
			const double d = 12 + 60 * ease_out(k);
			p.setPen(QPen(c, 4 * (1 - k) + 1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
			p.setBrush(Qt::NoBrush);
			p.save();
			p.translate(r.at);
			p.rotate(45 * k);
			p.drawRect(QRectF(-d, -d, 2 * d, 2 * d));
			p.restore();
		}
	}

	// Затемнить всё, кроме области, и поставить угловые скобки.
	void spotlight(QPainter &p, const QPolygonF &q, double k, QColor c = col::accent)
	{
		if (k <= 0)
			return;
		QPainterPath outside;
		outside.addRect(-50, -50, kW + 100, kH + 100);
		QPainterPath in;
		in.addPolygon(q);
		in.closeSubpath();
		p.fillPath(outside.subtracted(in), QColor(0, 0, 0, (int)(150 * k)));
		brackets(p, q, k, c);
	}

	void brackets(QPainter &p, const QPolygonF &q, double k, QColor c)
	{
		const QPointF ctr = q.boundingRect().center();
		const double grow = (1 - ease_out(k)) * 0.25;
		p.save();
		p.setPen(QPen(c, 5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
		p.setBrush(Qt::NoBrush);
		p.setOpacity(p.opacity() * clamp01(k * 2));
		for (int i = 0; i < 4; i++) {
			const QPointF a = q[i] + (q[i] - ctr) * grow;
			const QPointF nx = q[(i + 1) % 4] + (q[(i + 1) % 4] - ctr) * grow, pv = q[(i + 3) % 4] + (q[(i + 3) % 4] - ctr) * grow;
			auto toward = [&](QPointF b) {
				const QPointF d = b - a;
				const double len = std::hypot(d.x(), d.y());
				return a + d * std::min(0.3, 26.0 / std::max(1.0, len));
			};
			QPainterPath l;
			l.moveTo(toward(pv));
			l.lineTo(a);
			l.lineTo(toward(nx));
			p.drawPath(l);
		}
		p.restore();
	}

	// Подпись к выделенной области: линия с изломом и моноширинная плашка.
	void callout(QPainter &p, const QPolygonF &q, const QString &text, double k, bool above = true)
	{
		if (k <= 0)
			return;
		const QRectF b = q.boundingRect();
		const QPointF a = above ? QPointF(b.right() - 60, b.top() - 4) : QPointF(b.right() - 60, b.bottom() + 4);
		const QPointF mid = a + QPointF(0, above ? -46 : 46);
		const QPointF end = mid + QPointF(60, 0);
		p.save();
		p.setPen(QPen(col::white, 3, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
		const double e = ease_out(k);
		QPainterPath path;
		path.moveTo(a);
		path.lineTo(a + (mid - a) * std::min(1.0, e * 2));
		if (e > 0.5)
			path.lineTo(mid + (end - mid) * (e - 0.5) * 2);
		p.drawPath(path);
		p.fillRect(QRectF(a - QPointF(5, 5), QSizeF(10, 10)), col::white);
		p.restore();
		if (e > 0.5) {
			const QFont f = mono_font(28);
			const double w = QFontMetricsF(f).horizontalAdvance(text) + 28 * 1.3;
			chip(p, end + QPointF(w / 2, 0), text, 28, col::accent, col::white, 1, clamp01((e - 0.5) * 3));
		}
	}

	// ----- фон: спокойная клетка одного цвета, без движения
	void background(QPainter &p, double)
	{
		p.fillRect(QRectF(-50, -50, kW + 100, kH + 100), col::bg);
		const double cell = 60;
		for (int i = 0; i * cell <= kW; i++) {
			p.setPen(QPen(QColor(255, 255, 255, i % 4 == 0 ? 20 : 9), 1));
			p.drawLine(QPointF(i * cell, -50), QPointF(i * cell, kH + 50));
		}
		for (int j = 0; j * cell <= kH; j++) {
			p.setPen(QPen(QColor(255, 255, 255, j % 4 == 0 ? 20 : 9), 1));
			p.drawLine(QPointF(-50, j * cell), QPointF(kW + 50, j * cell));
		}
		// Синие крестики на пересечениях крупной клетки
		p.setPen(QPen(QColor(col::accent.red(), col::accent.green(), col::accent.blue(), 110), 2));
		for (int i = 0; i * cell <= kW; i += 4)
			for (int j = 0; j * cell <= kH; j += 4) {
				const QPointF c(i * cell, j * cell);
				p.drawLine(c - QPointF(7, 0), c + QPointF(7, 0));
				p.drawLine(c - QPointF(0, 7), c + QPointF(0, 7));
			}
	}

	// Синяя наклонная плашка поперёк экрана — графический приём вместо лучей.
	void band(QPainter &p, double cy, double h, double local, double out, QColor c, bool from_left = true)
	{
		if (local <= 0 || out >= 1)
			return;
		const double k = ease_out(local / 0.35), ko = ease_in(out);
		const double w = kW + 400;
		double x = from_left ? lerp(-w, -200, k) : lerp(kW + 200, -200, k);
		x += ko * (from_left ? kW + 400 : -(kW + 400));
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawPolygon(QPolygonF({QPointF(x + 120, cy - h / 2), QPointF(x + w + 120, cy - h / 2), QPointF(x + w, cy + h / 2),
					 QPointF(x, cy + h / 2)}));
	}
	// Шторка из наклонных полос: закрывает экран на стыке сцен.
	void wipe(QPainter &p, double t, double t0, bool flip)
	{
		const double d = 0.42;
		if (t < t0 - d / 2 - 0.1 || t > t0 + d / 2 + 0.15)
			return;
		const QColor cols[] = {col::accent, col::accent_dk, col::bg};
		for (int i = 0; i < 3; i++) {
			const double u = (t - (t0 - d / 2) - i * 0.04) / d;
			if (u <= 0 || u >= 1)
				continue;
			const double x = lerp(-2.0 * kW, 1.4 * kW, ease_in_out(u));
			QPolygonF band({QPointF(x, -20), QPointF(x + kW * 1.1, -20), QPointF(x + kW * 1.1 - 500, kH + 20),
					QPointF(x - 500, kH + 20)});
			if (flip)
				for (QPointF &pt : band)
					pt.setX(kW - pt.x());
			p.setPen(Qt::NoPen);
			p.setBrush(cols[i]);
			p.drawPolygon(band);
		}
	}

	// ----- сцены
	void draw_logo(QPainter &p, QPointF c, double s)
	{
		const QRectF box(c.x() - 90 * s, c.y() - 90 * s, 180 * s, 180 * s);
		p.fillRect(box.translated(14 * s, 14 * s), col::accent_dk);
		checker(p, box, 30 * s, col::accent, col::accent_lt);
		p.fillRect(box.adjusted(30 * s, 30 * s, -30 * s, -30 * s), col::bg);
		draw_icon(p, "mic", c, 96 * s, col::white, 2.0);
	}

	void scene_intro(QPainter &p, double t)
	{
		const double out = ramp(t, T_STREAM - 0.2, T_STREAM + 0.05);
		hud(p, s_.sec_intro, 1, 8, t - 0.3, out);
		band(p, 470, 250, t - bt(2) + 0.08, out, col::accent_dk);
		const double a = ease_out_back(ramp(t, 0.35, 0.95));
		if (a > 0) {
			p.save();
			p.translate(560, 470);
			p.scale(a, a);
			p.rotate((1 - a) * -20);
			draw_logo(p, QPointF(0, 0), 1.5);
			p.restore();
		}
		big_text(p, s_.w_intro, QPointF(760, 560), 300, t - bt(2), out, col::white);
		big_text(p, s_.title, QPointF(780, 660), 92, t - bt(4), out, col::white, Qt::AlignLeft, col::accent);
		tag_line(p, QPointF(780, 700), s_.p_obs, s_.tag_intro, t - bt(5), out);
		// На каждой доле — квадратики разлетаются от логотипа
		const int b = beat_index(t);
		if (b != last_intro_beat_ && t > 0.4 && t < T_STREAM - 0.3) {
			last_intro_beat_ = b;
			parts_.burst(QPointF(560, 470), 10, 300, 700, {col::accent, col::white, col::accent_dk}, 4, 0.7, 14, 300);
		}
	}

	double viewers_at(double t) const
	{
		if (t < T_AFTER)
			return std::max(19.0, 34 - (t - T_STREAM) * 3.4);
		return 21 + (2847 - 21) * ease_out_expo((t - T_AFTER - 0.25) / 2.6);
	}

	// Пролёт камеры над трансляцией: влёт сбоку, проезд к чату, уход в сторону.
	Pose fly(double t, double t0, double t1, double side)
	{
		const double u = (t - t0) / (t1 - t0);
		Pose in{kW / 2.0 + side * 1300, kH / 2.0 + 260, 1400, -14, side * -55, side * -6, 0.96, 1};
		Pose a{kW / 2.0 + side * 110, kH / 2.0 + 10, 120, -9, side * -16, 0, 0.96, 1};
		Pose b{kW / 2.0 - side * 110, kH / 2.0 - 10, -160, -5, side * 10, 0, 0.96, 1};
		Pose out{kW / 2.0 - side * 1500, kH / 2.0 - 80, 600, -10, side * 55, side * 5, 0.96, 1};
		if (u < 0.16)
			return mix(in, a, ease_out(u / 0.16));
		if (u < 0.9)
			return mix(a, b, ease_in_out((u - 0.16) / 0.74));
		return mix(b, out, ease_in((u - 0.9) / 0.1));
	}

	// После дропа: влёт, общий план, наезд на растущий счётчик зрителей.
	Pose after_camera(double t) const
	{
		const double t0 = T_AFTER - 0.1;
		Pose in{kW / 2.0 - 1300, kH / 2.0 + 260, 1400, -14, 55, 6, 0.96, 1};
		Pose wide{kW / 2.0 - 50, kH / 2.0 + 10, 100, -8, 12, 0, 0.96, 1};
		Pose close{560, 70, -80, -5, 7, 0, 1.4, 1}; // счётчик и чат крупно
		if (t < t0 + 0.45)
			return mix(in, wide, ease_out((t - t0) / 0.45));
		Pose drift = wide;
		drift.cx -= (t - t0 - 0.45) * 30;
		if (t < t0 + 1.1)
			return drift;
		Pose hold = close;
		hold.cx -= (t - t0 - 1.75) * 25;
		return mix(drift, hold, ease_in_out((t - t0 - 1.1) / 0.65));
	}

	void scene_before(QPainter &p, double t)
	{
		const double out = ramp(t, T_GROOVE - 0.15, T_GROOVE);
		while (chat_before_n_ < s_.chat_before.size() && t >= bt(9 + chat_before_n_ * 1.5)) {
			chat_.push_back({s_.chat_before[chat_before_n_], bt(9 + chat_before_n_ * 1.5)});
			chat_before_n_++;
		}
		Pose pose = fly(t, T_STREAM - 0.1, T_GROOVE + 0.2, 1);
		// Плохой звук: картинку иногда дёргает
		const int glitch = (int)(t * 15);
		if (hash01(glitch * 3) > 0.8)
			pose.cx += (hash01(glitch * 3 + 1) - 0.5) * 24;
		const QImage img = stream_.render(t, false, viewers_at(t), chat_, 0);
		const QTransform tx = draw_card(p, img, QSizeF(StreamView::TW, StreamView::TH), pose, QColor(0x55, 0x5B, 0x6A),
						QColor(0, 0, 0, 200), 22);
		const int beat = beat_index(t);
		if (beat != last_before_beat_ && t > T_STREAM + 0.4 && out <= 0) {
			last_before_beat_ = beat;
			parts_.icons(tx.map(StreamView::viewers_pos()), 2, {"user-minus"}, {col::white, col::gray}, 60, -120, 1.1, 40);
		}
		hud(p, s_.sec_stream, 2, 8, t - T_STREAM - 0.1, out);
		big_text(p, s_.w_before, QPointF(70, 1000), 150, t - T_STREAM - 0.3, out, col::white, Qt::AlignLeft, QColor(0x3A, 0x40, 0x50));
		tag_line(p, QPointF(70, 1018), s_.p_chat, s_.tag_before, t - bt(11), out, 26);
	}

	void scene_after(QPainter &p, double t)
	{
		const double out = ramp(t, T_FEAT - 0.15, T_FEAT);
		if (!after_started_) {
			after_started_ = true;
			chat_.clear();
		}
		while (chat_after_n_ < s_.chat_after.size() && t >= T_AFTER + 0.3 + chat_after_n_ * kBeat * 0.5) {
			chat_.push_back({s_.chat_after[chat_after_n_], T_AFTER + 0.3 + chat_after_n_ * kBeat * 0.5});
			chat_after_n_++;
		}
		Pose pose = after_camera(t);
		pose.scale *= 1 + 0.012 * pulse(t);
		const double pop = std::exp(-since_beat(t) * 10) * kick(t);
		const QImage img = stream_.render(t, true, viewers_at(t), chat_, pop);
		const QTransform tx = draw_card(p, img, QSizeF(StreamView::TW, StreamView::TH), pose, col::accent,
						col::accent_dk, 22);
		// Зрители прибывают: иконки и осколки на каждую долю
		const int beat = beat_index(t);
		if (beat != last_after_beat_ && out <= 0 && t > T_AFTER + 0.2) {
			last_after_beat_ = beat;
			parts_.icons(tx.map(StreamView::viewers_pos()), 3, {"user-plus", "heart", "user-plus"},
				     {col::accent, col::white, col::accent_lt}, 90, 300, 1.3, 46);
			parts_.icons(tx.map(StreamView::follow_pos()), 2, {"heart"}, {col::accent_lt, col::white}, 60, 240, 1.2, 40);
			parts_.burst(tx.map(StreamView::viewers_pos()), 10, 250, 700, {col::accent, col::white, col::accent_dk}, 4, 0.6, 14, 500);
		}
		hud(p, s_.sec_stream, 4, 8, t - T_AFTER - 0.1, out);
		big_text(p, s_.w_after, QPointF(kW - 70, 1000), 150, t - T_AFTER - 0.2, out, col::white, Qt::AlignRight, col::accent);
		const QFont f = mono_font(26);
		const double w = QFontMetricsF(f).horizontalAdvance(QStringLiteral("[%1]: %2_").arg(s_.p_live, s_.tag_after)) + 26 * 1.2;
		tag_line(p, QPointF(kW - 70 - w, 1018), s_.p_live, s_.tag_after, t - T_AFTER - 0.6, out, 26);
	}

	// Окно фильтра: влёт, экскурсия, перетаскивание точек, «Умная настройка» и дроп.
	void scene_window(QPainter &p, double t)
	{
		const double in = ramp(t, T_GROOVE - 0.05, T_GROOVE + 0.7);
		const QRectF meters = card_rect(block_).adjusted(-6, -4, 6, 0);
		const QRectF meters_rows(meters.left(), meters.top(), meters.width(), 52);
		const QRectF status(meters.left(), meters.top() + 52, meters.width(), 26);
		const QRectF eqr = card_rect(eq_).adjusted(-6, -6, 6, 6);
		const QPointF eq_c = eqr.center();

		Pose wide = focus(QPointF(kCard.width() / 2, kCard.height() / 2), 1.0, -5, -10);
		wide.cy += 40;
		Pose start = wide;
		start.z = 1500;
		start.ry = 42;
		start.rx = -14;
		start.cx += 600;
		Pose on_meters = focus(meters.center() + QPointF(-40, 80), 1.45, -4, -7);
		Pose on_eq = focus(eq_c + QPointF(0, 30), 1.55, -3, -5);
		QPushButton *btn = win_.calib;
		const double scroll_k = smooth(ramp(t, T_RISE - 0.3, T_RISE + 0.5));
		win_.scroll->verticalScrollBar()->setValue(
			(int)(170 * scroll_k * (t < T_DROP ? 1 : 1 - smooth(ramp(t, T_DROP, T_DROP + 0.22)))));
		const QRectF btnr = card_rect(btn).adjusted(-6, -6, 6, 6);
		Pose on_btn = focus(btnr.center() + QPointF(0, -60), 1.75, -3, -4);
		Pose drop_pose = focus(eq_c + QPointF(0, 60), 1.3, -4, -7);

		Pose pose;
		if (t < bt(18))
			pose = mix(start, wide, ease_out(in));
		else if (t < bt(21))
			pose = mix(wide, on_meters, smooth(ramp(t, bt(18), bt(18.8))));
		else if (t < T_RISE)
			pose = mix(on_meters, on_eq, smooth(ramp(t, bt(21), bt(22))));
		else if (t < T_DROP)
			pose = mix(on_eq, on_btn, smooth(ramp(t, T_RISE, T_RISE + 0.6)));
		else
			pose = mix(on_btn, drop_pose, ease_out(ramp(t, T_DROP, T_DROP + 0.5)));
		if (t >= T_DROP)
			pose.scale *= 1 + 0.06 * std::exp(-(t - T_DROP) * 5);
		pose.cy += 6 * std::sin(t * 1.3);

		interact(t);
		draw_window(p, pose, t >= T_RISE && t < T_DROP ? col::accent : col::line);

		// Подсветка частей окна
		const double hl_levels = ramp(t, bt(18.5), bt(19)) * (1 - ramp(t, bt(19.8), bt(20.2)));
		const double hl_ai = ramp(t, bt(20), bt(20.4)) * (1 - ramp(t, bt(21), bt(21.4)));
		const double hl_eq = ramp(t, bt(21.6), bt(22)) * (1 - ramp(t, bt(22.4), bt(22.8)));
		spotlight(p, map_rect(meters_rows), hl_levels);
		callout(p, map_rect(meters_rows), s_.call_levels, hl_levels);
		spotlight(p, map_rect(status), hl_ai, col::accent_lt);
		callout(p, map_rect(status), s_.call_ai, hl_ai, false);
		spotlight(p, map_rect(eqr), hl_eq);
		callout(p, map_rect(eqr), s_.call_spectrum, hl_eq);
		const double hl_drag = ramp(t, bt(23), bt(23.3)) * (1 - ramp(t, T_RISE - 0.2, T_RISE));
		callout(p, map_rect(eqr), s_.call_drag, hl_drag);

		// «Умная настройка»: скобки на кнопке, сегментная зарядка, отсчёт
		const QPolygonF bq = map_rect(btnr);
		const QPointF bc = bq.boundingRect().center();
		if (t >= T_RISE && t < T_DROP) {
			const double k = ramp(t, T_RISE + 0.2, T_RISE + 0.5);
			spotlight(p, bq, k * 0.9);
			const double charge = ramp(t, T_CLICK, T_DROP);
			const QRectF b = bq.boundingRect();
			const int segs = 24;
			const double sw = b.width() / segs;
			for (int i = 0; i < segs; i++) {
				const bool on = (i + 1) <= charge * segs;
				p.fillRect(QRectF(b.left() + i * sw + 2, b.bottom() + 22, sw - 4, 16),
					   on ? (i % 2 ? col::accent_lt : col::accent) : QColor(255, 255, 255, 34));
			}
			if (t > T_CLICK) {
				chip(p, QPointF(b.center().x(), b.bottom() + 84),
				     s_.listening + QStringLiteral("  %1%").arg((int)(charge * 100)), 28, col::bg, col::accent_lt, 1,
				     ramp(t, T_CLICK, T_CLICK + 0.2), "mic", col::accent);
				for (int i = 0; i < 3; i++) {
					const double a = t - bt(29 + i);
					if (a >= 0 && a < kBeat) {
						const double s = 1.5 - 0.5 * ease_out(a / 0.2);
						p.save();
						p.setOpacity(1 - ramp(a, kBeat * 0.65, kBeat));
						big_text(p, QString::number(3 - i), QPointF(kW - 260, kH / 2.0 + 170 * s), 420 * s, 1, 0,
							 i == 2 ? col::accent : col::white, Qt::AlignHCenter);
						p.restore();
					}
				}
			}
		}
		// Заголовки
		if (t < T_RISE) {
			hud(p, s_.sec_live, 3, 8, t - T_GROOVE - 0.2, ramp(t, T_RISE - 0.2, T_RISE));
			big_text(p, s_.w_groove, QPointF(70, 1000), 130, t - T_GROOVE - 0.3, ramp(t, T_RISE - 0.25, T_RISE), col::white,
				 Qt::AlignLeft, col::accent_dk);
			tag_line(p, QPointF(70, 1018), s_.p_filter, s_.tag_groove, t - T_GROOVE - 0.8, ramp(t, T_RISE - 0.25, T_RISE), 26);
		} else if (t < T_DROP) {
			hud(p, s_.sec_setup, 3, 8, t - T_RISE, ramp(t, T_DROP - 0.1, T_DROP));
			big_text(p, s_.w_rise, QPointF(70, 1000), 130, t - T_RISE - 0.1, ramp(t, T_DROP - 0.1, T_DROP), col::white,
				 Qt::AlignLeft, col::accent);
			tag_line(p, QPointF(70, 1018), s_.p_setup, s_.tag_rise, t - T_CLICK, ramp(t, T_DROP - 0.1, T_DROP), 26);
		}
		if (t >= T_DROP) {
			// Штамп «ГОТОВО»
			const double a = t - T_DROP;
			const double s = 1.7 - 0.7 * ease_out(a / 0.18);
			const double o = ramp(a, 0, 0.05) * (1 - ramp(t, T_AFTER - 0.2, T_AFTER));
			if (o > 0) {
				p.save();
				p.setOpacity(o);
				p.translate(kW / 2.0, 250);
				p.rotate(-6);
				p.scale(s, s);
				const QFont f = display_font(170);
				const double w = QFontMetricsF(f).horizontalAdvance(s_.w_done);
				p.fillRect(QRectF(-w / 2 - 50 + 16, -110 + 16, w + 100, 200), col::accent_dk);
				p.fillRect(QRectF(-w / 2 - 50, -110, w + 100, 200), col::accent);
				p.setFont(f);
				p.setPen(col::white);
				p.shear(-0.2, 0);
				p.drawText(QPointF(-w / 2 + 30, 55), s_.w_done);
				p.restore();
			}
		}
		if (t >= T_DROP && !drop_burst_) {
			drop_burst_ = true;
			parts_.burst(bc, 140, 400, 1700, {col::accent, col::white, col::accent_dk, col::accent_lt}, 4, 1.2, 22, 900);
		}
		if (t >= T_CLICK && t < T_DROP && frame_no_ % 2 == 0)
			parts_.implode(bc, 3 + (int)(8 * ramp(t, T_CLICK, T_DROP)), 520, frame_no_ % 4 ? col::accent : col::white);
	}

	// Мышь и значения в окне: перетаскивание точек, нажатие кнопки, морфинг кривой.
	void interact(double t)
	{
		struct Drag {
			int node;
			QPointF d;
			double t0;
		};
		// Через две доли: подвод курсора ко второй точке не должен начаться, пока тянется первая.
		static const Drag drags[] = {{4, QPointF(16, -30), bt(23)}, {1, QPointF(-6, -24), bt(25)}};
		for (const Drag &d : drags) {
			const double k = t - d.t0;
			if (k < -0.45 || k > kBeat * 0.95 || drag_released_[d.node])
				continue;
			const QPointF node = eq_->nodePoint(d.node);
			if (k < 0) {
				if (drag_from_.isNull())
					drag_from_ = cursor_on_ ? cursor_ : map_pt(card_rect(eq_).center() + QPointF(120, 140));
				move_cursor(drag_from_, map_pt(card_rect(eq_).topLeft() + node), (k + 0.45) / 0.45);
				mock::send_mouse(eq_, QEvent::MouseMove, node, Qt::NoButton);
				continue;
			}
			if (drag_node_ != d.node) {
				drag_node_ = d.node;
				drag_grab_ = node;
				mock::send_mouse(eq_, QEvent::MouseButtonPress, node, Qt::LeftButton);
				click(map_pt(card_rect(eq_).topLeft() + node));
			}
			const QPointF pt = drag_grab_ + d.d * ease_out(k / (kBeat * 0.7));
			mock::send_mouse(eq_, QEvent::MouseMove, pt, Qt::LeftButton);
			cursor_ = map_pt(card_rect(eq_).topLeft() + pt);
			if (frame_no_ % 2 == 0)
				parts_.burst(cursor_, 2, 80, 260, {col::accent, col::white}, 0, 0.4, 8, 200);
			if (k > kBeat * 0.8) {
				drag_released_[d.node] = true;
				mock::send_mouse(eq_, QEvent::MouseButtonRelease, pt, Qt::NoButton);
				drag_from_ = QPointF();
				// Курсор ушёл с графика — подпись над точкой гаснет.
				mock::send_mouse(eq_, QEvent::MouseMove, QPointF(-200, -200), Qt::NoButton);
				QEvent leave(QEvent::Leave);
				QApplication::sendEvent(eq_, &leave);
			}
		}
		// Курсор к кнопке и нажатие
		const QRectF btnr = card_rect(win_.calib);
		if (t >= T_RISE && t < T_CLICK) {
			if (rise_from_.isNull())
				rise_from_ = cursor_;
			move_cursor(rise_from_, map_pt(btnr.center() + QPointF(60, 4)), ramp(t, T_RISE + 0.1, T_CLICK - 0.05));
		}
		if (t >= T_CLICK && !clicked_) {
			clicked_ = true;
			win_.calib->setDown(true);
			click(map_pt(btnr.center() + QPointF(60, 4)));
		}
		if (t >= T_CLICK + 0.15 && t < T_DROP && win_.calib->isDown()) {
			win_.calib->setDown(false);
			win_.calib->setText(link_->tr("Calib.Stop"));
		}
		if (t >= T_CLICK && t < T_DROP)
			cursor_ = map_pt(btnr.center() + QPointF(60, 4));
		if (t >= T_DROP && !morph_started_) {
			morph_started_ = true;
			win_.calib->setText(link_->tr("Calib.Start"));
			cursor_on_ = false;
			for (const char *k : {kLowGain, kMudGain, kNasalGain, kPresGain, kAirGain, kDenoiseAmount, kCompAmount})
				morph_from_[k] = link_->get_double(k);
		}
		if (morph_started_) {
			const double k = ease_out_back(ramp(t, T_DROP, T_DROP + 0.6));
			const std::map<std::string, double> target = {{kLowGain, 3.0},  {kMudGain, -3.5},     {kNasalGain, -2.5},
								       {kPresGain, 4.5}, {kAirGain, 3.5},      {kDenoiseAmount, 90},
								       {kCompAmount, 65}};
			for (auto &kv : target)
				link_->set_double(kv.first.c_str(), lerp(morph_from_[kv.first], kv.second, k));
		}
	}

	void scene_features(QPainter &p, double t)
	{
		const double in = ramp(t, T_FEAT - 0.05, T_FEAT + 0.5);
		const double out = ramp(t, T_LANG - 0.15, T_LANG);
		const int i = std::clamp(beat_index(t) - beat_index(T_FEAT + 0.01), 0, 7);
		QGroupBox *g = win_.groups[(size_t)i];
		auto target_of = [&](int gi) { return std::max(0, win_.groups[(size_t)gi]->y() - 40); };
		const double u = t - bt(40 + i);
		const double from = i == 0 ? target_of(0) : target_of(i - 1);
		win_.scroll->verticalScrollBar()->setValue((int)lerp(from, target_of(i), ease_out(u / 0.3)));
		static const std::vector<std::pair<int, std::pair<double, double>>> moves = {
			{0, {20, 100}}, {1, {-50, -12}}, {5, {-3, 6}}, {7, {0, 70}}, {8, {10, 75}}, {9, {0, 60}}, {10, {0, -5}}, {12, {-6, -1}}};
		const auto &mv = moves[(size_t)i];
		SliderRow &row = win_.sliders[(size_t)mv.first];
		const double v = lerp(mv.second.first, mv.second.second, ease_out(ramp(u, 0.05, 0.4)));
		link_->set_double(row.key, v);
		step_ui(0);

		Pose a{-500, kH / 2.0 + 40, 900, -8, 42, 0, 0.95, 1};
		Pose b{620, kH / 2.0 + 50, 0, -5, 16, 0, 1.0, 1};
		Pose pose = mix(a, b, ease_out(in));
		pose.cx -= ease_in(out) * 300;
		pose.scale *= 1 + 0.012 * pulse(t);
		draw_window(p, pose);
		const QRectF gr = card_rect(g).adjusted(-6, -4, 6, 6);
		spotlight(p, map_rect(gr), ramp(t, T_FEAT + 0.1, T_FEAT + 0.3) * (1 - out), i % 2 ? col::accent_lt : col::accent);
		const double frac = (v - row.min) / (row.max - row.min);
		const QRectF sr = card_rect(row.slider);
		const QPointF handle = map_pt(QPointF(sr.left() + 7 + frac * (sr.width() - 14), sr.center().y()));
		if (u > 0.05 && u < 0.4 && frame_no_ % 2 == 0)
			parts_.burst(handle, 3, 100, 420, {col::accent, col::white}, 0, 0.45, 9, 300);

		// Карточка функции — переворачивается на каждую долю
		const QImage &card = feature_cards_[(size_t)i];
		const double k = ease_out_back(u / 0.3);
		Pose cp{1440, 530, (1 - k) * -300, -6, -10 + (1 - k) * 70, (1 - k) * 4, 1.0, clamp01(u / 0.08)};
		cp.cx += ease_in(out) * 700;
		draw_card(p, card, QSizeF(560, 582), cp, QColor(0, 0, 0, 0), i % 2 ? col::accent : col::accent_dk, 16);
		if (feat_beat_ != i) {
			feat_beat_ = i;
			parts_.burst(QPointF(1440 - 280, 530), 16, 300, 900, {col::accent, col::accent_dk, col::white}, 4, 0.6, 16, 600);
			parts_.burst(QPointF(1440 + 280, 530), 16, 300, 900, {col::accent, col::accent_dk, col::white}, 4, 0.6, 16, 600);
		}
		// Индикатор: 8 квадратов под карточкой
		for (int j = 0; j < 8; j++) {
			const QRectF r(1440 - 4 * 34 + j * 34 + 4, 862, 26, 26);
			p.fillRect(r, j <= i ? (j % 2 ? col::accent_dk : col::accent) : QColor(255, 255, 255, 30));
		}
		hud(p, s_.sec_feat, 5, 8, t - T_FEAT - 0.1, out);
		big_text(p, s_.w_feat, QPointF(kW - 70, 190), 110, t - T_FEAT - 0.2, out, col::white, Qt::AlignRight, col::accent);
		tag_line(p, QPointF(70, 1018), s_.p_filter, s_.tag_feat, t - T_FEAT - 0.4, out, 26);
	}

	void scene_languages(QPainter &p, double t)
	{
		const double u = t - T_LANG;
		const double out = ramp(t, T_BREAK - 0.15, T_BREAK);
		const int n = (int)lang_cards_.size();
		const double step_deg = 360.0 / n;
		const double base = lerp(0, -step_deg * (n - 1), smooth(ramp(u, 0.15, 3.6)));
		const double R = 1050;
		struct Item {
			int i;
			double a, z;
		};
		std::vector<Item> items;
		for (int i = 0; i < n; i++) {
			const double a = std::remainder(base + i * step_deg, 360.0);
			if (std::cos(a * M_PI / 180) < 0.05)
				continue;
			items.push_back({i, a, R * (1 - std::cos(a * M_PI / 180))});
		}
		std::sort(items.begin(), items.end(), [](const Item &x, const Item &y) { return x.z > y.z; });
		const double in = ease_out(ramp(u, -0.05, 0.45));
		int front = -1;
		double best = 1e9;
		for (const Item &it : items) {
			const double rad = it.a * M_PI / 180;
			Pose pose;
			pose.cx = kW / 2.0 + R * std::sin(rad);
			pose.cy = kH / 2.0 + 40;
			pose.z = it.z + (1 - in) * 1200;
			pose.rx = -6;
			pose.ry = -it.a;
			pose.scale = 0.68;
			pose.cy += ease_in(out) * 900;
			pose.opacity = 0.3 + 0.7 * std::pow(std::cos(rad), 3);
			const bool is_front = std::abs(it.a) < step_deg / 2;
			draw_card(p, lang_cards_[(size_t)it.i], kCard * kCardScale, pose, is_front ? col::accent : col::line,
				  QColor(0, 0, 0, 160), 16);
			if (std::abs(it.a) < best) {
				best = std::abs(it.a);
				front = it.i;
			}
		}
		if (front != lang_front_) {
			lang_front_ = front;
			lang_change_t_ = t;
		}
		hud(p, s_.sec_lang, 6, 8, u - 0.1, out);
		if (front >= 0) {
			p.save();
			const QString name = QString::fromUtf8(lang_order_[(size_t)front].name);
			big_text(p, name, QPointF(kW / 2.0, 230), 110, (t - lang_change_t_) + 0.05, out, col::white, Qt::AlignHCenter,
				 front % 2 ? col::accent_dk : col::accent);
			p.restore();
			chip(p, QPointF(kW - 160, 70), QStringLiteral("[ %1/12 ]").arg(front + 1, 2, 10, QChar('0')), 26, col::bg, col::white,
			     1, 1 - out, "globe", col::line);
		}
		big_text(p, s_.w_lang, QPointF(70, 1000), 130, u - 0.1, out, col::white, Qt::AlignLeft, col::accent);
		tag_line(p, QPointF(70, 1018), s_.p_lang, s_.tag_lang, u - 0.4, out, 26);
	}

	// Окно обновления в стиле консоли: шапка, сегментная полоса, проверки.
	void progress_card(QPainter &p, double t, const Pose &pose)
	{
		QImage img(1200, 600, QImage::Format_ARGB32_Premultiplied);
		img.fill(col::panel);
		QPainter q(&img);
		q.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
		q.fillRect(QRectF(0, 0, 1200, 90), col::panel2);
		q.setFont(mono_font(34));
		q.setPen(col::white);
		q.drawText(QPointF(40, 58), s_.upd_head);
		q.setPen(col::accent);
		q.drawText(QPointF(40 + QFontMetricsF(mono_font(34)).horizontalAdvance(s_.upd_head + "  "), 58), QStringLiteral(">>"));
		const double prog = ease_out(ramp(t, bt(60.2), bt(62)));
		const bool done = t >= bt(62.8);
		const QString pct = QStringLiteral("[ v1.4.0  %1% ]").arg((int)(prog * 100), 3);
		q.setPen(col::gray);
		q.drawText(QPointF(1160 - QFontMetricsF(mono_font(34)).horizontalAdvance(pct), 58), pct);
		const int segs = 30;
		for (int i = 0; i < segs; i++) {
			const bool on = (i + 1) <= prog * segs;
			q.fillRect(QRectF(40 + i * 37.3, 130, 31, 40), on ? (done ? col::accent_lt : col::accent) : QColor(255, 255, 255, 26));
		}
		auto row = [&](double y, const QString &icon, const QString &text, double k) {
			if (k <= 0)
				return;
			q.setOpacity(k);
			draw_icon(q, icon, QPointF(62, y - 12), 40, col::accent, 2.2);
			q.setFont(mono_font(32));
			q.setPen(col::white);
			q.drawText(QPointF(104, y), text);
			const QString dots = QString(40 - text.size(), QChar('.'));
			q.setPen(col::dim);
			q.drawText(QPointF(104 + QFontMetricsF(mono_font(32)).horizontalAdvance(text + " "), y), dots);
			q.setPen(col::accent);
			q.drawText(QPointF(1080, y), QStringLiteral("OK"));
			draw_icon(q, "check", QPointF(1145, y - 12), 38, col::accent, 2.6);
			q.setOpacity(1);
		};
		row(250, "shield-check", s_.upd_sig, ramp(t, bt(62), bt(62.2)));
		row(320, "lock", s_.upd_sha, ramp(t, bt(62.4), bt(62.6)));
		if (done) {
			q.fillRect(QRectF(0, 420, 1200, 180), col::accent);
			draw_icon(q, "check", QPointF(80, 510), 72, col::white, 3);
			q.setFont(display_font(64));
			q.setPen(col::white);
			q.drawText(QPointF(140, 534), s_.upd_ready);
		}
		q.end();
		const QTransform tx = draw_card(p, img, QSizeF(1000, 500), pose, col::line, col::accent_dk, 18);
		if (prog > 0 && prog < 1 && frame_no_ % 2 == 0)
			parts_.burst(tx.map(QPointF(40 + 1120 * prog, 150)), 3, 100, 420, {col::accent, col::white}, 0, 0.4, 9, 300);
		if (done && !upd_done_burst_) {
			upd_done_burst_ = true;
			parts_.burst(tx.map(QPointF(600, 510)), 70, 300, 1200, {col::accent, col::white, col::accent_dk}, 4, 0.9, 18, 800);
		}
	}

	void scene_update(QPainter &p, double t)
	{
		const double in = ramp(t, T_BREAK - 0.05, T_BREAK + 0.6);
		const double out = ramp(t, T_DROP2 - 0.2, T_DROP2);
		if (link_->badge.isEmpty() && t > T_BREAK + 0.3)
			link_->badge = link_->tr("Live.Update").arg("1.4.0");
		win_.scroll->verticalScrollBar()->setValue(0);
		step_ui(0);
		const QRectF status = card_rect(block_);
		const QFontMetricsF fm(vsui::style::font(block_, 0.9));
		const double bw = std::min(fm.horizontalAdvance(link_->badge), status.width() * 0.62);
		const QRectF badge(status.right() - bw - 4, status.top() + 50, bw + 8, 24);

		Pose a = focus(QPointF(kCard.width() / 2, 300), 1.0, -12, -24);
		a.z = 1200;
		a.cx -= 500;
		Pose b = focus(QPointF(badge.center().x() - 120, 230), 1.45, -4, -7);
		Pose pose = mix(a, b, ease_out(in));
		pose.cy += 5 * std::sin(t * 1.1) - ease_in(out) * 900;
		draw_window(p, pose);

		const QPolygonF bq = map_rect(badge.adjusted(-6, -6, 6, 6));
		const double hl = ramp(t, bt(57), bt(57.4)) * (1 - ramp(t, bt(58.6), bt(59)));
		spotlight(p, bq, hl);
		const QPointF badge_pt = bq.boundingRect().center();
		const double dlg_flip = ease_out(ramp(t, bt(58.2), bt(58.9)));
		const double to_progress = ease_out(ramp(t, bt(60), bt(60.6)));
		Pose dp{1240, 640, 0, -4, 90 * (1 - dlg_flip) - 80 * to_progress, 0, 1.0, dlg_flip * (1 - ramp(t, bt(60), bt(60.35)))};
		dp.cy -= ease_in(out) * 900;
		const QSizeF dsize = dialog_size_ * 2.3;
		QTransform dtx;
		if (dp.opacity > 0)
			dtx = draw_card(p, dialog_, dsize, dp, col::accent, col::accent_dk, 16);
		const QPointF install_pt = dtx.map(install_center_ * kDpr);
		if (t >= bt(57) && t < bt(58)) {
			if (upd_from_.isNull())
				upd_from_ = QPointF(kW * 0.7, kH * 0.9);
			move_cursor(upd_from_, badge_pt, ramp(t, bt(57), bt(57.9)));
		}
		if (t >= bt(58) && !badge_clicked_) {
			badge_clicked_ = true;
			click(badge_pt);
		}
		if (t >= bt(59) && t < bt(60) && dp.opacity > 0.5)
			move_cursor(badge_pt, install_pt, ramp(t, bt(59), bt(59.85)));
		if (t >= bt(60) && !install_clicked_) {
			install_clicked_ = true;
			click(cursor_);
		}
		if (t >= bt(60.3))
			cursor_on_ = false;
		if (to_progress > 0) {
			Pose pp{1240, 640, 0, -4, -90 * (1 - to_progress), 0, 1.0, to_progress};
			pp.cy -= ease_in(out) * 900;
			progress_card(p, t, pp);
		}
		hud(p, s_.sec_upd, 7, 8, t - T_BREAK - 0.1, out);
		big_text(p, s_.w_upd, QPointF(70, 1000), 130, t - T_BREAK - 0.3, out, col::white, Qt::AlignLeft, col::accent_dk);
		tag_line(p, QPointF(70, 1018), s_.p_github, s_.tag_upd, t - T_BREAK - 0.8, out, 26);
	}

	void scene_finale(QPainter &p, double t)
	{
		const double u = t - T_DROP2;
		if (!finale_burst_ && u >= 0) {
			finale_burst_ = true;
			parts_.burst(QPointF(kW / 2.0, 460), 160, 400, 1800, {col::accent, col::accent_dk, col::white, col::accent_lt}, 4, 1.3, 22, 900);
		}
		for (int i = 0; i < 2; i++) {
			const double st = i ? T_STAB2 : T_STAB1;
			if (t >= st && !stab_done_[i]) {
				stab_done_[i] = true;
				parts_.burst(QPointF(kW / 2.0, 640), 90, 400, 1500, {col::accent, col::accent_dk, col::white}, 4, 0.9, 18, 800);
			}
		}
		const double punch = 1 + 0.04 * pulse(t) + 0.05 * std::exp(-(t - T_STAB1) * 10) * (t >= T_STAB1) +
				     0.05 * std::exp(-(t - T_STAB2) * 10) * (t >= T_STAB2);
		p.save();
		p.translate(kW / 2.0, 540);
		p.scale(punch, punch);
		p.translate(-kW / 2.0, -540);
		band(p, 380, 260, u + 0.05, 0, col::accent, false);
		big_text(p, s_.w_fin, QPointF(kW / 2.0, 470), 290, u - 0.02, 0, col::white, Qt::AlignHCenter);
		const double a = ease_out_back(ramp(u, 0.25, 0.7));
		if (a > 0) {
			p.save();
			p.translate(560, 660);
			p.scale(a * 0.62, a * 0.62);
			draw_logo(p, QPointF(0, 0), 1.0);
			p.restore();
		}
		big_text(p, s_.title, QPointF(660, 690), 92, u - 0.35, 0, col::white, Qt::AlignLeft, col::accent);
		{
			const QFont f = mono_font(26);
			const double w = QFontMetricsF(f).horizontalAdvance(s_.subtitle) + 26 * 1.3;
			chip(p, QPointF(668 + w / 2, 742), s_.subtitle, 26, col::bg, col::white, 1, ramp(u, 0.5, 0.8));
		}
		const double cta = ease_out_back((u - 0.9) / 0.3);
		if (cta > 0)
			chip(p, QPointF(kW / 2.0, 850), s_.fin_cta, 40, col::accent, col::white, 0.75 + 0.25 * cta + 0.03 * pulse(t),
			     clamp01(cta * 2), "download");
		p.restore();
		tag_line(p, QPointF(kW / 2.0 - 330, 930), s_.p_github, s_.tag_fin, u - 1.2, 0, 30);
	}

	bool ru_;
	Script s_;
	Chrome chrome_;
	QString locale_;
	StreamView stream_;
	std::shared_ptr<MockLink> link_;
	MockWindow win_;
	vsui::LiveBlock *block_ = nullptr;
	vsui::EqCurveView *eq_ = nullptr;
	QTransform card_tx_;
	Particles parts_;
	std::vector<Ripple> ripples_;
	std::vector<ChatLine> chat_;
	size_t chat_before_n_ = 0, chat_after_n_ = 0;
	bool after_started_ = false;
	int last_after_beat_ = -1, last_before_beat_ = -1, last_intro_beat_ = -1;

	std::vector<Lang> lang_order_;
	std::vector<QImage> lang_cards_;
	int lang_front_ = -1;
	double lang_change_t_ = 0;
	std::vector<QImage> feature_cards_;
	int feat_beat_ = -1;
	QImage dialog_;
	QSizeF dialog_size_;
	QPointF install_center_;

	double t_ = 0;
	int frame_no_ = 0;
	QPointF cursor_{kW * 0.7, kH * 0.85};
	bool cursor_on_ = false;
	QPointF drag_from_, drag_grab_, rise_from_, upd_from_;
	int drag_node_ = -1;
	std::map<int, bool> drag_released_;
	bool clicked_ = false, morph_started_ = false, drop_burst_ = false;
	std::map<std::string, double> morph_from_;
	bool badge_clicked_ = false, install_clicked_ = false, upd_done_burst_ = false, finale_burst_ = false;
	bool stab_done_[2] = {false, false};

public:
	void update(double t, double dt)
	{
		frame_no_++;
		// Живой блок тикает только пока окно в кадре (дальше — дешёвые сцены).
		if ((t >= T_GROOVE - 0.1 && t < T_AFTER + 0.3) || (t >= T_FEAT - 0.1 && t < T_LANG + 0.3) ||
		    (t >= T_BREAK - 0.1 && t < T_DROP2 + 0.3))
			step_ui(dt);
	}
};

} // namespace

int main(int argc, char **argv)
{
	qputenv("QT_SCALE_FACTOR", QByteArray::number(kDpr));
	QApplication app(argc, argv);
	if (argc < 6) {
		std::printf("usage: %s <data> <ffmpeg> <music> <out.mp4> ru|en\n"
			    "       %s <data> stills <dir> ru|en t1,t2,...\n",
			    argv[0], argv[0]);
		return 1;
	}
	apply_obs_theme(app);
	g_data = QString::fromLocal8Bit(argv[1]);
	const bool stills = QByteArray(argv[2]) == "stills";
	const bool ru = QByteArray(stills ? argv[4] : argv[5]) != "en";
	vsui::install_props_injector([](int, QWidget *parent) -> QWidget * {
		auto *b = new vsui::LiveBlock(g_factory_link, parent);
		b->setManualClock(true);
		return b;
	});

	Director director(ru);

	std::vector<int> still_frames;
	QString still_dir;
	QProcess enc;
	if (stills) {
		still_dir = QString::fromLocal8Bit(argv[3]);
		for (const QString &s : QString::fromLocal8Bit(argv[5]).split(','))
			still_frames.push_back((int)std::lround(s.toDouble() * kFps));
	} else {
		enc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
		enc.start(QString::fromLocal8Bit(argv[2]),
			  {"-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra", "-s", QStringLiteral("%1x%2").arg(kW).arg(kH),
			   "-r", QString::number(kFps), "-i", "-", "-i", QString::fromLocal8Bit(argv[3]), "-map", "0:v", "-map", "1:a",
			   "-c:v", "libx264", "-preset", "slow", "-crf", "17", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k",
			   "-af", QStringLiteral("afade=t=out:st=%1:d=0.4").arg(T_END - 0.45), "-t", QString::number(T_END),
			   "-movflags", "+faststart", QString::fromLocal8Bit(argv[4])});
		if (!enc.waitForStarted()) {
			std::printf("cannot start ffmpeg\n");
			return 2;
		}
	}

	const int total = (int)(T_END * kFps);
	const int last_still = still_frames.empty() ? -1 : *std::max_element(still_frames.begin(), still_frames.end());
	QImage canvas(kW, kH, QImage::Format_ARGB32_Premultiplied);
	QElapsedTimer clock;
	clock.start();
	for (int i = 0; i < total; i++) {
		if (stills && i > last_still)
			break;
		canvas.fill(Qt::black);
		director.frame(canvas, (double)i / kFps, 1.0 / kFps);
		if (stills) {
			if (std::find(still_frames.begin(), still_frames.end(), i) != still_frames.end()) {
				const QString path = QStringLiteral("%1/f%2.png").arg(still_dir).arg(i / (double)kFps, 5, 'f', 2, QChar('0'));
				canvas.convertToFormat(QImage::Format_RGB32).save(path);
				std::printf("still %s\n", path.toLocal8Bit().constData());
			}
		} else {
			enc.write(reinterpret_cast<const char *>(canvas.constBits()), canvas.sizeInBytes());
			while (enc.bytesToWrite() > 128LL * 1024 * 1024)
				enc.waitForBytesWritten(1000);
		}
		if (i % 150 == 0)
			std::printf("frame %d / %d (%.1f s)\n", i, total, clock.elapsed() / 1000.0);
		std::fflush(stdout);
	}
	if (!stills) {
		enc.closeWriteChannel();
		enc.waitForFinished(-1);
		std::printf("done: %s, %d frames in %.1f s\n", argv[4], total, clock.elapsed() / 1000.0);
		return enc.exitCode();
	}
	return 0;
}
