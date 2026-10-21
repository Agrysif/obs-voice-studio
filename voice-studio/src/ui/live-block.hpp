#pragma once

// Живой блок в начале окна фильтра: уровни «Вход/Выход», состояние обработки и кривая
// эквалайзера поверх спектра голоса. Минимализм в цветах темы OBS, без градиентов.

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <vector>

#include "live-link.hpp"

namespace vsui {

class EqCurveView;

class LiveBlock : public QWidget {
public:
	explicit LiveBlock(std::shared_ptr<LiveLink> link, QWidget *parent = nullptr);
	~LiveBlock() override;

	int heightForWidth(int w) const override;
	bool hasHeightForWidth() const override { return true; }
	QSize sizeHint() const override { return QSize(480, heightForWidth(480)); }
	QSize minimumSizeHint() const override { return QSize(260, heightForWidth(260)); }

protected:
	void paintEvent(QPaintEvent *) override;
	void resizeEvent(QResizeEvent *) override;
	void showEvent(QShowEvent *) override;
	void hideEvent(QHideEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;

private:
	struct Bar {
		double rms = -120.0, peak = -120.0, hold = -120.0, hold_t = 0.0;
	};

	int meters_height() const;
	int eq_height(int w) const;
	int hint_height(int w) const;
	void tick();
	void sync();
	void update_bar(Bar &b, double rms, double peak, double dt);
	void paint_bar(QPainter &p, const QRectF &row, const Bar &b, const QString &label, double label_w);

	std::shared_ptr<LiveLink> link_;
	EqCurveView *eq_ = nullptr;
	QTimer timer_;
	QElapsedTimer clock_;
	int ticks_ = 0;
	bool active_ = false;

	Bar in_, out_;
	double gr_ = 0.0, gate_ = 0.0, vad_ = 0.0;
	bool signal_ = false;
	bool enabled_ = true;
	std::vector<float> spectrum_;
	QString hint_;
	QString badge_;     // «⬆ Доступна версия …» в строке состояния
	QRectF badge_rect_; // куда нажимать
};

} // namespace vsui
