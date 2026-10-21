#pragma once

// Автообновление в OBS: меню «Сервис → 🎙 Голосовая студия», проверка при запуске
// (раз в сутки, можно выключить), окно «Доступна новая версия» и установка.

#include <QString>

namespace vsup {

void init_updater();     // obs_module_load (только при наличии окна OBS)
void shutdown_updater(); // выход из OBS / выгрузка плагина

// Для живого блока в окне фильтра: текст «⬆ Доступна версия …» или пусто.
QString update_badge();
void open_update_dialog();

} // namespace vsup
