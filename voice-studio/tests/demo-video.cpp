// Демо-ролик «Голосовой студии» под музыку (123 уд/мин, 35,5 с): настоящий живой блок
// и окно настроек в 3D, подсветка, частицы, сцена трансляции со зрителями и чатом.
// Кадры считаются по виртуальному времени (30 к/с), поэтому тяжёлые эффекты не дают
// рывков; готовые кадры уходят в ffmpeg вместе с музыкой.
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
#include <QRegularExpression>
#include <QScrollBar>

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
constexpr double kDpr = 2.0;      // окна рисуются в двойном разрешении — можно приближать
const QSize kWin(880, 700);       // окно OBS в логических точках
constexpr double kTitleH = 36.0;  // заголовок окна на карточке
const QSizeF kCard(880, 736);     // карточка окна (с заголовком), логические точки
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
// Есть ли в этот момент бочка (во вступлении — тише, в паузах перед дропами — нет).
double kick(double t)
{
	if (t < T_GROOVE)
		return 0.45;
	if ((t > bt(26) && t < T_DROP) || (t > bt(56.5) && t < T_DROP2) || t > bt(71.5))
		return 0.0;
	return 1.0;
}
double pulse(double t, double decay = 7.0)
{
	return kick(t) * std::exp(-since_beat(t) * decay);
}
double energy(double t)
{
	if (t < T_GROOVE)
		return 0.35;
	if (t < T_RISE)
		return 0.6;
	if (t < T_DROP)
		return lerp(0.55, 0.95, ramp(t, T_RISE, T_DROP));
	if (t < T_BREAK)
		return 1.0;
	if (t < T_DROP2)
		return 0.45;
	return 1.0;
}

double hash01(int i)
{
	uint32_t x = (uint32_t)i * 2654435761u;
	x ^= x >> 13;
	x *= 0x5bd1e995u;
	x ^= x >> 15;
	return (x & 0xFFFFFF) / double(0xFFFFFF);
}

const QColor kAccent(0x5B, 0x8C, 0xFF), kViolet(0xA2, 0x6B, 0xFF), kGood(0x34, 0xE0, 0xA8), kBad(0xFF, 0x5A, 0x5F),
	kGold(0xFF, 0xC8, 0x57);

// ---------------------------------------------------------------------------
// Тексты ролика

struct Msg {
	QString user, text;
	QColor color;
};
struct Feature {
	QString icon, title, desc;
};
struct Script {
	QString title, subtitle, tagline;
	QString before_head, before_sub, tag_before, tag_after, channel, stream_title, viewers_word, follow, chat_head;
	std::vector<Msg> chat_before, chat_after;
	QString groove_head, call_levels, call_ai, call_spectrum, call_drag;
	QString rise_head, rise_sub, listening, done;
	QString after_head, after_sub;
	QString feat_head;
	std::vector<Feature> features;
	QString lang_head, lang_sub;
	QString upd_head, upd_sub, upd_sig, upd_sha;
	QString fin_sub, fin_chips, fin_cta;
};

Script script_for(bool ru)
{
	const QColor c1(0xFF, 0x7A, 0x7A), c2(0x7A, 0xC8, 0xFF), c3(0xFF, 0xD2, 0x6E), c4(0x9E, 0xFF, 0x9A),
		c5(0xD1, 0x9B, 0xFF), c6(0xFF, 0x9E, 0xD8);
	Script s;
	if (ru) {
		s.title = QStringLiteral("Голосовая студия");
		s.subtitle = QStringLiteral("плагин для OBS Studio");
		s.tagline = QStringLiteral("студийный звук любого микрофона");
		s.before_head = QStringLiteral("Шум, гул, треск…");
		s.before_sub = QStringLiteral("…и зрители уходят");
		s.tag_before = QStringLiteral("БЕЗ ПЛАГИНА");
		s.tag_after = QStringLiteral("С ГОЛОСОВОЙ СТУДИЕЙ");
		s.channel = QStringLiteral("твой_канал");
		s.stream_title = QStringLiteral("Вечерний стрим: болтаем и играем 🎮");
		s.viewers_word = QStringLiteral("зрителей");
		s.follow = QStringLiteral("♥ Подписаться");
		s.chat_head = QStringLiteral("ЧАТ ТРАНСЛЯЦИИ");
		s.chat_before = {{"kirill_88", "что за шум??", c1},
				 {"nastya_k", "микро трещит 😬", c2},
				 {"max_play", "тебя плохо слышно", c3},
				 {"zloy_kot", "выключи вентилятор пж", c4},
				 {"lena_s", "ушла, уши болят", c5}};
		s.chat_after = {{"dima_pro", "ОГО звук 🔥", c4},     {"nastya_k", "как на радио!", c2},
				{"max_play", "что за микро??", c3},    {"kirill_88", "студия 😳", c1},
				{"vasya_tv", "+ подписка", c6},        {"anon228", "голос 🔥🔥🔥", c5},
				{"lena_s", "вернулась ради звука", c5}, {"pro100", "скинь настройки!", c3},
				{"katya", "❤️❤️❤️", c6},               {"denis", "наконец-то чисто", c2},
				{"olya_m", "кайф слушать", c4},        {"tim", "лучший звук на твиче", c1}};
		s.groove_head = QStringLiteral("Прямо в окне фильтра OBS");
		s.call_levels = QStringLiteral("Живые уровни");
		s.call_ai = QStringLiteral("ИИ слышит голос");
		s.call_spectrum = QStringLiteral("Спектр вашего голоса");
		s.call_drag = QStringLiteral("Тяните точки мышью");
		s.rise_head = QStringLiteral("✨ Умная настройка");
		s.rise_sub = QStringLiteral("10 секунд — и всё под ваш голос");
		s.listening = QStringLiteral("Слушаю ваш голос…");
		s.done = QStringLiteral("Готово!");
		s.after_head = QStringLiteral("Звук как в студии");
		s.after_sub = QStringLiteral("— и зрители остаются");
		s.feat_head = QStringLiteral("Всё в одном фильтре");
		s.features = {{"🤖", "ИИ-шумодав", "Нейросеть убирает гул, улицу и клавиатуру"},
			      {"🚪", "Гейт по голосу", "Тишина в паузах, щелчки не пролезают"},
			      {"🎚", "Эквалайзер", "Глубина, чёткость, воздух — мышкой"},
			      {"🐍", "Де-эссер", "Мягкие «с» и «ш» без свиста"},
			      {"🗜", "Компрессор", "Ровная громкость одной ручкой"},
			      {"🔥", "Тепло", "Ламповый бархат в голосе"},
			      {"🎭", "Смена голоса", "±12 полутонов без бурундука"},
			      {"🛡", "Лимитер", "Не хрипит на крике и смехе"}};
		s.lang_head = QStringLiteral("12 языков");
		s.lang_sub = QStringLiteral("Плагин говорит на языке вашего OBS");
		s.upd_head = QStringLiteral("Обновляется сам");
		s.upd_sub = QStringLiteral("Каждое обновление проверяется цифровой подписью");
		s.upd_sig = QStringLiteral("Подпись проверена");
		s.upd_sha = QStringLiteral("Файлы целы (SHA-256)");
		s.fin_sub = QStringLiteral("Бесплатный плагин для OBS Studio");
		s.fin_chips = QStringLiteral("ИИ-шумодав   ·   Умная настройка   ·   12 языков   ·   Автообновление");
		s.fin_cta = QStringLiteral("Скачать бесплатно");
	} else {
		s.title = QStringLiteral("Voice Studio");
		s.subtitle = QStringLiteral("a plugin for OBS Studio");
		s.tagline = QStringLiteral("studio sound from any microphone");
		s.before_head = QStringLiteral("Noise, hum, crackle…");
		s.before_sub = QStringLiteral("…and viewers leave");
		s.tag_before = QStringLiteral("WITHOUT PLUGIN");
		s.tag_after = QStringLiteral("WITH VOICE STUDIO");
		s.channel = QStringLiteral("your_channel");
		s.stream_title = QStringLiteral("Late night stream: chatting & gaming 🎮");
		s.viewers_word = QStringLiteral("viewers");
		s.follow = QStringLiteral("♥ Follow");
		s.chat_head = QStringLiteral("STREAM CHAT");
		s.chat_before = {{"kevin_88", "what's that noise??", c1},
				 {"nastya_k", "mic is crackling 😬", c2},
				 {"max_play", "can barely hear you", c3},
				 {"grumpy_cat", "turn off the fan pls", c4},
				 {"lena_s", "leaving, my ears hurt", c5}};
		s.chat_after = {{"dima_pro", "WOW the audio 🔥", c4},  {"nastya_k", "sounds like radio!", c2},
				{"max_play", "what mic is that??", c3}, {"kevin_88", "studio quality 😳", c1},
				{"sam_tv", "just followed", c6},        {"anon228", "voice 🔥🔥🔥", c5},
				{"lena_s", "came back for the sound", c5}, {"pro100", "share your settings!", c3},
				{"katya", "❤️❤️❤️", c6},                {"denis", "finally crystal clear", c2},
				{"olya_m", "so nice to listen", c4},    {"tim", "best audio on the platform", c1}};
		s.groove_head = QStringLiteral("Right inside the OBS filter window");
		s.call_levels = QStringLiteral("Live levels");
		s.call_ai = QStringLiteral("AI hears your voice");
		s.call_spectrum = QStringLiteral("Your voice spectrum");
		s.call_drag = QStringLiteral("Drag the points");
		s.rise_head = QStringLiteral("✨ Smart setup");
		s.rise_sub = QStringLiteral("10 seconds — tuned to your voice");
		s.listening = QStringLiteral("Listening to your voice…");
		s.done = QStringLiteral("Done!");
		s.after_head = QStringLiteral("Studio sound");
		s.after_sub = QStringLiteral("— and viewers stay");
		s.feat_head = QStringLiteral("All in one filter");
		s.features = {{"🤖", "AI noise removal", "A neural net removes hum, street and keys"},
			      {"🚪", "Voice gate", "Silent pauses, no keyboard clicks"},
			      {"🎚", "Equalizer", "Depth, clarity, air — with the mouse"},
			      {"🐍", "De-esser", "Smooth S and SH, no whistling"},
			      {"🗜", "Compressor", "Even loudness with one knob"},
			      {"🔥", "Warmth", "Tube-style velvet in your voice"},
			      {"🎭", "Voice changer", "±12 semitones, no chipmunks"},
			      {"🛡", "Limiter", "No clipping when you shout or laugh"}};
		s.lang_head = QStringLiteral("12 languages");
		s.lang_sub = QStringLiteral("The plugin follows your OBS language");
		s.upd_head = QStringLiteral("Updates itself");
		s.upd_sub = QStringLiteral("Every update is verified with a digital signature");
		s.upd_sig = QStringLiteral("Signature verified");
		s.upd_sha = QStringLiteral("Files intact (SHA-256)");
		s.fin_sub = QStringLiteral("A free plugin for OBS Studio");
		s.fin_chips = QStringLiteral("AI denoise   ·   Smart setup   ·   12 languages   ·   Auto-updates");
		s.fin_cta = QStringLiteral("Download free");
	}
	return s;
}

struct Lang {
	const char *code, *name;
};
const Lang kLangs[] = {{"en-US", "English"},   {"be-BY", "Беларуская"},   {"uk-UA", "Українська"},
		       {"kk-KZ", "Қазақша"},   {"ky-KG", "Кыргызча"},     {"uz-UZ", "Oʻzbekcha"},
		       {"tg-TJ", "Тоҷикӣ"},    {"az-AZ", "Azərbaycanca"}, {"hy-AM", "Հայերեն"},
		       {"tt-RU", "Татарча"},   {"ba-RU", "Башҡортса"},    {"ru-RU", "Русский"}};

// ---------------------------------------------------------------------------
// Шрифты и текст

QString g_family = QStringLiteral("Segoe UI");

QFont ui_font(double px, int weight = QFont::Normal)
{
	QFont f(g_family);
	f.setPixelSize((int)std::lround(px));
	f.setWeight(QFont::Weight(weight));
	return f;
}

bool has_emoji(const QString &s)
{
	static const QRegularExpression re(QStringLiteral("[\\x{1F000}-\\x{1FAFF}\\x{2600}-\\x{27BF}\\x{2B00}-\\x{2BFF}]"));
	return re.match(s).hasMatch();
}

// Заголовок со словами, которые по очереди выезжают снизу; glow — свечение акцентом.
void headline(QPainter &p, const QString &text, QPointF anchor, double px, double local, double out, int weight,
	      QColor color = Qt::white, QColor glow = kAccent, Qt::Alignment align = Qt::AlignHCenter)
{
	if (local <= 0 || out >= 1)
		return;
	const QFont f = ui_font(px, weight);
	const QFontMetricsF fm(f);
	const QStringList words = text.split(' ');
	const double space = fm.horizontalAdvance(' ');
	double total = -space;
	std::vector<double> widths;
	for (const QString &w : words) {
		widths.push_back(fm.horizontalAdvance(w));
		total += widths.back() + space;
	}
	double x = align == Qt::AlignLeft ? anchor.x() : anchor.x() - total / 2;
	const double fade_out = 1 - smooth(out);
	const double base = p.opacity();
	for (int i = 0; i < words.size(); i++) {
		const double k = ease_out((local - i * 0.06) / 0.4);
		if (k > 0) {
			const QPointF pos(x, anchor.y() + (1 - k) * px * 0.55 - smooth(out) * px * 0.3);
			p.setOpacity(base * k * fade_out);
			if (glow.alpha() > 0 && !has_emoji(words[i])) {
				QPainterPath path;
				path.addText(pos, f, words[i]);
				p.setCompositionMode(QPainter::CompositionMode_Plus);
				for (int g = 0; g < 3; g++) {
					QColor c = glow;
					c.setAlpha(28 + g * 18);
					p.strokePath(path, QPen(c, px * (0.22 - g * 0.07), Qt::SolidLine, Qt::RoundCap,
								Qt::RoundJoin));
				}
				p.setCompositionMode(QPainter::CompositionMode_SourceOver);
				p.fillPath(path, color);
			} else {
				p.setFont(f);
				p.setPen(color);
				p.drawText(pos, words[i]);
			}
		}
		x += widths[(size_t)i] + space;
	}
	p.setOpacity(base);
}

void pill(QPainter &p, QPointF center, const QString &text, double px, QColor bg, QColor fg, double scale = 1,
	  double opacity = 1)
{
	if (opacity <= 0)
		return;
	const QFont f = ui_font(px * scale, QFont::DemiBold);
	const QFontMetricsF fm(f);
	const QSizeF s(fm.horizontalAdvance(text) + px * 1.4 * scale, fm.height() + px * 0.6 * scale);
	const QRectF r(center - QPointF(s.width() / 2, s.height() / 2), s);
	const double base = p.opacity();
	p.setOpacity(base * opacity);
	p.setPen(Qt::NoPen);
	p.setBrush(bg);
	p.drawRoundedRect(r, s.height() / 2, s.height() / 2);
	p.setFont(f);
	p.setPen(fg);
	p.drawText(r, Qt::AlignCenter, text);
	p.setOpacity(base);
}

QString group_digits(long long n)
{
	QString s = QString::number(n);
	for (int i = s.size() - 3; i > 0; i -= 3)
		s.insert(i, QChar(0x2009));
	return s;
}

// ---------------------------------------------------------------------------
// Картинки-заготовки: свечение, тень, виньетка

QImage glow_sprite(QColor c, int size = 96)
{
	QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	QRadialGradient g(size / 2.0, size / 2.0, size / 2.0);
	QColor a = c, b = c;
	a.setAlpha(255);
	b.setAlpha(0);
	QColor mid = c;
	mid.setAlpha(90);
	g.setColorAt(0, a);
	g.setColorAt(0.25, mid);
	g.setColorAt(1, b);
	p.fillRect(img.rect(), g);
	return img;
}

void box_blur_alpha(QImage &img, int r)
{
	const int w = img.width(), h = img.height();
	std::vector<int> a((size_t)w * h), tmp((size_t)w * h);
	for (int y = 0; y < h; y++) {
		const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
		for (int x = 0; x < w; x++)
			a[(size_t)y * w + x] = qAlpha(line[x]);
	}
	for (int pass = 0; pass < 3; pass++) {
		for (int y = 0; y < h; y++) {
			int sum = 0;
			for (int x = -r; x <= r; x++)
				sum += a[(size_t)y * w + std::clamp(x, 0, w - 1)];
			for (int x = 0; x < w; x++) {
				tmp[(size_t)y * w + x] = sum / (2 * r + 1);
				sum += a[(size_t)y * w + std::min(x + r + 1, w - 1)] - a[(size_t)y * w + std::max(x - r, 0)];
			}
		}
		for (int x = 0; x < w; x++) {
			int sum = 0;
			for (int y = -r; y <= r; y++)
				sum += tmp[(size_t)std::clamp(y, 0, h - 1) * w + x];
			for (int y = 0; y < h; y++) {
				a[(size_t)y * w + x] = sum / (2 * r + 1);
				sum += tmp[(size_t)std::min(y + r + 1, h - 1) * w + x] - tmp[(size_t)std::max(y - r, 0) * w + x];
			}
		}
	}
	for (int y = 0; y < h; y++) {
		QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
		for (int x = 0; x < w; x++)
			line[x] = qRgba(0, 0, 0, a[(size_t)y * w + x]);
	}
}

QImage make_shadow()
{
	QImage img(520, 460, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 230));
	p.drawRoundedRect(QRectF(60, 60, 400, 340), 18, 18);
	p.end();
	box_blur_alpha(img, 14);
	return img;
}

QImage make_vignette()
{
	QImage img(kW, kH, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	QRadialGradient g(kW / 2.0, kH / 2.0, kW * 0.62);
	g.setColorAt(0.55, QColor(0, 0, 0, 0));
	g.setColorAt(1.0, QColor(0, 0, 0, 200));
	p.fillRect(img.rect(), g);
	return img;
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

struct Assets {
	QImage shadow, vignette;
	std::map<QRgb, QImage> glows;
	const QImage &glow(QColor c)
	{
		auto it = glows.find(c.rgb());
		if (it == glows.end())
			it = glows.emplace(c.rgb(), glow_sprite(c)).first;
		return it->second;
	}
};

Assets *g_assets = nullptr;

void draw_glow(QPainter &p, QPointF c, double size, QColor color, double opacity)
{
	if (opacity <= 0.003 || size <= 0.5)
		return;
	const double base = p.opacity();
	p.setOpacity(base * std::min(1.0, opacity));
	p.drawImage(QRectF(c.x() - size / 2, c.y() - size / 2, size, size), g_assets->glow(color));
	p.setOpacity(base);
}

// Карточка с мягкой тенью и (по желанию) светящейся кромкой.
QTransform draw_card(QPainter &p, const QImage &img, QSizeF disp, const Pose &pose, double edge_glow = 0,
		     QColor edge = kAccent)
{
	if (pose.opacity <= 0.003)
		return QTransform();
	Pose sp = pose;
	sp.cy += 34 * pose.scale;
	const QTransform st = card_transform(g_assets->shadow.size(), QSizeF(disp.width() * 1.3, disp.height() * 1.36), sp);
	p.save();
	p.setOpacity(pose.opacity * 0.85);
	p.setTransform(st);
	p.drawImage(0, 0, g_assets->shadow);
	p.restore();

	const QTransform t = card_transform(img.size(), disp, pose);
	p.save();
	p.setOpacity(pose.opacity);
	p.setTransform(t);
	p.drawImage(0, 0, img);
	p.restore();

	if (edge_glow > 0) {
		const QPolygonF q = project_quad(disp, pose);
		p.save();
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		p.setBrush(Qt::NoBrush);
		for (int g = 0; g < 3; g++) {
			QColor c = edge;
			c.setAlphaF(std::min(1.0, edge_glow * pose.opacity * (0.12 + g * 0.18)));
			p.setPen(QPen(c, 14 - g * 5.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			p.drawPolygon(q);
		}
		p.restore();
	}
	return t;
}

// ---------------------------------------------------------------------------
// Частицы

struct Particle {
	QPointF p, v;
	double age = 0, life = 1, size = 10, spin = 0;
	QColor color;
	int kind = 0; // 0 светящаяся точка, 1 искра-штрих, 2 значок (эмодзи), 3 конфетти
	QString glyph;
	double gravity = 0, drag = 0.0;
};

class Particles {
public:
	std::vector<Particle> list;
	std::mt19937 rng{20261021};

	double rnd(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }

	void burst(QPointF at, int n, double speed_min, double speed_max, std::vector<QColor> colors, int kind = 1,
		   double life = 0.9, double size = 10, double gravity = 380)
	{
		for (int i = 0; i < n; i++) {
			Particle q;
			const double a = rnd(0, 2 * M_PI), s = rnd(speed_min, speed_max);
			q.p = at;
			q.v = QPointF(std::cos(a) * s, std::sin(a) * s);
			q.life = life * rnd(0.6, 1.2);
			q.size = size * rnd(0.6, 1.3);
			q.color = colors[(size_t)(i % colors.size())];
			q.kind = kind;
			q.gravity = gravity;
			q.drag = 1.6;
			q.spin = rnd(-8, 8);
			list.push_back(q);
		}
	}

	void glyphs(QPointF at, int n, const QStringList &g, double spread, double rise, double life = 1.6,
		    double size = 34)
	{
		for (int i = 0; i < n; i++) {
			Particle q;
			q.p = at + QPointF(rnd(-spread, spread), rnd(-10, 10));
			q.v = QPointF(rnd(-60, 60), -rise * rnd(0.7, 1.3));
			q.life = life * rnd(0.8, 1.2);
			q.size = size * rnd(0.75, 1.25);
			q.kind = 2;
			q.glyph = g[(int)rnd(0, g.size() - 0.001)];
			q.gravity = -40;
			q.drag = 0.4;
			list.push_back(q);
		}
	}

	// Частицы, которые стягиваются к точке (зарядка перед дропом).
	void implode(QPointF to, int n, double radius, QColor c)
	{
		for (int i = 0; i < n; i++) {
			Particle q;
			const double a = rnd(0, 2 * M_PI), r = radius * rnd(0.7, 1.2);
			q.p = to + QPointF(std::cos(a) * r, std::sin(a) * r);
			q.life = rnd(0.45, 0.7);
			q.v = (to - q.p) / q.life;
			q.size = rnd(6, 14);
			q.color = c;
			q.kind = 1;
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
		}
		list.erase(std::remove_if(list.begin(), list.end(), [](const Particle &q) { return q.age >= q.life; }),
			   list.end());
	}

	void draw(QPainter &p)
	{
		for (const Particle &q : list) {
			const double k = q.age / q.life, fade = 1 - k * k;
			if (q.kind == 0) {
				p.setCompositionMode(QPainter::CompositionMode_Plus);
				draw_glow(p, q.p, q.size * 3 * (1 - k * 0.5), q.color, fade);
			} else if (q.kind == 1) {
				p.setCompositionMode(QPainter::CompositionMode_Plus);
				QColor c = q.color;
				c.setAlphaF(fade);
				const QPointF tail = q.p - q.v * 0.035;
				p.setPen(QPen(c, std::max(1.0, q.size * 0.28 * (1 - k)), Qt::SolidLine, Qt::RoundCap));
				p.drawLine(tail, q.p);
				draw_glow(p, q.p, q.size * 1.6, q.color, fade * 0.6);
			} else if (q.kind == 2) {
				p.setCompositionMode(QPainter::CompositionMode_SourceOver);
				p.setOpacity(std::min(1.0, fade * 1.4) * std::min(1.0, q.age * 6));
				p.setFont(ui_font(q.size));
				p.setPen(Qt::white);
				p.drawText(q.p, q.glyph);
				p.setOpacity(1);
			} else {
				p.setCompositionMode(QPainter::CompositionMode_SourceOver);
				p.save();
				p.translate(q.p);
				p.rotate(q.age * q.spin * 57.3);
				QColor c = q.color;
				c.setAlphaF(fade);
				p.fillRect(QRectF(-q.size / 2, -q.size / 4, q.size, q.size / 2), c);
				p.restore();
			}
		}
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}
};

// ---------------------------------------------------------------------------
// Трансляция (условная площадка, без чужих логотипов)

struct ChatLine {
	Msg msg;
	double at;
};

class StreamView {
public:
	static constexpr int TW = 1600, TH = 900;

	explicit StreamView(const Script &s) : s_(s)
	{
		game_ = QImage(1200, 675, QImage::Format_ARGB32_Premultiplied);
		QPainter p(&game_);
		p.setRenderHint(QPainter::Antialiasing);
		QLinearGradient sky(0, 0, 0, 675);
		sky.setColorAt(0, QColor(0x12, 0x0A, 0x35));
		sky.setColorAt(0.55, QColor(0x3A, 0x12, 0x5A));
		sky.setColorAt(1, QColor(0x0B, 0x08, 0x1C));
		p.fillRect(game_.rect(), sky);
		for (int i = 0; i < 120; i++) {
			p.setPen(QColor(255, 255, 255, 60 + (int)(hash01(i * 7) * 150)));
			p.drawPoint(QPointF(hash01(i * 3) * 1200, hash01(i * 5) * 330));
		}
		QRadialGradient sun(600, 330, 170);
		sun.setColorAt(0, QColor(0xFF, 0xD3, 0x6E));
		sun.setColorAt(0.7, QColor(0xFF, 0x4F, 0x9A));
		sun.setColorAt(1, QColor(0xFF, 0x4F, 0x9A, 0));
		p.setPen(Qt::NoPen);
		p.setBrush(sun);
		p.drawEllipse(QPointF(600, 330), 170, 170);
		for (int i = 0; i < 6; i++)
			p.fillRect(QRectF(380, 300 + i * 22, 440, 4 + i * 1.5), QColor(0x2A, 0x0E, 0x4A));
		QPainterPath hills;
		hills.moveTo(0, 430);
		for (int x = 0; x <= 1200; x += 60)
			hills.lineTo(x, 380 + 50 * std::sin(x * 0.011) + 30 * std::sin(x * 0.037));
		hills.lineTo(1200, 675);
		hills.lineTo(0, 675);
		p.fillPath(hills, QColor(0x14, 0x08, 0x2C));
		p.setPen(QPen(QColor(0xFF, 0x4F, 0xC8, 150), 2));
		for (int i = -12; i <= 12; i++)
			p.drawLine(QPointF(600 + i * 25, 440), QPointF(600 + i * 160, 675));
		for (int i = 0; i < 8; i++) {
			const double y = 440 + 235 * std::pow(i / 8.0, 2);
			p.drawLine(QPointF(0, y), QPointF(1200, y));
		}
		p.end();

		cam_ = QImage(230, 230, QImage::Format_ARGB32_Premultiplied);
		cam_.fill(Qt::transparent);
		QPainter c(&cam_);
		c.setRenderHint(QPainter::Antialiasing);
		QPainterPath circle;
		circle.addEllipse(QRectF(0, 0, 230, 230));
		c.setClipPath(circle);
		QLinearGradient bg(0, 0, 0, 230);
		bg.setColorAt(0, QColor(0x2E, 0x38, 0x58));
		bg.setColorAt(1, QColor(0x16, 0x1C, 0x2E));
		c.fillRect(cam_.rect(), bg);
		c.setPen(Qt::NoPen);
		c.setBrush(QColor(0x9A, 0xAB, 0xD4));
		c.drawEllipse(QPointF(115, 98), 44, 50);
		c.drawRoundedRect(QRectF(40, 158, 150, 110), 60, 60);
		c.setBrush(Qt::NoBrush);
		c.setPen(QPen(QColor(0x24, 0x28, 0x36), 10, Qt::SolidLine, Qt::RoundCap));
		c.drawArc(QRectF(62, 44, 106, 110), 20 * 16, 140 * 16);
		c.setBrush(QColor(0x24, 0x28, 0x36));
		c.setPen(Qt::NoPen);
		c.drawRoundedRect(QRectF(56, 94, 16, 34), 6, 6);
		c.drawRoundedRect(QRectF(158, 94, 16, 34), 6, 6);
		c.setPen(QPen(QColor(0x24, 0x28, 0x36), 5, Qt::SolidLine, Qt::RoundCap));
		c.drawLine(QPointF(66, 124), QPointF(98, 146));
		c.end();
	}

	// Речь стримера: огибающая громкости.
	static double speech(double t)
	{
		const double phrase = std::sin(2 * M_PI * 0.31 * t) > -0.5 ? 1.0 : 0.15;
		const double syl = std::pow(std::max(0.0, std::sin(2 * M_PI * 3.3 * t)), 0.5);
		return phrase * (0.25 + 0.75 * syl);
	}

	QImage render(double t, double clean, double viewers, const std::vector<ChatLine> &chat, double follow_glow)
	{
		QImage img(TW, TH, QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		QPainter p(&img);
		p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
		QPainterPath frame;
		frame.addRoundedRect(QRectF(0, 0, TW, TH), 18, 18);
		p.setClipPath(frame);
		p.fillRect(img.rect(), QColor(0x0E, 0x10, 0x16));

		// Видео: «игра» + камера стримера + звук
		p.drawImage(QPointF(-10 + 10 * std::sin(t * 0.7), 0), game_);
		const double env = speech(t);
		const QPointF cam_c(40 + 115, 675 - 40 - 115);
		for (int r = 0; r < 3; r++) {
			const double rr = 120 + r * 16 + env * 22;
			QColor c = clean > 0.5 ? kGood : kBad;
			c.setAlpha((int)(170 - r * 50) * (0.4 + env * 0.6));
			p.setPen(QPen(c, 4 - r));
			p.setBrush(Qt::NoBrush);
			if (clean > 0.5) {
				p.drawEllipse(cam_c, rr, rr);
			} else {
				QPolygonF jag;
				for (int a = 0; a <= 72; a++) {
					const double ang = a * 2 * M_PI / 72;
					const double j = rr + (hash01(a * 13 + (int)(t * 30) * 7 + r) - 0.5) * 22;
					jag << cam_c + QPointF(std::cos(ang) * j, std::sin(ang) * j);
				}
				p.drawPolyline(jag);
			}
		}
		p.drawImage(cam_c - QPointF(115, 115), cam_);

		// Волна голоса
		const QRectF wave(300, 585, 860, 70);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 120));
		p.drawRoundedRect(wave, 14, 14);
		QPainterPath wpath;
		for (int i = 0; i <= 170; i++) {
			const double x = wave.left() + 14 + i * (wave.width() - 28) / 170;
			const double e = speech(t - (170 - i) * 0.012);
			double y = std::sin(i * 0.9 + t * 30) * e * 26;
			if (clean < 0.5)
				y = y * 0.8 + (hash01(i * 31 + (int)(t * 30) * 17) - 0.5) * 30 +
				    (hash01(i + (int)(t * 30)) > 0.93 ? 28 : 0);
			const QPointF pt(x, wave.center().y() + y);
			if (i == 0)
				wpath.moveTo(pt);
			else
				wpath.lineTo(pt);
		}
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(clean > 0.5 ? kGood : kBad, 3));
		p.drawPath(wpath);

		// Помехи без плагина
		if (clean < 1) {
			const double k = 1 - clean;
			for (int i = 0; i < 1600; i++) {
				const int seed = i * 97 + (int)(t * 30) * 7919;
				p.setPen(QColor(255, 255, 255, (int)(110 * k * hash01(seed))));
				p.drawPoint(QPointF(hash01(seed + 1) * 1200, hash01(seed + 2) * 675));
			}
			for (int i = 0; i < 3; i++) {
				const int seed = (int)(t * 12) * 31 + i;
				if (hash01(seed) > 0.55) {
					const double y = hash01(seed + 5) * 640;
					p.fillRect(QRectF(0, y, 1200, 6 + hash01(seed + 9) * 14), QColor(255, 255, 255, (int)(26 * k)));
				}
			}
		}

		// Значки поверх видео
		pill(p, QPointF(78, 42), QStringLiteral("● LIVE"), 22, QColor(0xE5, 0x2B, 0x3C), Qt::white);
		pill(p, QPointF(200, 42), QStringLiteral("👁 ") + group_digits((long long)viewers), 22, QColor(0, 0, 0, 150),
		     Qt::white);
		{
			const QString tag = clean > 0.5 ? QStringLiteral("🎙 ") + s_.tag_after : QStringLiteral("🔇 ") + s_.tag_before;
			const QFont f = ui_font(22, QFont::Bold);
			const double w = QFontMetricsF(f).horizontalAdvance(tag) + 34;
			pill(p, QPointF(1200 - 24 - w / 2, 42), tag, 22, clean > 0.5 ? QColor(0x12, 0x8F, 0x6A) : QColor(0xB0, 0x1E, 0x2E),
			     Qt::white);
		}

		// Полоса под видео
		p.fillRect(QRectF(0, 675, 1200, 225), QColor(0x13, 0x15, 0x1D));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x6B, 0x4D, 0xE6));
		p.drawEllipse(QPointF(70, 745), 38, 38);
		p.setFont(ui_font(30, QFont::Bold));
		p.setPen(Qt::white);
		p.drawText(QPointF(126, 735), s_.channel);
		p.setFont(ui_font(24));
		p.setPen(QColor(0xB8, 0xBE, 0xCC));
		p.drawText(QPointF(126, 772), s_.stream_title);
		p.setFont(ui_font(52, QFont::Bold));
		p.setPen(clean > 0.5 ? kGood : QColor(0xE8, 0xEA, 0xF0));
		const QString vtext = QStringLiteral("👁 ") + group_digits((long long)viewers);
		p.drawText(QRectF(700, 700, 470, 70), Qt::AlignRight | Qt::AlignVCenter, vtext);
		p.setFont(ui_font(22));
		p.setPen(QColor(0x9A, 0xA0, 0xB0));
		p.drawText(QRectF(700, 768, 470, 30), Qt::AlignRight | Qt::AlignVCenter, s_.viewers_word);
		{
			const QRectF fb(126, 812, 250, 56);
			QColor fbc(0x7C, 0x4D, 0xFF);
			p.setBrush(fbc);
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(fb, 10, 10);
			if (follow_glow > 0) {
				p.setCompositionMode(QPainter::CompositionMode_Plus);
				QColor g = fbc;
				g.setAlphaF(clamp01(follow_glow) * 0.8);
				p.setPen(QPen(g, 10));
				p.setBrush(Qt::NoBrush);
				p.drawRoundedRect(fb.adjusted(-4, -4, 4, 4), 12, 12);
				p.setCompositionMode(QPainter::CompositionMode_SourceOver);
			}
			p.setFont(ui_font(24, QFont::DemiBold));
			p.setPen(Qt::white);
			p.drawText(fb, Qt::AlignCenter, s_.follow);
		}

		// Чат
		const QRectF chat_r(1200, 0, 400, 900);
		p.fillRect(chat_r, QColor(0x18, 0x1A, 0x22));
		p.fillRect(QRectF(1200, 0, 2, 900), QColor(255, 255, 255, 18));
		p.setFont(ui_font(20, QFont::Bold));
		p.setPen(QColor(0xC8, 0xCC, 0xD8));
		p.drawText(QRectF(1222, 0, 360, 64), Qt::AlignVCenter, s_.chat_head);
		p.fillRect(QRectF(1200, 64, 400, 1), QColor(255, 255, 255, 18));
		double y = 870;
		const QFont uf = ui_font(23, QFont::Bold), mf = ui_font(23);
		for (auto it = chat.rbegin(); it != chat.rend() && y > 90; ++it) {
			const double age = t - it->at;
			if (age < 0)
				continue;
			const double k = ease_out(age / 0.25);
			const QString line = it->msg.user + ": " + it->msg.text;
			const QRectF box(1222 + (1 - k) * 60, 0, 360, 1000);
			const QRectF br = QFontMetricsF(mf).boundingRect(box, Qt::TextWordWrap, line);
			y -= br.height() + 14;
			p.setOpacity(k);
			p.fillRect(QRectF(1212, y - 6, 380, br.height() + 12), QColor(255, 255, 255, (int)(14 * (1 - ease_out(age / 1.2)))));
			p.setFont(uf);
			p.setPen(it->msg.color);
			const double uw = QFontMetricsF(uf).horizontalAdvance(it->msg.user + ": ");
			p.drawText(QPointF(box.left(), y + QFontMetricsF(mf).ascent()), it->msg.user + ":");
			p.setFont(mf);
			p.setPen(QColor(0xE6, 0xE8, 0xEF));
			// Первая строка — после имени, перенос — с начала строки.
			QString rest = it->msg.text;
			p.drawText(QRectF(box.left() + uw, y, 360 - uw, br.height() + 4), Qt::TextWordWrap, rest);
			p.setOpacity(1);
		}
		p.end();
		return img;
	}

	// Где на текстуре счётчик зрителей и кнопка подписки (для частиц).
	static QPointF viewers_pos() { return QPointF(1050, 735); }
	static QPointF follow_pos() { return QPointF(250, 840); }
	static QPointF cam_pos() { return QPointF(155, 520); }

private:
	const Script &s_;
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
	Director(bool ru) : ru_(ru), s_(script_for(ru)), chrome_(chrome_for(ru)), stream_(s_)
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
		// Тряска камеры: нарастает перед дропом, удар на дропе.
		const double shake = 6 * ease_in(ramp(t, T_CLICK, T_DROP)) * (t < T_DROP ? 1 : 0) +
				     14 * std::exp(-(t - T_DROP) * 6) * (t >= T_DROP ? 1 : 0) +
				     12 * std::exp(-(t - T_DROP2) * 6) * (t >= T_DROP2 ? 1 : 0);
		if (shake > 0.2)
			p.translate((hash01((int)(t * 60)) - 0.5) * shake, (hash01((int)(t * 60) + 99) - 0.5) * shake);
		background(p, t);

		if (t < T_STREAM)
			scene_intro(p, t);
		else if (t < T_GROOVE + 0.35)
			scene_before(p, t);
		if (t >= T_GROOVE && t < T_AFTER)
			scene_window(p, t);
		if (t >= T_AFTER && t < T_FEAT + 0.3)
			scene_after(p, t);
		if (t >= T_FEAT && t < T_LANG + 0.2)
			scene_features(p, t);
		if (t >= T_LANG && t < T_BREAK + 0.2)
			scene_languages(p, t);
		if (t >= T_BREAK && t < T_DROP2 + 0.25)
			scene_update(p, t);
		if (t >= T_DROP2)
			scene_finale(p, t);

		parts_.draw(p);
		draw_ripples(p, t);
		if (cursor_on_)
			draw_cursor(p, cursor_);

		p.resetTransform();
		p.drawImage(0, 0, g_assets->vignette);
		// Вспышки на дропах и в конце.
		double flash = 0.85 * std::exp(-(t - T_DROP) * 7) * (t >= T_DROP) + 0.7 * std::exp(-(t - T_DROP2) * 7) * (t >= T_DROP2) +
			       0.45 * std::exp(-(t - T_STAB1) * 9) * (t >= T_STAB1) + 0.45 * std::exp(-(t - T_STAB2) * 9) * (t >= T_STAB2) +
			       0.5 * std::exp(-(t - T_AFTER) * 10) * (t >= T_AFTER) + 0.25 * ease_in(ramp(t, T_DROP - 0.5, T_DROP)) * (t < T_DROP);
		if (flash > 0.01)
			p.fillRect(canvas.rect(), QColor(255, 255, 255, (int)(255 * clamp01(flash))));
		const double black = std::max(1 - ramp(t, 0, 0.5), ramp(t, T_END - 0.45, T_END - 0.05));
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
		card.fill(Qt::transparent);
		QPainter p(&card);
		p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
		QPainterPath clip;
		clip.addRoundedRect(QRectF(card.rect()), 12 * d, 12 * d);
		p.setClipPath(clip);
		p.fillRect(card.rect(), QColor(0x1D, 0x1F, 0x26));
		p.fillRect(QRectF(0, 0, card.width(), kTitleH * d), QColor(0x15, 0x17, 0x1D));
		p.setFont(ui_font(14 * d));
		p.setPen(QColor(0xC9, 0xCD, 0xD6));
		p.drawText(QRectF(14 * d, 0, card.width() * 0.7, kTitleH * d), Qt::AlignVCenter, chrome_.title);
		p.setPen(QPen(QColor(0x8A, 0x8F, 0x9C), 1.3 * d));
		const double cy = kTitleH * d / 2, rx = card.width() - 22 * d;
		p.drawLine(QPointF(rx - 5 * d, cy - 5 * d), QPointF(rx + 5 * d, cy + 5 * d));
		p.drawLine(QPointF(rx - 5 * d, cy + 5 * d), QPointF(rx + 5 * d, cy - 5 * d));
		p.drawRect(QRectF(rx - 45 * d, cy - 5 * d, 10 * d, 10 * d));
		p.drawLine(QPointF(rx - 82 * d, cy), QPointF(rx - 72 * d, cy));
		p.drawImage(QRectF(0, kTitleH * d, kWin.width() * d, kWin.height() * d), grab);
		p.setClipping(false);
		p.setPen(QPen(QColor(255, 255, 255, 34), 1.5 * d));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(card.rect()).adjusted(1, 1, -1, -1), 12 * d, 12 * d);
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

	QTransform draw_window(QPainter &p, const Pose &pose, double glow = 0.4)
	{
		const QImage img = card_image(win_.win);
		card_tx_ = draw_card(p, img, kCard * kCardScale, pose, glow);
		pose_ = pose;
		return card_tx_;
	}

	// ----- языки, окно обновления, карточки функций
	void build_languages()
	{
		for (const Lang &l : kLangs) {
			if (locale_ == l.code)
				continue;
			lang_order_.push_back(l);
		}
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

	void build_features()
	{
		for (size_t i = 0; i < s_.features.size(); i++) {
			const Feature &f = s_.features[i];
			QImage img(1300, 460, QImage::Format_ARGB32_Premultiplied);
			img.fill(Qt::transparent);
			QPainter p(&img);
			p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
			const QRectF r(10, 10, 1280, 440);
			p.setPen(QPen(QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 150), 4));
			p.setBrush(QColor(0x10, 0x12, 0x1B, 238));
			p.drawRoundedRect(r, 40, 40);
			p.setFont(ui_font(170));
			p.setPen(Qt::white);
			p.drawText(QRectF(40, 10, 300, 440), Qt::AlignCenter, f.icon);
			p.setFont(ui_font(28, QFont::DemiBold));
			p.setPen(kAccent.lighter(130));
			p.drawText(QRectF(360, 70, 860, 50), Qt::AlignLeft | Qt::AlignVCenter,
				   QStringLiteral("%1 / %2").arg(i + 1, 2, 10, QChar('0')).arg(s_.features.size(), 2, 10, QChar('0')));
			p.setFont(ui_font(96, QFont::Bold));
			p.setPen(Qt::white);
			p.drawText(QRectF(360, 120, 900, 130), Qt::AlignLeft | Qt::AlignVCenter, f.title);
			p.setFont(ui_font(46));
			p.setPen(QColor(0xB5, 0xBB, 0xCB));
			p.drawText(QRectF(360, 255, 890, 170), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, f.desc);
			feature_cards_.push_back(img);
		}
	}

	// ----- курсор и «клики»
	void move_cursor(QPointF from, QPointF to, double k)
	{
		cursor_on_ = true;
		cursor_ = from + (to - from) * smooth(k);
	}

	void click(QPointF at, QColor c = kAccent)
	{
		ripples_.push_back({at, t_, c});
		parts_.burst(at, 18, 120, 420, {c, Qt::white}, 1, 0.5, 9, 0);
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

	void draw_ripples(QPainter &p, double t)
	{
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		for (const Ripple &r : ripples_) {
			const double k = (t - r.t0) / 0.55;
			if (k < 0 || k > 1)
				continue;
			QColor c = r.c;
			c.setAlphaF(1 - k);
			p.setPen(QPen(c, 6 * (1 - k) + 1));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(r.at, 10 + 70 * ease_out(k), 10 + 70 * ease_out(k));
		}
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}

	void shockwave(QPainter &p, QPointF at, double t0, double t, QColor c, double max_r = 1300)
	{
		const double k = (t - t0) / 0.9;
		if (k < 0 || k > 1)
			return;
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		for (int i = 0; i < 3; i++) {
			QColor cc = i == 1 ? Qt::white : c;
			cc.setAlphaF((1 - k) * (0.5 - i * 0.12));
			const double r = max_r * ease_out(k) - i * 26;
			if (r > 0) {
				p.setPen(QPen(cc, 22 * (1 - k) + 2));
				p.setBrush(Qt::NoBrush);
				p.drawEllipse(at, r, r * 0.92);
			}
		}
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}

	// Затемнить всё, кроме области, и обвести её светом.
	void spotlight(QPainter &p, const QPolygonF &q, double k, QColor c = kAccent)
	{
		if (k <= 0)
			return;
		QPainterPath outside;
		outside.addRect(-50, -50, kW + 100, kH + 100);
		QPainterPath in;
		in.addPolygon(q);
		in.closeSubpath();
		p.fillPath(outside.subtracted(in), QColor(0, 0, 0, (int)(150 * k)));
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		p.setBrush(Qt::NoBrush);
		for (int g = 0; g < 3; g++) {
			QColor cc = c;
			cc.setAlphaF(k * (0.14 + g * 0.22));
			p.setPen(QPen(cc, 16 - g * 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			p.drawPolygon(q);
		}
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}

	void callout(QPainter &p, const QPolygonF &q, const QString &text, double k, bool above = true)
	{
		if (k <= 0)
			return;
		const QRectF b = q.boundingRect();
		const QPointF anchor = above ? QPointF(b.right() - 40, b.top()) : QPointF(b.right() - 40, b.bottom());
		const QPointF label = anchor + QPointF(60, above ? -70 : 70);
		p.setPen(QPen(QColor(255, 255, 255, (int)(200 * k)), 3));
		p.drawLine(anchor, anchor + (label - anchor) * ease_out(k));
		draw_glow(p, anchor, 40, kAccent, k);
		pill(p, label, text, 32, QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 235), Qt::white,
		     0.7 + 0.3 * ease_out_back(k), clamp01(k * 2));
	}

	// ----- фон
	void background(QPainter &p, double t)
	{
		p.fillRect(QRectF(-50, -50, kW + 100, kH + 100), QColor(0x05, 0x06, 0x0B));
		const double e = energy(t), pl = pulse(t);
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		draw_glow(p, QPointF(430 + 80 * std::sin(t * 0.31), 330 + 60 * std::cos(t * 0.23)), 1500, QColor(0x1D, 0x3F, 0xB8),
			  0.22 + 0.25 * e + 0.25 * pl * e);
		draw_glow(p, QPointF(1500 + 90 * std::cos(t * 0.27), 760 + 50 * std::sin(t * 0.35)), 1500,
			  QColor(0x5A, 0x22, 0xB0), 0.18 + 0.25 * e + 0.25 * pl * e);
		// Пол-сетка в перспективе
		const double hy = 700;
		QColor gc = kAccent;
		gc.setAlphaF(0.05 + 0.12 * e + 0.15 * pl * e);
		p.setPen(QPen(gc, 1.5));
		for (int i = -24; i <= 24; i++)
			p.drawLine(QPointF(kW / 2.0 + i * 40, hy), QPointF(kW / 2.0 + i * 520, kH + 40));
		const double speed = 0.6 + 1.6 * e;
		for (int k = 0; k < 14; k++) {
			const double z = 14 - std::fmod(k + t * speed, 14.0);
			const double y = hy + (kH + 40 - hy) * (1.0 / z);
			QColor c = gc;
			c.setAlphaF(gc.alphaF() * clamp01(1.3 - z / 14));
			p.setPen(QPen(c, 1.5));
			p.drawLine(QPointF(-50, y), QPointF(kW + 50, y));
		}
		// Пылинки
		for (int i = 0; i < 70; i++) {
			const double x = std::fmod(hash01(i) * kW + t * (8 + 30 * hash01(i + 1)), (double)kW);
			const double y = std::fmod(hash01(i + 2) * kH - t * (10 + 25 * hash01(i + 3)) + kH * 4, (double)kH);
			draw_glow(p, QPointF(x, y), 10 + 14 * hash01(i + 4), i % 3 ? kAccent : kViolet, 0.25 + 0.4 * hash01(i + 5));
		}
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	}

	// Тёмная полоса под заголовками вверху экрана.
	void top_band(QPainter &p, double k)
	{
		if (k <= 0)
			return;
		QLinearGradient g(0, 0, 0, 260);
		g.setColorAt(0, QColor(0, 0, 0, (int)(220 * k)));
		g.setColorAt(1, QColor(0, 0, 0, 0));
		p.fillRect(QRectF(-50, -50, kW + 100, 310), g);
	}

	// ----- сцены
	void draw_mic(QPainter &p, QPointF c, double s, double glow)
	{
		QPainterPath mic;
		mic.addRoundedRect(QRectF(c.x() - 34 * s, c.y() - 90 * s, 68 * s, 120 * s), 34 * s, 34 * s);
		QPainterPath stand;
		stand.moveTo(c.x() - 58 * s, c.y() - 10 * s);
		stand.cubicTo(c.x() - 58 * s, c.y() + 60 * s, c.x() + 58 * s, c.y() + 60 * s, c.x() + 58 * s, c.y() - 10 * s);
		stand.moveTo(c.x(), c.y() + 48 * s);
		stand.lineTo(c.x(), c.y() + 90 * s);
		stand.moveTo(c.x() - 34 * s, c.y() + 92 * s);
		stand.lineTo(c.x() + 34 * s, c.y() + 92 * s);
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		draw_glow(p, c, 520 * s, kAccent, 0.5 * glow);
		for (int g = 0; g < 3; g++) {
			QColor cc = kAccent;
			cc.setAlphaF(0.2 + g * 0.15);
			p.setPen(QPen(cc, (26 - g * 8) * s, Qt::SolidLine, Qt::RoundCap));
			p.setBrush(Qt::NoBrush);
			p.drawPath(stand);
			p.drawPath(mic);
		}
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);
		p.setPen(QPen(Qt::white, 9 * s, Qt::SolidLine, Qt::RoundCap));
		p.setBrush(Qt::NoBrush);
		p.drawPath(stand);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawPath(mic);
		p.setPen(QPen(QColor(0x1C, 0x28, 0x48), 4 * s));
		for (int i = 0; i < 4; i++)
			p.drawLine(QPointF(c.x() - 18 * s, c.y() - 66 * s + i * 18 * s), QPointF(c.x() + 18 * s, c.y() - 66 * s + i * 18 * s));
	}

	void scene_intro(QPainter &p, double t)
	{
		const double out = ramp(t, T_STREAM - 0.45, T_STREAM);
		const double zoom = 1 + 2.5 * ease_in(out);
		p.save();
		p.translate(kW / 2.0, kH / 2.0 - 40);
		p.scale(zoom, zoom);
		p.translate(-kW / 2.0, -(kH / 2.0 - 40));
		p.setOpacity(1 - out);
		// Кольца на долях
		for (int k = 0; k < 8; k++) {
			const double age = t - bt(k);
			if (age < 0 || age > 1.4)
				continue;
			p.setCompositionMode(QPainter::CompositionMode_Plus);
			QColor c = k % 2 ? kViolet : kAccent;
			c.setAlphaF(0.5 * (1 - age / 1.4) * (1 - out));
			p.setPen(QPen(c, 3));
			p.setBrush(Qt::NoBrush);
			const double r = 120 + age * 420;
			p.drawEllipse(QPointF(kW / 2.0, 360), r, r);
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
		}
		const double appear = ease_out_back(ramp(t, 0.3, 1.1));
		p.setOpacity((1 - out) * clamp01(t / 0.6));
		draw_mic(p, QPointF(kW / 2.0, 380), 1.15 * appear, 0.6 + 0.6 * pulse(t, 5));
		p.setOpacity(1);
		headline(p, s_.title, QPointF(kW / 2.0, 640), 118, t - 1.0, out, QFont::Bold);
		headline(p, s_.subtitle, QPointF(kW / 2.0, 730), 50, t - 1.7, out, QFont::Normal, QColor(0xC8, 0xCD, 0xD9),
			 QColor(0, 0, 0, 0));
		headline(p, s_.tagline, QPointF(kW / 2.0, 800), 34, t - 2.3, out, QFont::Normal, kAccent.lighter(140),
			 QColor(0, 0, 0, 0));
		p.restore();
	}

	double viewers_at(double t) const
	{
		if (t < T_AFTER)
			return std::max(19.0, 34 - (t - T_STREAM) * 3.4);
		return 21 + (2847 - 21) * ease_out_expo((t - T_AFTER - 0.25) / 2.6);
	}

	void scene_before(QPainter &p, double t)
	{
		const double in = ramp(t, T_STREAM, T_STREAM + 0.6);
		const double out = ramp(t, T_GROOVE, T_GROOVE + 0.35);
		while (chat_before_n_ < s_.chat_before.size() && t >= bt(9 + chat_before_n_ * 1.5)) {
			chat_.push_back({s_.chat_before[chat_before_n_], bt(9 + chat_before_n_ * 1.5)});
			chat_before_n_++;
		}
		Pose a{kW / 2.0 + 700, kH / 2.0 + 60, 900, 4, -38, 0, 0.82, 0};
		Pose b{kW / 2.0, kH / 2.0 + 70, 0, 4, -7 + 3 * std::sin(t * 0.8), 0, 0.88, 1};
		Pose c{kW / 2.0 - 900, kH / 2.0 + 70, 300, 0, 40, 0, 0.8, 0};
		Pose pose = out > 0 ? mix(b, c, ease_in(out)) : mix(a, b, ease_out_back(in));
		// Плохой звук: картинку иногда дёргает, как от помех.
		const int glitch = (int)(t * 15);
		if (in >= 1 && hash01(glitch * 3) > 0.8)
			pose.cx += (hash01(glitch * 3 + 1) - 0.5) * 26;
		const QImage img = stream_.render(t, 0, viewers_at(t), chat_, 0);
		const QTransform tx = draw_card(p, img, QSizeF(StreamView::TW, StreamView::TH), pose, 0.5, kBad);
		stream_tx_ = tx;
		// Зрители уходят: «−1» падает со счётчика на каждую долю.
		const int beat = (int)std::floor((t - kB0) / kBeat);
		if (beat != last_before_beat_ && in >= 1 && out <= 0) {
			last_before_beat_ = beat;
			parts_.glyphs(tx.map(StreamView::viewers_pos()), 2, {"−1", "👋", "😴"}, 60, -140, 1.2, 34);
		}
		top_band(p, 1 - out);
		headline(p, s_.before_head, QPointF(kW / 2.0, 110), 74, t - T_STREAM - 0.4, out, QFont::Bold, Qt::white, kBad);
		headline(p, s_.before_sub, QPointF(kW / 2.0, 172), 40, t - bt(12), out, QFont::Normal, QColor(0xFF, 0xB0, 0xB3),
			 QColor(0, 0, 0, 0));
	}

	void scene_after(QPainter &p, double t)
	{
		const double in = ramp(t, T_AFTER, T_AFTER + 0.45);
		const double out = ramp(t, T_FEAT, T_FEAT + 0.3);
		if (!after_started_) {
			after_started_ = true;
			chat_.clear();
		}
		while (chat_after_n_ < s_.chat_after.size() && t >= T_AFTER + 0.3 + chat_after_n_ * kBeat * 0.5) {
			chat_.push_back({s_.chat_after[chat_after_n_], T_AFTER + 0.3 + chat_after_n_ * kBeat * 0.5});
			chat_after_n_++;
		}
		Pose a{kW / 2.0, kH / 2.0 + 70, -500, 0, 0, 0, 0.9, 0};
		Pose b{kW / 2.0, kH / 2.0 + 70, 0, 3, 6 * std::sin((t - T_AFTER) * 0.9), 0, 0.88, 1};
		Pose c{kW / 2.0 + 1000, kH / 2.0 + 70, 400, 0, -45, 0, 0.8, 0};
		Pose pose = out > 0 ? mix(b, c, ease_in(out)) : mix(a, b, ease_out(in));
		const double follow = 0.5 + 0.5 * std::sin(t * 9);
		const QImage img = stream_.render(t, 1, viewers_at(t), chat_, follow);
		const QTransform tx = draw_card(p, img, QSizeF(StreamView::TW, StreamView::TH), pose, 0.7 + 0.6 * pulse(t), kGood);
		stream_tx_ = tx;
		// Зрители «прилетают»: значки и сердечки на каждую долю.
		const int beat = (int)std::floor((t - kB0) / kBeat);
		if (beat != last_after_beat_ && out <= 0) {
			last_after_beat_ = beat;
			parts_.glyphs(tx.map(StreamView::viewers_pos()), 4, {"👤", "+1", "❤️", "🔥"}, 90, 260, 1.4, 38);
			parts_.glyphs(tx.map(StreamView::follow_pos()), 3, {"❤️", "💜", "⭐"}, 80, 220, 1.3, 34);
			parts_.burst(tx.map(StreamView::viewers_pos()), 16, 200, 600, {kGood, Qt::white, kGold}, 1, 0.6, 10, 200);
		}
		top_band(p, 1 - out);
		headline(p, s_.after_head, QPointF(kW / 2.0, 110), 80, t - T_AFTER - 0.2, out, QFont::Bold, Qt::white, kGood);
		headline(p, s_.after_sub, QPointF(kW / 2.0, 172), 40, t - T_AFTER - 0.8, out, QFont::Normal, QColor(0xB4, 0xFF, 0xE0),
			 QColor(0, 0, 0, 0));
	}

	// Окно фильтра: влёт, экскурсия, перетаскивание точек, «Умная настройка» и дроп.
	void scene_window(QPainter &p, double t)
	{
		const double in = ramp(t, T_GROOVE, T_GROOVE + 0.75);
		const QRectF meters = card_rect(block_).adjusted(-6, -4, 6, 0);
		const QRectF meters_rows(meters.left(), meters.top(), meters.width(), 52);
		const QRectF status(meters.left(), meters.top() + 52, meters.width(), 26);
		const QRectF eqr = card_rect(eq_).adjusted(-6, -6, 6, 6);
		const QPointF eq_c = eqr.center();

		// Камера по долям: общий план → уровни → ИИ → эквалайзер → кнопка
		Pose wide = focus(QPointF(kCard.width() / 2, kCard.height() / 2), 1.0, 4, -9);
		wide.cy += 40;
		Pose start = wide;
		start.z = 1500;
		start.ry = 38;
		start.rx = 12;
		start.opacity = 0;
		Pose on_meters = focus(meters.center() + QPointF(-40, 80), 1.45, 3, -6);
		Pose on_eq = focus(eq_c + QPointF(0, 30), 1.55, 2, -4);
		QPushButton *btn = win_.calib;
		// Кнопка «Умная настройка»: прокручиваем окно, чтобы она была на виду.
		const double scroll_k = smooth(ramp(t, T_RISE - 0.3, T_RISE + 0.5));
		win_.scroll->verticalScrollBar()->setValue((int)(170 * scroll_k * (t < T_DROP ? 1 : 1 - smooth(ramp(t, T_DROP, T_DROP + 0.22)))));
		const QRectF btnr = card_rect(btn).adjusted(-6, -6, 6, 6);
		Pose on_btn = focus(btnr.center() + QPointF(0, -60), 1.75, 2, -3);
		Pose drop_pose = focus(eq_c + QPointF(0, 60), 1.3, 3, -6);

		Pose pose;
		if (t < bt(18))
			pose = mix(start, wide, ease_out_back(in * 1.0));
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

		// --- взаимодействие (до отрисовки, чтобы кадр показал результат)
		interact(t);

		draw_window(p, pose, 0.35 + 0.5 * pulse(t) + (t > T_DROP ? 0.6 * std::exp(-(t - T_DROP) * 3) : 0));

		// Подсветка частей окна
		const double hl_levels = ramp(t, bt(18.5), bt(19)) * (1 - ramp(t, bt(19.8), bt(20.2)));
		const double hl_ai = ramp(t, bt(20), bt(20.4)) * (1 - ramp(t, bt(21), bt(21.4)));
		const double hl_eq = ramp(t, bt(21.6), bt(22)) * (1 - ramp(t, bt(22.4), bt(22.8)));
		spotlight(p, map_rect(meters_rows), hl_levels);
		callout(p, map_rect(meters_rows), s_.call_levels, hl_levels);
		spotlight(p, map_rect(status), hl_ai, kGood);
		callout(p, map_rect(status), s_.call_ai, hl_ai, false);
		spotlight(p, map_rect(eqr), hl_eq);
		callout(p, map_rect(eqr), s_.call_spectrum, hl_eq);
		const double hl_drag = ramp(t, bt(23), bt(23.3)) * (1 - ramp(t, T_RISE - 0.2, T_RISE));
		callout(p, map_rect(eqr), s_.call_drag, hl_drag);

		// «Умная настройка»: подсветка кнопки, зарядка, отсчёт
		const QPolygonF bq = map_rect(btnr);
		const QPointF bc = bq.boundingRect().center();
		if (t >= T_RISE && t < T_DROP) {
			const double k = ramp(t, T_RISE + 0.2, T_RISE + 0.5);
			spotlight(p, bq, k * 0.9, kGold);
			const double charge = ramp(t, T_CLICK, T_DROP);
			// Полоса зарядки под кнопкой
			const QRectF b = bq.boundingRect();
			const QRectF bar(b.left(), b.bottom() + 22, b.width(), 10);
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 255, 255, 40));
			p.drawRoundedRect(bar, 5, 5);
			p.setBrush(kGold);
			p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * charge, bar.height()), 5, 5);
			p.setCompositionMode(QPainter::CompositionMode_Plus);
			draw_glow(p, QPointF(bar.left() + bar.width() * charge, bar.center().y()), 90, kGold, charge > 0 ? 0.9 : 0);
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
			if (t > T_CLICK) {
				pill(p, QPointF(b.center().x(), b.bottom() + 80), s_.listening, 30, QColor(0, 0, 0, 200), kGold, 1,
				     ramp(t, T_CLICK, T_CLICK + 0.2));
				// Отсчёт 3-2-1 на последних долях
				for (int i = 0; i < 3; i++) {
					const double a = t - bt(29 + i);
					if (a >= 0 && a < kBeat) {
						const double s = 1.6 - 0.6 * ease_out(a / 0.25);
						p.setOpacity(1 - ramp(a, kBeat * 0.6, kBeat));
						p.setFont(ui_font(220 * s, QFont::Black));
						QPainterPath num;
						const QString n = QString::number(3 - i);
						const QFontMetricsF fm(ui_font(220 * s, QFont::Black));
						num.addText(QPointF(kW - 330 - fm.horizontalAdvance(n) / 2, kH / 2.0 + fm.ascent() / 2.6),
							    ui_font(220 * s, QFont::Black), n);
						p.setCompositionMode(QPainter::CompositionMode_Plus);
						p.strokePath(num, QPen(QColor(kGold.red(), kGold.green(), kGold.blue(), 90), 24));
						p.setCompositionMode(QPainter::CompositionMode_SourceOver);
						p.fillPath(num, Qt::white);
						p.setOpacity(1);
					}
				}
			}
		}
		top_band(p, 1 - ramp(t, T_AFTER - 0.2, T_AFTER));
		// Заголовки
		if (t < T_RISE)
			headline(p, s_.groove_head, QPointF(kW / 2.0, 110), 70, t - T_GROOVE - 0.3, ramp(t, T_RISE - 0.35, T_RISE),
				 QFont::Bold);
		else if (t < T_DROP)
			headline(p, s_.rise_head, QPointF(kW / 2.0, 110), 80, t - T_RISE - 0.1, ramp(t, T_DROP - 0.15, T_DROP),
				 QFont::Bold, Qt::white, kGold);
		if (t >= T_RISE && t < T_DROP)
			headline(p, s_.rise_sub, QPointF(kW / 2.0, 172), 40, t - T_RISE - 0.5, ramp(t, T_DROP - 0.15, T_DROP),
				 QFont::Normal, QColor(0xFF, 0xE2, 0xA0), QColor(0, 0, 0, 0));
		if (t >= T_DROP) {
			shockwave(p, bc, T_DROP, t, kGold);
			const double a = t - T_DROP;
			const double s = 0.6 + 0.4 * ease_out_back(a / 0.35);
			pill(p, QPointF(kW / 2.0, 170), QStringLiteral("✅ ") + s_.done, 64, QColor(0x12, 0x8F, 0x6A, 235), Qt::white, s,
			     ramp(a, 0, 0.1) * (1 - ramp(t, T_AFTER - 0.25, T_AFTER)));
		}

		// Частицы на дропе
		if (t >= T_DROP && !drop_burst_) {
			drop_burst_ = true;
			parts_.burst(bc, 160, 300, 1500, {kGold, Qt::white, kAccent, kViolet}, 1, 1.1, 14, 500);
			parts_.burst(bc, 50, 100, 700, {kGold, kAccent}, 0, 1.2, 16, 120);
			parts_.burst(bc, 60, 200, 900, {kGold, kGood, kAccent, kViolet, QColor(0xFF, 0x6E, 0xC7)}, 3, 1.6, 16, 700);
		}
		if (t >= T_CLICK && t < T_DROP && frame_no_ % 2 == 0)
			parts_.implode(bc, 4 + (int)(10 * ramp(t, T_CLICK, T_DROP)), 520, frame_no_ % 4 ? kGold : Qt::white);
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
			parts_.burst(cursor_, 3, 60, 240, {kAccent, Qt::white}, 1, 0.45, 8, 120);
			if (k > kBeat * 0.8 && !drag_released_[d.node]) {
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
			click(map_pt(btnr.center() + QPointF(60, 4)), kGold);
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
			const std::map<std::string, double> target = {{kLowGain, 3.0},      {kMudGain, -3.5},     {kNasalGain, -2.5},
								       {kPresGain, 4.5},     {kAirGain, 3.5},      {kDenoiseAmount, 90},
								       {kCompAmount, 65}};
			for (auto &kv : target)
				link_->set_double(kv.first.c_str(), lerp(morph_from_[kv.first], kv.second, k));
		}
	}

	void scene_features(QPainter &p, double t)
	{
		const double in = ramp(t, T_FEAT, T_FEAT + 0.5);
		const double out = ramp(t, T_LANG - 0.1, T_LANG + 0.2);
		const int i = std::clamp((int)std::floor((t - T_FEAT) / kBeat), 0, 7);
		QGroupBox *g = win_.groups[(size_t)i];
		// Прокрутка к разделу i
		auto target_of = [&](int gi) {
			return std::max(0, win_.groups[(size_t)gi]->y() - 40);
		};
		const double u = t - bt(40 + i);
		const double from = i == 0 ? target_of(0) : target_of(i - 1);
		win_.scroll->verticalScrollBar()->setValue((int)lerp(from, target_of(i), ease_out(u / 0.3)));
		// Ползунок раздела двигается
		static const std::vector<std::pair<int, std::pair<double, double>>> moves = {
			{0, {20, 100}}, {1, {-50, -12}}, {5, {-3, 6}}, {7, {0, 70}}, {8, {10, 75}}, {9, {0, 60}}, {10, {0, -5}}, {12, {-6, -1}}};
		const auto &mv = moves[(size_t)i];
		SliderRow &row = win_.sliders[(size_t)mv.first];
		const double v = lerp(mv.second.first, mv.second.second, ease_out(ramp(u, 0.05, 0.4)));
		link_->set_double(row.key, v);
		step_ui(0);

		Pose a{-300, kH / 2.0 + 40, 900, 4, 40, 0, 0.95, 0};
		Pose b{610, kH / 2.0 + 60, 0, 3, 16, 0, 1.02, 1};
		Pose c{720, kH / 2.0 + 40, 1600, 3, 30, 0, 1.0, 0};
		Pose pose = out > 0 ? mix(b, c, ease_in(out)) : mix(a, b, ease_out(in));
		pose.scale *= 1 + 0.015 * pulse(t);
		draw_window(p, pose, 0.3 + 0.4 * pulse(t));
		const QRectF gr = card_rect(g).adjusted(-6, -4, 6, 6);
		spotlight(p, map_rect(gr), (1 - out) * ramp(t, T_FEAT + 0.1, T_FEAT + 0.3), kAccent);
		// Искры у ручки ползунка
		const double frac = (v - row.min) / (row.max - row.min);
		const QRectF sr = card_rect(row.slider);
		const QPointF handle = map_pt(QPointF(sr.left() + 7 + frac * (sr.width() - 14), sr.center().y()));
		if (u > 0.05 && u < 0.4)
			parts_.burst(handle, 5, 80, 380, {kAccent, Qt::white, kViolet}, 1, 0.5, 9, 200);
		p.setCompositionMode(QPainter::CompositionMode_Plus);
		draw_glow(p, handle, 70, kAccent, (1 - out) * 0.9);
		p.setCompositionMode(QPainter::CompositionMode_SourceOver);

		// Карточка функции: влетает на каждую долю
		if (out < 1) {
			const QImage &card = feature_cards_[(size_t)i];
			const double k = ease_out_back(u / 0.28);
			Pose cp{1470, 600, (1 - k) * -500, 0, -14 + (1 - k) * -20, (1 - k) * 4, 1.0, clamp01(u / 0.1) * (1 - out)};
			draw_card(p, card, QSizeF(800, 283), cp, 0.8 * (1 - ramp(u, 0, 0.5)) + 0.25, kAccent);
			if (feat_beat_ != i) {
				feat_beat_ = i;
				parts_.burst(QPointF(1470 - 390, 600), 30, 300, 900, {kAccent, kViolet, Qt::white}, 1, 0.6, 10, 100);
				parts_.burst(QPointF(1470 + 390, 600), 30, 300, 900, {kAccent, kViolet, Qt::white}, 1, 0.6, 10, 100);
			}
		}
		top_band(p, 1 - out);
		headline(p, s_.feat_head, QPointF(kW / 2.0, 110), 72, t - T_FEAT - 0.1, out, QFont::Bold);
	}

	void scene_languages(QPainter &p, double t)
	{
		const double u = t - T_LANG;
		const double out = ramp(t, T_BREAK - 0.1, T_BREAK + 0.2);
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
			double a = base + i * step_deg;
			a = std::remainder(a, 360.0);
			const double rad = a * M_PI / 180;
			if (std::cos(rad) < 0.05)
				continue;
			items.push_back({i, a, R * (1 - std::cos(rad))});
		}
		std::sort(items.begin(), items.end(), [](const Item &x, const Item &y) { return x.z > y.z; });
		const double in = ease_out(ramp(u, 0, 0.5));
		int front = -1;
		double best = 1e9;
		for (const Item &it : items) {
			const double rad = it.a * M_PI / 180;
			Pose pose;
			pose.cx = kW / 2.0 + R * std::sin(rad);
			pose.cy = kH / 2.0 + 70;
			pose.z = it.z + (1 - in) * 1200 + ease_in(out) * 1500;
			pose.ry = -it.a;
			pose.scale = 0.72;
			pose.opacity = (0.35 + 0.65 * std::pow(std::cos(rad), 3)) * in * (1 - out);
			const bool is_front = std::abs(it.a) < step_deg / 2;
			draw_card(p, lang_cards_[(size_t)it.i], kCard * kCardScale, pose, is_front ? 0.9 + 0.6 * pulse(t) : 0.15);
			if (std::abs(it.a) < best) {
				best = std::abs(it.a);
				front = it.i;
			}
		}
		if (front != lang_front_) {
			lang_front_ = front;
			lang_change_t_ = t;
			if (u > 0.2)
				parts_.burst(QPointF(kW / 2.0, 950), 24, 200, 700, {kAccent, kViolet, Qt::white}, 1, 0.5, 9, 0);
		}
		top_band(p, 1 - out);
		headline(p, s_.lang_head, QPointF(kW / 2.0, 110), 80, u - 0.1, out, QFont::Bold);
		headline(p, s_.lang_sub, QPointF(kW / 2.0, 172), 38, u - 0.6, out, QFont::Normal, QColor(0xC8, 0xCD, 0xD9),
			 QColor(0, 0, 0, 0));
		if (front >= 0) {
			const double k = ease_out_back((t - lang_change_t_) / 0.25);
			pill(p, QPointF(kW / 2.0, 990), QString::fromUtf8(lang_order_[(size_t)front].name), 48,
			     QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 230), Qt::white, 0.75 + 0.25 * k, (1 - out) * in);
		}
	}

	void progress_card(QPainter &p, double t, const Pose &pose)
	{
		QImage img(1200, 520, QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		QPainter q(&img);
		q.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
		q.setPen(QPen(QColor(255, 255, 255, 40), 3));
		q.setBrush(QColor(0x1D, 0x1F, 0x26));
		q.drawRoundedRect(QRectF(4, 4, 1192, 512), 28, 28);
		const double prog = ease_out(ramp(t, bt(60.2), bt(62)));
		const bool done = t >= bt(62.8);
		q.setFont(ui_font(40, QFont::DemiBold));
		q.setPen(Qt::white);
		q.drawText(QRectF(60, 40, 1080, 70), Qt::AlignVCenter,
			   done ? QStringLiteral("✅ ") + s_.done + QStringLiteral("  1.4.0")
				: link_->tr("Update.Downloading"));
		const QRectF bar(60, 140, 1080, 26);
		q.setPen(Qt::NoPen);
		q.setBrush(QColor(255, 255, 255, 30));
		q.drawRoundedRect(bar, 13, 13);
		q.setBrush(done ? kGood : kAccent);
		q.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * prog, bar.height()), 13, 13);
		q.setFont(ui_font(34));
		const double c1 = ramp(t, bt(62), bt(62.3)), c2 = ramp(t, bt(62.4), bt(62.7));
		q.setOpacity(c1);
		q.setPen(kGood);
		q.drawText(QRectF(60, 220, 1080, 60), Qt::AlignVCenter, QStringLiteral("🛡  ") + s_.upd_sig + QStringLiteral("  ✓"));
		q.setOpacity(c2);
		q.drawText(QRectF(60, 300, 1080, 60), Qt::AlignVCenter, QStringLiteral("🔒  ") + s_.upd_sha + QStringLiteral("  ✓"));
		q.setOpacity(ramp(t, bt(62.8), bt(63.1)));
		q.setPen(QColor(0xC8, 0xCD, 0xD9));
		q.setFont(ui_font(30));
		q.drawText(QRectF(60, 380, 1080, 110), Qt::AlignVCenter | Qt::TextWordWrap,
			   link_->tr("Update.Done").arg("1.4.0"));
		q.end();
		const QTransform tx = draw_card(p, img, QSizeF(1000, 433), pose, done ? 0.9 : 0.5, done ? kGood : kAccent);
		// Поток частиц по полосе
		if (prog > 0 && prog < 1)
			parts_.burst(tx.map(QPointF(60 + 1080 * prog, 153)), 4, 100, 420, {kAccent, Qt::white}, 1, 0.45, 8, 0);
		if (done && !upd_done_burst_) {
			upd_done_burst_ = true;
			parts_.burst(tx.map(QPointF(600, 260)), 90, 300, 1100, {kGood, Qt::white, kAccent}, 1, 0.9, 12, 300);
		}
	}

	void scene_update(QPainter &p, double t)
	{
		const double in = ramp(t, T_BREAK, T_BREAK + 0.6);
		const double out = ramp(t, T_DROP2 - 0.2, T_DROP2 + 0.2);
		if (link_->badge.isEmpty() && t > T_BREAK + 0.3)
			link_->badge = link_->tr("Live.Update").arg("1.4.0");
		win_.scroll->verticalScrollBar()->setValue(0);
		step_ui(0);
		const QRectF status = card_rect(block_);
		const QFontMetricsF fm(vsui::style::font(block_, 0.9));
		const double bw = std::min(fm.horizontalAdvance(link_->badge), status.width() * 0.62);
		const QRectF badge(status.right() - bw - 4, status.top() + 50, bw + 8, 24);

		Pose a = focus(QPointF(kCard.width() / 2, 300), 1.0, 10, -20);
		a.z = 1200;
		a.opacity = 0;
		Pose b = focus(QPointF(badge.center().x() - 120, 230), 1.45, 4, -6);
		Pose c = b;
		c.z = 2600;
		c.ry = 50;
		c.opacity = 0;
		Pose pose = out > 0 ? mix(b, c, ease_in(out)) : mix(a, b, ease_out(in));
		pose.cy += 5 * std::sin(t * 1.1);
		draw_window(p, pose, 0.35);

		const QPolygonF bq = map_rect(badge.adjusted(-6, -6, 6, 6));
		const double hl = ramp(t, bt(57), bt(57.4)) * (1 - ramp(t, bt(58.6), bt(59)));
		spotlight(p, bq, hl, kGold);
		if (t < bt(58.6)) {
			p.setCompositionMode(QPainter::CompositionMode_Plus);
			draw_glow(p, bq.boundingRect().center(), 260 + 60 * std::sin(t * 8), kGold, 0.5 * hl);
			p.setCompositionMode(QPainter::CompositionMode_SourceOver);
		}
		// Курсор: к надписи → клик → к «Обновить» → клик
		const QPointF badge_pt = bq.boundingRect().center();
		const double dlg_flip = ease_out(ramp(t, bt(58.2), bt(58.9)));
		const double to_progress = ease_out(ramp(t, bt(60), bt(60.6)));
		Pose dp{1240, 640, 0, 0, 90 * (1 - dlg_flip) - 80 * to_progress, 0, 1.0, dlg_flip * (1 - ramp(t, bt(60), bt(60.35)))};
		const QSizeF dsize = dialog_size_ * 2.3;
		QTransform dtx;
		if (dp.opacity > 0)
			dtx = draw_card(p, dialog_, dsize, dp, 0.6, kAccent);
		const QPointF install_pt = dtx.map(install_center_ * kDpr);
		if (t >= bt(57) && t < bt(58)) {
			if (upd_from_.isNull())
				upd_from_ = QPointF(kW * 0.7, kH * 0.9);
			move_cursor(upd_from_, badge_pt, ramp(t, bt(57), bt(57.9)));
		}
		if (t >= bt(58) && !badge_clicked_) {
			badge_clicked_ = true;
			click(badge_pt, kGold);
		}
		if (t >= bt(59) && t < bt(60) && dp.opacity > 0.5)
			move_cursor(badge_pt, install_pt, ramp(t, bt(59), bt(59.85)));
		if (t >= bt(60) && !install_clicked_) {
			install_clicked_ = true;
			click(cursor_, kAccent);
		}
		if (t >= bt(60.3))
			cursor_on_ = false;
		if (to_progress > 0) {
			Pose pp{1240, 640, 0, 0, -90 * (1 - to_progress), 0, 1.0, to_progress * (1 - out)};
			progress_card(p, t, pp);
		}
		top_band(p, 1 - out);
		headline(p, s_.upd_head, QPointF(kW / 2.0, 110), 80, t - T_BREAK - 0.2, out, QFont::Bold);
		headline(p, s_.upd_sub, QPointF(kW / 2.0, 172), 38, t - T_BREAK - 0.8, out, QFont::Normal, QColor(0xC8, 0xCD, 0xD9),
			 QColor(0, 0, 0, 0));
	}

	void scene_finale(QPainter &p, double t)
	{
		const double u = t - T_DROP2;
		shockwave(p, QPointF(kW / 2.0, 420), T_DROP2, t, kAccent, 1500);
		if (!finale_burst_) {
			finale_burst_ = true;
			parts_.burst(QPointF(kW / 2.0, 420), 180, 300, 1600, {kAccent, kViolet, Qt::white, kGold}, 1, 1.2, 14, 400);
			parts_.burst(QPointF(kW / 2.0, 420), 80, 200, 1100, {kAccent, kViolet, kGold, kGood, QColor(0xFF, 0x6E, 0xC7)}, 3, 1.8, 18, 600);
		}
		for (double st : {T_STAB1, T_STAB2}) {
			if (t >= st && !stab_done_[st == T_STAB1 ? 0 : 1]) {
				stab_done_[st == T_STAB1 ? 0 : 1] = true;
				parts_.burst(QPointF(kW / 2.0, 640), 120, 300, 1400, {kAccent, kViolet, Qt::white}, 1, 0.9, 12, 300);
			}
		}
		if (frame_no_ % 2 == 0 && u < 3.0)
			parts_.burst(QPointF(kW * hash01(frame_no_), kH + 20), 2, 300, 700, {kAccent, kViolet}, 0, 2.0, 12, -200);

		const double punch = 1 + 0.05 * pulse(t, 9) + 0.06 * std::exp(-(t - T_STAB1) * 8) * (t >= T_STAB1) +
				     0.06 * std::exp(-(t - T_STAB2) * 8) * (t >= T_STAB2);
		p.save();
		p.translate(kW / 2.0, 520);
		p.scale(punch, punch);
		p.translate(-kW / 2.0, -520);
		draw_mic(p, QPointF(kW / 2.0, 300), 0.75 * ease_out_back(u / 0.5), 0.8 + 0.6 * pulse(t));
		headline(p, s_.title, QPointF(kW / 2.0, 560), 132, u - 0.05, 0, QFont::Bold);
		headline(p, s_.fin_sub, QPointF(kW / 2.0, 640), 46, u - 0.35, 0, QFont::Normal, QColor(0xC8, 0xCD, 0xD9), QColor(0, 0, 0, 0));
		headline(p, s_.fin_chips, QPointF(kW / 2.0, 715), 32, u - 0.6, 0, QFont::Normal, kAccent.lighter(140), QColor(0, 0, 0, 0));
		const double cta = ease_out_back((u - 0.9) / 0.35);
		if (cta > 0) {
			pill(p, QPointF(kW / 2.0, 830), QStringLiteral("⬇  ") + s_.fin_cta, 46, QColor(0x2F, 0x6F, 0xF0), Qt::white,
			     0.7 + 0.3 * cta + 0.04 * pulse(t, 9), clamp01(cta * 2));
			pill(p, QPointF(kW / 2.0, 925), QStringLiteral("github.com/") + QStringLiteral(VOICE_STUDIO_UPDATE_REPO), 34,
			     QColor(255, 255, 255, 26), QColor(0xDD, 0xE4, 0xFF), 1, clamp01((u - 1.1) / 0.3));
		}
		p.restore();
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
	QTransform card_tx_, stream_tx_;
	Pose pose_;
	Particles parts_;
	std::vector<Ripple> ripples_;
	std::vector<ChatLine> chat_;
	size_t chat_before_n_ = 0, chat_after_n_ = 0;
	bool after_started_ = false;
	int last_after_beat_ = -1, last_before_beat_ = -1;

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
		if ((t >= T_GROOVE && t < T_AFTER) || (t >= T_FEAT && t < T_LANG + 0.2) || (t >= T_BREAK && t < T_DROP2 + 0.8))
			step_ui(dt);
		(void)t;
	}
};

void pick_font()
{
	const QStringList fams = QFontDatabase::families();
	for (const char *f : {"Segoe UI Variable Display", "Segoe UI"})
		if (fams.contains(QString::fromUtf8(f))) {
			g_family = QString::fromUtf8(f);
			return;
		}
}

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
	pick_font();
	g_data = QString::fromLocal8Bit(argv[1]);
	const bool stills = QByteArray(argv[2]) == "stills";
	const bool ru = QByteArray(stills ? argv[4] : argv[5]) != "en";
	vsui::install_props_injector([](int, QWidget *parent) -> QWidget * {
		auto *b = new vsui::LiveBlock(g_factory_link, parent);
		b->setManualClock(true);
		return b;
	});

	Assets assets;
	assets.shadow = make_shadow();
	assets.vignette = make_vignette();
	g_assets = &assets;
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
