# Шаблон нового плагина OBS

Минимальный аудиофильтр «Громкость» на той же инфраструктуре, что и Голосовая студия.

```
_shared/
├─ cmake/ObsPluginCommon.cmake     общие настройки сборки: libobs, Qt, установка в OBS
└─ scripts/
   ├─ setup-obs-sdk.ps1            готовит SDK под установленную версию OBS
   └─ obs-build-tools.ps1          поиск CMake/OBS, общие функции для build.ps1
_sdk/                              (создаётся автоматически) заголовки libobs + obs.lib — не редактировать
```

## Как сделать новый плагин

1. Скопируйте папку `_template` и назовите копию по имени плагина латиницей, например `scene-tools`.
   Имя папки станет именем плагина (`scene-tools.dll`).
2. Поменяйте код в `src/plugin-main.cpp` и тексты в `data/locale/*.ini`.
3. Соберите и установите: `.\build.ps1 -Install` в папке плагина, затем перезапустите OBS.

Нужны OBS Studio 28 или новее, Visual Studio 2022 с компонентом «Разработка классических приложений на C++» и Git.

SDK OBS готовится автоматически при первой сборке. После **обновления OBS** выполните

```powershell
powershell -ExecutionPolicy Bypass -File _shared\scripts\setup-obs-sdk.ps1 -Force -WithQt
```

и пересоберите плагины. Скрипт скачает заголовки новой версии, пересоздаст `obs.lib` и `obs-frontend-api.lib`
и подтянет подходящий Qt (`-WithQt` нужен плагинам со своим интерфейсом).

Плагину со своим окном или доком в CMakeLists.txt достаточно вызвать `obs_find_qt()` и подключить
`Qt6::Widgets` и `OBS::frontend-api` (пример — `voice-studio`).
