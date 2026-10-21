// Инструмент автообновления: самопроверка ядра, сборка пакета для выпуска и проверка
// опубликованного выпуска тем же кодом, что работает в плагине.
//
//   voice-studio-update-tool selftest
//   voice-studio-update-tool pack <out.bin> <папка плагина> <путь>...   (пути: bin/64bit/…, data/…)
//   voice-studio-update-tool verify <update.txt> <update.txt.sig>       (ключ, вшитый в плагин)
//   voice-studio-update-tool online <владелец/репозиторий> [папка для установки]

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "public-key.hpp"
#include "update-core.hpp"

namespace fs = std::filesystem;
using namespace vsup;

namespace {

int g_failed = 0;

void check(bool ok, const char *what)
{
	std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
	if (!ok)
		g_failed++;
}

bool read_file(const fs::path &p, std::string &out)
{
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

bool write_file(const fs::path &p, const std::string &data)
{
	std::error_code ec;
	fs::create_directories(p.parent_path(), ec);
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	return out && out.write(data.data(), (std::streamsize)data.size());
}

// Подписать сообщение новым ключом (только для самопроверки: настоящий ключ — у разработчика).
struct TestSigner {
	BCRYPT_ALG_HANDLE alg = nullptr;
	BCRYPT_KEY_HANDLE key = nullptr;
	uint8_t pub[64] = {};

	TestSigner()
	{
		BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
		BCryptGenerateKeyPair(alg, &key, 256, 0);
		BCryptFinalizeKeyPair(key, 0);
		uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
		ULONG got = 0;
		BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &got, 0);
		std::memcpy(pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
	}
	~TestSigner()
	{
		BCryptDestroyKey(key);
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	bool sign(const std::string &msg, uint8_t sig[64])
	{
		uint8_t hash[32];
		ULONG got = 0;
		return sha256(msg.data(), msg.size(), hash) &&
		       BCryptSignHash(key, nullptr, hash, 32, sig, 64, &got, 0) == 0 && got == 64;
	}
};

int selftest()
{
	check(compare_versions("1.10.0", "1.9.3") > 0 && compare_versions("1.3.0", "1.3") == 0 &&
		      compare_versions("1.2.9", "1.3.0") < 0,
	      "сравнение версий");

	uint8_t hash[32];
	check(sha256("abc", 3, hash) &&
		      to_hex(hash, 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	      "SHA-256");

	std::vector<uint8_t> raw;
	check(base64_decode("aGVsbG8gd29ybGQ=", raw) && std::string(raw.begin(), raw.end()) == "hello world",
	      "base64");

	TestSigner signer;
	const std::string msg = "name=voice-studio\nversion=9.9.9\n";
	uint8_t sig[64];
	check(signer.sign(msg, sig) && verify_signature(msg, sig, signer.pub), "подпись: верная принимается");
	check(!verify_signature(msg + " ", sig, signer.pub), "подпись: изменённый текст отклоняется");
	check(!verify_signature(msg, sig, kUpdatePublicKey), "подпись: чужой ключ отклоняется");

	Manifest m;
	const std::string text = "name=voice-studio\nversion=1.3.0\nobs_min=32.0.0\nobs_max_major=32\n"
				 "bundle=voice-studio-1.3.0-update.bin\nbundle_sha256=" +
				 std::string(64, 'a') + "\nbundle_size=123\nnotes_ru=Строка 1\\nСтрока 2\n";
	check(parse_manifest(text, m) && m.version == "1.3.0" && m.obs_max_major == 32 && m.bundle_size == 123 &&
		      m.notes_ru == "Строка 1\nСтрока 2",
	      "описание версии читается");
	Manifest bad;
	check(!parse_manifest("name=voice-studio\nversion=1\nbundle=../evil\nbundle_sha256=" + std::string(64, 'a') +
				      "\nbundle_size=1\n",
			      bad),
	      "описание версии: опасное имя пакета отклоняется");

	std::vector<BundleFile> files = {{"bin/64bit/voice-studio.dll", "NEW-DLL"},
					 {"data/locale/ru-RU.ini", "VoiceStudio.Test=\"Новая\"\n"}};
	std::vector<BundleFile> back;
	check(read_bundle(write_bundle(files), back) && back.size() == 2 && back[1].data == files[1].data,
	      "пакет: запись и чтение");
	for (const char *evil : {"../x.dll", "bin/64bit/../../x.dll", "C:/x.dll", "bin\\64bit\\x.dll", "other/x"}) {
		std::vector<BundleFile> e = {{evil, "x"}};
		std::vector<BundleFile> r;
		if (read_bundle(write_bundle(e), r)) {
			check(false, (std::string("пакет: путь отклоняется: ") + evil).c_str());
			return 1;
		}
	}
	check(true, "пакет: пути вне папки плагина отклоняются");
	std::string truncated = write_bundle(files);
	truncated.pop_back();
	check(!read_bundle(truncated, back), "пакет: обрезанный файл отклоняется");

	// Установка в настоящую папку: старая библиотека открыта (как у работающего OBS).
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const fs::path root = fs::path(tmp) / (L"vs-update-test-" + std::to_wstring(GetCurrentProcessId()));
	std::error_code ec;
	fs::remove_all(root, ec);
	write_file(root / "bin/64bit/voice-studio.dll", "OLD-DLL");
	write_file(root / "data/locale/ru-RU.ini", "old");
	HANDLE busy = CreateFileW((root / "bin/64bit/voice-studio.dll").c_str(), GENERIC_READ,
				  FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
	check(busy != INVALID_HANDLE_VALUE, "установка: старая библиотека «занята»");
	const std::string err = apply_bundle(root.wstring(), files);
	std::string dll, ini;
	read_file(root / "bin/64bit/voice-studio.dll", dll);
	read_file(root / "data/locale/ru-RU.ini", ini);
	check(err.empty() && dll == "NEW-DLL" && ini == files[1].data, "установка: новые файлы на месте");
	int old_count = 0;
	for (auto &e : fs::directory_iterator(root / "bin/64bit"))
		old_count += e.path().filename().wstring().find(L".old-") != std::wstring::npos;
	check(old_count == 1, "установка: прежняя версия отложена до перезапуска");
	CloseHandle(busy);
	cleanup_old_files(root.wstring());
	int left = 0;
	for (auto &e : fs::recursive_directory_iterator(root))
		left += e.path().filename().wstring().find(L".old-") != std::wstring::npos;
	check(left == 0, "установка: после перезапуска остатки удаляются");
	fs::remove_all(root, ec);

	std::printf(g_failed ? "\n%d проверок не прошли\n" : "\nВсе проверки пройдены\n", g_failed);
	return g_failed ? 1 : 0;
}

int pack(const std::wstring &out, const std::wstring &root, const std::vector<std::wstring> &paths)
{
	std::vector<BundleFile> files;
	for (const std::wstring &p : paths) {
		BundleFile f;
		f.path = wide_to_utf8(p);
		if (!read_file(fs::path(root) / p, f.data)) {
			std::printf("cannot read %s\n", f.path.c_str());
			return 2;
		}
		files.push_back(std::move(f));
	}
	const std::string bytes = write_bundle(files);
	std::vector<BundleFile> back;
	if (!read_bundle(bytes, back) || !write_file(out, bytes)) {
		std::printf("cannot write bundle\n");
		return 2;
	}
	uint8_t hash[32];
	sha256(bytes.data(), bytes.size(), hash);
	std::printf("size=%zu\nsha256=%s\n", bytes.size(), to_hex(hash, 32).c_str());
	return 0;
}

int verify(const std::wstring &text_path, const std::wstring &sig_path)
{
	std::string text, sig;
	std::vector<uint8_t> raw;
	if (!read_file(text_path, text) || !read_file(sig_path, sig) || !base64_decode(sig, raw) || raw.size() != 64) {
		std::printf("[FAIL] cannot read update.txt / signature\n");
		return 2;
	}
	const bool ok = verify_signature(text, raw.data(), kUpdatePublicKey);
	Manifest m;
	const bool parsed = parse_manifest(text, m);
	std::printf("%s signature\n%s manifest (version %s, bundle %s)\n", ok ? "[ OK ]" : "[FAIL]",
		    parsed ? "[ OK ]" : "[FAIL]", m.version.c_str(), m.bundle.c_str());
	return ok && parsed ? 0 : 1;
}

// То же, что делает плагин: скачать описание, проверить подпись, скачать пакет, сверить хеш
// и (если указана папка) установить.
int online(const std::string &repo, const std::wstring &install_root)
{
	const std::string base = "https://github.com/" + repo;
	const std::string ua = "VoiceStudio-Updater/test";
	const HttpResult text = http_get(base + "/releases/latest/download/update.txt", 64 * 1024, ua);
	const HttpResult sig = http_get(base + "/releases/latest/download/update.txt.sig", 4096, ua);
	check(text.ok && sig.ok, "скачаны update.txt и подпись");
	if (!text.ok || !sig.ok) {
		std::printf("  %s / %s\n", text.error.c_str(), sig.error.c_str());
		return 1;
	}
	std::vector<uint8_t> raw;
	check(base64_decode(sig.body, raw) && raw.size() == 64 && verify_signature(text.body, raw.data(), kUpdatePublicKey),
	      "подпись выпуска верна");
	Manifest m;
	check(parse_manifest(text.body, m), "описание версии читается");
	std::printf("  версия %s, пакет %s (%llu байт)\n", m.version.c_str(), m.bundle.c_str(),
		    (unsigned long long)m.bundle_size);
	const HttpResult bin = http_get(base + "/releases/download/v" + m.version + "/" + m.bundle, 128 << 20, ua);
	uint8_t hash[32];
	check(bin.ok && bin.body.size() == m.bundle_size && sha256(bin.body.data(), bin.body.size(), hash) &&
		      to_hex(hash, 32) == m.bundle_sha256,
	      "пакет скачан, хеш совпадает");
	std::vector<BundleFile> files;
	check(read_bundle(bin.body, files), "пакет читается");
	for (const BundleFile &f : files)
		std::printf("  %s (%zu байт)\n", f.path.c_str(), f.data.size());
	if (!install_root.empty())
		check(apply_bundle(install_root, files).empty(), "пакет установлен");
	return g_failed ? 1 : 0;
}

} // namespace

int main()
{
	SetConsoleOutputCP(CP_UTF8);
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	const std::wstring cmd = argc > 1 ? argv[1] : L"";
	if (cmd == L"selftest")
		return selftest();
	if (cmd == L"pack" && argc >= 5)
		return pack(argv[2], argv[3], std::vector<std::wstring>(argv + 4, argv + argc));
	if (cmd == L"verify" && argc == 4)
		return verify(argv[2], argv[3]);
	if (cmd == L"online" && argc >= 3)
		return online(wide_to_utf8(argv[2]), argc > 3 ? argv[3] : L"");
	std::printf("usage: voice-studio-update-tool selftest | pack <out> <root> <paths...> | verify <txt> <sig> |"
		    " online <owner/repo> [install root]\n");
	return 1;
}
