#include "props-injector.hpp"

#include <QApplication>
#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QTimer>

namespace vsui {

namespace {

constexpr ushort kEdge = 0x2063; // невидимый разделитель
constexpr ushort kZero = 0x200B; // пробел нулевой ширины
constexpr ushort kOne = 0x200C;  // несоединитель нулевой ширины
constexpr int kBits = 16;

// Ставит блок на место строки-метки. true — получилось (или уже сделано).
bool inject(QLabel *label, int token, const BlockFactory &factory)
{
	if (label->property("vs_live_done").toBool())
		return true;
	QWidget *container = label->parentWidget();
	auto *form = container ? qobject_cast<QFormLayout *>(container->layout()) : nullptr;
	if (!form)
		return false;
	int row = -1;
	QFormLayout::ItemRole role;
	form->getWidgetPosition(label, &row, &role);
	if (row < 0)
		return false;

	label->setProperty("vs_live_done", true);
	QWidget *block = factory(token, container);
	if (!block)
		return true;
	form->insertRow(row, block);       // строка во всю ширину окна
	form->setRowVisible(row + 1, false); // сама метка не нужна
	return true;
}

class Injector : public QObject {
public:
	explicit Injector(BlockFactory factory) : factory_(std::move(factory)) {}

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (ev->type() != QEvent::Polish)
			return false;
		auto *label = qobject_cast<QLabel *>(obj);
		int token = 0;
		if (!label || !parse_marker(label->text(), token))
			return false;
		if (!inject(label, token, factory_)) {
			// Строка ещё не в макете — пробуем сразу после текущего события.
			QPointer<QLabel> guard(label);
			BlockFactory factory = factory_;
			QTimer::singleShot(0, label, [guard, token, factory] {
				if (guard)
					inject(guard, token, factory);
			});
		}
		return false;
	}

private:
	BlockFactory factory_;
};

Injector *g_injector = nullptr;

} // namespace

QString make_marker(int token)
{
	QString s{QChar(kEdge)};
	for (int b = kBits - 1; b >= 0; b--)
		s += QChar(((token >> b) & 1) ? kOne : kZero);
	s += QChar(kEdge);
	return s;
}

bool parse_marker(const QString &text, int &token)
{
	if (text.size() != kBits + 2 || text.front().unicode() != kEdge || text.back().unicode() != kEdge)
		return false;
	int v = 0;
	for (int i = 1; i <= kBits; i++) {
		const ushort c = text[i].unicode();
		if (c != kZero && c != kOne)
			return false;
		v = (v << 1) | (c == kOne ? 1 : 0);
	}
	token = v;
	return true;
}

void install_props_injector(BlockFactory factory)
{
	if (g_injector || !qApp)
		return;
	g_injector = new Injector(std::move(factory));
	qApp->installEventFilter(g_injector);
}

void uninstall_props_injector()
{
	if (!g_injector)
		return;
	if (qApp)
		qApp->removeEventFilter(g_injector);
	delete g_injector;
	g_injector = nullptr;
}

} // namespace vsui
