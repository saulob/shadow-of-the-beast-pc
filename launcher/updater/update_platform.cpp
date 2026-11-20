#include "updater/update_platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "updater/update_paths.h"

namespace trg::update {

unsigned long CurrentProcessId() {
  return GetCurrentProcessId();
}

std::vector<std::string> CurrentProcessArguments() {
  int count = 0;
  wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) {
    return {};
  }
  std::vector<std::string> result;
  for (int i = 1; i < count; ++i) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, arguments[i], -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
      result.emplace_back();
      continue;
    }
    std::string text(size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, arguments[i], -1, text.data(), length, nullptr, nullptr);
    text.resize(size_t(length - 1));
    result.push_back(std::move(text));
  }
  LocalFree(arguments);
  return result;
}

std::filesystem::path CurrentExecutablePath() {
  std::vector<wchar_t> buffer(1024);
  for (;;) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size()) {
      return std::filesystem::path(std::wstring(buffer.data(), length));
    }
    buffer.resize(buffer.size() * 2);
  }
}

bool HasOriginalAppImage(std::filesystem::path&) {
  return false;
}

namespace {

bool IsUpdateTestVariable(const wchar_t* entry) {
  const wchar_t* equals = std::wcschr(entry, L'=');
  if (!equals) {
    return false;
  }
  const int length = static_cast<int>(equals - entry);
  static constexpr const wchar_t* kNames[] = {
      L"SOTB_UPDATE_TEST", L"SOTB_UPDATE_TEST_CURRENT_VERSION", L"SOTB_UPDATE_TEST_RELEASE_JSON",
      L"SOTB_UPDATE_TEST_ASSET_SHA256"};
  for (const wchar_t* name : kNames) {
    if (CompareStringOrdinal(entry, length, name, static_cast<int>(std::wcslen(name)), TRUE) == CSTR_EQUAL) {
      return true;
    }
  }
  return false;
}

std::vector<wchar_t> FilteredEnvironment() {
  std::vector<wchar_t> result;
  LPWCH block = GetEnvironmentStringsW();
  if (!block) {
    return result;
  }
  for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
    if (IsUpdateTestVariable(entry)) {
      continue;
    }
    result.insert(result.end(), entry, entry + std::wcslen(entry) + 1);
  }
  FreeEnvironmentStringsW(block);
  result.push_back(L'\0');
  return result;
}

}  // namespace

bool LaunchApplication(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                       const std::filesystem::path& working_directory, std::string& error,
                       bool clear_update_test_environment) {
  auto quote = [](const std::wstring& argument) {
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
      return argument;
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : argument) {
      if (c == L'\\') {
        ++backslashes;
      } else if (c == L'"') {
        quoted.append(backslashes * 2 + 1, L'\\');
        quoted += L'"';
        backslashes = 0;
      } else {
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted += c;
      }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
  };

  std::wstring command_line = quote(executable.wstring());
  for (const std::string& argument : arguments) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, argument.c_str(), int(argument.size()), nullptr, 0);
    std::wstring wide(size_t(std::max(length, 0)), L'\0');
    if (length > 0) {
      MultiByteToWideChar(CP_UTF8, 0, argument.c_str(), int(argument.size()), wide.data(), length);
    }
    command_line += L' ';
    command_line += wide.empty() ? L"\"\"" : quote(wide);
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> environment;
  DWORD creation_flags = 0;
  LPVOID environment_block = nullptr;
  if (clear_update_test_environment) {
    environment = FilteredEnvironment();
    if (environment.empty()) {
      error = "Could not prepare the program environment.";
      return false;
    }
    environment_block = environment.data();
    creation_flags |= CREATE_UNICODE_ENVIRONMENT;
  }
  if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, creation_flags,
                      environment_block, working_directory.c_str(), &startup, &process)) {
    error = "Could not start the program (" + std::to_string(GetLastError()) + ").";
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool LaunchUpdateHelper(const std::filesystem::path& helper, const std::filesystem::path& job_file,
                        std::string& error) {
  if (!IsRegularFile(helper) || !IsRegularFile(job_file)) {
    error = "The update helper is missing from this installation.";
    return false;
  }
  return LaunchApplication(helper, {"--job", Utf8Path(job_file)}, job_file.parent_path(), error);
}

bool WaitForProcess(unsigned long pid, int timeout_ms, std::string& error) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
  if (!process) {
    if (GetLastError() == ERROR_INVALID_PARAMETER) {
      return true;
    }
    error = "The launcher process could not be opened.";
    return false;
  }
  const DWORD result = WaitForSingleObject(process, DWORD(timeout_ms));
  CloseHandle(process);
  if (result == WAIT_OBJECT_0) {
    return true;
  }
  error = result == WAIT_TIMEOUT ? "The launcher did not exit in time." : "Waiting for the launcher failed.";
  return false;
}

bool ApplyAppImageUpdate(const std::filesystem::path&, const std::filesystem::path&,
                         const std::vector<std::string>&, std::string& error) {
  error = "AppImage updates are not supported.";
  return false;
}

}  // namespace trg::update
