#pragma once

// Аудиофильтр «Голосовая студия» и то, что нужно красивой панели для работы с ним.

#include <obs.h>

#include <memory>
#include <string>
#include <vector>

#include "shared-state.hpp"

namespace vs {

constexpr const char *kFilterId = "voice_studio_filter";

struct FilterInstance {
	obs_source_t *filter = nullptr; // со ссылкой: вызывающий освобождает через obs_source_release
	std::shared_ptr<SharedState> shared;
};

// Все живые экземпляры фильтра (в порядке создания).
std::vector<FilterInstance> list_filter_instances();

// Состояние конкретного фильтра или nullptr.
std::shared_ptr<SharedState> find_shared_state(obs_source_t *filter);

// Начать «Умную настройку» или остановить уже идущую.
void toggle_calibration(SharedState &shared);

// Живой блок (уровни + эквалайзер) регистрирует здесь функцию, которая выдаёт невидимую
// метку для строки в начале окна свойств; на её место затем встаёт сам блок.
using LiveMarkerFn = std::string (*)(obs_source_t *filter);
void set_live_marker_hook(LiveMarkerFn fn);

} // namespace vs

// Регистрирует аудиофильтр в OBS.
void register_voice_filter();
