#include "update-core.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace vsup {

namespace {

constexpr char kBundleMagic[8] = {'V', 'S', 'U', 'P', 'D', 'T', '0', '1'};

struct WinHttpHandle {
	HINTERNET h = nullptr;
	explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
	~WinHttpHandle()
	{
		if (h)
			WinHttpCloseHandle(h);
	}
	WinHttpHandle(const WinHttpHandle &) = delete;
	WinHttpHandle &operator=(const WinHttpHandle &) = delete;
};

// Только относительные пути внутри папки плагина: bin/64bit/… или data/…
bool valid_relative_path(const std::string &p)
{
	if (p.empty() || p.size() > 260 || p.front() == '/')
		return false;
	if (p.find("..") != std::string::npos || p.find('\\') != std::string::npos || p.find(':') != std::string::npos)
		return false;
	return p.rfind("bin/64bit/", 0) == 0 || p.rfind("data/", 0) == 0;
}

std::string unescape(const std::string &v)
{
	std::string out;
	out.reserve(v.size());
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == 'n') {
			out += '\n';
			i++;
		} else {
			out += v[i];
		}
	}
	return out;
}

} // namespace

std::wstring utf8_to_wide(const std::string &s)
{
	if (s.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string wide_to_utf8(const std::wstring &w)
{
	if (w.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

HttpResult http_get(const std::string &url, size_t max_bytes, const std::string &user_agent,
		    const std::atomic<bool> *cancel)
{
	HttpResult r;
	const std::wstring wurl = utf8_to_wide(url);
	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	uc.dwHostNameLength = (DWORD)-1;
	uc.dwUrlPathLength = (DWORD)-1;
	uc.dwExtraInfoLength = (DWORD)-1;
	if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
		r.error = "bad url";
		return r;
	}
	const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
	std::wstring object(uc.lpszUrlPath, uc.dwUrlPathLength);
	if (uc.lpszExtraInfo)
		object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

	WinHttpHandle session(WinHttpOpen(utf8_to_wide(user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
					  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.h) {
		r.error = "winhttp";
		return r;
	}
	WinHttpSetTimeouts(session.h, 8000, 8000, 10000, 10000);
	DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	}

	WinHttpHandle connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
	if (!connect.h) {
		r.error = "connect";
		return r;
	}
	WinHttpHandle request(WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
						 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!request.h) {
		r.error = "request";
		return r;
	}
	if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(request.h, nullptr)) {
		r.error = "network error " + std::to_string(GetLastError());
		return r;
	}

	DWORD status = 0, len = sizeof(status);
	WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			    &status, &len, WINHTTP_NO_HEADER_INDEX);
	r.status = (int)status;

	for (;;) {
		if (cancel && cancel->load()) {
			r.error = "cancelled";
			return r;
		}
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(request.h, &avail)) {
			r.error = "read error";
			return r;
		}
		if (avail == 0)
			break;
		if (r.body.size() + avail > max_bytes) {
			r.error = "too large";
			return r;
		}
		const size_t old = r.body.size();
		r.body.resize(old + avail);
		DWORD got = 0;
		if (!WinHttpReadData(request.h, r.body.data() + old, avail, &got)) {
			r.error = "read error";
			return r;
		}
		r.body.resize(old + got);
	}
	r.ok = status == 200;
	if (!r.ok)
		r.error = "http " + std::to_string(status);
	return r;
}

bool sha256(const void *data, size_t size, uint8_t out[32])
{
	return BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)size, out, 32) == 0;
}

std::string to_hex(const uint8_t *data, size_t size)
{
	static const char *digits = "0123456789abcdef";
	std::string s;
	s.reserve(size * 2);
	for (size_t i = 0; i < size; i++) {
		s += digits[data[i] >> 4];
		s += digits[data[i] & 15];
	}
	return s;
}

bool verify_signature(const std::string &message, const uint8_t signature[64], const uint8_t pubkey[64])
{
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) != 0)
		return false;
	std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
	auto *hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
	hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
	hdr->cbKey = 32;
	std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), pubkey, 64);

	BCRYPT_KEY_HANDLE key = nullptr;
	bool ok = false;
	if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0) == 0) {
		uint8_t hash[32];
		if (sha256(message.data(), message.size(), hash))
			ok = BCryptVerifySignature(key, nullptr, hash, 32, (PUCHAR)signature, 64, 0) == 0;
		BCryptDestroyKey(key);
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	return ok;
}

bool base64_decode(const std::string &text, std::vector<uint8_t> &out)
{
	out.clear();
	uint32_t acc = 0;
	int bits = 0;
	for (char c : text) {
		int v;
		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+')
			v = 62;
		else if (c == '/')
			v = 63;
		else if (c == '=' || c == '\r' || c == '\n' || c == ' ')
			continue;
		else
			return false;
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((uint8_t)((acc >> bits) & 0xFF));
		}
	}
	return true;
}

int compare_versions(const std::string &a, const std::string &b)
{
	std::istringstream sa(a), sb(b);
	for (int i = 0; i < 4; i++) {
		long va = 0, vb = 0;
		std::string pa, pb;
		if (std::getline(sa, pa, '.'))
			va = std::strtol(pa.c_str(), nullptr, 10);
		if (std::getline(sb, pb, '.'))
			vb = std::strtol(pb.c_str(), nullptr, 10);
		if (va != vb)
			return va < vb ? -1 : 1;
	}
	return 0;
}

bool parse_manifest(const std::string &text, Manifest &m)
{
	std::map<std::string, std::string> kv;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == std::string::npos)
			continue;
		kv[line.substr(0, eq)] = unescape(line.substr(eq + 1));
	}
	m.name = kv["name"];
	m.version = kv["version"];
	m.obs_min = kv["obs_min"];
	m.obs_max_major = std::atoi(kv["obs_max_major"].c_str());
	m.bundle = kv["bundle"];
	m.bundle_sha256 = kv["bundle_sha256"];
	m.bundle_size = std::strtoull(kv["bundle_size"].c_str(), nullptr, 10);
	m.page = kv["page"];
	m.notes_ru = kv["notes_ru"];
	m.notes_en = kv["notes_en"];
	// Имя пакета подставляется в ссылку — только безопасные символы.
	for (char c : m.bundle)
		if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.'))
			return false;
	return !m.name.empty() && !m.version.empty() && !m.bundle.empty() && m.bundle_sha256.size() == 64 &&
	       m.bundle_size > 0;
}

bool read_bundle(const std::string &bytes, std::vector<BundleFile> &files)
{
	files.clear();
	size_t pos = 0;
	auto take = [&](void *dst, size_t n) {
		if (pos + n > bytes.size())
			return false;
		std::memcpy(dst, bytes.data() + pos, n);
		pos += n;
		return true;
	};
	char magic[8];
	uint32_t count = 0;
	if (!take(magic, 8) || std::memcmp(magic, kBundleMagic, 8) != 0 || !take(&count, 4) || count > 1000)
		return false;
	for (uint32_t i = 0; i < count; i++) {
		uint16_t plen = 0;
		uint64_t size = 0;
		if (!take(&plen, 2) || pos + plen > bytes.size())
			return false;
		BundleFile f;
		f.path.assign(bytes.data() + pos, plen);
		pos += plen;
		if (!take(&size, 8) || size > bytes.size() - pos || !valid_relative_path(f.path))
			return false;
		f.data.assign(bytes.data() + pos, (size_t)size);
		pos += (size_t)size;
		files.push_back(std::move(f));
	}
	return pos == bytes.size() && !files.empty();
}

std::string write_bundle(const std::vector<BundleFile> &files)
{
	std::string out(kBundleMagic, 8);
	const uint32_t count = (uint32_t)files.size();
	out.append(reinterpret_cast<const char *>(&count), 4);
	for (const BundleFile &f : files) {
		const uint16_t plen = (uint16_t)f.path.size();
		const uint64_t size = f.data.size();
		out.append(reinterpret_cast<const char *>(&plen), 2);
		out += f.path;
		out.append(reinterpret_cast<const char *>(&size), 8);
		out += f.data;
	}
	return out;
}

std::string apply_bundle(const std::wstring &plugin_root, const std::vector<BundleFile> &files)
{
	const fs::path root(plugin_root);
	for (const BundleFile &f : files)
		if (!valid_relative_path(f.path))
			return "format";

	const auto stamp = std::to_wstring(
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
			.count());
	std::vector<fs::path> written;
	auto remove_written = [&] {
		std::error_code ec;
		for (const fs::path &p : written)
			fs::remove(p, ec);
	};

	// 1. Сначала кладём все новые файлы рядом (*.new) — если места или прав нет, ничего не ломаем.
	for (const BundleFile &f : files) {
		const fs::path target = root / fs::path(utf8_to_wide(f.path));
		std::error_code ec;
		fs::create_directories(target.parent_path(), ec);
		fs::path tmp = target;
		tmp += L".new";
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out || !out.write(f.data.data(), (std::streamsize)f.data.size())) {
			out.close();
			remove_written();
			return "rights";
		}
		out.close();
		written.push_back(tmp);
	}

	// 2. Подменяем: старый файл -> *.old-<время> (даже занятый OBS), новый — на его место.
	//    Если что-то не вышло — возвращаем всё как было.
	std::vector<std::pair<fs::path, fs::path>> swapped; // (файл, его прежняя версия или пусто)
	auto rollback = [&] {
		std::error_code ec;
		for (auto it = swapped.rbegin(); it != swapped.rend(); ++it) {
			fs::remove(it->first, ec);
			if (!it->second.empty())
				fs::rename(it->second, it->first, ec);
		}
		remove_written();
	};
	for (const BundleFile &f : files) {
		const fs::path target = root / fs::path(utf8_to_wide(f.path));
		fs::path tmp = target;
		tmp += L".new";
		fs::path old = target;
		old += L".old-" + stamp;
		std::error_code ec;
		const bool had_old = fs::exists(target, ec);
		if (had_old) {
			fs::rename(target, old, ec);
			if (ec) {
				rollback();
				return "rights";
			}
		}
		fs::rename(tmp, target, ec);
		if (ec) {
			std::error_code back;
			if (had_old)
				fs::rename(old, target, back);
			rollback();
			return "rights";
		}
		swapped.emplace_back(target, had_old ? old : fs::path());
	}
	return {};
}

void cleanup_old_files(const std::wstring &plugin_root)
{
	std::error_code ec;
	std::vector<fs::path> leftovers;
	for (const wchar_t *sub : {L"bin", L"data"}) {
		const fs::path dir = fs::path(plugin_root) / sub;
		if (!fs::exists(dir, ec))
			continue;
		for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
		     it.increment(ec)) {
			const std::wstring name = it->path().filename().wstring();
			if (name.find(L".old-") != std::wstring::npos ||
			    (name.size() > 4 && name.compare(name.size() - 4, 4, L".new") == 0))
				leftovers.push_back(it->path());
		}
	}
	for (const fs::path &p : leftovers) {
		std::error_code rm;
		fs::remove(p, rm); // занятые OBS файлы просто останутся до следующего раза
	}
}

} // namespace vsup
