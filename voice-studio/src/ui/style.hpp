#pragma once

// Минималистичный стиль: только цвета и шрифт текущей темы OBS, без градиентов.

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QWidget>

namespace vsui::style {

inline bool is_dark(const QPalette &p)
{
	return p.color(QPalette::Window).lightness() < 128;
}

inline QColor text(const QPalette &p, int alpha = 255)
{
	QColor c = p.color(QPalette::WindowText);
	c.setAlpha(alpha);
	return c;
}

inline QColor accent(const QPalette &p, int alpha = 255)
{
	QColor c = p.color(QPalette::Highlight);
	c.setAlpha(alpha);
	return c;
}

// Подложка графиков и индикаторов — чуть темнее (или светлее в светлой теме) фона окна.
inline QColor well(const QPalette &p)
{
	const QColor w = p.color(QPalette::Window);
	return is_dark(p) ? w.darker(130) : w.darker(108);
}

// Цвета индикаторов уровня — как у микшера OBS (зелёный, жёлтый, красный).
inline QColor meter_ok()
{
	return QColor(76, 175, 80);
}
inline QColor meter_warn()
{
	return QColor(255, 193, 7);
}
inline QColor meter_hot()
{
	return QColor(229, 57, 53);
}

// Шрифт виджета, масштабированный относительно шрифта темы.
inline QFont font(const QWidget *w, double scale = 1.0, int weight = -1)
{
	QFont f = w->font();
	if (f.pointSizeF() > 0)
		f.setPointSizeF(f.pointSizeF() * scale);
	else if (f.pixelSize() > 0)
		f.setPixelSize((int)(f.pixelSize() * scale + 0.5));
	if (weight > 0)
		f.setWeight(QFont::Weight(weight));
	return f;
}

} // namespace vsui::style
