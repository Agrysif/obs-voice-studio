#pragma once

// Кривая эквалайзера поверх живого спектра голоса — плоско, в цветах темы OBS.
// Точки полос можно тянуть мышью: влево-вправо — частота, вверх-вниз — усиление,
// колёсико — ширина полосы, двойной щелчок — сброс в 0 дБ.

#include <QImage>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <vector>

namespace vsui {

struct EqState {
	bool eq_on = true;
	bool hpf_on = true;
	double hpf_freq = 80;
	int hpf_slope = 24;
	double low_gain = 0, low_freq = 120;
	double mud_gain = 0, mud_freq = 300, mud_q = 1.0;
	double nasal_gain = 0, nasal_freq = 1000, nasal_q = 1.4;
	double pres_gain = 0, pres_freq = 4000, pres_q = 0.9;
	double air_gain = 0, air_freq = 10000;
	double lpf_freq = 20000;
};

class EqCurveView : public QWidget {
public:
	explicit EqCurveView(QWidget *parent = nullptr);

	void setState(const EqState &s);
	const EqState &state() const { return s_; }
	void setSampleRate(int sr);
	// Спектр в дБ по логарифмическим полосам 20 Гц..20 кГц (любое число полос).
	void setSpectrum(const std::vector<float> &db, double dt);
	// Названия точек: срез гула, глубина, гулкость, гнусавость, чёткость, воздух.
	void setTexts(const QStringList &node_names, const QString &hz, const QString &khz, const QString &db,
		      const QString &off_hint);
	bool interacting() const { return drag_ >= 0; }
	// Где сейчас нарисована точка полосы (0 — срез гула … 5 — воздух); для тестов.
	QPointF nodePoint(int i) const { return node_pos(i); }

	QSize sizeHint() const override { return QSize(420, 170); }

	std::function<void(const char *key, double value, bool final)> onChanged;

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void mouseDoubleClickEvent(QMouseEvent *) override;
	void wheelEvent(QWheelEvent *) override;
	void leaveEvent(QEvent *) override;
	void changeEvent(QEvent *) override;

private:
	void rebuild_layers();
	void paint_grid(QPainter &p, const QRectF &r, double zero);
	void paint_curve(QPainter &p, const QRectF &r, double zero);
	void paint_nodes(QPainter &p);
	QRectF plot() const;
	double x_of(double f) const;
	double f_of(double x) const;
	double y_of(double db) const;
	double db_of(double y) const;
	double response_db(const EqState &st, double f) const;
	static void apply_band(EqState &st, int i, double freq, double gain);
	double node_freq(int i) const;
	double band_gain(int i) const;
	double node_q(int i) const;
	double node_level(int i) const;
	QPointF node_pos(int i) const;
	double snap_freq(int i, double freq) const;
	double solve_gain(int i, double freq, double target_db) const;
	int hit(const QPointF &pos) const;
	void set_node(int i, double freq, double gain, bool final);
	void mark_dirty()
	{
		curve_dirty_ = true;
		nodes_dirty_ = true;
	}
	QString freq_text(double f) const;

	EqState s_;
	int sample_rate_ = 48000;
	std::vector<float> spectrum_;
	int hover_ = -1;
	int drag_ = -1;
	QStringList names_;
	QString hz_ = " Hz", khz_ = " kHz", db_ = " dB", off_hint_;
	QImage grid_, curve_;
	double grid_dpr_ = 0.0;
	bool curve_dirty_ = true;
	mutable bool nodes_dirty_ = true;
	mutable double node_db_[6] = {};
};

} // namespace vsui
