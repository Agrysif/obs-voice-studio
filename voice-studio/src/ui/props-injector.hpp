#pragma once

// Встраивание живого блока в стандартное окно свойств фильтра OBS.
//
// API свойств OBS не умеет показывать свои виджеты. Поэтому фильтр добавляет в начало
// настроек информационную строку с невидимой меткой (символы нулевой ширины), а этот
// модуль, заметив такую строку при отрисовке окна, ставит на её место живой блок.
// Если устройство окна в будущих версиях OBS изменится, метка останется пустой строкой —
// ничего не сломается, просто блока не будет.

#include <QString>
#include <QWidget>

#include <functional>

namespace vsui {

// Невидимая метка с номером (номер связывает строку с конкретным фильтром).
QString make_marker(int token);
bool parse_marker(const QString &text, int &token);

// factory создаёт блок для номера метки (nullptr — пропустить).
using BlockFactory = std::function<QWidget *(int token, QWidget *parent)>;
void install_props_injector(BlockFactory factory);
void uninstall_props_injector();

} // namespace vsui
