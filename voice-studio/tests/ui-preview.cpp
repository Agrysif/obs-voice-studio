// Просмотр живого блока без OBS: макет окна «Фильтры» в цветах темы OBS (Yami)
// с «игрушечными» данными (синтетический голос). Блок встраивается тем же механизмом,
// что и в OBS, — через невидимую строку-метку.
//
//   voice-studio-ui-preview <папка data> show [язык]
//   voice-studio-ui-preview <папка data> shot <out.png> [normal|hover|drag|eqoff|nosignal|update|bench] [язык]
//
// Язык — имя файла перевода без .ini (ru-RU, en-US, kk-KZ, …), по умолчанию ru-RU.
// Демо-ролик — tests/demo-video.cpp.
// Запускать с OBS\bin\64bit в PATH (obs.dll, Qt) и QT_PLUGIN_PATH на ту же папку.

#include <QEventLoop>
#include <QImage>
#include <QTimer>

#include <cstdio>

#include "eq-view.hpp"
#include "preview-mock.hpp"

using namespace vs::key;
using namespace mock;

namespace {

void run_for(int ms)
{
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	if (argc < 3) {
		std::printf("usage: %s <data dir> show [locale] | shot <out.png> [scenario] [locale]\n", argv[0]);
		return 1;
	}
	apply_obs_theme(app);
	const QString data = QString::fromLocal8Bit(argv[1]);
	const QString mode = argv[2];
	const QString locale = mode == "show" ? (argc > 3 ? argv[3] : "ru-RU") : (argc > 5 ? argv[5] : "ru-RU");
	auto link = std::make_shared<MockLink>(data + "/locale/" + locale + ".ini");
	vsui::install_props_injector(
		[link](int, QWidget *parent) -> QWidget * { return new vsui::LiveBlock(link, parent); });

	std::unique_ptr<QWidget> win(build_window(*link, chrome_for(locale.startsWith("ru"))).win);
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
		link->signal = false;
	if (scenario == "eqoff")
		link->set_bool(kEqEnabled, false);
	if (scenario == "update")
		link->badge = link->tr("Live.Update").arg("1.4.0");

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
