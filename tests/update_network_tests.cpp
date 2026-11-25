// Update network tests: the controller against a scripted loopback HTTP server. Every failure must end in a
// terminal state that offers a retry, and a retry must start clean.
// Run: sotb-update-network-tests

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <trg/download.h>

#include "updater/retry_policy.h"
#include "updater/sha256.h"
#include "updater/third_party/miniz/miniz.h"
#include "updater/update_controller.h"
#include "updater/update_paths.h"
#include "updater/update_platform.h"
#include "update_config.h"

using namespace trg::update;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
constexpr int kNoSignal = 0;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
constexpr int kNoSignal = MSG_NOSIGNAL;
void CloseSocket(Socket s) { close(s); }
#endif

bool Readable(Socket socket, int milliseconds) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(socket, &set);
  timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
  return select(int(socket) + 1, &set, nullptr, nullptr, &timeout) > 0;
}

struct Step {
  enum class Kind { kBody, kStatus, kRedirect, kCutShort, kStall };
  Kind kind = Kind::kBody;
  int status = 200;
  std::string body;
  std::string location;
};

Step Body(std::string body) { return {Step::Kind::kBody, 200, std::move(body), {}}; }
Step Status(int status) { return {Step::Kind::kStatus, status, "unavailable", {}}; }
Step Redirect(std::string location) { return {Step::Kind::kRedirect, 302, {}, std::move(location)}; }
Step CutShort(std::string body) { return {Step::Kind::kCutShort, 200, std::move(body), {}}; }
Step Stall(std::string body) { return {Step::Kind::kStall, 200, std::move(body), {}}; }

// Serves one connection at a time. A path's steps answer its requests in order; the last one repeats.
class Server {
 public:
  Server() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    listen(listener_, 8);
    socklen_t length = sizeof(address);
    getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] { Run(); });
  }
  ~Server() {
    stop_ = true;
    thread_.join();
    CloseSocket(listener_);
#ifdef _WIN32
    WSACleanup();
#endif
  }

  std::string Url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
  bool Allows(const std::string& url) const { return url.rfind(Url("/"), 0) == 0; }

  void Script(const std::string& path, std::vector<Step> steps) {
    std::lock_guard<std::mutex> lock(mutex_);
    scripts_[path] = std::move(steps);
    counts_[path] = 0;
  }
  int Requests(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return counts_[path];
  }
  std::string LastRequest(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return captured_[path];
  }

 private:
  void Run() {
    while (!stop_) {
      if (!Readable(listener_, 50)) {
        continue;
      }
      const Socket client = accept(listener_, nullptr, nullptr);
      if (client != kNoSocket) {
        Serve(client);
        CloseSocket(client);
      }
    }
  }

  void Send(Socket client, const std::string& text) { send(client, text.data(), int(text.size()), kNoSignal); }

  void Serve(Socket client) {
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && Readable(client, 2000)) {
      const int count = recv(client, buffer, sizeof(buffer), 0);
      if (count <= 0) {
        return;
      }
      request.append(buffer, size_t(count));
    }
    const size_t start = request.find(' ');
    const size_t end = request.find(' ', start + 1);
    if (start == std::string::npos || end == std::string::npos) {
      return;
    }
    const std::string path = request.substr(start + 1, end - start - 1);

    Step step = Status(404);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto script = scripts_.find(path);
      if (script != scripts_.end()) {
        const int index = counts_[path]++;
        captured_[path] = request;
        step = script->second[std::min<size_t>(size_t(index), script->second.size() - 1)];
      }
    }

    const auto head = [&](int status, size_t length, const std::string& extra) {
      return "HTTP/1.1 " + std::to_string(status) + " Scripted\r\nContent-Length: " + std::to_string(length) +
             "\r\nConnection: close\r\n" + extra + "\r\n";
    };
    switch (step.kind) {
      case Step::Kind::kBody:
      case Step::Kind::kStatus:
        Send(client, head(step.status, step.body.size(), "") + step.body);
        break;
      case Step::Kind::kRedirect:
        Send(client, head(step.status, 0, "Location: " + step.location + "\r\n"));
        break;
      case Step::Kind::kCutShort:
        Send(client, head(200, step.body.size(), "") + step.body.substr(0, step.body.size() / 2));
        break;
      case Step::Kind::kStall:
        Send(client, head(200, step.body.size(), "") + step.body.substr(0, step.body.size() / 2));
        while (!stop_ && Readable(client, 50) == false) {
        }
        break;
    }
  }

  Socket listener_ = kNoSocket;
  unsigned short port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::map<std::string, std::vector<Step>> scripts_;
  std::map<std::string, int> counts_;
  std::map<std::string, std::string> captured_;
};

class TempDir {
 public:
  TempDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("sotb-update-network-tests-" + std::to_string(now));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

std::string MakeZip(const std::string& name, const std::string& content) {
  mz_zip_archive archive{};
  if (mz_zip_writer_init_heap(&archive, 0, 0) == MZ_FALSE ||
      mz_zip_writer_add_mem(&archive, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION) == MZ_FALSE) {
    return {};
  }
  void* data = nullptr;
  size_t size = 0;
  std::string result;
  if (mz_zip_writer_finalize_heap_archive(&archive, &data, &size) != MZ_FALSE) {
    result.assign(static_cast<const char*>(data), size);
    mz_free(data);
  }
  mz_zip_writer_end(&archive);
  return result;
}

constexpr const char* kPackageName = "Package-v1.1.0.zip";
constexpr const char* kChecksumName = "Package-v1.1.0.zip.sha256";

struct Fixture {
  Server server;
  TempDir temp;
  std::string package = MakeZip("app.exe", "new launcher");
  std::mutex wait_mutex;
  std::vector<int> waits;

  std::vector<int> Waits() {
    std::lock_guard<std::mutex> lock(wait_mutex);
    return waits;
  }

  std::string ReleaseJson(bool with_digest) {
    const std::string digest = with_digest ? ",\"digest\":\"sha256:" + Sha256Hex(package) + "\"" : std::string();
    const std::string checksum = "{\"name\":\"" + std::string(kChecksumName) + "\",\"browser_download_url\":\"" +
                                 server.Url("/checksum") + "\",\"size\":100}";
    return "{\"tag_name\":\"v1.1.0\",\"draft\":false,\"prerelease\":false,\"html_url\":\"" + server.Url("/page") +
           "\",\"assets\":[{\"name\":\"" + kPackageName + "\",\"browser_download_url\":\"" + server.Url("/package") +
           "\",\"size\":" + std::to_string(package.size()) + digest + "}," + checksum + "]}";
  }

  UpdateConfig Config() {
    UpdateConfig config;
    config.current_version = "1.0.0";
    config.releases_url = server.Url("/release");
    config.user_agent = "update-network-tests";
    config.asset_name = [](const SemanticVersion&) { return std::string(kPackageName); };
    config.checksum_asset_name = [](const SemanticVersion&) { return std::string(kChecksumName); };
    config.install_dir = temp.path / "install";
    config.restart_executable = config.install_dir / "app.exe";
    config.required_staged_files = {"app.exe"};
    config.install_supported = true;
    config.url_filter = [this](const std::string& url) { return server.Allows(url); };
    config.connect_timeout_ms = 2000;
    config.receive_timeout_ms = 400;
    config.retry_wait = [this](int delay_ms, trg::Task& task) {
      std::lock_guard<std::mutex> lock(wait_mutex);
      waits.push_back(delay_ms);
      return !task.cancelled();
    };
    return config;
  }

  void ServeRelease(bool with_digest) { server.Script("/release", {Body(ReleaseJson(with_digest))}); }
  void ServeChecksum() { server.Script("/checksum", {Body(Sha256Hex(package) + "  " + kPackageName + "\n")}); }
};

bool Working(UpdateState state) {
  return state == UpdateState::kChecking || state == UpdateState::kDownloading || state == UpdateState::kVerifying;
}

// Polls like the launcher does until the controller leaves its working states, or the deadline passes.
UpdateSnapshot Settle(UpdateController& controller, int limit_ms = 15000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limit_ms);
  do {
    controller.Poll();
    if (!Working(controller.snapshot().state) && !controller.Busy()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < deadline);
  return controller.snapshot();
}

UpdateSnapshot CheckAndSettle(UpdateController& controller) {
  controller.StartCheck(true);
  return Settle(controller);
}

UpdateSnapshot DownloadAndSettle(UpdateController& controller) {
  controller.StartDownload();
  return Settle(controller);
}

UpdateSnapshot RetryAndSettle(UpdateController& controller) {
  controller.Retry();
  return Settle(controller);
}

int Workspaces() {
  int count = 0;
  const std::string prefix = "trg-update-" + std::to_string(CurrentProcessId()) + "-";
  std::error_code ec;
  for (std::filesystem::directory_iterator it(std::filesystem::temp_directory_path(), ec), end; !ec && it != end;
       it.increment(ec)) {
    if (Utf8Path(it->path().filename()).rfind(prefix, 0) == 0) {
      ++count;
    }
  }
  return count;
}

void ExpectFailedWithRetry(const UpdateSnapshot& snapshot) {
  CHECK(snapshot.state == UpdateState::kFailed);
  CHECK(snapshot.retry_available);
  CHECK(!snapshot.error.empty());
}

void TestReleaseCheckFailures() {
  std::printf("release check failures\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());

  fixture.server.Script("/release", {Status(504), Status(504), Status(504), Body(fixture.ReleaseJson(true))});
  UpdateSnapshot snapshot = CheckAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.retry_check);
  CHECK(snapshot.error.find("504") != std::string::npos);
  snapshot = RetryAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kAvailable);
  CHECK(snapshot.latest_version == "1.1.0");
  CHECK(fixture.server.Requests("/release") == 4);

  Fixture closed;
  UpdateController closed_controller(closed.Config());
  closed.server.Script("/release", {CutShort(closed.ReleaseJson(true)), CutShort(closed.ReleaseJson(true)), CutShort(closed.ReleaseJson(true)), Body(closed.ReleaseJson(true))});
  snapshot = CheckAndSettle(closed_controller);
  ExpectFailedWithRetry(snapshot);
  snapshot = RetryAndSettle(closed_controller);
  CHECK(snapshot.state == UpdateState::kAvailable);

  Fixture stalled;
  UpdateController stalled_controller(stalled.Config());
  stalled.server.Script("/release", {Stall(stalled.ReleaseJson(true)), Stall(stalled.ReleaseJson(true)), Stall(stalled.ReleaseJson(true)), Body(stalled.ReleaseJson(true))});
  const auto started = std::chrono::steady_clock::now();
  snapshot = CheckAndSettle(stalled_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
  snapshot = RetryAndSettle(stalled_controller);
  CHECK(snapshot.state == UpdateState::kAvailable);

  Fixture large;
  UpdateController large_controller(large.Config());
  large.server.Script("/release", {Body(std::string(size_t(5) << 20, ' '))});
  ExpectFailedWithRetry(CheckAndSettle(large_controller));
}

void TestChecksumFailures() {
  std::printf("checksum request failures\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());
  fixture.ServeRelease(false);
  fixture.server.Script("/package", {Body(fixture.package)});
  fixture.server.Script("/checksum",
                        {Status(504), Status(504), Status(504), Body(Sha256Hex(fixture.package) + "  " + kPackageName + "\n")});
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);

  const int workspaces = Workspaces();
  UpdateSnapshot snapshot = DownloadAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(!snapshot.retry_check);
  CHECK(snapshot.error.find("504") != std::string::npos);
  CHECK(fixture.server.Requests("/package") == 0);
  CHECK(Workspaces() == workspaces);

  snapshot = RetryAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);
  CHECK(fixture.server.Requests("/checksum") == 4);
}

void TestPackageFailures() {
  std::printf("package download failures\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());
  fixture.ServeRelease(true);
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);

  fixture.server.Script("/package", {Status(504), Status(504), Status(504), Body(fixture.package)});
  const int workspaces = Workspaces();
  UpdateSnapshot snapshot = DownloadAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("504") != std::string::npos);
  CHECK(snapshot.progress < 0.0f);
  CHECK(Workspaces() == workspaces);
  snapshot = RetryAndSettle(controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);

  Fixture closed;
  UpdateController closed_controller(closed.Config());
  closed.ServeRelease(true);
  CHECK(CheckAndSettle(closed_controller).state == UpdateState::kAvailable);
  closed.server.Script("/package", {CutShort(closed.package), CutShort(closed.package), CutShort(closed.package), Body(closed.package)});
  const int closed_workspaces = Workspaces();
  snapshot = DownloadAndSettle(closed_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(Workspaces() == closed_workspaces);
  snapshot = RetryAndSettle(closed_controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);
  CHECK(closed.server.Requests("/package") == 4);

  Fixture stalled;
  UpdateController stalled_controller(stalled.Config());
  stalled.ServeRelease(true);
  CHECK(CheckAndSettle(stalled_controller).state == UpdateState::kAvailable);
  stalled.server.Script("/package", {Stall(stalled.package), Stall(stalled.package), Stall(stalled.package), Body(stalled.package)});
  const auto started = std::chrono::steady_clock::now();
  snapshot = DownloadAndSettle(stalled_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(!stalled_controller.Busy());
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
  snapshot = RetryAndSettle(stalled_controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);

  Fixture oversized;
  UpdateController oversized_controller(oversized.Config());
  oversized.ServeRelease(true);
  CHECK(CheckAndSettle(oversized_controller).state == UpdateState::kAvailable);
  oversized.server.Script("/package", {Body(std::string(size_t(2) << 20, 'x'))});
  ExpectFailedWithRetry(DownloadAndSettle(oversized_controller));

  Fixture corrupt;
  UpdateController corrupt_controller(corrupt.Config());
  corrupt.ServeRelease(true);
  CHECK(CheckAndSettle(corrupt_controller).state == UpdateState::kAvailable);
  std::string altered = corrupt.package;
  altered.back() = static_cast<char>(altered.back() ^ 0x55);
  corrupt.server.Script("/package", {Body(altered)});
  snapshot = DownloadAndSettle(corrupt_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("SHA-256") != std::string::npos);
}

void TestRedirects() {
  std::printf("redirects\n");
  Fixture fixture;
  UpdateController controller(fixture.Config());
  fixture.ServeRelease(true);
  CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);

  fixture.server.Script("/package", {Redirect(fixture.server.Url("/asset"))});
  fixture.server.Script("/asset", {Body(fixture.package)});
  CHECK(DownloadAndSettle(controller).state == UpdateState::kReadyToRestart);

  Fixture foreign;
  UpdateController foreign_controller(foreign.Config());
  foreign.ServeRelease(true);
  CHECK(CheckAndSettle(foreign_controller).state == UpdateState::kAvailable);
  foreign.server.Script("/package", {Redirect("http://localhost:1/package")});
  UpdateSnapshot snapshot = DownloadAndSettle(foreign_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("not allowed") != std::string::npos);

  Fixture loop;
  UpdateController loop_controller(loop.Config());
  loop.ServeRelease(true);
  CHECK(CheckAndSettle(loop_controller).state == UpdateState::kAvailable);
  loop.server.Script("/package", {Redirect(loop.server.Url("/package"))});
  snapshot = DownloadAndSettle(loop_controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.error.find("redirected too many times") != std::string::npos);
  CHECK(loop.server.Requests("/package") == 6);

  Fixture malformed;
  UpdateController malformed_controller(malformed.Config());
  malformed.ServeRelease(true);
  CHECK(CheckAndSettle(malformed_controller).state == UpdateState::kAvailable);
  malformed.server.Script("/package", {Redirect("::not a url")});
  ExpectFailedWithRetry(DownloadAndSettle(malformed_controller));
}

void TestAddressPolicyInController() {
  std::printf("address policy in the controller\n");
  Fixture fixture;
  UpdateConfig config = fixture.Config();
  config.url_filter = nullptr;
  config.releases_url = "https://example.com/releases/latest";
  config.download_url_prefix = "https://github.com/saulob/shadow-of-the-beast-pc/releases/download/";
  UpdateController controller(config);
  const UpdateSnapshot snapshot = CheckAndSettle(controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(snapshot.retry_check);

  UpdateConfig http_config = fixture.Config();
  http_config.url_filter = nullptr;
  UpdateController http_controller(http_config);
  fixture.ServeRelease(true);
  ExpectFailedWithRetry(CheckAndSettle(http_controller));
  CHECK(fixture.server.Requests("/release") == 0);
}

class LogLines {
 public:
  std::function<void(std::string_view)> Sink() {
    return [this](std::string_view text) {
      std::lock_guard<std::mutex> lock(mutex_);
      lines_.emplace_back(text);
    };
  }
  bool Contains(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(lines_.begin(), lines_.end(),
                       [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }

 private:
  std::mutex mutex_;
  std::vector<std::string> lines_;
};

// GitHub's latest-release address answers 404 until the first release is published.
void TestNoReleasePublished() {
  std::printf("no release published yet\n");
  for (const bool manual : {false, true}) {
    Fixture fixture;
    fixture.server.Script("/release", {Status(404)});
    LogLines log;
    UpdateConfig config = fixture.Config();
    config.log = log.Sink();
    UpdateController controller(config);
    controller.StartCheck(manual);
    UpdateSnapshot snapshot = Settle(controller);
    CHECK(snapshot.state == UpdateState::kNoUpdate);
    CHECK(snapshot.no_release);
    CHECK(snapshot.error.empty());
    CHECK(!snapshot.retry_available);
    CHECK(fixture.server.Requests("/release") == 1);
    CHECK(log.Contains("No release has been published yet"));
    CHECK(!log.Contains("failed"));
    CHECK(!log.Contains("404"));
    controller.StartCheck(true);
    snapshot = Settle(controller);
    CHECK(snapshot.state == UpdateState::kNoUpdate);
    CHECK(fixture.server.Requests("/release") == 1);
  }

  Fixture published;
  published.server.Script("/release", {Status(404), Body(published.ReleaseJson(true))});
  UpdateController before(published.Config());
  CHECK(CheckAndSettle(before).state == UpdateState::kNoUpdate);
  UpdateController after(published.Config());
  const UpdateSnapshot found = CheckAndSettle(after);
  CHECK(found.state == UpdateState::kAvailable);
  CHECK(!found.no_release);
  CHECK(found.latest_version == "1.1.0");

  for (const int status : {403, 500}) {
    Fixture limited;
    limited.server.Script("/release", {Status(status)});
    LogLines log;
    UpdateConfig config = limited.Config();
    config.log = log.Sink();
    UpdateController quiet(config);
    quiet.StartCheck(false);
    const UpdateSnapshot automatic = Settle(quiet);
    CHECK(automatic.state == UpdateState::kIdle);
    CHECK(!automatic.no_release);
    CHECK(log.Contains("Update check failed"));
    CHECK(log.Contains(std::to_string(status)));
    UpdateController asked(limited.Config());
    const UpdateSnapshot requested = CheckAndSettle(asked);
    ExpectFailedWithRetry(requested);
    CHECK(!requested.no_release);
  }

  Fixture missing;
  missing.ServeRelease(true);
  missing.server.Script("/package", {Status(404)});
  UpdateController package_controller(missing.Config());
  CHECK(CheckAndSettle(package_controller).state == UpdateState::kAvailable);
  const UpdateSnapshot download = DownloadAndSettle(package_controller);
  ExpectFailedWithRetry(download);
  CHECK(download.error.find("404") != std::string::npos);
}

void TestProductionConfig() {
  std::printf("production update address and asset names\n");
  TempDir temp;
  const UpdateConfig config = Sotb::MakeUpdateConfig(temp.path);
  CHECK(config.releases_url == "https://api.github.com/repos/saulob/shadow-of-the-beast-pc/releases/latest");
  CHECK(config.download_url_prefix == "https://github.com/saulob/shadow-of-the-beast-pc/releases/download/");
  CHECK(config.current_version == SOTB_PORT_VERSION);
  CHECK(config.user_agent == std::string("ShadowOfTheBeastPC-Updater/") + SOTB_PORT_VERSION);
  CHECK(!config.url_filter);
  CHECK(UrlHostAllowed(config.releases_url, config.allowed_hosts));
  const std::optional<SemanticVersion> current = ParseVersion(SOTB_PORT_VERSION);
  CHECK(current && current->stable());

  const SemanticVersion next = *ParseVersion("0.2.0");
#ifdef _WIN32
  CHECK(config.package_kind == UpdatePackageKind::kZip);
  CHECK(config.asset_name(next) == "ShadowOfTheBeastPC-v0.2.0-windows-x64.zip");
  CHECK(config.checksum_asset_name(next) == "ShadowOfTheBeastPC-v0.2.0-windows-x64.zip.sha256");
  CHECK(config.restart_executable.filename() == "shadow_of_the_beast.exe");
#else
  CHECK(config.package_kind == UpdatePackageKind::kAppImage);
  CHECK(config.asset_name(next) == "ShadowOfTheBeastPC-v0.2.0-x86_64.AppImage");
  CHECK(config.checksum_asset_name(next) == "ShadowOfTheBeastPC-v0.2.0-x86_64.AppImage.sha256");
  CHECK(config.restart_executable == config.appimage_path);
#endif
  const std::string asset = "https://github.com/saulob/shadow-of-the-beast-pc/releases/download/v0.2.0/" + config.asset_name(next);
  CHECK(UrlAllowed(asset, config.allowed_hosts, config.download_url_prefix));
  CHECK(UrlHostAllowed("https://release-assets.githubusercontent.com/github-production-release-asset/1/2",
                       config.allowed_hosts));
  CHECK(!UrlAllowed("https://github.com/saulob/dragons-crown-pro-pc/releases/download/v0.2.0/" + config.asset_name(next),
                    config.allowed_hosts, config.download_url_prefix));
  CHECK(!UrlAllowed("http://github.com/saulob/shadow-of-the-beast-pc/releases/download/v0.2.0/" + config.asset_name(next),
                    config.allowed_hosts, config.download_url_prefix));
  CHECK(!UrlHostAllowed("https://github.com.example.com/saulob/shadow-of-the-beast-pc", config.allowed_hosts));
  CHECK(!UrlHostAllowed("https://user@github.com/saulob/shadow-of-the-beast-pc", config.allowed_hosts));
}

void TestTransportTimeouts() {
  std::printf("transport timeouts\n");
  Fixture fixture;
  trg::HttpOptions options;
  options.receive_timeout_ms = 300;
  options.total_timeout_ms = 3000;
  options.allow_url = [&](const std::string& url) { return fixture.server.Allows(url); };

  std::string out;
  fixture.server.Script("/slow", {Stall("0123456789")});
  const auto started = std::chrono::steady_clock::now();
  CHECK(!trg::HttpGet(fixture.server.Url("/slow"), out, nullptr, options).empty());
  CHECK(out.empty());
  const auto bound = std::chrono::milliseconds(options.total_timeout_ms + options.receive_timeout_ms * 2);
  CHECK(std::chrono::steady_clock::now() - started < bound);

  fixture.server.Script("/ok", {Body("hello")});
  CHECK(trg::HttpGet(fixture.server.Url("/ok"), out, nullptr, options).empty());
  CHECK(out == "hello");
  CHECK(!trg::HttpGet("http://127.0.0.1:1/ok", out, nullptr, options).empty());
}

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteAll(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::string RunDownload(const std::string& url, const std::filesystem::path& destination, const trg::HttpOptions& options,
                        trg::Task::State* state = nullptr, std::function<void(trg::Task&)> during = {}) {
  trg::Task task;
  task.Start("test", [&](trg::Task& running) { return trg::DownloadFile(running, url, destination.string(), options); });
  if (during) {
    during(task);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (task.running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::string error;
  const trg::Task::State result = task.Finish(&error);
  if (state) {
    *state = result;
  }
  return error;
}

void TestTransientClassification() {
  std::printf("transient status classification and report\n");
  Fixture fixture;
  trg::HttpOptions options;
  options.allow_url = [&](const std::string& url) { return fixture.server.Allows(url); };
  std::string out;
  for (const int status : {408, 429, 500, 502, 503, 504, 400, 401, 403, 404}) {
    const bool transient = status == 408 || status == 429 || status >= 500;
    fixture.server.Script("/status", {Status(status)});
    trg::HttpReport report;
    options.report = &report;
    CHECK(!trg::HttpGet(fixture.server.Url("/status"), out, nullptr, options).empty());
    CHECK(report.status == status);
    CHECK(report.transient == transient);
    CHECK(!report.cancelled);
  }

  fixture.server.Script("/redirect", {Redirect(fixture.server.Url("/ok"))});
  fixture.server.Script("/ok", {Body("hello")});
  trg::HttpReport report;
  options.report = &report;
  CHECK(trg::HttpGet(fixture.server.Url("/redirect"), out, nullptr, options).empty());
  CHECK(out == "hello");
  CHECK(report.status == 200);
  CHECK(report.bytes == 5);
  CHECK(report.expected == 5);
  CHECK(report.redirects == 1);
  CHECK(report.host == "127.0.0.1");
  CHECK(!report.transient);
}

void TestSensitiveHeadersAndLogs() {
  std::printf("sensitive headers on redirect and in logs\n");
  const std::string secret = "Bearer secret-token-5f3a";
  Server first;
  Server second;
  trg::HttpOptions options;
  options.headers = {{"Authorization", secret}, {"Cookie", "session=secret-cookie-7c1d"}, {"Accept", "text/plain"}};
  options.allow_url = [&](const std::string& url) { return first.Allows(url) || second.Allows(url); };
  std::vector<std::string> lines;
  options.log = [&](const std::string& text) { lines.push_back(text); };
  options.operation = "test";

  first.Script("/start", {Redirect(second.Url("/target"))});
  second.Script("/target", {Body("payload")});
  std::string out;
  CHECK(trg::HttpGet(first.Url("/start"), out, nullptr, options).empty());
  CHECK(out == "payload");
  CHECK(first.LastRequest("/start").find("secret-token-5f3a") != std::string::npos);
  CHECK(first.LastRequest("/start").find("secret-cookie-7c1d") != std::string::npos);
  const std::string redirected = second.LastRequest("/target");
  CHECK(!redirected.empty());
  CHECK(redirected.find("secret-token-5f3a") == std::string::npos);
  CHECK(redirected.find("secret-cookie-7c1d") == std::string::npos);
  CHECK(redirected.find("Authorization") == std::string::npos);
  for (const std::string& line : lines) {
    CHECK(line.find("secret-token-5f3a") == std::string::npos);
    CHECK(line.find("secret-cookie-7c1d") == std::string::npos);
  }

  lines.clear();
  Fixture fixture;
  options.allow_url = [&](const std::string& url) { return fixture.server.Allows(url); };
  fixture.server.Script("/big", {Body(std::string(size_t(1) << 20, 'x'))});
  CHECK(trg::HttpGet(fixture.server.Url("/big"), out, nullptr, options).empty());
  CHECK(out.size() == (size_t(1) << 20));
  CHECK(lines.size() <= 4);
}

void TestDestinationPreserved() {
  std::printf("download keeps the previous file on every failure\n");
  TempDir dir;
  const std::filesystem::path destination = dir.path / "package.zip";
  const std::filesystem::path partial = dir.path / "package.zip.part";
  Fixture fixture;
  trg::HttpOptions options;
  options.receive_timeout_ms = 300;
  options.total_timeout_ms = 3000;
  options.allow_url = [&](const std::string& url) { return fixture.server.Allows(url); };
  const std::string body(size_t(256) << 10, 'n');
  const auto intact = [&] { return ReadAll(destination) == "old" && !std::filesystem::exists(partial); };

  WriteAll(destination, "old");
  WriteAll(partial, "stale partial from an earlier run");
  fixture.server.Script("/file", {Status(503)});
  CHECK(!RunDownload(fixture.server.Url("/file"), destination, options).empty());
  CHECK(intact());

  fixture.server.Script("/file", {CutShort(body)});
  CHECK(!RunDownload(fixture.server.Url("/file"), destination, options).empty());
  CHECK(intact());

  fixture.server.Script("/file", {Stall(body)});
  CHECK(!RunDownload(fixture.server.Url("/file"), destination, options).empty());
  CHECK(intact());

  fixture.server.Script("/file", {Body(body)});
  options.max_bytes = 1000;
  CHECK(!RunDownload(fixture.server.Url("/file"), destination, options).empty());
  CHECK(intact());
  options.max_bytes = 0;

  CHECK(!RunDownload("http://127.0.0.1:1/file", destination, options).empty());
  CHECK(intact());

  fixture.server.Script("/file", {Stall(body)});
  trg::Task::State state = trg::Task::State::kIdle;
  options.receive_timeout_ms = 20000;
  options.total_timeout_ms = 0;
  const auto started = std::chrono::steady_clock::now();
  const std::string cancelled = RunDownload(fixture.server.Url("/file"), destination, options, &state, [](trg::Task& task) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    task.Cancel();
  });
  CHECK(state == trg::Task::State::kCancelled || !cancelled.empty());
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
  CHECK(intact());

  fixture.server.Script("/file", {Body(body)});
  CHECK(RunDownload(fixture.server.Url("/file"), destination, options).empty());
  CHECK(ReadAll(destination) == body);
  CHECK(!std::filesystem::exists(partial));
}

struct DownloadRun {
  Fixture fixture;
  std::unique_ptr<UpdateController> controller;
  std::vector<std::string> log_lines;
  std::mutex log_mutex;

  void Start(std::function<void(UpdateConfig&)> adjust = {}) {
    fixture.ServeRelease(true);
    UpdateConfig config = fixture.Config();
    config.log = [this](std::string_view text) {
      std::lock_guard<std::mutex> lock(log_mutex);
      log_lines.emplace_back(text);
    };
    if (adjust) {
      adjust(config);
    }
    controller = std::make_unique<UpdateController>(config);
    CHECK(CheckAndSettle(*controller).state == UpdateState::kAvailable);
  }
  bool Logged(const std::string& text) {
    std::lock_guard<std::mutex> lock(log_mutex);
    return std::any_of(log_lines.begin(), log_lines.end(),
                       [&](const std::string& line) { return line.find(text) != std::string::npos; });
  }
  int PackageRequests() { return fixture.server.Requests("/package"); }
};

void TestRetryPolicy() {
  std::printf("retry policy\n");
  CHECK(kMaxDownloadAttempts == 3);
  CHECK(RetryDelayMs(1) == 1000);
  CHECK(RetryDelayMs(2) == 2500);
  CHECK(RetryDelayMs(1) + RetryDelayMs(2) <= 5000);
  CHECK(ShouldRetry(1, true, false));
  CHECK(ShouldRetry(2, true, false));
  CHECK(!ShouldRetry(3, true, false));
  CHECK(!ShouldRetry(1, false, false));
  CHECK(!ShouldRetry(1, true, true));
}

void TestAutomaticRetries() {
  std::printf("automatic retries\n");
  {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {Body(run.fixture.package)});
    CHECK(DownloadAndSettle(*run.controller).state == UpdateState::kReadyToRestart);
    CHECK(run.PackageRequests() == 1);
    CHECK(run.fixture.Waits().empty());
  }
  for (const int status : {408, 429, 500, 502, 503, 504}) {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {Status(status), Body(run.fixture.package)});
    CHECK(DownloadAndSettle(*run.controller).state == UpdateState::kReadyToRestart);
    CHECK(run.PackageRequests() == 2);
    CHECK(run.fixture.Waits() == std::vector<int>({1000}));
    CHECK(run.Logged("attempt 1 of 3"));
    CHECK(run.Logged("HTTP " + std::to_string(status)));
    CHECK(run.Logged("retrying in 1000 ms"));
  }
  {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {CutShort(run.fixture.package), Body(run.fixture.package)});
    CHECK(DownloadAndSettle(*run.controller).state == UpdateState::kReadyToRestart);
    CHECK(run.PackageRequests() == 2);
  }
  {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {Status(503), Status(503), Body(run.fixture.package)});
    CHECK(DownloadAndSettle(*run.controller).state == UpdateState::kReadyToRestart);
    CHECK(run.PackageRequests() == 3);
    CHECK(run.fixture.Waits() == std::vector<int>({1000, 2500}));
  }
  {
    Fixture release;
    release.server.Script("/release", {Status(503), Body(release.ReleaseJson(true))});
    UpdateController controller(release.Config());
    CHECK(CheckAndSettle(controller).state == UpdateState::kAvailable);
    CHECK(release.server.Requests("/release") == 2);
    CHECK(release.Waits() == std::vector<int>({1000}));
  }
}

void TestPermanentFailuresDoNotRetry() {
  std::printf("permanent failures do not retry\n");
  for (const int status : {400, 401, 403, 404}) {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {Status(status), Body(run.fixture.package)});
    ExpectFailedWithRetry(DownloadAndSettle(*run.controller));
    CHECK(run.PackageRequests() == 1);
    CHECK(run.fixture.Waits().empty());
  }
  {
    DownloadRun run;
    run.Start();
    std::string altered = run.fixture.package;
    altered.back() = static_cast<char>(altered.back() ^ 0x55);
    run.fixture.server.Script("/package", {Body(altered), Body(run.fixture.package)});
    const UpdateSnapshot snapshot = DownloadAndSettle(*run.controller);
    ExpectFailedWithRetry(snapshot);
    CHECK(snapshot.error.find("SHA-256") != std::string::npos);
    CHECK(run.PackageRequests() == 1);
  }
  {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {Body(std::string(size_t(2) << 20, 'x')), Body(run.fixture.package)});
    ExpectFailedWithRetry(DownloadAndSettle(*run.controller));
    CHECK(run.PackageRequests() == 1);
  }
  {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {Redirect("http://localhost:1/package"), Body(run.fixture.package)});
    const UpdateSnapshot snapshot = DownloadAndSettle(*run.controller);
    ExpectFailedWithRetry(snapshot);
    CHECK(snapshot.error.find("not allowed") != std::string::npos);
    CHECK(run.PackageRequests() == 1);
    CHECK(run.fixture.Waits().empty());
  }
  {
    DownloadRun run;
    run.Start();
    run.fixture.server.Script("/package", {Redirect(run.fixture.server.Url("/package"))});
    ExpectFailedWithRetry(DownloadAndSettle(*run.controller));
    CHECK(run.PackageRequests() == 6);
    CHECK(run.fixture.Waits().empty());
  }
}

void TestRetryExhaustionAndManualRetry() {
  std::printf("retry exhaustion and manual retry\n");
  DownloadRun run;
  run.Start();
  const std::filesystem::path installed = run.fixture.temp.path / "install" / "app.exe";
  std::filesystem::create_directories(installed.parent_path());
  WriteAll(installed, "old");
  const int workspaces = Workspaces();

  run.fixture.server.Script("/package", {Status(503), Status(503), Status(503), Body(run.fixture.package)});
  UpdateSnapshot snapshot = DownloadAndSettle(*run.controller);
  ExpectFailedWithRetry(snapshot);
  CHECK(run.PackageRequests() == 3);
  CHECK(snapshot.error.find("503") != std::string::npos);
  CHECK(snapshot.error.find("Giving up after 3 attempts") != std::string::npos);
  CHECK(run.fixture.Waits() == std::vector<int>({1000, 2500}));
  CHECK(run.Logged("attempt 2 of 3"));
  CHECK(run.Logged("attempt 3 of 3"));
  CHECK(run.Logged("retries exhausted"));
  CHECK(ReadAll(installed) == "old");
  CHECK(Workspaces() == workspaces);

  snapshot = RetryAndSettle(*run.controller);
  CHECK(snapshot.state == UpdateState::kReadyToRestart);
  CHECK(run.PackageRequests() == 4);
  CHECK(run.fixture.Waits().size() == 2);
  CHECK(ReadAll(installed) == "old");
  CHECK(!run.Logged("Bearer"));
  CHECK(!run.Logged("Cookie"));
}

void TestRetrySequenceIsExclusive() {
  std::printf("no overlapping retry sequences\n");
  std::atomic<bool> waiting{false};
  std::atomic<bool> proceed{false};
  DownloadRun run;
  run.Start([&](UpdateConfig& config) {
    config.retry_wait = [&](int, trg::Task& task) {
      waiting = true;
      while (!proceed && !task.cancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      return !task.cancelled();
    };
  });
  run.fixture.server.Script("/package", {Status(503), Body(run.fixture.package)});
  run.controller->StartDownload();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!waiting && std::chrono::steady_clock::now() < deadline) {
    run.controller->Poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(waiting);
  CHECK(run.controller->Busy());
  run.controller->Poll();
  CHECK(run.controller->snapshot().state == UpdateState::kDownloading);
  CHECK(!run.controller->snapshot().retry_available);
  run.controller->StartDownload();
  run.controller->Retry();
  CHECK(run.PackageRequests() == 1);
  proceed = true;
  CHECK(Settle(*run.controller).state == UpdateState::kReadyToRestart);
  CHECK(run.PackageRequests() == 2);
}

void TestRetryCancellation() {
  std::printf("cancellation stops retries\n");
  {
    DownloadRun run;
    run.Start([](UpdateConfig& config) {
      config.retry_wait = [](int, trg::Task& task) {
        task.Cancel();
        return false;
      };
    });
    run.fixture.server.Script("/package", {Status(503), Body(run.fixture.package)});
    const UpdateSnapshot snapshot = DownloadAndSettle(*run.controller);
    CHECK(snapshot.state == UpdateState::kFailed);
    CHECK(run.PackageRequests() == 1);
    CHECK(!run.controller->Busy());
  }
  {
    DownloadRun run;
    run.Start([](UpdateConfig& config) { config.receive_timeout_ms = 20000; });
    run.fixture.server.Script("/package", {Stall(run.fixture.package), Body(run.fixture.package)});
    const auto started = std::chrono::steady_clock::now();
    run.controller->StartDownload();
    const auto deadline = started + std::chrono::seconds(10);
    while (run.PackageRequests() == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    run.controller->Cancel();
    const UpdateSnapshot snapshot = Settle(*run.controller);
    CHECK(snapshot.state == UpdateState::kFailed);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
    CHECK(run.PackageRequests() == 1);
    CHECK(run.fixture.Waits().empty());
    CHECK(!run.controller->Busy());
  }
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  TestReleaseCheckFailures();
  TestChecksumFailures();
  TestPackageFailures();
  TestRedirects();
  TestAddressPolicyInController();
  TestNoReleasePublished();
  TestProductionConfig();
  TestTransportTimeouts();
  TestTransientClassification();
  TestSensitiveHeadersAndLogs();
  TestDestinationPreserved();
  TestRetryPolicy();
  TestAutomaticRetries();
  TestPermanentFailuresDoNotRetry();
  TestRetryExhaustionAndManualRetry();
  TestRetrySequenceIsExclusive();
  TestRetryCancellation();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
