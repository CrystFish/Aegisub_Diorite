// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "ocr_process.h"

#include "../options.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

std::string TrimLeft(std::string text) {
	size_t pos = 0;
	while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
		++pos;
	return text.substr(pos);
}

std::string TrimRight(std::string text) {
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.pop_back();
	return text;
}

std::string JsonEscape(std::string const& text) {
	std::string out;
	out.reserve(text.size() + 8);
	for (unsigned char ch : text) {
		switch (ch) {
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
					out += buf;
				}
				else
					out += static_cast<char>(ch);
		}
	}
	return out;
}

} // namespace

namespace ocr {

struct OCRProcess::Impl {
#ifdef _WIN32
	HANDLE child = nullptr;
	HANDLE stdin_write = nullptr;
	HANDLE stdout_read = nullptr;
#endif
	bool running = false;
};

OCRProcess::OCRProcess()
: impl(new Impl())
{
}

OCRProcess::~OCRProcess() {
	Stop();
}

bool OCRProcess::Start(agi::fs::path const& executable, agi::fs::path const& models_dir,
                       agi::fs::path const& config_path, std::string& diagnostic) {
	Stop();
	diagnostic.clear();

#ifndef _WIN32
	diagnostic = "Persistent OCR process is only available on Windows builds.";
	return false;
#else
	if (!agi::fs::FileExists(executable)) {
		diagnostic = "OCR runtime executable not found: " + executable.string();
		return false;
	}
	if (!agi::fs::DirectoryExists(models_dir)) {
		diagnostic = "OCR models directory not found: " + models_dir.string();
		return false;
	}
	if (!agi::fs::FileExists(config_path)) {
		diagnostic = "OCR config file not found: " + config_path.string();
		return false;
	}

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;

	HANDLE stdin_read = nullptr;
	if (!CreatePipe(&stdin_read, &impl->stdin_write, &sa, 0)) {
		diagnostic = "Failed to create OCR stdin pipe.";
		return false;
	}
	HANDLE stdout_write = nullptr;
	if (!CreatePipe(&impl->stdout_read, &stdout_write, &sa, 0)) {
		CloseHandle(stdin_read);
		impl->stdin_write = nullptr;
		diagnostic = "Failed to create OCR stdout pipe.";
		return false;
	}

	// The parent-side ends must not be inherited by the child.
	SetHandleInformation(impl->stdin_write, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(impl->stdout_read, HANDLE_FLAG_INHERIT, 0);

	HANDLE stderr_write = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
	                                  &sa, OPEN_EXISTING, 0, nullptr);

	STARTUPINFOW si;
	std::memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = stdin_read;
	si.hStdOutput = stdout_write;
	si.hStdError = stderr_write != INVALID_HANDLE_VALUE ? stderr_write : stdout_write;

	std::wstring command = L"\"" + executable.wstring() + L"\"";
	command += L" -models_path=\"" + models_dir.wstring() + L"\"";
	command += L" -config_path=\"" + config_path.wstring() + L"\"";
	command += L" -ensure_ascii=false";

	PROCESS_INFORMATION pi;
	std::memset(&pi, 0, sizeof(pi));
	if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, TRUE,
	                    CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		diagnostic = "Failed to start OCR runtime process.";
		CloseHandle(stdin_read);
		CloseHandle(stdout_write);
		CloseHandle(impl->stdin_write);
		CloseHandle(impl->stdout_read);
		impl->stdin_write = impl->stdout_read = nullptr;
		if (stderr_write != INVALID_HANDLE_VALUE)
			CloseHandle(stderr_write);
		return false;
	}

	CloseHandle(pi.hThread);
	impl->child = pi.hProcess;
	CloseHandle(stdin_read);
	CloseHandle(stdout_write);
	if (stderr_write != INVALID_HANDLE_VALUE)
		CloseHandle(stderr_write);

	impl->running = true;

	// The engine prints "OCR init completed." to stdout once the models are
	// loaded. Wait for it so callers never fire requests at a half-initialized
	// process and startup failures surface immediately instead of after the
	// first request. WaitForSingleObject cannot be used for this: on anonymous
	// pipe read handles it is signaled by write-completion well before the
	// bytes are visible to PeekNamedPipe, so poll PeekNamedPipe instead.
	std::string startup_output;
	const DWORD init_timeout_ms = 30000;
	DWORD elapsed = 0;
	bool initialized = false;
	while (!initialized && elapsed < init_timeout_ms) {
		DWORD available = 0;
		if (!PeekNamedPipe(impl->stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
			diagnostic = "Failed inspecting OCR runtime output during initialization.";
			Stop();
			return false;
		}

		if (available > 0) {
			char chunk[4096];
			DWORD got = 0;
			if (!ReadFile(impl->stdout_read, chunk, std::min<DWORD>(available, sizeof(chunk)),
			              &got, nullptr) || got == 0) {
				diagnostic = "Failed reading OCR runtime output during initialization.";
				Stop();
				return false;
			}
			startup_output.append(chunk, got);

			size_t pos;
			while ((pos = startup_output.find('\n')) != std::string::npos) {
				std::string line = startup_output.substr(0, pos);
				startup_output.erase(0, pos + 1);
				if (line.find("init completed") != std::string::npos) {
					initialized = true;
					break;
				}
			}
			continue;
		}

		if (WaitForSingleObject(impl->child, 0) == WAIT_OBJECT_0) {
			diagnostic = "OCR runtime exited during initialization.\n\n" + startup_output;
			Stop();
			return false;
		}

		Sleep(10);
		elapsed += 10;
	}

	if (!initialized) {
		diagnostic = "OCR runtime timed out while initializing.\n\n" + startup_output;
		Stop();
		return false;
	}

	return true;
#endif
}

bool OCRProcess::FindRuntime(agi::fs::path& executable, agi::fs::path& models_dir,
                             agi::fs::path& config_path, std::string& diagnostic) {
	diagnostic.clear();

	auto runtime_dir = config::path->Decode("?data/ocr");
	if (!agi::fs::DirectoryExists(runtime_dir)) {
		diagnostic = "OCR files are not installed. Reinstall Aegisub and select the OCR option to enable this feature.";
		return false;
	}

	executable = runtime_dir / "bin" / "PaddleOCR-json.exe";
	models_dir = runtime_dir / "models";
	config_path = models_dir / "config_ppocrv5.txt";

	if (!agi::fs::FileExists(executable) || !agi::fs::DirectoryExists(models_dir)
	    || !agi::fs::FileExists(config_path)) {
		diagnostic = "OCR files are missing or incomplete. Reinstall Aegisub and select the OCR option to repair this feature.";
		return false;
	}

	return true;
}

OCRResult OCRProcess::RunImage(agi::fs::path const& image_path, OCROptions const& options,
                               bool detect_only, std::function<bool()> const& cancel) {
	OCRResult result;

#ifndef _WIN32
	result.diagnostic = "Persistent OCR process is only available on Windows builds.";
	return result;
#else
	if (!impl->running || !impl->child || !impl->stdin_write || !impl->stdout_read) {
		result.diagnostic = "OCR process is not running.";
		return result;
	}

	std::wstring wide = image_path.wstring();
	std::string utf8_path;
	// wstring is UTF-16 on Windows; convert to UTF-8 for the JSON request.
	int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 0) {
		result.diagnostic = "Failed to convert the image path for the OCR process.";
		return result;
	}
	utf8_path.resize(needed - 1);
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8_path[0], needed, nullptr, nullptr);

	// Pipe-mode requests only carry the image path. det/rec/cls are startup
	// parameters: the engine ignores them in per-request JSON and always uses
	// the flags from the command line, so requesting "detect only" here would
	// be silently ignored. Callers approximate detection-only mode by checking
	// result.lines (recognition output is stripped below when requested).
	std::string json = "{\"image_path\":\"" + JsonEscape(utf8_path) + "\"}\n";

	DWORD written = 0;
	if (!WriteFile(impl->stdin_write, json.data(), static_cast<DWORD>(json.size()),
	               &written, nullptr) || written != json.size()) {
		result.diagnostic = "Failed to send image to OCR process.";
		return result;
	}

	std::string buffer;
	char chunk[4096];
	const DWORD timeout_ms = 60000;
	DWORD elapsed = 0;
	while (true) {
		if (cancel && cancel()) {
			result.diagnostic = "cancelled";
			// The pending response would corrupt the next request; retire the
			// process instead of leaving it in a desynchronized state.
			Stop();
			return result;
		}

		// Poll PeekNamedPipe rather than waiting on the pipe handle:
		// WaitForSingleObject on an anonymous pipe read handle is signaled by
		// write-completion before the bytes are actually readable, which used
		// to make every request fail with "closed its output". Only treat the
		// stream as closed when the child process has actually exited.
		DWORD available = 0;
		if (!PeekNamedPipe(impl->stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
			result.diagnostic = "Failed inspecting OCR process output.";
			Stop();
			return result;
		}

		if (available > 0) {
			DWORD to_read = std::min<DWORD>(available, sizeof(chunk));
			DWORD got = 0;
			if (!ReadFile(impl->stdout_read, chunk, to_read, &got, nullptr) || got == 0) {
				result.diagnostic = "Failed reading OCR process output.";
				Stop();
				return result;
			}
			buffer.append(chunk, got);

			size_t pos;
			while ((pos = buffer.find('\n')) != std::string::npos) {
				std::string line = TrimRight(buffer.substr(0, pos));
				buffer.erase(0, pos + 1);
				line = TrimLeft(line);
				if (line.empty())
					continue;
				if (line[0] != '{' && line[0] != '[')
					continue; // engine startup chatter

				result = ParsePaddleOCRJson(line, options);
				if (result.ok && detect_only) {
					for (auto& ocr_line : result.lines)
						ocr_line.text.clear();
					result.text.clear();
				}
				return result;
			}
			continue;
		}

		if (WaitForSingleObject(impl->child, 0) == WAIT_OBJECT_0) {
			result.diagnostic = "OCR process closed its output.";
			Stop();
			return result;
		}

		if (elapsed >= timeout_ms) {
			result.diagnostic = "OCR process timed out.";
			Stop();
			return result;
		}

		Sleep(10);
		elapsed += 10;
	}
#endif
}

void OCRProcess::Stop() {
#ifdef _WIN32
	if (impl->stdin_write) {
		// Ask the engine to exit gracefully; terminate if it ignores us.
		DWORD written = 0;
		WriteFile(impl->stdin_write, "exit\n", 5, &written, nullptr);
	}
	if (impl->child) {
		if (WaitForSingleObject(impl->child, 2000) != WAIT_OBJECT_0)
			TerminateProcess(impl->child, 0);
		CloseHandle(impl->child);
	}
	if (impl->stdin_write)
		CloseHandle(impl->stdin_write);
	if (impl->stdout_read)
		CloseHandle(impl->stdout_read);
	impl->child = nullptr;
	impl->stdin_write = nullptr;
	impl->stdout_read = nullptr;
#endif
	impl->running = false;
}

bool OCRProcess::IsRunning() const {
	return impl->running;
}

} // namespace ocr
