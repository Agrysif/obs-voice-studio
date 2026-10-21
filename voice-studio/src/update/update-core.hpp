#pragma once

// Ядро автообновления (без OBS и Qt): скачивание по HTTPS, проверка подписи,
// разбор описания версии и установка пакета обновления.
//
// Как устроен выпуск (см. voice-studio/release.ps1):
//   update.txt      — описание версии (ключ=значение), подписано ключом разработчика;
//   update.txt.sig  — подпись ECDSA P-256 (SHA-256), base64 от 64 байт r||s;
//   *-update.bin    — пакет файлов; его SHA-256 записан в подписанном update.txt.
// Подделать обновление, не имея секретного ключа разработчика, нельзя — даже
// получив доступ к странице проекта на GitHub.

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vsup {

struct HttpResult {
	bool ok = false;
	int status = 0;
	std::string body;
	std::string error;
};

// GET по HTTPS (WinHTTP, переходы по редиректам GitHub включены).
// cancel — прервать скачивание (выход из OBS).
HttpResult http_get(const std::string &url, size_t max_bytes, const std::string &user_agent,
		    const std::atomic<bool> *cancel = nullptr);

bool sha256(const void *data, size_t size, uint8_t out[32]);
std::string to_hex(const uint8_t *data, size_t size);

// Проверка подписи ECDSA P-256: pubkey — 64 байта (X||Y), signature — 64 байта (r||s).
bool verify_signature(const std::string &message, const uint8_t signature[64], const uint8_t pubkey[64]);
bool base64_decode(const std::string &text, std::vector<uint8_t> &out);

// «1.10.0» > «1.9.3». Возвращает <0, 0, >0.
int compare_versions(const std::string &a, const std::string &b);

struct Manifest {
	std::string name, version, obs_min, bundle, bundle_sha256, page, notes_ru, notes_en;
	int obs_max_major = 0;
	uint64_t bundle_size = 0;
};

// Разбор update.txt (строки ключ=значение, \n внутри значений — как в переводах OBS).
bool parse_manifest(const std::string &text, Manifest &m);

struct BundleFile {
	std::string path; // относительно папки плагина: bin/64bit/…, data/…
	std::string data;
};

bool read_bundle(const std::string &bytes, std::vector<BundleFile> &files);
std::string write_bundle(const std::vector<BundleFile> &files); // для тестов

// Ставит файлы в папку плагина (plugin_root/bin/64bit, plugin_root/data).
// Занятую OBS библиотеку переименовывает (это Windows разрешает) и кладёт новую рядом.
// Возвращает пустую строку или код ошибки: "rights", "format".
std::string apply_bundle(const std::wstring &plugin_root, const std::vector<BundleFile> &files);

// Удаляет *.old-* после прошлых обновлений (то, что ещё занято, — пропускает).
void cleanup_old_files(const std::wstring &plugin_root);

std::wstring utf8_to_wide(const std::string &s);
std::string wide_to_utf8(const std::wstring &w);

} // namespace vsup
