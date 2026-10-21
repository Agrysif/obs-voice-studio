## 🎙 Voice Studio 1.3.0

**Studio-grade microphone processing for OBS Studio**: AI noise suppression (RNNoise 0.2), one-click Smart setup,
a live EQ you drag over your voice spectrum, compressor, de-esser, warmth and a formant-preserving voice changer.

![Voice Studio](https://raw.githubusercontent.com/Agrysif/obs-voice-studio/main/voice-studio/docs/eq-drag-en.gif)

### What's new
- **Automatic updates.** The plugin checks GitHub Releases at most once a day and installs new versions in one click.
  Every update is verified with an ECDSA P-256 signature and SHA-256 before any file is changed. Settings are in
  **Tools → 🎙 Voice Studio**.
- **12 languages:** English, Русский, Беларуская, Українська, Қазақша, Кыргызча, Oʻzbekcha, Тоҷикӣ, Azərbaycanca,
  Հայերեն, Татарча, Башҡортса.
- **Live block in the filter window**, styled like OBS: input/output levels, AI voice detection, compression and an
  EQ curve on top of the live voice spectrum.
- EQ points sit exactly on the curve and follow the cursor while you drag them.

### Install
1. Download **`voice-studio-1.3.0-windows-x64.zip`** below.
2. Unzip it and double-click **`install.cmd`** (no admin rights needed).
3. Restart OBS → Audio Mixer → **⋮** next to the mic → **Filters** → **+** → **🎙 Voice Studio**.

Requires OBS Studio 32 and Windows 10/11 64-bit. Future updates arrive automatically.

---

## 🎙 Голосовая студия 1.3.0

**Студийный звук микрофона в OBS Studio**: ИИ-шумоподавление (RNNoise 0.2), «Умная настройка» в одно нажатие,
эквалайзер, который тянется мышью поверх спектра голоса, компрессор, де-эссер, тепло и смена голоса с сохранением
формант.

### Что нового
- **Автообновление.** Плагин раз в сутки проверяет GitHub Releases и ставит новую версию в одно нажатие.
  Каждое обновление проверяется подписью ECDSA P-256 и SHA-256 до того, как изменится хоть один файл.
  Настройки — **Сервис → 🎙 Голосовая студия**.
- **12 языков**: английский, русский, белорусский, украинский, казахский, киргизский, узбекский, таджикский,
  азербайджанский, армянский, татарский, башкирский.
- **Живой блок в окне фильтра** в стиле OBS: уровни «Вход/Выход», ИИ-детектор голоса, работа компрессора
  и кривая эквалайзера поверх живого спектра голоса.
- Точки эквалайзера точно лежат на кривой и не отстают от курсора.

### Установка
1. Скачайте **`voice-studio-1.3.0-windows-x64.zip`** ниже.
2. Распакуйте и дважды щёлкните **`install.cmd`** (права администратора не нужны).
3. Перезапустите OBS → Микшер аудио → **⋮** у микрофона → **Фильтры** → **+** → **🎙 Голосовая студия**.

Нужны OBS Studio 32 и Windows 10/11, 64 бит. Следующие обновления придут сами.

---
<sub>Files for the updater (`update.txt`, `update.txt.sig`, `*-update.bin`) are used by the plugin itself, you don't
need to download them. / Файлы `update.txt`, `update.txt.sig` и `*-update.bin` нужны самому плагину для
автообновления, скачивать их не нужно.</sub>
