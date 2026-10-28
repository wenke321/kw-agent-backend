#include "function.h"

#include "helpers/log.h"

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <limits>
#endif

namespace kw {

namespace fs = std::filesystem;

/*-store_jsonstr-------------------------------------------------------*/

void sleep_backoff(int attempt) {
  std::this_thread::sleep_for(kRetryBackoff * (attempt + 1));
}

std::string make_temp_suffix() {
  static std::atomic<uint64_t> s_seq{0};
  const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
#ifdef _WIN32
  const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto seq = s_seq.fetch_add(1, std::memory_order_relaxed);
  return ".tmp." + std::to_string(pid) + "." + std::to_string(tid) + "." +
         std::to_string(ts) + "." + std::to_string(seq);
}

// 对已 fileno 的 fd 做 fsync，处理 EINTR
bool fsync_fd(int fd) {
#ifdef _WIN32
  const intptr_t osfh = _get_osfhandle(fd);
  if (osfh == -1) return false;
  HANDLE h = reinterpret_cast<HANDLE>(osfh);
  if (h == INVALID_HANDLE_VALUE) return false;
  return ::FlushFileBuffers(h) != 0;
#else
  for (;;) {
    if (::fsync(fd) == 0) return true;
    if (errno != EINTR) return false;
  }
#endif
}

// 关键修复：先 fflush 把 stdio 缓冲推进内核，再 fsync 落盘
bool flush_and_sync(std::FILE* fp) {
  if (std::fflush(fp) != 0) return false;
  const int fd = ::fileno(fp);
  if (fd < 0) return false;
  return fsync_fd(fd);
}

bool sync_directory(const fs::path& dir) {
#ifdef _WIN32
  (void)dir;
  return true;  // Windows 无对应稳定接口
#else
  int flags = O_RDONLY;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  int fd;
  do {
    fd = ::open(dir.c_str(), flags);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) return false;

  int saved = 0;
  bool ok = false;
  for (;;) {
    if (::fsync(fd) == 0) {
      ok = true;
      break;
    }
    if (errno != EINTR) {
      saved = errno;
      break;
    }
  }
  ::close(fd);

  // 部分 FS（tmpfs / 某些网络 FS / 容器 overlay）不支持目录 fsync
  if (!ok && saved == EINVAL) return true;
  if (!ok) errno = saved;
  return ok;
#endif
}

bool atomic_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
  for (int i = 0; i < kRetryCount; ++i) {
    if (::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
    const DWORD err = ::GetLastError();
    if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) {
      return false;
    }
    sleep_backoff(i);
  }
  return false;
#else
  for (int i = 0; i < kRetryCount; ++i) {
    if (::rename(from.c_str(), to.c_str()) == 0) return true;
    if (errno != EINTR && errno != EBUSY) return false;
    sleep_backoff(i);
  }
  return false;
#endif
}

bool is_valid_file_name(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  if (name.size() > kMaxFileNameLen) return false;
  if (name.find('/') != std::string::npos) return false;
#ifdef _WIN32
  if (name.find('\\') != std::string::npos) return false;
  if (name.find(':') != std::string::npos) return false;
#endif
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool store_jsonstr(const std::string& _file_name, const std::string& _jsonstr,
                   const std::string& _prefix) {
  // ---------- 参数校验 ----------
  if (!is_valid_file_name(_file_name)) {
    LOG_ERROR("store_jsonstr: invalid file name: '{}'", _file_name);
    return false;
  }
  if (_prefix.empty()) {
    LOG_ERROR("store_jsonstr: empty prefix, file_name={}", _file_name);
    return false;
  }

  // ---------- 准备目录 ----------
  std::error_code ec;
  const fs::path dir = fs::path(_prefix);
  fs::create_directories(dir, ec);
  if (ec) {
    LOG_ERROR("store_jsonstr: create_directories failed, prefix={}, err={}",
              _prefix, ec.message());
    return false;
  }

  const fs::path target = dir / (_file_name + ".json");
  const fs::path tmp = dir / (_file_name + ".json" + make_temp_suffix());

  // ---------- 写临时文件 ----------
  std::FILE* fp = nullptr;
#ifdef _WIN32
  fp = ::_wfopen(tmp.wstring().c_str(), L"wb");
#else
  fp = std::fopen(tmp.c_str(), "wb");
#endif
  if (!fp) {
    const int e = errno;
    LOG_ERROR("store_jsonstr: fopen temp failed, path={}, errno={} ({})",
              tmp.string(), e, std::strerror(e));
    return false;
  }

  bool ok = true;

  if (!_jsonstr.empty()) {
    std::size_t written = 0;
    while (written < _jsonstr.size()) {
      const std::size_t n = std::fwrite(_jsonstr.data() + written, 1,
                                        _jsonstr.size() - written, fp);
      if (n == 0) {
        const int e = errno;
        LOG_ERROR(
            "store_jsonstr: fwrite failed, path={}, wrote={}/{}, errno={}",
            tmp.string(), written, _jsonstr.size(), e);
        ok = false;
        break;
      }
      written += n;
    }
  }

  if (ok && !flush_and_sync(fp)) {
    const int e = errno;
    LOG_ERROR("store_jsonstr: fflush/fsync failed, path={}, errno={} ({})",
              tmp.string(), e, std::strerror(e));
    ok = false;
  }

  if (std::fclose(fp) != 0) {
    const int e = errno;
    LOG_ERROR("store_jsonstr: fclose failed, path={}, errno={} ({})",
              tmp.string(), e, std::strerror(e));
    ok = false;
  }

  if (!ok) {
    std::error_code rmec;
    fs::remove(tmp, rmec);
    return false;
  }

  // ---------- 原子替换 ----------
  if (!atomic_replace(tmp, target)) {
#ifdef _WIN32
    const long long e = static_cast<long long>(::GetLastError());
#else
    const long long e = errno;
#endif
    LOG_ERROR("store_jsonstr: atomic replace failed, tmp={}, target={}, err={}",
              tmp.string(), target.string(), e);
    std::error_code rmec;
    fs::remove(tmp, rmec);
    return false;
  }

  // ---------- 持久化目录项 ----------
  if (!sync_directory(dir)) {
    const int e = errno;
    LOG_ERROR(
        "store_jsonstr: directory fsync failed (rename may not be durable), "
        "dir={}, errno={}",
        dir.string(), e);
    // 数据本身已落盘；此处仅目录项持久性打折，不算失败
  }

  return true;
}
/*-store_jsonstr-------------------------------------------------------*/

/*-load_jsonstr-------------------------------------------------------*/

void strip_utf8_bom(std::string& s) noexcept {
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    s.erase(0, 3);
  }
}

// JSON 标准要求 UTF-8；遇到 UTF-16/UTF-32 BOM
// 提前拒绝，避免解析器报无意义的错误
bool has_utf16_or_utf32_bom(const std::string& s) noexcept {
  const auto b0 = s.size() > 0 ? static_cast<unsigned char>(s[0]) : 0;
  const auto b1 = s.size() > 1 ? static_cast<unsigned char>(s[1]) : 0;
  const auto b2 = s.size() > 2 ? static_cast<unsigned char>(s[2]) : 0;
  const auto b3 = s.size() > 3 ? static_cast<unsigned char>(s[3]) : 0;

  // UTF-16 BE / LE
  if ((b0 == 0xFE && b1 == 0xFF) || (b0 == 0xFF && b1 == 0xFE)) return true;

  // UTF-32 BE / LE
  if ((b0 == 0x00 && b1 == 0x00 && b2 == 0xFE && b3 == 0xFF) ||
      (b0 == 0xFF && b1 == 0xFE && b2 == 0x00 && b3 == 0x00))
    return true;

  return false;
}

// 读取 JSON 文本文件。
// 成功：返回 true，_out_json 为剥离 UTF-8 BOM 后的内容，_err 为空。
// 失败：返回 false，_out_json 清空，_err 为错误原因短句。
bool load_jsonstr(const std::string& _file_path, std::string& _out_json,
                  std::string& _err) {
  _err.clear();
  _out_json.clear();

  try {
    if (_file_path.empty()) {
      _err = "empty file path";
      LOG_ERROR("load_jsonstr: {}", _err);
      return false;
    }

    std::error_code ec;
    const fs::path path(_file_path);

    // ---- 1. 文件状态检查 ----
    const fs::file_status st = fs::status(path, ec);
    if (ec) {
      _err = "stat failed: " + ec.message();
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (!fs::exists(st)) {
      _err = "file not found";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (!fs::is_regular_file(st)) {
      _err = "not a regular file";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

    // ---- 2. 初始大小检查（仅作快速失败，真正上限在读取循环里再兜底） ----
    const std::uintmax_t file_size = fs::file_size(path, ec);
    if (ec) {
      _err = "get size failed: " + ec.message();
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (file_size == 0) {
      _err = "empty file";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }
    if (file_size > kMaxJsonFileSize) {
      _err = "file too large";
      LOG_ERROR("load_jsonstr: {} ('{}', {} bytes, limit {} bytes)", _err,
                _file_path, file_size, kMaxJsonFileSize);
      return false;
    }

    // ---- 3. 二进制打开，避免 Windows CRLF 转换 ----
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs) {
      _err = "open failed";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

    // ---- 4. 分块读取；即使文件在 stat 之后变大，也不会突破大小上限 ----
    std::string content;
    content.reserve(static_cast<std::size_t>(file_size));

    std::array<char, kReadChunkSize> buf{};
    for (;;) {
      ifs.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const std::streamsize got = ifs.gcount();

      if (got > 0) {
        const auto uGot = static_cast<std::uintmax_t>(got);
        if (static_cast<std::uintmax_t>(content.size()) + uGot >
            kMaxJsonFileSize) {
          _err = "file too large while reading";
          LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
          return false;
        }
        content.append(buf.data(), static_cast<std::size_t>(got));
      }

      if (ifs.bad()) {
        _err = "io error while reading";
        LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
        return false;
      }
      if (ifs.eof()) break;
      if (ifs.fail()) {
        _err = "read failed";
        LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
        return false;
      }
    }

    if (content.empty()) {
      _err = "empty content";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

    // ---- 5. 拒绝 UTF-16/UTF-32，剥离 UTF-8 BOM ----
    if (has_utf16_or_utf32_bom(content)) {
      _err = "unsupported encoding (utf-16/utf-32 bom)";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

    strip_utf8_bom(content);

    if (content.empty()) {
      _err = "empty content after bom strip";
      LOG_ERROR("load_jsonstr: {} ('{}')", _err, _file_path);
      return false;
    }

    _out_json = std::move(content);
    return true;
  } catch (const std::exception& e) {
    _err = std::string("exception: ") + e.what();
    LOG_ERROR("load_jsonstr: exception while loading '{}': {}", _file_path,
              e.what());
    _out_json.clear();
    return false;
  } catch (...) {
    _err = "unknown exception";
    LOG_ERROR("load_jsonstr: unknown exception while loading '{}'", _file_path);
    _out_json.clear();
    return false;
  }
}
/*-load_jsonstr-------------------------------------------------------*/

/*-exec_cmd-------------------------------------------------------*/

#if defined(__linux__)
// extern char** environ;

// ---------- syscall / 常量 兼容 ----------
#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif
#ifndef __NR_pidfd_open
#ifdef SYS_pidfd_open
#define __NR_pidfd_open SYS_pidfd_open
#else
#define __NR_pidfd_open 434
#endif
#endif

constexpr std::size_t kMaxStreamBytes = 8u * 1024u * 1024u;
constexpr std::size_t kReadChunk = 64u * 1024u;
constexpr int kDrainGraceMs = 500;
constexpr int kKillWaitMs = 2000;
constexpr int kFinalReapMs = 500;
constexpr int kMaxPollWaitMs = 100;

// ---------------------------------------------------------------------------
static std::string cmd_for_log(const std::string& cmd) {
  constexpr std::size_t kMax = 256;
  if (cmd.size() <= kMax) return cmd;
  return cmd.substr(0, kMax) + "...(truncated)";
}

static std::string errno_string(int e) {
  char buf[128];
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  return std::string(::strerror_r(e, buf, sizeof(buf)));
#else
  const int r = ::strerror_r(e, buf, sizeof(buf));
  if (r == 0) return std::string(buf);
  return "errno=" + std::to_string(e);
#endif
}

static std::int64_t monotonic_ms() {
  struct timespec ts;
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 +
         static_cast<std::int64_t>(ts.tv_nsec) / 1000000;
}
static std::int64_t add_ms_saturating(std::int64_t base, int ms) {
  if (base < 0) return -1;
  if (ms < 0) return base;
  const auto maxv = std::numeric_limits<std::int64_t>::max();
  if (base > maxv - ms) return maxv;
  return base + ms;
}

static bool ensure_stdio_open() {
  for (int fd = 0; fd < 3; ++fd) {
    if (::fcntl(fd, F_GETFD) != -1) continue;
    if (errno != EBADF) continue;
    int nul;
    do {
      nul = ::open("/dev/null", O_RDWR);
    } while (nul < 0 && errno == EINTR);
    if (nul < 0) return false;
    if (nul != fd) {
      if (::dup2(nul, fd) < 0) {
        ::close(nul);
        return false;
      }
      ::close(nul);
    }
  }
  return true;
}

static void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}
static void set_nonblock(int fd) {
  int fl = ::fcntl(fd, F_GETFL);
  if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
static void report_exec_errno(int fd, int err) noexcept {
  ssize_t n;
  do {
    n = ::write(fd, &err, sizeof(err));
  } while (n < 0 && errno == EINTR);
  (void)n;
}

static void close_inherited_fds(int keep_fd, long max_fd) noexcept {
  // #if defined(__linux__)
  bool ok = false;
  if (keep_fd >= 3) {
    if (keep_fd > 3) {
      if (::syscall(__NR_close_range, 3U, (unsigned)keep_fd - 1U, 0U) == 0)
        ok = true;
    } else
      ok = true;
    if (ok) {
      if (::syscall(__NR_close_range, (unsigned)keep_fd + 1U, ~0U, 0U) == 0)
        return;
    }
  }
  // #endif
  //   for (long fd = 3; fd < max_fd; ++fd) {
  //     if (fd == keep_fd) continue;
  //     ::close((int)fd);
  //   }
}

static void drain_stream(int fd, std::string& out, bool& eof, bool& truncated,
                         int& read_errno, char* buf, std::size_t buf_sz) {
  if (eof || fd < 0) {
    eof = true;
    return;
  }
  for (;;) {
    ssize_t n = ::read(fd, buf, buf_sz);
    if (n > 0) {
      std::size_t take = (std::size_t)n;
      if (out.size() < kMaxStreamBytes) {
        std::size_t room = kMaxStreamBytes - out.size();
        if (take > room) {
          take = room;
          truncated = true;
        }
        out.append(buf, take);
      } else
        truncated = true;
      continue;
    }
    if (n == 0) {
      eof = true;
      return;
    }
    const int e = errno;
    if (e == EINTR) continue;
    if (e == EAGAIN || e == EWOULDBLOCK) return;
    read_errno = e;
    eof = true;
    return;
  }
}

// =============================================================================
// 子进程：只调用 async-signal-safe 函数，不构造/析构任何 C++ 对象
// =============================================================================
[[noreturn]]
static void child_run(const char* cmd_cstr, int devnull_rd, int out_wr,
                      int err_wr, int exec_wr, long max_fd) noexcept {
  // 1. 先全阻塞
  sigset_t all_mask;
  ::sigfillset(&all_mask);
  ::sigprocmask(SIG_SETMASK, &all_mask, nullptr);

  // 2. 重置 SIG_IGN → SIG_DFL（execve 保留 SIG_IGN 状态，不重置会污染 sh）
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  ::sigemptyset(&dfl.sa_mask);
  for (int sig = 1; sig < NSIG; ++sig) {
    if (sig == SIGKILL || sig == SIGSTOP) continue;
    struct sigaction cur;
    if (::sigaction(sig, nullptr, &cur) == 0 && cur.sa_handler == SIG_IGN)
      ::sigaction(sig, &dfl, nullptr);
  }

  // debug
  //   {
  //     char buf[256];
  //     std::snprintf(buf, sizeof(buf),
  //                   "child_run: pid=%d ppid=%d about to set PDEATHSIG\n",
  //                   (int)::getpid(), (int)::getppid());
  //     (void)!::write(STDERR_FILENO, buf, std::strlen(buf));
  //   }
  // debug

  //  3. PR_SET_PDEATHSIG：父进程崩溃时 kill 本进程，防孤儿
  if (::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0) {
    // int e = errno;
    // char buf[256];
    // std::snprintf(buf, sizeof(buf), "child_run: prctl(PDEATHSIG) failed:
    // %d\n",
    //               e);
    // (void)!::write(STDERR_FILENO, buf, std::strlen(buf));
    report_exec_errno(exec_wr, errno);
    _exit(127);
  }
  // prctl 与 getppid 之间父进程可能已死，PDEATHSIG 事件已发生不会补发
  if (::getppid() == 1) _exit(127);

  // 4. setpgid：失败上报，否则超时 kill(-pid) 打空
  if (::setpgid(0, 0) != 0) {
    report_exec_errno(exec_wr, errno);
    _exit(127);
  }

  // 5. stdin → /dev/null；stdout/stderr → pipes
  if (::dup2(devnull_rd, STDIN_FILENO) < 0) {
    report_exec_errno(exec_wr, errno);
    _exit(127);
  }
  if (devnull_rd > 2) ::close(devnull_rd);

  if (::dup2(out_wr, STDOUT_FILENO) < 0) {
    report_exec_errno(exec_wr, errno);
    _exit(127);
  }
  if (out_wr > 2) ::close(out_wr);

  if (::dup2(err_wr, STDERR_FILENO) < 0) {
    report_exec_errno(exec_wr, errno);
    _exit(127);
  }
  if (err_wr > 2) ::close(err_wr);

  // 6. 关闭其它继承 fd（保留 exec_wr 用于上报）
  close_inherited_fds(exec_wr, max_fd);

  // 7. 解除信号阻塞（execve 会保留 mask，必须此刻清空）
  sigset_t empty;
  ::sigemptyset(&empty);
  ::sigprocmask(SIG_SETMASK, &empty, nullptr);

  // 8. execve 多候选路径（argv[0] 用 "sh" 让脚本 $0 为 sh）
  char* const argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                        const_cast<char*>(cmd_cstr), nullptr};
  static const char* const kShells[] = {"/bin/sh", "/usr/bin/sh",
                                        "/bin/busybox"};
  for (const char* shell : kShells) {
    ::execve(shell, argv, environ);
    if (errno != ENOENT) break;
  }
  report_exec_errno(exec_wr, errno);
  _exit(127);
}

static void append_replacement(std::string& s) { s.append("\xEF\xBF\xBD", 3); }

// truncated == true: 末尾不完整但前缀合法的序列静默丢弃（输入确实被截断）
// truncated == false: 末尾不完整序列替换为一个 U+FFFD（输入应完整，视为损坏）
[[nodiscard]] static std::string sanitize_utf8(const std::string& in,
                                               bool truncated) {
  std::string out;
  out.reserve(in.size());

  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  const std::size_t n = in.size();
  std::size_t i = 0;

  while (i < n) {
    // ASCII 快速路径：批量拷贝
    if (p[i] < 0x80) {
      std::size_t j = i + 1;
      while (j < n && p[j] < 0x80) ++j;
      out.append(reinterpret_cast<const char*>(p + i), j - i);
      i = j;
      continue;
    }

    const unsigned char c = p[i];
    std::size_t len = 0;
    unsigned char lo = 0x80, hi = 0xBF;

    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
    } else if (c == 0xE0) {
      len = 3;
      lo = 0xA0;  // 排除 overlong
    } else if (c >= 0xE1 && c <= 0xEC) {
      len = 3;
    } else if (c == 0xED) {
      len = 3;
      hi = 0x9F;  // 排除 surrogate
    } else if (c >= 0xEE && c <= 0xEF) {
      len = 3;
    } else if (c == 0xF0) {
      len = 4;
      lo = 0x90;  // 排除 overlong
    } else if (c >= 0xF1 && c <= 0xF3) {
      len = 4;
    } else if (c == 0xF4) {
      len = 4;
      hi = 0x8F;  // 排除 > U+10FFFF
    } else {
      append_replacement(out);  // 孤立 continuation / C0/C1 / F5-FF
      ++i;
      continue;
    }

    // 先验证已有后续字节，再判断是否截断，避免吞掉非 continuation 字节
    std::size_t k = 1;
    for (; k < len && i + k < n; ++k) {
      const unsigned char b = p[i + k];
      const unsigned char l = (k == 1) ? lo : 0x80;
      const unsigned char h = (k == 1) ? hi : 0xBF;
      if (b < l || b > h) break;
    }

    if (k == len) {
      // 完整合法序列
      out.append(reinterpret_cast<const char*>(p + i), len);
      i += len;
    } else if (i + k == n) {
      // 已到末尾且前缀合法 → 真正的截断
      if (!truncated) append_replacement(out);
      break;
    } else {
      // p[i+k] 非法：按 maximal subpart 替换 [i, i+k)
      append_replacement(out);
      i += k;
    }
  }
  return out;
}

// =============================================================================
command_result exec_cmd(const std::string& cmd, int timeout_ms) {
  command_result res;
  res.ok = false;
  res.exit_code = -1;
  res.term_signal = 0;
  res.timed_out = false;
  res.output_truncated = false;

  const std::string cmd_log = cmd_for_log(cmd);

  if (cmd.find('\0') != std::string::npos) {
    res.sys_error = "command contains embedded NUL byte";
    LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
    return res;
  }
  if (!ensure_stdio_open()) {
    res.sys_error = std::string("ensure stdio: ") + errno_string(errno);
    LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
    return res;
  }

  // [新增] 快速失败：clock_gettime 不可用不应该静默丢掉超时
  std::int64_t now0 = monotonic_ms();
  if (now0 < 0) {
    res.sys_error = "clock_gettime(CLOCK_MONOTONIC) failed";
    LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
    return res;
  }

  long max_fd = 65536;
  {
    struct rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
      constexpr rlim_t kCap = 1u << 20;
      max_fd = (long)std::min<rlim_t>(rl.rlim_cur, kCap);
      if (max_fd < 3) max_fd = 3;
    }
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int exec_pipe[2] = {-1, -1};
  int devnull_fd = -1;
  int pidfd = -1;

  auto close_pipe = [](int p[2]) {
    if (p[0] >= 0) {
      ::close(p[0]);
      p[0] = -1;
    }
    if (p[1] >= 0) {
      ::close(p[1]);
      p[1] = -1;
    }
  };
  auto close_all = [&]() {
    close_pipe(out_pipe);
    close_pipe(err_pipe);
    close_pipe(exec_pipe);
    if (devnull_fd >= 0) {
      ::close(devnull_fd);
      devnull_fd = -1;
    }
    if (pidfd >= 0) {
      ::close(pidfd);
      pidfd = -1;
    }
  };

  // [新增] 预打开 /dev/null，子进程无需 open
  devnull_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (devnull_fd < 0) {
    res.sys_error = std::string("open(/dev/null): ") + errno_string(errno);
    LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
    return res;
  }

  if (::pipe2(out_pipe, O_CLOEXEC) != 0 || ::pipe2(err_pipe, O_CLOEXEC) != 0 ||
      ::pipe2(exec_pipe, O_CLOEXEC) != 0) {
    res.sys_error = std::string("pipe2: ") + errno_string(errno);
    LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
    close_all();
    return res;
  }

  const char* cmd_cstr = cmd.c_str();

  // ---------- 创建子进程 ----------
  pid_t pid = ::fork();
  if (pid < 0) {
    const int e = errno;
    res.sys_error = std::string("fork: ") + errno_string(e);
    LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
    close_all();
    return res;
  }

  if (pid == 0) {
    // 子进程逻辑不变（child_run）
    child_run(cmd_cstr, devnull_fd, out_pipe[1], err_pipe[1], exec_pipe[1],
              max_fd);
    // child_run 是 [[noreturn]]，不会到这里
  }

  // ============ 父进程 ============
  pidfd = (int)::syscall(__NR_pidfd_open, pid, 0);
  if (pidfd < 0) {
    const int e = errno;
    if (e == ESRCH) {
      // 子进程在 fork 和 pidfd_open 之间已退出且被 autoreap
      // （通常是调用方 SIGCHLD=SIG_IGN/SA_NOCLDWAIT）
      LOG_ERROR(
          "exec_cmd: pidfd_open({}) ESRCH — child already reaped; "
          "this usually means SIGCHLD is SIG_IGN or SA_NOCLDWAIT "
          "[cmd={}]",
          pid, cmd_log);
      // pidfd 保持 -1，走 waitpid 轮询（大概率也拿不到）
    } else if (e == ENOSYS) {
      LOG_ERROR(
          "exec_cmd: pidfd_open unavailable, falling back to "
          "waitpid polling [cmd={}]",
          cmd_log);
    } else {
      LOG_ERROR(
          "exec_cmd: pidfd_open({}) failed: {}, using waitpid "
          "[cmd={}]",
          pid, errno_string(e), cmd_log);
    }
    // 继续，reap_nonblocking 里 pidfd<0 会走 waitpid 分支
  }
  if (::setpgid(pid, pid) != 0) {
    const int e = errno;
    if (e != EACCES && e != ESRCH && e != EINVAL && e != EPERM)
      LOG_ERROR("exec_cmd: setpgid({},{}) failed: {}", pid, pid,
                errno_string(e));
  }

  close_fd(out_pipe[1]);
  close_fd(err_pipe[1]);
  close_fd(exec_pipe[1]);
  if (devnull_fd >= 0) {
    ::close(devnull_fd);
    devnull_fd = -1;
  }
  set_nonblock(out_pipe[0]);
  set_nonblock(err_pipe[0]);
  set_nonblock(exec_pipe[0]);

  std::string out_buf, err_buf;
  out_buf.reserve(std::min<std::size_t>(kMaxStreamBytes, 64 * 1024));
  err_buf.reserve(std::min<std::size_t>(kMaxStreamBytes, 64 * 1024));

  static thread_local std::vector<char> tls_read_buf;
  if (tls_read_buf.size() < kReadChunk) tls_read_buf.resize(kReadChunk);
  char* const read_buf = tls_read_buf.data();
  const std::size_t read_buf_sz = tls_read_buf.size();

  bool out_eof = false, err_eof = false, exec_done = false;
  bool child_exited = false, child_status_known = false;
  int child_exit_code = -1, child_term_signal = 0;
  bool killed = false;
  bool out_truncated = false, err_truncated = false;
  int out_read_errno = 0, err_read_errno = 0;

  std::int64_t deadline =
      (timeout_ms > 0) ? add_ms_saturating(now0, timeout_ms) : -1;
  std::int64_t grace_deadline = -1;
  std::int64_t giveup_deadline = -1;

  auto reap_nonblocking = [&]() {
    if (child_exited) return;
    if (pidfd >= 0) {
      siginfo_t si{};
      int r = ::waitid(P_PIDFD, pidfd, &si, WEXITED | WNOHANG);
      if (r == 0) {
        if (si.si_pid == pid) {
          child_exited = true;
          child_status_known = true;
          if (si.si_code == CLD_EXITED)
            child_exit_code = si.si_status;
          else if (si.si_code == CLD_KILLED || si.si_code == CLD_DUMPED)
            child_term_signal = si.si_status;
        }
      } else if (errno == ECHILD) {
        child_exited = true;
        LOG_ERROR(
            "exec_cmd: waitid(P_PIDFD) ECHILD pid={} — child "
            "reaped externally (SIGCHLD=SIG_IGN / waitpid(-1) "
            "conflict) [cmd={}]",
            pid, cmd_log);
      }
    } else {
      // fallback: 无 pidfd，轮询 waitpid
      int status = 0;
      pid_t r = ::waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        child_exited = true;
        child_status_known = true;
        if (WIFEXITED(status))
          child_exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
          child_term_signal = WTERMSIG(status);
      } else if (r < 0 && errno == ECHILD) {
        child_exited = true;
        LOG_ERROR(
            "exec_cmd: waitpid ECHILD pid={} (SIGCHLD=SIG_IGN "
            "or concurrent wait) [cmd={}]",
            pid, cmd_log);
      }
    }
  };

  auto force_kill = [&]() {
    if (killed) return;
    killed = true;
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
  };
  auto drain_out = [&](bool& eof) {
    if (eof || out_pipe[0] < 0) {
      eof = true;
      return;
    }
    drain_stream(out_pipe[0], out_buf, eof, out_truncated, out_read_errno,
                 read_buf, read_buf_sz);
    if (eof) close_fd(out_pipe[0]);
  };
  auto drain_err = [&](bool& eof) {
    if (eof || err_pipe[0] < 0) {
      eof = true;
      return;
    }
    drain_stream(err_pipe[0], err_buf, eof, err_truncated, err_read_errno,
                 read_buf, read_buf_sz);
    if (eof) close_fd(err_pipe[0]);
  };

  for (;;) {
    reap_nonblocking();
    const std::int64_t now = monotonic_ms();

    if (giveup_deadline > 0 && now >= giveup_deadline) {
      if (!child_exited) {
        LOG_ERROR("exec_cmd: giving up pid={} after SIGKILL [cmd={}]", pid,
                  cmd_log);
        if (res.sys_error.empty())
          res.sys_error = "child did not exit after SIGKILL";
      }
      break;
    }
    if (child_exited && (!out_eof || !err_eof || !exec_done) &&
        grace_deadline < 0)
      grace_deadline = add_ms_saturating(now, kDrainGraceMs);

    if (child_exited && out_eof && err_eof && exec_done) break;

    if (!res.timed_out && deadline > 0 && now >= deadline) {
      res.timed_out = true;
      LOG_ERROR("exec_cmd: timeout {}ms, killing pid={} [cmd={}]", timeout_ms,
                pid, cmd_log);
      force_kill();
      if (grace_deadline < 0)
        grace_deadline = add_ms_saturating(now, kDrainGraceMs);
    }
    if (grace_deadline > 0 && now >= grace_deadline) {
      if (!out_eof) {
        close_fd(out_pipe[0]);
        out_eof = true;
        out_truncated = true;
      }
      if (!err_eof) {
        close_fd(err_pipe[0]);
        err_eof = true;
        err_truncated = true;
      }
      if (!exec_done) {
        close_fd(exec_pipe[0]);
        exec_done = true;
      }
      grace_deadline = -1;
      if (!child_exited) {
        force_kill();
        const auto end = add_ms_saturating(monotonic_ms(), kFinalReapMs);
        while (!child_exited && monotonic_ms() < end) {
          reap_nonblocking();
          if (child_exited) break;
          struct timespec ts{0, 2'000'000};
          ::nanosleep(&ts, nullptr);
        }
        if (!child_exited)
          giveup_deadline = add_ms_saturating(monotonic_ms(), kFinalReapMs);
      }
      continue;
    }

    struct pollfd pfds[4];
    int nfds = 0, oi = -1, ei = -1, xi = -1, pi = -1;
    if (!exec_done && exec_pipe[0] >= 0) {
      pfds[nfds] = {exec_pipe[0], POLLIN, 0};
      xi = nfds++;
    }
    if (!out_eof && out_pipe[0] >= 0) {
      pfds[nfds] = {out_pipe[0], POLLIN, 0};
      oi = nfds++;
    }
    if (!err_eof && err_pipe[0] >= 0) {
      pfds[nfds] = {err_pipe[0], POLLIN, 0};
      ei = nfds++;
    }
    if (!child_exited && pidfd >= 0) {
      pfds[nfds] = {pidfd, POLLIN, 0};
      pi = nfds++;
    }

    if (nfds == 0) {
      if (child_exited) break;
      struct timespec ts{0, 10 * 1000000};
      ::nanosleep(&ts, nullptr);
      continue;
    }

    int poll_timeout = kMaxPollWaitMs;
    const std::int64_t now2 = monotonic_ms();
    auto clamp_dl = [&](std::int64_t dl) {
      if (dl <= 0) return;
      std::int64_t r = dl - now2;
      int g = r <= 0 ? 0 : (r > INT_MAX ? INT_MAX : (int)r);
      if (g < poll_timeout) poll_timeout = g;
    };
    if (deadline > 0 && !res.timed_out) clamp_dl(deadline);
    if (grace_deadline > 0) clamp_dl(grace_deadline);
    if (giveup_deadline > 0) clamp_dl(giveup_deadline);

    int pr = ::poll(pfds, nfds, poll_timeout);
    if (pr < 0) {
      if (errno == EINTR) continue;
      if (res.sys_error.empty()) {
        res.sys_error = std::string("poll: ") + errno_string(errno);
        LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
      }
      force_kill();
      break;
    }
    if (pr == 0) continue;

    if (pi >= 0 && (pfds[pi].revents & (POLLIN | POLLHUP | POLLERR)))
      reap_nonblocking();

    if (xi >= 0 &&
        (pfds[xi].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      for (;;) {
        int child_errno = 0;
        ssize_t n = ::read(exec_pipe[0], &child_errno, sizeof(child_errno));
        if (n == (ssize_t)sizeof(child_errno)) {
          if (res.sys_error.empty()) {
            res.sys_error = std::string("execve: ") + errno_string(child_errno);
            LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
          }
          exec_done = true;
          close_fd(exec_pipe[0]);
          force_kill();
          break;
        }
        if (n == 0) {
          exec_done = true;
          close_fd(exec_pipe[0]);
          break;
        }
        if (n < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          if (res.sys_error.empty()) {
            res.sys_error =
                std::string("read exec pipe: ") + errno_string(errno);
            LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
          }
          exec_done = true;
          close_fd(exec_pipe[0]);
          force_kill();
          break;
        }
        // 短读 —— 原子性破坏，视为致命
        if (res.sys_error.empty())
          res.sys_error = "read exec pipe: short read (atomicity violated)";
        exec_done = true;
        close_fd(exec_pipe[0]);
        force_kill();
        break;
      }
    }
    if (oi >= 0 && (pfds[oi].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
      drain_out(out_eof);
    if (ei >= 0 && (pfds[ei].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
      drain_err(err_eof);
  }

  // ---------- 兜底 ----------
  if (!child_exited) {
    force_kill();
    const auto end = add_ms_saturating(monotonic_ms(), kKillWaitMs);
    while (!child_exited && monotonic_ms() < end) {
      reap_nonblocking();
      if (child_exited) break;
      struct timespec ts{0, 2'000'000};
      ::nanosleep(&ts, nullptr);
    }
    if (!child_exited) {
      LOG_ERROR("exec_cmd: pid={} unreaped after {}ms SIGKILL [cmd={}]", pid,
                kKillWaitMs, cmd_log);
      if (res.sys_error.empty()) res.sys_error = "child unreaped after SIGKILL";
    }
  }

  if (exec_pipe[0] >= 0) close_fd(exec_pipe[0]);
  if (!out_eof) {
    if (out_pipe[0] >= 0)
      drain_stream(out_pipe[0], out_buf, out_eof, out_truncated, out_read_errno,
                   read_buf, read_buf_sz);
    if (!out_eof) {
      out_truncated = true;
      out_eof = true;
    }
    close_fd(out_pipe[0]);
  }
  if (!err_eof) {
    if (err_pipe[0] >= 0)
      drain_stream(err_pipe[0], err_buf, err_eof, err_truncated, err_read_errno,
                   read_buf, read_buf_sz);
    if (!err_eof) {
      err_truncated = true;
      err_eof = true;
    }
    close_fd(err_pipe[0]);
  }
  if (pidfd >= 0) {
    ::close(pidfd);
    pidfd = -1;
  }

  if (res.sys_error.empty()) {
    if (out_read_errno)
      res.sys_error =
          std::string("read stdout: ") + errno_string(out_read_errno);
    else if (err_read_errno)
      res.sys_error =
          std::string("read stderr: ") + errno_string(err_read_errno);
    if (!res.sys_error.empty())
      LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
  }

  if (child_status_known) {
    res.exit_code = child_exit_code;
    res.term_signal = child_term_signal;
  } else if (child_exited) {
    if (res.sys_error.empty()) {
      res.sys_error = "child status unknown (reaped externally)";
      LOG_ERROR("exec_cmd: {} [cmd={}]", res.sys_error, cmd_log);
    }
    res.exit_code = -1;
  } else {
    res.exit_code = -1;
    if (res.sys_error.empty()) res.sys_error = "child unreaped";
  }

  res.output_truncated = out_truncated || err_truncated;
  res.out = sanitize_utf8(out_buf, out_truncated);
  res.err = sanitize_utf8(err_buf, err_truncated);

  // 语义提醒：ok 严格等于"命令成功"，若需要"执行成功"请用
  // !timed_out && sys_error.empty() && term_signal==0 单独判断。
  res.ok = (res.exit_code == 0) && !res.timed_out && res.sys_error.empty() &&
           res.term_signal == 0;

  if (!res.ok)
    LOG_ERROR(
        "exec_cmd: failed exit_code={} signal={} timeout={} "
        "trunc={} sys_error='{}' [cmd={}]",
        res.exit_code, res.term_signal, res.timed_out, res.output_truncated,
        res.sys_error, cmd_log);
  return res;
}
#elif defined(_WIN32)

constexpr size_t kMaxCaptureBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxCmdLineChars = 8191;   // cmd.exe /c 参数上限
constexpr DWORD kReaderJoinGraceMs = 5000;  // Job 生效时的宽限期（EOF 即返回）
constexpr DWORD kReaderJoinGraceNoJobMs = 500;  // Job 未生效时缩短宽限期
constexpr int kCancelRetries = 200;

// ---------- 编码 ----------

// 失败时返回空字符串（签名不允许
// optional，调用方需自行区分“空输入”与“转换失败”）。
inline std::wstring utf8_to_wide_strict(const std::string& s) {
  if (s.empty()) return {};
  if (s.size() > static_cast<size_t>(INT_MAX)) return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                              static_cast<int>(s.size()), &w[0], n);
  if (r != n) return {};
  return w;
}

// 失败时返回空字符串（不再返回原串，避免产生非 UTF-8 结果）。
inline std::string cp_to_utf8(const std::string& s, UINT cp) {
  if (s.empty()) return {};
  if (s.size() > static_cast<size_t>(INT_MAX)) return {};
  // MB_ERR_INVALID_CHARS 仅对 CP_UTF8 有效；其他代码页传 0。
  DWORD mbFlags = (cp == CP_UTF8) ? MB_ERR_INVALID_CHARS : 0;
  int wlen = MultiByteToWideChar(cp, mbFlags, s.data(),
                                 static_cast<int>(s.size()), nullptr, 0);
  if (wlen <= 0) return {};
  std::wstring w(static_cast<size_t>(wlen), L'\0');
  int rw = MultiByteToWideChar(cp, mbFlags, s.data(),
                               static_cast<int>(s.size()), &w[0], wlen);
  if (rw != wlen) return {};
  // WC_ERR_INVALID_CHARS 仅对 CP_UTF8 有效（Vista+）。
  int u8len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), wlen,
                                  nullptr, 0, nullptr, nullptr);
  if (u8len <= 0) return {};
  std::string u8(static_cast<size_t>(u8len), '\0');
  int ru = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), wlen,
                               &u8[0], u8len, nullptr, nullptr);
  if (ru != u8len) return {};
  return u8;
}

// 失败时返回空字符串（签名限制；调用方若需区分，建议改用 optional 版本）。
inline std::string ensure_utf8(std::string s) {
  if (s.empty()) return s;
  if (s.size() > static_cast<size_t>(INT_MAX)) return {};

  // 0) UTF-32 BOM：本函数不支持 UTF-32，显式拒绝，避免被误判为 UTF-16。
  if (s.size() >= 4) {
    unsigned char b0 = static_cast<unsigned char>(s[0]);
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    unsigned char b2 = static_cast<unsigned char>(s[2]);
    unsigned char b3 = static_cast<unsigned char>(s[3]);
    if ((b0 == 0xFF && b1 == 0xFE && b2 == 0x00 && b3 == 0x00) ||  // UTF-32 LE
        (b0 == 0x00 && b1 == 0x00 && b2 == 0xFE && b3 == 0xFF)) {  // UTF-32 BE
      return {};
    }
  }

  // 1) UTF-16 BOM（至少 2 字节；载荷必须为偶数长度）
  if (s.size() >= 2) {
    unsigned char b0 = static_cast<unsigned char>(s[0]);
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    bool le = (b0 == 0xFF && b1 == 0xFE);
    bool be = (b0 == 0xFE && b1 == 0xFF);
    if (le || be) {
      if ((s.size() - 2) % 2 != 0) return {};  // 截断的 UTF-16
      int wlen = static_cast<int>((s.size() - 2) / 2);
      if (wlen == 0) return {};
      std::wstring w(static_cast<size_t>(wlen), L'\0');
      // 逐字节组装，避免 reinterpret_cast<wchar_t*> 的对齐/严格别名问题。
      for (int i = 0; i < wlen; ++i) {
        unsigned char a = static_cast<unsigned char>(s[2 + 2 * i]);
        unsigned char b = static_cast<unsigned char>(s[3 + 2 * i]);
        w[static_cast<size_t>(i)] =
            le ? static_cast<wchar_t>(a | (static_cast<unsigned>(b) << 8))
               : static_cast<wchar_t>((static_cast<unsigned>(a) << 8) | b);
      }
      int u8len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                                      wlen, nullptr, 0, nullptr, nullptr);
      if (u8len <= 0) return {};
      std::string u8(static_cast<size_t>(u8len), '\0');
      int ru = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(),
                                   wlen, &u8[0], u8len, nullptr, nullptr);
      if (ru != u8len) return {};
      return u8;
    }
  }

  // 2) 严格校验 UTF-8；合法则直接返回（UTF-8 BOM 也合法，保留）
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                          static_cast<int>(s.size()), nullptr, 0) > 0) {
    return s;
  }

  // 3) 回退 OEM。注意 GetOEMCP() 不一定等于控制台输出代码页；
  //    若来源是控制台，调用方可能更适合用 GetConsoleOutputCP()。
  return cp_to_utf8(s, GetOEMCP());
}

// ---------- 环境变量（TOCTOU 重试） ----------

std::wstring get_env_dynamic(const wchar_t* name) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    SetLastError(0);
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) {
      // ERROR_ENVVAR_NOT_FOUND(不存在) 或 ERROR_SUCCESS(存在但为空)
      return {};
    }
    std::wstring v(n, L'\0');
    SetLastError(0);
    DWORD r = GetEnvironmentVariableW(name, v.data(), n);
    if (r == 0) return {};
    if (r < n) {
      v.resize(r);
      return v;
    }
    // r >= n：探测后值变长，重试
  }
  return {};
}

// 从 System32 解析 cmd.exe 绝对路径，避免相对路径劫持
std::wstring system_cmd_path() {
  wchar_t sysdir[MAX_PATH];
  UINT n = GetSystemDirectoryW(sysdir, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    std::wstring p(sysdir, n);
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    p += L"cmd.exe";
    return p;
  }
  return L"cmd.exe";  // 极低概率，保留兜底
}

// ---------- 取消令牌 ----------

struct cancel_token {
  std::atomic<HANDLE> thread_handle{nullptr};
  std::atomic<bool> stopped{false};
};

// 关键修复：重试直到线程真正退出，消除“Cancel 早于 ReadFile”的竞态。
void cancel_read(cancel_token& tok) noexcept {
  tok.stopped.store(true, std::memory_order_release);
  HANDLE h = tok.thread_handle.load(std::memory_order_acquire);
  if (!h) return;
  for (int i = 0; i < kCancelRetries; ++i) {
    CancelSynchronousIo(h);
    if (WaitForSingleObject(h, 10) == WAIT_OBJECT_0) return;
  }
}

// noexcept：bad_alloc 不得逃逸
void read_all(HANDLE h, std::string& out, bool& truncated,
              cancel_token& tok) noexcept {
  char buf[8192];
  DWORD n = 0;
  bool capped = false;
  while (!tok.stopped.load(std::memory_order_relaxed)) {
    if (!ReadFile(h, buf, static_cast<DWORD>(sizeof(buf)), &n, nullptr)) break;
    if (n == 0) break;
    if (capped) continue;  // 已封顶：只排空不存储
    size_t room = kMaxCaptureBytes - out.size();
    try {
      if (n > room) {
        out.append(buf, room);
        capped = true;
      } else {
        out.append(buf, n);
      }
    } catch (...) {
      capped = true;  // 分配失败：排空剩余输出
    }
  }
  truncated = capped;
}

// ---------- RAII ----------

struct handle_guard {
  HANDLE h = nullptr;
  handle_guard() = default;
  explicit handle_guard(HANDLE x) : h(x) {}
  ~handle_guard() { reset(); }
  handle_guard(const handle_guard&) = delete;
  handle_guard& operator=(const handle_guard&) = delete;
  void reset(HANDLE x = nullptr) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = x;
  }
  HANDLE release() {
    HANDLE x = h;
    h = nullptr;
    return x;
  }
};

struct attr_list_guard {
  LPPROC_THREAD_ATTRIBUTE_LIST a = nullptr;
  ~attr_list_guard() {
    if (a) DeleteProcThreadAttributeList(a);
  }
};

// 带宽限期的 join。正常 EOF 时立即返回；否则主动取消，保证 join 一定返回。
void join_reader(std::thread& t, cancel_token& tok, DWORD grace_ms) {
  if (!t.joinable()) return;
  if (WaitForSingleObject(t.native_handle(), grace_ms) == WAIT_TIMEOUT) {
    cancel_read(tok);
  }
  t.join();
}

// timeout_ms == 0 表示不超时（无限等待）
command_result exec_cmd(
    const std::string& cmd,
    DWORD timeout_ms) {  // 注：timeout_ms == 0 表示无限等待；INFINITE
                         // 亦等价无限等待。
  command_result result;

  if (cmd.empty()) {
    result.std_err = "exec_cmd: empty command";
    return result;
  }
  if (cmd.find('\0') != std::string::npos) {
    result.std_err = "exec_cmd: command contains NUL byte";
    return result;
  }

  std::wstring wcmd = utf8_to_wide_strict(cmd);
  if (wcmd.empty()) {
    result.std_err = "exec_cmd: command is not valid UTF-8";
    return result;
  }

  std::wstring exe = get_env_dynamic(L"COMSPEC");
  if (exe.empty()) exe = system_cmd_path();  // 绝对路径兜底

  // 给 argv[0] 加引号，防止 exe 路径含空格时 argv[0] 被截断
  std::wstring cmdline = L"\"" + exe + L"\" /s /c \"" + wcmd + L"\"";
  if (cmdline.size() > kMaxCmdLineChars) {
    result.std_err = "exec_cmd: command line too long";
    return result;
  }
  std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back(L'\0');

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  handle_guard stdout_r, stdout_w, stderr_r, stderr_w, nul_h, job;

  if (!CreatePipe(&stdout_r.h, &stdout_w.h, &sa, 64 * 1024)) {
    result.std_err = "exec_cmd: CreatePipe(stdout) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }
  if (!CreatePipe(&stderr_r.h, &stderr_w.h, &sa, 64 * 1024)) {
    result.std_err = "exec_cmd: CreatePipe(stderr) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }
  if (!SetHandleInformation(stdout_r.h, HANDLE_FLAG_INHERIT, 0)) {
    result.std_err = "exec_cmd: SetHandleInformation(stdout) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }
  if (!SetHandleInformation(stderr_r.h, HANDLE_FLAG_INHERIT, 0)) {
    result.std_err = "exec_cmd: SetHandleInformation(stderr) failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }

  nul_h.h =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                  OPEN_EXISTING, 0, nullptr);
  if (nul_h.h == INVALID_HANDLE_VALUE) {
    nul_h.h = nullptr;
    result.std_err =
        "exec_cmd: open NUL failed, err=" + std::to_string(GetLastError());
    return result;
  }

  job.h = CreateJobObjectW(nullptr, nullptr);
  if (job.h) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &jeli,
                            sizeof(jeli));
  }

  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  if (attr_size == 0) {
    result.std_err = "exec_cmd: attr list sizing failed";
    return result;
  }
  std::vector<BYTE> attr_buf(attr_size);
  attr_list_guard attr_guard;
  attr_guard.a =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  if (!InitializeProcThreadAttributeList(attr_guard.a, 1, 0, &attr_size)) {
    attr_guard.a = nullptr;
    result.std_err =
        "exec_cmd: InitializeProcThreadAttributeList failed, err=" +
        std::to_string(GetLastError());
    return result;
  }

  HANDLE inherit_handles[] = {stdout_w.h, stderr_w.h, nul_h.h};
  if (!UpdateProcThreadAttribute(
          attr_guard.a, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_handles,
          sizeof(inherit_handles), nullptr, nullptr)) {
    result.std_err = "exec_cmd: UpdateProcThreadAttribute failed, err=" +
                     std::to_string(GetLastError());
    return result;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = stdout_w.h;
  si.StartupInfo.hStdError = stderr_w.h;
  si.StartupInfo.hStdInput = nul_h.h;
  si.lpAttributeList = attr_guard.a;

  DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                CREATE_SUSPENDED | CREATE_BREAKAWAY_FROM_JOB;

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(exe.c_str(), cmdline_buf.data(), nullptr, nullptr,
                           TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
  if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
    flags &= ~CREATE_BREAKAWAY_FROM_JOB;
    ok = CreateProcessW(exe.c_str(), cmdline_buf.data(), nullptr, nullptr, TRUE,
                        flags, nullptr, nullptr, &si.StartupInfo, &pi);
  }
  if (!ok) {
    result.std_err =
        "exec_cmd: CreateProcess failed, err=" + std::to_string(GetLastError());
    return result;
  }

  handle_guard process_guard(pi.hProcess);
  handle_guard thread_guard(pi.hThread);

  stdout_w.reset();
  stderr_w.reset();
  nul_h.reset();

  bool job_assigned = false;
  if (job.h) {
    if (AssignProcessToJobObject(job.h, pi.hProcess)) {
      job_assigned = true;
    } else {
      DWORD e = GetLastError();
      if (e != ERROR_ACCESS_DENIED) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        result.std_err = "exec_cmd: AssignProcessToJobObject failed, err=" +
                         std::to_string(e);
        return result;
      }
      // ERROR_ACCESS_DENIED：父进程 Job 不允许嵌套，降级继续
    }
  }

  if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
    if (job_assigned)
      TerminateJobObject(job.h, 1);
    else
      TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    result.std_err =
        "exec_cmd: ResumeThread failed, err=" + std::to_string(GetLastError());
    return result;
  }

  std::string out_buf, err_buf;
  bool trunc_out = false, trunc_err = false;
  cancel_token tok_out, tok_err;
  HANDLE h_out = stdout_r.h;
  HANDLE h_err = stderr_r.h;
  std::thread t_out, t_err;

  try {
    t_out = std::thread([h_out, &out_buf, &trunc_out, &tok_out] {
      read_all(h_out, out_buf, trunc_out, tok_out);
    });
    tok_out.thread_handle.store(t_out.native_handle(),
                                std::memory_order_release);

    t_err = std::thread([h_err, &err_buf, &trunc_err, &tok_err] {
      read_all(h_err, err_buf, trunc_err, tok_err);
    });
    tok_err.thread_handle.store(t_err.native_handle(),
                                std::memory_order_release);
  } catch (const std::system_error&) {
    // 统一走 join_reader，保证即便线程卡在 append 也能被取消兜底
    cancel_read(tok_out);
    cancel_read(tok_err);
    if (job_assigned)
      TerminateJobObject(job.h, 1);
    else
      TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
    job.reset();
    join_reader(t_out, tok_out, kReaderJoinGraceNoJobMs);
    join_reader(t_err, tok_err, kReaderJoinGraceNoJobMs);
    result.std_err = "exec_cmd: failed to start capture threads";
    return result;
  }

  DWORD wait_rc =
      WaitForSingleObject(pi.hProcess, timeout_ms == 0 ? INFINITE : timeout_ms);
  std::string wait_error;
  if (wait_rc == WAIT_TIMEOUT) {
    result.timed_out = true;
    if (job_assigned)
      TerminateJobObject(job.h, 1);
    else
      TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  } else if (wait_rc == WAIT_FAILED) {
    wait_error = "exec_cmd: WaitForSingleObject failed, err=" +
                 std::to_string(GetLastError());
    if (job_assigned)
      TerminateJobObject(job.h, 1);
    else
      TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }

  DWORD ec = 0;
  if (GetExitCodeProcess(pi.hProcess, &ec))
    result.exit_code = static_cast<int>(ec);

  // 关闭 Job：KILL_ON_JOB_CLOSE 确保后台孙进程一并终止、管道写端真正关闭
  job.reset();

  // Job 生效时 EOF 即刻到达，join_reader 立即返回；
  // Job 未生效时用较短宽限期，避免对带后台子进程的命令产生固定 5s 延迟。
  DWORD grace = job_assigned ? kReaderJoinGraceMs : kReaderJoinGraceNoJobMs;
  join_reader(t_out, tok_out, grace);
  join_reader(t_err, tok_err, grace);

  result.truncated = trunc_out || trunc_err;
  result.std_out = ensure_utf8(std::move(out_buf));
  result.std_err = ensure_utf8(std::move(err_buf));
  // result.std_out=std::move(out_buf);
  // result.std_err=std::move(err_buf);
  if (!wait_error.empty()) {
    if (!result.std_err.empty()) result.std_err += "\n";
    result.std_err += wait_error;
  }
  return result;
}
#endif
/*-exec_cmd-------------------------------------------------------*/

/*-file insert-------------------------------------------------------*/
#if defined(__linux__)

// 要求: Linux >= 6.12, C++17

bool write_all(int fd, const void* data, size_t len) {
  const char* p = static_cast<const char*>(data);
  while (len > 0) {
    ssize_t w = ::write(fd, p, len);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) {
      errno = EIO;
      return false;
    }
    p += w;
    len -= static_cast<size_t>(w);
  }
  return true;
}

struct FdGuard {
  int fd;
  explicit FdGuard(int f = -1) noexcept : fd(f) {}
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  FdGuard(FdGuard&& o) noexcept : fd(o.fd) { o.fd = -1; }
  FdGuard& operator=(FdGuard&& o) noexcept {
    if (this != &o) {
      reset();
      fd = o.fd;
      o.fd = -1;
    }
    return *this;
  }
  ~FdGuard() { reset(); }
  void reset(int f = -1) {
    if (fd >= 0) ::close(fd);
    fd = f;
  }
  int release() {
    int r = fd;
    fd = -1;
    return r;
  }
};

struct TempFileGuard {
  int fd = -1;
  std::string path;
  bool committed = false;
  TempFileGuard() = default;
  TempFileGuard(const TempFileGuard&) = delete;
  TempFileGuard& operator=(const TempFileGuard&) = delete;
  ~TempFileGuard() {
    if (fd >= 0) ::close(fd);
    if (!committed && !path.empty()) ::unlink(path.c_str());
  }
  void set(int f, std::string p) {
    fd = f;
    path = std::move(p);
  }
  void release_fd() { fd = -1; }
  void commit() { committed = true; }
};

std::string parent_dir(const std::string& path) {
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

bool flock_exclusive(int fd) {
  while (::flock(fd, LOCK_EX) < 0) {
    if (errno == EINTR) continue;
    return false;
  }
  return true;
}

// 打开并加锁锁文件；关键：加锁后验证 inode
// 一致，解决"锁文件被并发删除/替换"竞态
int acquire_lock(const std::string& lock_path, std::string& err) {
  int fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                  0600);
  if (fd < 0) {
    err = "open lock failed: " + errno_string(errno);
    return -1;
  }
  if (!flock_exclusive(fd)) {
    int e = errno;
    ::close(fd);
    err = "flock failed: " + errno_string(e);
    return -1;
  }
  struct stat st_path{}, st_fd{};
  if (::lstat(lock_path.c_str(), &st_path) < 0) {
    int e = errno;
    ::close(fd);
    err = "lstat lock failed: " + errno_string(e);
    return -1;
  }
  if (::fstat(fd, &st_fd) < 0) {
    int e = errno;
    ::close(fd);
    err = "fstat lock failed: " + errno_string(e);
    return -1;
  }
  if (st_path.st_dev != st_fd.st_dev || st_path.st_ino != st_fd.st_ino) {
    ::close(fd);
    err = "lock file replaced concurrently";
    return -1;
  }
  return fd;
}

bool copy_xattrs(int src_fd, int dst_fd, std::string& err) {
  std::vector<char> names;
  for (int attempt = 0; attempt < 4; ++attempt) {
    ssize_t sz = ::flistxattr(src_fd, nullptr, 0);
    if (sz < 0) {
      if (errno == ENOTSUP || errno == EOPNOTSUPP) return true;
      err = "flistxattr failed: " + errno_string(errno);
      return false;
    }
    if (sz == 0) return true;
    names.resize(static_cast<size_t>(sz));
    ssize_t n = ::flistxattr(src_fd, names.data(), names.size());
    if (n < 0) {
      if (errno == ERANGE) continue;  // 列表在两次调用间变长了
      if (errno == ENOTSUP || errno == EOPNOTSUPP) return true;
      err = "flistxattr failed: " + errno_string(errno);
      return false;
    }
    for (char* p = names.data(); p < names.data() + n;
         p += std::strlen(p) + 1) {
      const char* name = p;

      // 内容已改变，跳过与内容绑定的安全属性（校验后必然失败）
      if (std::strcmp(name, "security.capability") == 0 ||
          std::strcmp(name, "security.ima") == 0 ||
          std::strcmp(name, "security.evm") == 0) {
        LOG_ERROR("copy_xattrs: skip content-bound xattr '{}'", name);
        continue;
      }
      // selinux 标签应交给 restorecon / file_contexts 处理
      if (std::strcmp(name, "security.selinux") == 0) continue;
      // 非 root 拷贝 trusted.* 无意义
      if (std::strncmp(name, "trusted.", 8) == 0 && ::geteuid() != 0) {
        LOG_ERROR("copy_xattrs: skip privileged xattr '{}'", name);
        continue;
      }

      ssize_t vsz = ::fgetxattr(src_fd, name, nullptr, 0);
      if (vsz < 0) {
        if (errno == ENODATA) continue;
        if (errno == EPERM || errno == EACCES || errno == ENOTSUP) {
          LOG_ERROR("copy_xattrs: read '{}' denied: {}", name,
                    errno_string(errno));
          continue;
        }
        err = std::string("fgetxattr('") + name +
              "') failed: " + errno_string(errno);
        return false;
      }
      std::vector<char> value(static_cast<size_t>(vsz));
      ssize_t got = ::fgetxattr(src_fd, name, value.data(), value.size());
      if (got < 0) {
        if (errno == ENODATA) continue;
        err = std::string("fgetxattr('") + name +
              "') failed: " + errno_string(errno);
        return false;
      }
      if (::fsetxattr(dst_fd, name, value.data(), static_cast<size_t>(got), 0) <
          0) {
        int e = errno;
        if (e == EPERM || e == EOPNOTSUPP || e == ENOTSUP) {
          LOG_ERROR("copy_xattrs: write '{}' skipped: {}", name,
                    errno_string(e));
          continue;
        }
        err =
            std::string("fsetxattr('") + name + "') failed: " + errno_string(e);
        return false;
      }
    }
    return true;
  }
  err = "flistxattr: xattr list keeps changing";
  return false;
}

bool same_stat_times(const struct stat& a, const struct stat& b) {
  return a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
         a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
         a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
         a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

std::string file_insert_line(const char* file_path, int line, const char* data,
                             size_t len) {
  if (file_path == nullptr || *file_path == '\0') {
    LOG_ERROR("file_insert_line: null or empty file_path");
    return "invalid argument: file_path";
  }
  if (line < 1) {
    LOG_ERROR("file_insert_line: invalid line {} for '{}'", line, file_path);
    return "invalid argument: line";
  }
  if (data == nullptr && len > 0) {
    LOG_ERROR("file_insert_line: null data with len={} for '{}'", len,
              file_path);
    return "invalid argument: data";
  }

  const std::string path(file_path);
  const uint64_t target_line = static_cast<uint64_t>(line);

  // ---------- 1. 锁文件 (inode 校验版本) ----------
  const std::string lock_path = path + ".lock";
  std::string lock_err;
  int lock_fd = acquire_lock(lock_path, lock_err);
  if (lock_fd < 0) {
    LOG_ERROR("file_insert_line: acquire lock '{}' failed: {}", lock_path,
              lock_err);
    return "lock failed: " + lock_err;
  }
  FdGuard lock_guard(lock_fd);

  // ---------- 2. 打开源文件 (拒绝符号链接) ----------
  int src_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (src_fd < 0) {
    int e = errno;
    LOG_ERROR("file_insert_line: open('{}') failed: {}", path, errno_string(e));
    return "open failed: " + errno_string(e);
  }
  FdGuard src_guard(src_fd);

  struct stat st{};
  if (::fstat(src_fd, &st) < 0) {
    int e = errno;
    LOG_ERROR("file_insert_line: fstat('{}') failed: {}", path,
              errno_string(e));
    return "fstat failed: " + errno_string(e);
  }
  if (!S_ISREG(st.st_mode)) {
    LOG_ERROR("file_insert_line: '{}' is not a regular file", path);
    return "not a regular file";
  }
  if (st.st_nlink > 1) {
    LOG_ERROR("file_insert_line: '{}' has {} hard links", path, st.st_nlink);
    return "file has multiple hard links";
  }

  // ---------- 3. 同目录临时文件 ----------
  const std::string dir = parent_dir(path);
  std::string tmpl = dir;
  if (tmpl.empty() || tmpl.back() != '/') tmpl += '/';
  tmpl += ".file_insert_line.XXXXXX";

  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');

  int tmp_fd = ::mkostemp(tmpl_buf.data(), O_CLOEXEC);
  if (tmp_fd < 0) {
    int e = errno;
    LOG_ERROR("file_insert_line: mkostemp in '{}' failed: {}", dir,
              errno_string(e));
    return "mkostemp failed: " + errno_string(e);
  }
  TempFileGuard tmp_guard;
  tmp_guard.set(tmp_fd, std::string(tmpl_buf.data()));

  // ---------- 4. 保留属主 / 权限 / xattr ----------
  if (::fchown(tmp_fd, st.st_uid, st.st_gid) < 0) {
    int e = errno;
    if (e == EPERM && st.st_uid == ::geteuid()) {
      if (::fchown(tmp_fd, static_cast<uid_t>(-1), st.st_gid) < 0) {
        e = errno;
        LOG_ERROR("file_insert_line: fchown('{}') failed: {}", tmp_guard.path,
                  errno_string(e));
        return "fchown failed: " + errno_string(e);
      }
    } else {
      LOG_ERROR("file_insert_line: fchown('{}') failed: {}", tmp_guard.path,
                errno_string(e));
      return "fchown failed: " + errno_string(e);
    }
  }

  mode_t mode = st.st_mode & 07777;
  if (mode & (S_ISUID | S_ISGID)) {
    LOG_ERROR("file_insert_line: clearing setuid/setgid on '{}'", path);
    mode &= ~(S_ISUID | S_ISGID);
  }
  if (::fchmod(tmp_fd, mode) < 0) {
    int e = errno;
    LOG_ERROR("file_insert_line: fchmod('{}') failed: {}", tmp_guard.path,
              errno_string(e));
    return "fchmod failed: " + errno_string(e);
  }

  // 必须在 fchmod 之后拷贝 system.posix_acl_access：chmod 会重写 ACL
  {
    std::string xerr;
    if (!copy_xattrs(src_fd, tmp_fd, xerr)) {
      LOG_ERROR("file_insert_line: copy xattrs to '{}' failed: {}",
                tmp_guard.path, xerr);
      return "copy xattrs failed: " + xerr;
    }
  }

  // ---------- 5. 流式复制 + 插入 ----------
  static constexpr size_t kBufSize = 64 * 1024;
  std::vector<char> buf(kBufSize);

  uint64_t lines_done = 0;
  uint64_t total_read = 0;
  bool inserted = false;
  char file_last_byte = 0;

  auto copy_out = [&](const char* p, size_t n) -> bool {
    return n == 0 || write_all(tmp_fd, p, n);
  };

  // compromise with goto
  struct stat st_after{}, st_path{};
  int dir_fd;
  //

  while (true) {
    ssize_t r = ::read(src_fd, buf.data(), buf.size());
    if (r < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      LOG_ERROR("file_insert_line: read('{}') failed: {}", path,
                errno_string(e));
      return "read failed: " + errno_string(e);
    }
    if (r == 0) break;

    total_read += static_cast<uint64_t>(r);
    file_last_byte = buf[static_cast<size_t>(r) - 1];

    size_t pos = 0;
    const size_t n = static_cast<size_t>(r);

    while (pos < n) {
      if (inserted) {
        if (!copy_out(buf.data() + pos, n - pos)) goto write_error;
        pos = n;
        continue;
      }
      size_t nl = std::string::npos;
      for (size_t i = pos; i < n; ++i) {
        if (buf[i] == '\n' && ++lines_done == target_line) {
          nl = i;
          break;
        }
      }
      if (nl == std::string::npos) {
        if (!copy_out(buf.data() + pos, n - pos)) goto write_error;
        pos = n;
      } else {
        if (!copy_out(buf.data() + pos, nl - pos + 1)) goto write_error;
        if (len > 0) {
          if (!copy_out(data, len)) goto write_error;
          if (data[len - 1] != '\n') {
            if (!copy_out("\n", 1)) goto write_error;
          }
        }
        inserted = true;
        pos = nl + 1;
      }
    }
  }

  // ---------- 6. 末行无 '\n' 的收尾 ----------
  // 语义与 wc -l 对齐：空文件有 0 行，插 line=1 视为越界（与 sed '1i' 一致）。
  if (!inserted) {
    if (lines_done == target_line - 1 && total_read > 0 &&
        file_last_byte != '\n') {
      if (!copy_out("\n", 1)) goto write_error;
      if (len > 0) {
        if (!copy_out(data, len)) goto write_error;
        if (data[len - 1] != '\n') {
          if (!copy_out("\n", 1)) goto write_error;
        }
      }
      inserted = true;
    } else {
      uint64_t total_lines =
          lines_done + ((total_read > 0 && file_last_byte != '\n') ? 1 : 0);
      LOG_ERROR(
          "file_insert_line: line {} out of range for '{}' "
          "(file has {} line(s))",
          line, path, total_lines);
      return "line out of range";
    }
  }

  // ---------- 7. 落盘 + 并发修改检测 ----------
  if (::fsync(tmp_fd) < 0) {
    int e = errno;
    LOG_ERROR("file_insert_line: fsync('{}') failed: {}", tmp_guard.path,
              errno_string(e));
    return "fsync failed: " + errno_string(e);
  }
  {
    if (::fstat(src_fd, &st_after) < 0) {
      int e = errno;
      LOG_ERROR("file_insert_line: fstat('{}') failed: {}", path,
                errno_string(e));
      return "fstat failed: " + errno_string(e);
    }
    if (::lstat(path.c_str(), &st_path) < 0) {
      int e = errno;
      LOG_ERROR("file_insert_line: lstat('{}') failed: {}", path,
                errno_string(e));
      return "file changed during operation: " + errno_string(e);
    }
    if (st_path.st_dev != st.st_dev || st_path.st_ino != st.st_ino) {
      LOG_ERROR("file_insert_line: '{}' was replaced during operation", path);
      return "file changed during operation: path replaced";
    }
    if (!same_stat_times(st, st_after)) {
      LOG_ERROR("file_insert_line: '{}' was modified during operation", path);
      return "file changed during operation: content modified";
    }
  }

  if (::close(tmp_fd) < 0) {
    int e = errno;
    tmp_guard.release_fd();
    LOG_ERROR("file_insert_line: close('{}') failed: {}", tmp_guard.path,
              errno_string(e));
    return "close failed: " + errno_string(e);
  }
  tmp_guard.release_fd();

  // ---------- 8. 原子替换 + 目录 fsync ----------
  if (::rename(tmp_guard.path.c_str(), path.c_str()) < 0) {
    int e = errno;
    LOG_ERROR("file_insert_line: rename('{}','{}') failed: {}", tmp_guard.path,
              path, errno_string(e));
    return "rename failed: " + errno_string(e);
  }
  tmp_guard.commit();

  dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dir_fd >= 0) {
    if (::fsync(dir_fd) < 0) {
      // 文件已替换成功，仅"重命名持久性"存疑：不是业务错误
      LOG_ERROR("file_insert_line: fsync dir '{}' failed after rename: {}", dir,
                errno_string(errno));
    }
    ::close(dir_fd);
  } else {
    LOG_ERROR("file_insert_line: open dir '{}' failed after rename: {}", dir,
              errno_string(errno));
  }
  return "success";

write_error:
  {
    int e = errno;
    LOG_ERROR("file_insert_line: write to '{}' failed: {}", tmp_guard.path,
              errno_string(e));
    return "write failed: " + errno_string(e);
  }
}
#elif defined(_WIN32)

// ---------- UTF-8 -> UTF-16（严格模式）----------
bool utf8_to_wide(const char* s, std::wstring& out) {
  out.clear();
  if (!s) return true;
  const int wlen =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
  if (wlen <= 0) return false;
  out.resize(static_cast<size_t>(wlen - 1));
  if (wlen > 1) {
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out.data(),
                              wlen) == 0) {
      out.clear();
      return false;
    }
  }
  return true;
}

// ---------- 完整写入 ----------
bool write_all(HANDLE h, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<size_t>(len - off, 1u << 20));
    DWORD written = 0;
    if (!::WriteFile(h, data + off, chunk, &written, nullptr)) return false;
    if (written == 0) {
      ::SetLastError(ERROR_WRITE_FAULT);
      return false;
    }
    off += written;
  }
  return true;
}

// ---------- 从 in 精确拷贝 count 字节到 out ----------
bool copy_exact(HANDLE in, HANDLE out, uint64_t count) {
  if (count == 0) return true;
  std::vector<char> buf(kCopyBufSize);
  uint64_t remaining = count;
  while (remaining > 0) {
    const DWORD to_read =
        static_cast<DWORD>(std::min<uint64_t>(remaining, kCopyBufSize));
    DWORD read_bytes = 0;
    if (!::ReadFile(in, buf.data(), to_read, &read_bytes, nullptr))
      return false;
    if (read_bytes == 0) {
      ::SetLastError(ERROR_HANDLE_EOF);
      return false;
    }
    if (!write_all(out, buf.data(), read_bytes)) return false;
    remaining -= read_bytes;
  }
  return true;
}

// ---------- 目录 ----------
std::wstring dir_of(const std::wstring& wpath) {
  if (wpath.empty()) return L".";
  const auto slash = wpath.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L".";
  // 盘符根： "C:\"
  if (slash == 2 && wpath.size() >= 3 && wpath[1] == L':') {
    return wpath.substr(0, 3);
  }
  // UNC： "\\server\share\..." -> "\\server\share\..."
  if (wpath.size() >= 2 && wpath[0] == L'\\' && wpath[1] == L'\\') {
    if (slash <= 1) return L".";  // "\\" 本身
    return wpath.substr(0, slash);
  }
  return wpath.substr(0, slash);
}

// 将 data 插入到 file_path 的"第 line 行之后"；
// line == 0 表示插到文件头；line 超出总行数则追加到文件尾。
std::string file_insert_line(const char* file_path, size_t line,
                             const char* data, size_t len) {
  // ---------- 参数校验 ----------
  if (!file_path || file_path[0] == '\0') {
    LOG_ERROR("file_insert_line: invalid file_path (null/empty)");
    return "invalid file_path";
  }
  if (!data && len > 0) {
    LOG_ERROR("file_insert_line: data is null but len={} (file={})", len,
              file_path);
    return "invalid data";
  }

  std::wstring wpath;
  if (!utf8_to_wide(file_path, wpath) || wpath.empty()) {
    LOG_ERROR("file_insert_line: invalid UTF-8 path, file={}", file_path);
    return "invalid path encoding";
  }

  // ---------- 打开源文件 ----------
  UniqueHandle hIn(::CreateFileW(
      wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!hIn.valid()) {
    const DWORD err = ::GetLastError();
    LOG_ERROR("file_insert_line: open '{}' for read failed, GetLastError={}",
              file_path, err);
    return "open failed: " + std::to_string(err);
  }

  LARGE_INTEGER file_size{};
  if (!::GetFileSizeEx(hIn.get(), &file_size)) {
    const DWORD err = ::GetLastError();
    LOG_ERROR(
        "file_insert_line: GetFileSizeEx failed, file={}, GetLastError={}",
        file_path, err);
    return "get size failed: " + std::to_string(err);
  }

  // ---------- 扫描插入点：定位到第 line 行的下一行开头 ----------
  uint64_t insert_pos = 0;
  bool need_leading_newline = false;  // 是否需要在 data 前补一个换行
  if (line > 0) {
    std::vector<char> buf(kScanBufSize);
    uint64_t buf_start = 0;
    size_t nl_seen = 0;
    bool found = false;
    bool scan_ok = true;
    DWORD scan_err = 0;

    for (;;) {
      DWORD read_bytes = 0;
      if (!::ReadFile(hIn.get(), buf.data(), static_cast<DWORD>(kScanBufSize),
                      &read_bytes, nullptr)) {
        scan_ok = false;
        scan_err = ::GetLastError();
        break;
      }
      if (read_bytes == 0) break;

      const char* p = buf.data();
      DWORD left = read_bytes;
      while (left > 0) {
        const void* hit = std::memchr(p, '\n', left);
        if (!hit) break;
        const char* nl = static_cast<const char*>(hit);
        if (++nl_seen == line) {
          // 第 line 个换行之后，就是第 line 行的下一行开头
          insert_pos = buf_start + static_cast<uint64_t>(nl - buf.data()) + 1;
          found = true;
          break;
        }
        left -= static_cast<DWORD>(nl - p) + 1;
        p = nl + 1;
      }
      if (found) break;
      buf_start += read_bytes;
    }

    if (!scan_ok) {
      LOG_ERROR(
          "file_insert_line: ReadFile failed while scanning lines, "
          "file={}, GetLastError={}",
          file_path, scan_err);
      return "scan failed: " + std::to_string(scan_err);
    }
    if (!found) insert_pos = static_cast<uint64_t>(file_size.QuadPart);

    // 如果插入点落在文件尾，且原文件末尾不是换行，则先补一个换行，
    // 保证 data 从第 line 行的下一行开始，而不是粘在最后一行后面。
    if (len > 0 && insert_pos == static_cast<uint64_t>(file_size.QuadPart) &&
        file_size.QuadPart > 0) {
      LARGE_INTEGER last_pos{};
      last_pos.QuadPart = file_size.QuadPart - 1;

      if (!::SetFilePointerEx(hIn.get(), last_pos, nullptr, FILE_BEGIN)) {
        const DWORD err = ::GetLastError();
        LOG_ERROR(
            "file_insert_line: seek to last byte failed, file={}, "
            "GetLastError={}",
            file_path, err);
        return "seek last byte failed: " + std::to_string(err);
      }

      char last_ch = 0;
      DWORD got = 0;
      if (!::ReadFile(hIn.get(), &last_ch, 1, &got, nullptr)) {
        const DWORD err = ::GetLastError();
        LOG_ERROR(
            "file_insert_line: read last byte failed, file={}, "
            "GetLastError={}",
            file_path, err);
        return "read last byte failed: " + std::to_string(err);
      }

      if (got == 1 && last_ch != '\n') {
        need_leading_newline = true;
      }
    }
  }

  // ---------- 同目录创建临时文件 ----------
  const std::wstring dir = dir_of(wpath);
  wchar_t tmp_file[MAX_PATH] = {0};
  if (!::GetTempFileNameW(dir.c_str(), L"ins", 0, tmp_file)) {
    const DWORD err = ::GetLastError();
    LOG_ERROR(
        "file_insert_line: GetTempFileNameW failed, file={}, GetLastError={}",
        file_path, err);
    return "temp file failed: " + std::to_string(err);
  }
  TempFileGuard tmp_guard(tmp_file);  // 出错自动删除

  UniqueHandle hOut(::CreateFileW(
      tmp_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!hOut.valid()) {
    const DWORD err = ::GetLastError();
    LOG_ERROR(
        "file_insert_line: create temp file failed, file={}, GetLastError={}",
        file_path, err);
    return "create temp failed: " + std::to_string(err);
  }

  // ---------- 写临时文件：[0,insert_pos) + (可选换行) + data +
  // [insert_pos,EOF) ----------
  bool ok = true;
  DWORD write_err = 0;
  auto fail = [&](DWORD e) {
    if (ok) {
      ok = false;
      write_err = e;
    }
  };

  // 1) 头部
  if (insert_pos > 0) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(hIn.get(), zero, nullptr, FILE_BEGIN))
      fail(::GetLastError());
    else if (!copy_exact(hIn.get(), hOut.get(), insert_pos))
      fail(::GetLastError());
  }

  // 2) 新数据：若需要，先补换行，确保从 line 的下一行开始
  if (ok && len > 0) {
    if (need_leading_newline) {
      const char nl = '\n';
      if (!write_all(hOut.get(), &nl, 1)) fail(::GetLastError());
    }
    if (ok && !write_all(hOut.get(), data, len)) fail(::GetLastError());
  }

  // 3) 尾部
  if (ok) {
    const uint64_t remaining =
        static_cast<uint64_t>(file_size.QuadPart) - insert_pos;
    if (remaining > 0) {
      if (insert_pos == 0) {
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(hIn.get(), zero, nullptr, FILE_BEGIN))
          fail(::GetLastError());
      }
      if (ok && !copy_exact(hIn.get(), hOut.get(), remaining))
        fail(::GetLastError());
    }
  }
  if (ok && !::FlushFileBuffers(hOut.get())) fail(::GetLastError());

  // ---------- 关闭句柄（替换前必须先关）----------
  hIn.close();
  hOut.close();

  if (!ok) {
    LOG_ERROR(
        "file_insert_line: write/copy temp failed, file={}, GetLastError={}",
        file_path, write_err);
    return "write failed: " + std::to_string(write_err);
  }

  // ---------- 原子替换 ----------
  // ReplaceFileW 保留原文件的 ACL / 属性 / 时间戳
  if (!::ReplaceFileW(wpath.c_str(), tmp_file, nullptr,
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
    const DWORD err_replace = ::GetLastError();
    if (!::MoveFileExW(tmp_file, wpath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const DWORD err_move = ::GetLastError();
      LOG_ERROR(
          "file_insert_line: replace failed, file={}, "
          "ReplaceFileW err={}, MoveFileExW err={}",
          file_path, err_replace, err_move);
      return "replace failed: " + std::to_string(err_move);
    }
    LOG_WARN(
        "file_insert_line: fallback MoveFileExW used "
        "(metadata may be lost), file={}, ReplaceFileW err={}",
        file_path, err_replace);
  }
  tmp_guard.dismiss();  // 替换成功后临时文件已不存在

  LOG_INFO(
      "file_insert_line: inserted {} bytes starting at next line after line {} into '{}'",
      len, line, file_path);
  return "success";
}

#endif
/*-file insert-------------------------------------------------------*/

/*-file read-------------------------------------------------------*/
#if defined(__linux__)

[[nodiscard]] bool fail(std::string& out, std::string msg) {
  out = std::move(msg);
  return false;
}

// 线程安全，可移植。等价于 strerror_r，但避免了 GNU/XSI 版本二义性。
std::string errno_str(int e) { return std::generic_category().message(e); }

void append_padded_int(std::string& dst, int64_t n, int width) {
  char buf[32];
  auto res = std::to_chars(buf, buf + sizeof(buf), n);
  const int len = static_cast<int>(res.ptr - buf);
  if (len < width) dst.append(static_cast<std::size_t>(width - len), ' ');
  dst.append(buf, res.ptr);
}

// 返回 true 表示成功（out 为内容）；false 表示失败（error 为原因）。
// 若 end 超过文件总行数，则返回 true 且内容为 [start, 实际最后行]，
// 实际最后行通过 actual_last 返回。
bool file_read_line(const std::string& file_path, int start, int end,
                    std::string& out, std::string& error) {
  out.clear();
  error.clear();

  // ---------- 1. 入参校验 ----------
  if (file_path.empty()) {
    error = "empty file path";
    LOG_ERROR("file_read_line: {}", error);
    return false;
  }
  if (start < 1) {
    error = "invalid start line: " + std::to_string(start);
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }
  if (end < start) {
    error = "invalid range: start=" + std::to_string(start) +
            ", end=" + std::to_string(end);
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }

  // ---------- 2. 文件类型校验（Linux 专用） ----------
  struct stat st{};
  if (::stat(file_path.c_str(), &st) != 0) {
    int e = errno;
    error =
        "stat failed: " + std::error_code(e, std::system_category()).message();
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    error = "not a regular file";
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }

  // ---------- 3. 打开文件 ----------
  std::ifstream ifs(file_path, std::ios::in | std::ios::binary);
  if (!ifs) {
    int e = errno;
    error =
        "cannot open: " + std::error_code(e, std::system_category()).message();
    LOG_ERROR("file_read_line({}): {}", file_path, error);
    return false;
  }

  // ---------- 4. 预算 ----------
  constexpr std::size_t kMaxLineLen = 16ULL * 1024 * 1024;   // 单行上限 16MB
  constexpr std::size_t kMaxTotalOut = 64ULL * 1024 * 1024;  // 总输出上限 64MB

  const std::size_t n_lines =
      static_cast<std::size_t>(end) - static_cast<std::size_t>(start) + 1;
  std::size_t est = (n_lines > kMaxTotalOut / 80) ? kMaxTotalOut : n_lines * 80;
  out.reserve(est);

  // ---------- 5. 逐行读取 ----------
  std::string line;
  int cur = 0;  // 循环条件保证 cur <= end，不会溢出
  while (cur < end && std::getline(ifs, line)) {
    ++cur;
    if (cur < start) continue;

    if (line.size() > kMaxLineLen) {
      out.clear();
      error = "line " + std::to_string(cur) + " exceeds " +
              std::to_string(kMaxLineLen) + " bytes";
      LOG_ERROR("file_read_line({}): {}", file_path, error);
      return false;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::size_t need = std::to_string(cur).size() + 2 + line.size() + 1;
    if (out.size() + need > kMaxTotalOut) {
      out.clear();
      error = "output exceeds " + std::to_string(kMaxTotalOut) + " bytes";
      LOG_ERROR("file_read_line({}): {}", file_path, error);
      return false;
    }
    out.append(std::to_string(cur)).append(": ").append(line).append("\n");
  }

  // ---------- 6. 错误判定 ----------
  if (ifs.bad() || (ifs.fail() && !ifs.eof())) {
    out.clear();
    error = "I/O error while reading '" + file_path + "'";
    LOG_ERROR("file_read_line: {}", error);
    return false;
  }

  if (cur < start) {
    out.clear();
    error = "range out of bounds: start=" + std::to_string(start) +
            ", total lines=" + std::to_string(cur) + ", file='" + file_path +
            "'";
    LOG_ERROR("file_read_line: {}", error);
    return false;
  }

  return true;
}

#elif defined(_WIN32)

std::string WideToUtf8(const wchar_t* w, int len) {
  if (w == nullptr || len <= 0) return {};
  int need =
      ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return {};
  std::string s(static_cast<size_t>(need), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), need, nullptr, nullptr);
  return s;
}

bool PathToWide(const std::string& path, std::wstring& out) {
  if (path.empty()) return false;

  UINT cp = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int len = ::MultiByteToWideChar(cp, flags, path.c_str(),
                                  static_cast<int>(path.size()), nullptr, 0);
  if (len <= 0) {
    cp = CP_ACP;
    flags = 0;
    len = ::MultiByteToWideChar(cp, flags, path.c_str(),
                                static_cast<int>(path.size()), nullptr, 0);
    if (len <= 0) return false;
  }
  out.resize(static_cast<size_t>(len));
  ::MultiByteToWideChar(cp, flags, path.c_str(), static_cast<int>(path.size()),
                        out.data(), len);
  return true;
}

std::string Win32ErrorString(DWORD err) {
  LPWSTR buf = nullptr;
  DWORD n = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buf), 0, nullptr);

  std::string msg;
  if (n > 0 && buf != nullptr) {
    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' ||
                     buf[n - 1] == L' ' || buf[n - 1] == L'\t')) {
      --n;
    }
    msg = WideToUtf8(buf, static_cast<int>(n));
    ::LocalFree(buf);
  }
  if (msg.empty()) msg = "Win32 error " + std::to_string(err);
  return msg;
}

void AppendLine(std::string& out, int lineNo, const std::string& content) {
  char num[16];
  int n = std::snprintf(num, sizeof(num), "%d: ", lineNo);
  if (n > 0) out.append(num, static_cast<size_t>(n));
  out.append(content);
  out.push_back('\n');
}

bool file_read_line(const std::string& file_path, int start, int end,
                    std::string& out) {
  out.clear();

  // ---------- 1. 参数校验 ----------
  if (file_path.empty()) {
    out = "file_read_line error: file_path is empty";
    return false;
  }
  if (start < 1) {
    out = "file_read_line error: start must be >= 1 (got " +
          std::to_string(start) + ")";
    return false;
  }
  if (end <= 0) {
    end = (start > INT_MAX - 2000) ? INT_MAX : start + 2000;
  }
  if (end < start) {
    out = "file_read_line error: end (" + std::to_string(end) +
          ") must be >= start (" + std::to_string(start) + ")";
    return false;
  }

  // ---------- 2. 路径 ----------
  std::wstring wpath;
  if (!PathToWide(file_path, wpath)) {
    out = "file_read_line error: cannot convert path to UTF-16: " + file_path;
    return false;
  }

  // ---------- 3. 打开 ----------
  HANDLE hFile =
      ::CreateFileW(wpath.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    out = "file_read_line error: cannot open \"" + file_path +
          "\": " + Win32ErrorString(::GetLastError());
    return false;
  }
  struct HandleGuard {
    HANDLE h;
    ~HandleGuard() {
      if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
  } guard{hFile};

  // ---------- 4. 流式读取 ----------
  std::vector<char> buf(kBufferSize);
  std::string line;
  line.reserve(512);
  out.reserve(
      std::min<size_t>(kMaxOutputBytes, static_cast<size_t>(4) * 1024 * 1024));

  int curLine = 1;
  int linesWritten = 0;
  bool lineTrunc = false;    // 当前行因 kMaxLineBytes 被截断
  bool outputTrunc = false;  // 输出因 kMaxOutputBytes 被截断
  bool stopRead = false;     // 命中 end 或输出满，不再继续读
  bool firstChunk = true;
  bool fileEnded = false;  // 读到 EOF

  while (!stopRead) {
    DWORD bytesRead = 0;
    if (!::ReadFile(hFile, buf.data(), kBufferSize, &bytesRead, nullptr)) {
      out = "file_read_line error: ReadFile failed: " +
            Win32ErrorString(::GetLastError());
      return false;
    }
    if (bytesRead == 0) {
      fileEnded = true;
      break;
    }

    size_t idx = 0;

    // ---- BOM 检测（仅第一次） ----
    if (firstChunk) {
      firstChunk = false;
      if (bytesRead >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
          static_cast<unsigned char>(buf[1]) == 0xBB &&
          static_cast<unsigned char>(buf[2]) == 0xBF) {
        idx = 3;
      } else if (bytesRead >= 2) {
        unsigned char b0 = static_cast<unsigned char>(buf[0]);
        unsigned char b1 = static_cast<unsigned char>(buf[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
          out =
              "file_read_line error: file is UTF-16LE encoded, "
              "only UTF-8/ANSI is supported: " +
              file_path;
          return false;
        }
        if (b0 == 0xFE && b1 == 0xFF) {
          out =
              "file_read_line error: file is UTF-16BE encoded, "
              "only UTF-8/ANSI is supported: " +
              file_path;
          return false;
        }
      }
    }

    // ---- 逐行扫描 ----
    while (idx < bytesRead) {
      const char* base = buf.data();
      const void* nl = std::memchr(base + idx, '\n', bytesRead - idx);
      size_t nlIdx =
          nl ? static_cast<size_t>(static_cast<const char*>(nl) - base)
             : static_cast<size_t>(bytesRead);
      size_t segLen = nlIdx - idx;

      // 追加本段（受行上限约束；一旦超限就只保留首个越界位置）
      if (segLen > 0 && !lineTrunc) {
        size_t room = kMaxLineBytes - line.size();
        if (segLen <= room) {
          line.append(base + idx, segLen);
        } else {
          line.append(base + idx, room);
          lineTrunc = true;
        }
      }

      if (nl == nullptr) break;  // 行未结束，等下一 chunk

      // ---- 一行结束 ----
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (lineTrunc) line += "...[line truncated]";

      if (curLine >= start && curLine <= end) {
        // 预留 snprintf 行号、"..." 与 '\n' 的空间
        if (out.size() + line.size() + 32 > kMaxOutputBytes) {
          outputTrunc = true;
          stopRead = true;
          break;
        }
        AppendLine(out, curLine, line);
        ++linesWritten;
      }

      ++curLine;
      line.clear();
      lineTrunc = false;
      idx = nlIdx + 1;

      if (curLine > end) {
        stopRead = true;
        break;
      }
    }
  }

  // ---------- 5. 末行（无尾换行符） ----------
  // 只有当读到了文件末尾且当前还有累积内容时才算末行；
  // 若因 stopRead（命中 end / 输出满）退出，说明已处理完所需内容。
  if (fileEnded && !line.empty() && curLine >= start && curLine <= end) {
    if (line.back() == '\r') line.pop_back();
    if (lineTrunc) line += "...[line truncated]";
    if (out.size() + line.size() + 32 <= kMaxOutputBytes) {
      AppendLine(out, curLine, line);
      ++linesWritten;
    } else {
      outputTrunc = true;
    }
  }

  // ---------- 6. 结果 ----------
  if (linesWritten == 0) {
    out = "file_read_line error: line range [" + std::to_string(start) + ", " +
          std::to_string(end) + "] contains no content";
    if (fileEnded) {
      // 读到 EOF 时 curLine-1 就是总行数
      out += "; file has only " + std::to_string(curLine - 1) + " line(s)";
    }
    out += ": " + file_path;
    return false;
  }

  if (outputTrunc) {
    out += "...[output truncated at ";
    out += std::to_string(kMaxOutputBytes);
    out += " bytes]\n";
  }
  return true;
}

#endif
/*-file read-------------------------------------------------------*/

/*-file delete-------------------------------------------------------*/
#if defined(__linux__)
// ---------- errno_str ----------
// std::string errno_str(int e) {
//   char buf[256] = {};
// #if defined(__GLIBC__) && defined(_GNU_SOURCE)
//   const char* s = ::strerror_r(e, buf, sizeof(buf));
//   if (s && s[0]) return std::string(s) + " (errno=" + std::to_string(e) +
//   ")";
// #else
//   if (::strerror_r(e, buf, sizeof(buf)) == 0 && buf[0])
//     return std::string(buf) + " (errno=" + std::to_string(e) + ")";
// #endif
//   return "errno=" + std::to_string(e);
// }

// ---------- copy_xattrs ----------
// 返回 0：成功（可能包含非 security.* 的警告，已记录）
// 返回 -1：list 失败，或 security.* 复制失败（安全上下文可能丢失）
// 说明：Linux 6.12+，src/dst 均为普通文件的 fd。
//       非 security.* 的失败仅记录 LOG_WARN，不影响返回值。
int copy_xattrs(int src, int dst) {
  std::vector<char> names;
  bool listed = false;

  // 获取名称列表，处理 ERANGE（期间有 xattr 被增加/删除）
  for (int attempt = 0; attempt < 8; ++attempt) {
    errno = 0;
    const ssize_t len = ::flistxattr(src, nullptr, 0);
    if (len < 0) {
      if (errno == ENOTSUP || errno == EOPNOTSUPP)
        return 0;  // src 不支持 xattr
      return -1;
    }
    if (len == 0) return 0;  // 无 xattr
    names.resize(static_cast<size_t>(len));
    const ssize_t nread = ::flistxattr(src, names.data(), names.size());
    if (nread < 0) {
      if (errno == ERANGE) continue;
      return -1;
    }
    if (nread == 0) return 0;
    names.resize(static_cast<size_t>(nread));
    listed = true;
    break;
  }
  if (!listed) {  // 多次重试仍 ERANGE
    errno = ERANGE;
    return -1;
  }

  int rc = 0;  // -1 表示 security.* 失败
  for (size_t i = 0; i < names.size();) {
    const char* name = names.data() + i;
    const size_t nlen = std::strlen(name);
    if (nlen == 0) {
      ++i;
      continue;
    }
    i += nlen + 1;

    const bool is_security = (std::strncmp(name, "security.", 9) == 0);

    // 读取值：先取长度，再读内容；ERANGE 重试
    std::vector<char> value;
    ssize_t got = -1;
    bool enodata = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
      const ssize_t vlen = ::fgetxattr(src, name, nullptr, 0);
      if (vlen < 0) {
        if (errno == ENODATA) enodata = true;  // 并发删除
        break;
      }
      value.assign(static_cast<size_t>(vlen), '\0');
      got = ::fgetxattr(src, name, value.data(), value.size());
      if (got >= 0) break;
      if (errno != ERANGE) break;
    }
    if (enodata) continue;
    if (got < 0) {
      if (is_security) {
        LOG_ERROR("copy_xattrs: fgetxattr('{}') failed: {}", name,
                  errno_str(errno));
        rc = -1;
      } else {
        LOG_WARN("copy_xattrs: fgetxattr('{}') failed: {}", name,
                 errno_str(errno));
      }
      continue;
    }
    value.resize(static_cast<size_t>(got));

    // size==0 时用 "" 而不是 nullptr，兼容性更好
    const char* vptr = value.empty() ? "" : value.data();
    if (::fsetxattr(dst, name, vptr, value.size(), 0) < 0) {
      if (is_security) {
        // security.selinux / security.capability 丢失属于安全边界变化
        LOG_ERROR("copy_xattrs: fsetxattr('{}') on target failed: {}", name,
                  errno_str(errno));
        rc = -1;
      } else {
        LOG_WARN("copy_xattrs: fsetxattr('{}') on target failed: {}", name,
                 errno_str(errno));
      }
    }
  }
  return rc;
}

// ---------- locate_lines ----------
int locate_lines(int fd, off_t file_size, int start, int end, off_t* S_out,
                 off_t* E_out) {
  if (file_size < 0) {
    errno = EINVAL;
    return -1;
  }
  if (start < 1 || end < start) {
    errno = EINVAL;
    return -1;
  }
  if (!S_out || !E_out) {
    errno = EINVAL;
    return -1;
  }
  *S_out = 0;
  *E_out = 0;

  off_t S = 0;
  bool found_S = (start == 1);
  const int64_t end_p1 = static_cast<int64_t>(end) + 1;
  int64_t cur = 1;
  off_t pos = 0;

  constexpr size_t BUF_SZ = 256 * 1024;
  std::vector<char> buf(BUF_SZ);

  while (pos < file_size) {
    const off_t want =
        std::min<off_t>(static_cast<off_t>(BUF_SZ), file_size - pos);
    ssize_t n;
    do {
      n = ::pread(fd, buf.data(), static_cast<size_t>(want), pos);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (n == 0) {
      errno = EIO;
      return -1;
    }  // 文件被并发截断

    size_t i = 0;
    const size_t un = static_cast<size_t>(n);
    const char* base = buf.data();
    while (i < un) {
      const void* p = ::memchr(base + i, '\n', un - i);
      if (!p) break;
      const size_t idx = static_cast<const char*>(p) - base;
      const off_t next = pos + static_cast<off_t>(idx) + 1;
      ++cur;
      if (cur == end_p1) {
        *S_out = S;
        *E_out = next;
        return 0;
      }
      if (!found_S && cur == start) {
        S = next;
        found_S = true;
      }
      i = idx + 1;
    }
    pos += static_cast<off_t>(n);
  }

  int64_t total_lines = 0;
  if (file_size > 0) {
    char lastc = 0;
    ssize_t n;
    do {
      n = ::pread(fd, &lastc, 1, file_size - 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) return -1;
    total_lines = (lastc == '\n') ? (cur - 1) : cur;
  }
  if (static_cast<int64_t>(start) > total_lines) return -2;
  if (!found_S) return -2;
  *S_out = S;
  *E_out = file_size;
  return 0;
}

// ---------- copy_fd_range ----------
int copy_fd_range(int src, int dst, off_t src_offset, off_t dst_offset,
                  off_t length) {
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  if (length == 0) return 0;

  // 防御：O_APPEND 下 pwrite/copy_file_range 会忽略显式偏移
  {
    const int fl = ::fcntl(dst, F_GETFL);
    if (fl < 0) return -1;
    if (fl & O_APPEND) {
      errno = EINVAL;
      return -1;
    }
  }

  off_t sp = src_offset;
  off_t dp = dst_offset;
  off_t remaining = length;

  // 阶段一：copy_file_range
  while (remaining > 0) {
    const size_t chunk = static_cast<size_t>(
        std::min<off_t>(static_cast<off_t>(SIZE_MAX / 2), remaining));
    const ssize_t n = ::copy_file_range(src, &sp, dst, &dp, chunk, 0);
    if (n > 0) {
      remaining -= n;
      continue;
    }
    if (n == 0) {
      errno = EIO;
      return -1;
    }
    if (errno == EINTR) continue;
    if (errno == EXDEV || errno == ENOSYS || errno == EOPNOTSUPP ||
        errno == EINVAL || errno == EPERM || errno == EBADF) {
      break;  // 退回 pread/pwrite
    }
    return -1;
  }
  if (remaining == 0) return 0;

  // 阶段二：pread/pwrite 显式偏移
  constexpr size_t BUF_SZ = 1024 * 1024;
  std::vector<char> buf(BUF_SZ);

  while (remaining > 0) {
    const size_t want = static_cast<size_t>(
        std::min<off_t>(static_cast<off_t>(BUF_SZ), remaining));
    ssize_t r;
    do {
      r = ::pread(src, buf.data(), want, sp);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    if (r == 0) {
      errno = EIO;
      return -1;
    }

    ssize_t written = 0;
    while (written < r) {
      const ssize_t w =
          ::pwrite(dst, buf.data() + written, static_cast<size_t>(r - written),
                   dp + written);
      if (w < 0) {
        if (errno == EINTR) continue;
        return -1;
      }
      if (w == 0) {
        errno = EIO;
        return -1;
      }
      written += w;
    }
    sp += r;
    dp += r;
    remaining -= r;
  }
  return 0;
}

// ---------- DeleteLineCleanup（保持不变） ----------
struct DeleteLineCleanup {
  int dirfd = -1;
  int fd = -1;
  int tmpfd = -1;
  std::string tmp_name;
  bool renamed = false;

  ~DeleteLineCleanup() {
    if (tmpfd >= 0) ::close(tmpfd);
    if (fd >= 0) ::close(fd);
    if (dirfd >= 0) {
      if (!renamed && !tmp_name.empty()) ::unlinkat(dirfd, tmp_name.c_str(), 0);
      ::close(dirfd);
    }
  }
};

// ---------- file_delete_line ----------
std::string file_delete_line(const std::string& file_path, int start, int end) {
  auto fail = [&](const std::string& reason) -> std::string {
    LOG_ERROR("file_delete_line failed: file='{}', start={}, end={}, reason={}",
              file_path, start, end, reason);
    return reason;
  };

  if (file_path.empty()) return fail("file_path is empty");
  if (start < 1)
    return fail("start must be >= 1, got " + std::to_string(start));
  if (end < start)
    return fail("end (" + std::to_string(end) + ") must be >= start (" +
                std::to_string(start) + ")");

  static_assert(sizeof(off_t) >= 8, "off_t must be 64-bit");

  char resolved[PATH_MAX];
  if (::realpath(file_path.c_str(), resolved) == nullptr)
    return fail("realpath: " + errno_str(errno));
  const std::string path(resolved);

  std::string dir, base;
  {
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
      dir = ".";
      base = path;
    } else if (pos == 0) {
      dir = "/";
      base = path.substr(1);
    } else {
      dir = path.substr(0, pos);
      base = path.substr(pos + 1);
    }
  }
  if (base.empty()) return fail("invalid file name: " + path);

  try {
    DeleteLineCleanup cl;

    cl.dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cl.dirfd < 0) return fail("open dir: " + errno_str(errno));

    cl.fd = ::openat(cl.dirfd, base.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (cl.fd < 0) return fail("open: " + errno_str(errno));

    // advisory 独占锁，抑制协作进程并发编辑
    if (::flock(cl.fd, LOCK_EX | LOCK_NB) < 0) {
      if (errno == EWOULDBLOCK)
        return fail("file is locked by another process");
      if (errno != ENOTSUP && errno != EOPNOTSUPP)
        return fail("flock: " + errno_str(errno));
      // 不支持 flock 的文件系统：继续（依靠后续并发检测兜底）
    }

    struct stat st{};
    if (::fstat(cl.fd, &st) < 0) return fail("fstat: " + errno_str(errno));
    if (!S_ISREG(st.st_mode)) return fail("not a regular file");
    if (st.st_nlink > 1)
      return fail(
          "file has multiple hard links (nlink=" + std::to_string(st.st_nlink) +
          "); refusing to modify in place");

    // 定位 S / E
    off_t S = 0, E = 0;
    const int rc = locate_lines(cl.fd, st.st_size, start, end, &S, &E);
    if (rc == -2)
      return fail("start line " + std::to_string(start) +
                  " exceeds file line count");
    if (rc < 0) return fail("locate_lines: " + errno_str(errno));

    // 创建临时文件（O_TMPFILE 在跨文件系统 linkat 时可用性受限，
    // 保留 O_CREAT|O_EXCL + 随机名方式，RAII 保证清理）
    {
      const auto pid = static_cast<unsigned long>(::getpid());
      const auto ts = static_cast<unsigned long long>(::time(nullptr));
      for (int attempt = 0; attempt < 100; ++attempt) {
        char nb[80];
        std::snprintf(nb, sizeof(nb), ".del.%lx.%llx.%x", pid, ts, attempt);
        cl.tmpfd =
            ::openat(cl.dirfd, nb, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (cl.tmpfd >= 0) {
          cl.tmp_name = nb;
          break;
        }
        if (errno != EEXIST) break;
      }
      if (cl.tmpfd < 0) return fail("create temp: " + errno_str(errno));
    }

    // 元数据顺序很关键：
    //   fchown 会清除 capability；fchmod 也会清除 capability
    //   所以 fchown -> fchmod -> copy_xattrs（security.capability 最后落）
    bool chown_ok = true;
    if (::fchown(cl.tmpfd, st.st_uid, st.st_gid) < 0) {
      const int e = errno;
      if (e != EPERM) return fail("fchown: " + errno_str(e));
      chown_ok = false;
    }
    mode_t mode = st.st_mode & 07777;
    if (!chown_ok) {
      // 归属没保住时，绝不把 setuid/setgid 落在当前用户的新文件上
      mode &= ~(S_ISUID | S_ISGID);
    }
    if (::fchmod(cl.tmpfd, mode) < 0)
      return fail("fchmod: " + errno_str(errno));

    // security.* 复制失败视为致命；其他失败仅日志
    if (copy_xattrs(cl.fd, cl.tmpfd) < 0)
      return fail(
          "copy_xattrs: critical xattr copy failed "
          "(see log for details)");

    // 复制头段 / 尾段
    if (copy_fd_range(cl.fd, cl.tmpfd, 0, 0, S) < 0) {
      const int e = errno;
      return fail("copy head: " + errno_str(e));
    }
    const off_t tail_len = st.st_size - E;
    if (tail_len > 0 && copy_fd_range(cl.fd, cl.tmpfd, E, S, tail_len) < 0) {
      const int e = errno;
      return fail("copy tail: " + errno_str(e));
    }

    // 落盘
    if (::fsync(cl.tmpfd) < 0) {
      const int e = errno;
      return fail("fsync tmp: " + errno_str(e));
    }
    if (::close(cl.tmpfd) < 0) {
      const int e = errno;
      cl.tmpfd = -1;  // POSIX close 已释放 fd
      return fail("close tmp: " + errno_str(e));
    }
    cl.tmpfd = -1;

    // 并发修改检测（dev/ino/size/mtime/ctime）
    {
      struct stat st_now{};
      if (::fstatat(cl.dirfd, base.c_str(), &st_now, AT_SYMLINK_NOFOLLOW) < 0) {
        const int e = errno;
        return fail("fstatat: " + errno_str(e));
      }
      if (st_now.st_dev != st.st_dev || st_now.st_ino != st.st_ino ||
          st_now.st_size != st.st_size ||
          st_now.st_mtim.tv_sec != st.st_mtim.tv_sec ||
          st_now.st_mtim.tv_nsec != st.st_mtim.tv_nsec ||
          st_now.st_ctim.tv_sec != st.st_ctim.tv_sec ||
          st_now.st_ctim.tv_nsec != st.st_ctim.tv_nsec) {
        return fail("source file was modified concurrently");
      }
    }

    // 原子替换
    if (::renameat(cl.dirfd, cl.tmp_name.c_str(), cl.dirfd, base.c_str()) < 0)
      return fail("renameat: " + errno_str(errno));
    cl.renamed = true;
    cl.tmp_name.clear();

    // rename 之后再释放源 fd，让 flock 覆盖替换动作
    ::close(cl.fd);
    cl.fd = -1;

    // 持久化目录项（已不可回滚，失败仅记录）
    if (::fsync(cl.dirfd) < 0) {
      LOG_ERROR(
          "file_delete_line: fsync dir '{}' failed: {} "
          "(file already replaced, result may not survive crash)",
          dir, errno_str(errno));
    }
    return "success";
  } catch (const std::exception& e) {
    return fail(std::string("exception: ") + e.what());
  }
}
#elif defined(_WIN32)
// UTF-8 std::string -> UTF-16 std::wstring
std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int src_len = static_cast<int>(s.size());
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                      src_len, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), src_len,
                            w.data(), n) != n) {
    return {};
  }
  return w;
}

std::string file_delete_line(const std::string& file_path, int start, int end) {
  // ---------- 1. 参数校验 ----------
  if (file_path.empty()) {
    const std::string err = "file_delete_line: file_path is empty";
    LOG_ERROR("{}", err);
    return err;
  }
  if (start < 1) {
    const std::string err =
        "file_delete_line: start must be >= 1, got " + std::to_string(start);
    LOG_ERROR("{}", err);
    return err;
  }
  if (end < start) {
    const std::string err = "file_delete_line: end(" + std::to_string(end) +
                            ") < start(" + std::to_string(start) + ")";
    LOG_ERROR("{}", err);
    return err;
  }

  // ---------- 2. 路径转换 ----------
  const std::wstring wpath = utf8_to_wide(file_path);
  if (wpath.empty()) {
    const std::string err =
        "file_delete_line: invalid UTF-8 file_path: " + file_path;
    LOG_ERROR("{}", err);
    return err;
  }

  // ---------- 3. 检查文件 ----------
  const DWORD attrs = ::GetFileAttributesW(wpath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) {
    const DWORD e = ::GetLastError();
    const std::string err =
        "file_delete_line: file not accessible: " + file_path +
        " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
    const std::string err =
        "file_delete_line: path is a directory: " + file_path;
    LOG_ERROR("{}", err);
    return err;
  }

  // 只读文件清掉属性，否则替换会失败
  const bool was_readonly = (attrs & FILE_ATTRIBUTE_READONLY) != 0;
  ReadonlyRestore ro_guard{&wpath, was_readonly};
  if (was_readonly) {
    if (!::SetFileAttributesW(wpath.c_str(),
                              attrs & ~FILE_ATTRIBUTE_READONLY)) {
      const DWORD e = ::GetLastError();
      const std::string err = "file_delete_line: cannot clear read-only on '" +
                              file_path +
                              "' (GetLastError=" + std::to_string(e) + ")";
      LOG_ERROR("{}", err);
      return err;
    }
  }

  // ---------- 4. 打开源文件 ----------
  FILE* raw_in = nullptr;
  if (::_wfopen_s(&raw_in, wpath.c_str(), L"rb") != 0 || raw_in == nullptr) {
    const DWORD e = ::GetLastError();
    const std::string err =
        "file_delete_line: cannot open for reading: " + file_path +
        " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  FileGuard in{raw_in};

  // ---------- 5. 创建同目录临时文件 ----------
  std::wstring tmp_dir = wpath;
  const auto sep = tmp_dir.find_last_of(L"\\/");
  if (sep != std::wstring::npos) {
    tmp_dir.resize(sep + 1);
  } else {
    tmp_dir = L".\\";
  }

  wchar_t tmp_path[MAX_PATH] = {};
  if (::GetTempFileNameW(tmp_dir.c_str(), L"fdl", 0, tmp_path) == 0) {
    const DWORD e = ::GetLastError();
    const std::string err =
        "file_delete_line: GetTempFileNameW failed for: " + file_path +
        " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  TempGuard temp{std::wstring(tmp_path)};

  FILE* raw_out = nullptr;
  if (::_wfopen_s(&raw_out, tmp_path, L"wb") != 0 || raw_out == nullptr) {
    const DWORD e = ::GetLastError();
    const std::string err =
        "file_delete_line: cannot create temp file for: " + file_path +
        " (GetLastError=" + std::to_string(e) + ")";
    LOG_ERROR("{}", err);
    return err;
  }
  FileGuard out{raw_out};

  // ---------- 6. 带缓冲逐块拷贝，跳过 [start, end] ----------
  // 保留原始行尾：\r 被当作普通字节收集，遇 \n 结束一行
  constexpr size_t kBufSize = 64 * 1024;
  std::vector<char> buf(kBufSize);

  bool ok = true;
  std::string err_reason;
  int line_no = 1;
  int total_lines = 0;  // 已确认存在的行数

  while (ok) {
    const size_t n = std::fread(buf.data(), 1, kBufSize, in.get());
    if (n == 0) break;

    size_t i = 0;
    while (i < n) {
      const char* nl =
          static_cast<const char*>(std::memchr(buf.data() + i, '\n', n - i));
      const size_t end_idx = nl ? static_cast<size_t>(nl - buf.data()) : n;
      const size_t chunk_len = end_idx - i;

      const bool keep = (line_no < start) || (line_no > end);

      if (chunk_len > 0) {
        // 该片段属于第 line_no 行 => 第 line_no 行确实存在
        if (line_no > total_lines) total_lines = line_no;
        if (keep &&
            std::fwrite(buf.data() + i, 1, chunk_len, out.get()) != chunk_len) {
          ok = false;
          err_reason = "fwrite failed";
          break;
        }
      }

      if (nl) {
        // 遇到 '\n' => 第 line_no 行（可能是空行）收尾
        if (line_no > total_lines) total_lines = line_no;
        if (keep && std::fputc('\n', out.get()) == EOF) {
          ok = false;
          err_reason = "fputc failed";
          break;
        }
        ++line_no;
        i = end_idx + 1;
      } else {
        i = n;  // 行可能跨块，交给下一次 fread
      }
    }
  }

  if (ok && std::ferror(in.get())) {
    ok = false;
    err_reason = "read error";
  }

  // ---------- 6b. start 超过文件总行数 -> 报错 ----------
  // 注意：end > total_lines 是允许的（删除到末尾）；
  //       仅当 start 越界时视为失败，原文件保持不变。
  if (ok && start > total_lines) {
    // 先关闭句柄，再让 RAII 清理临时文件
    in.reset();
    out.reset();
    const std::string err = "file_delete_line: start(" + std::to_string(start) +
                            ") exceeds total lines(" +
                            std::to_string(total_lines) + ") in '" + file_path +
                            "'";
    LOG_ERROR("{}", err);
    return err;
  }

  if (ok && std::fflush(out.get()) != 0) {
    ok = false;
    err_reason = "fflush failed";
  }
  if (ok) {
    // 确保数据落到磁盘，然后才能安全替换
    const int fd = ::_fileno(out.get());
    if (fd >= 0) {
      HANDLE h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
      if (h != INVALID_HANDLE_VALUE && !::FlushFileBuffers(h)) {
        // 落盘失败通常也代表写失败
        ok = false;
        err_reason = "FlushFileBuffers failed";
      }
    }
  }

  // 关闭句柄（替换前必须关闭）
  in.reset();
  out.reset();

  if (!ok) {
    const std::string err =
        "file_delete_line: I/O error on '" + file_path + "': " + err_reason;
    LOG_ERROR("{}", err);
    return err;  // TempGuard 自动删除临时文件
  }

  // ---------- 7. 原子替换原文件 ----------
  // 优先 ReplaceFileW：保留 ACL / 创建时间 / 属性等
  if (!::ReplaceFileW(wpath.c_str(), tmp_path, nullptr,
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
    const DWORD replace_err = ::GetLastError();
    // 回退：MoveFileExW
    if (!::MoveFileExW(tmp_path, wpath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const DWORD e = ::GetLastError();
      const std::string err = "file_delete_line: replace failed for '" +
                              file_path +
                              "' (ReplaceFile=" + std::to_string(replace_err) +
                              ", MoveFileEx=" + std::to_string(e) + ")";
      LOG_ERROR("{}", err);
      return err;
    }
  }
  temp.committed = true;

  // 恢复只读（ro_guard 析构也会做，这里显式让流程清楚）
  if (was_readonly) {
    const DWORD a = ::GetFileAttributesW(wpath.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) {
      ::SetFileAttributesW(wpath.c_str(), a | FILE_ATTRIBUTE_READONLY);
    }
  }
  ro_guard.active = false;

  return "success";
}
#endif
/*-file delete-------------------------------------------------------*/

/*-ripgrep-------------------------------------------------------*/
#if defined(__linux__)

// ============================================================
//  greptool.hpp
//  grep tool：调用 ripgrep 搜索文件，返回按文件分组的匹配行。
//  Requires: glibc >= 2.29 (posix_spawn_file_actions_addchdir_np)
//            Linux 6.12+, ripgrep 0.10+
// ============================================================

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // pipe2 / mkostemp / addchdir_np / O_CLOEXEC
#endif

static std::string errno_msg(const char* what, int err) {
  std::string s(what);
  s += ": ";
  s += std::generic_category().message(err);
  return s;
}

static bool has_nul(const std::string& s) {
  return s.find('\0') != std::string::npos;
}

static std::string trim_ws(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  std::size_t i = 0;
  while (i < s.size() && (s[i] == '\n' || s[i] == '\r' || s[i] == ' ')) ++i;
  s.erase(0, i);
  return s;
}

static bool write_all(int fd, const char* data, std::size_t len) {
  while (len > 0) {
    ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    len -= static_cast<std::size_t>(n);
  }
  return true;
}
static bool write_all(int fd, std::string_view s) {
  return write_all(fd, s.data(), s.size());
}

static bool path_inside(const std::string& base, const std::string& p) {
  if (base == "/") return !p.empty() && p.front() == '/';
  if (p == base) return true;
  return p.size() > base.size() && p.compare(0, base.size(), base) == 0 &&
         p[base.size()] == '/';
}

// ---------------- GrepTool ----------------

GrepTool::GrepTool(std::string workspace, std::size_t inline_limit,
                   std::size_t max_line_bytes, GrepOptions options)
    : workspace_(std::move(workspace)),
      inline_limit_(std::max<std::size_t>(1, inline_limit)),
      max_line_bytes_(std::clamp<std::size_t>(max_line_bytes, std::size_t{64},
                                              std::size_t{1} << 20)),
      options_(std::move(options)) {}

GrepResult GrepTool::run(const GrepParams& params) const {
  // 参数/环境错误 -> WARN（调用方问题）；系统错误 -> ERROR（本地环境问题）
  auto fail_user = [](std::string msg) -> GrepResult {
    LOG_WARN("grep: {}", msg);
    return GrepResult::failure(std::move(msg));
  };
  auto fail_sys = [](std::string msg) -> GrepResult {
    LOG_ERROR("grep: {}", msg);
    return GrepResult::failure(std::move(msg));
  };

  // ---- 入参校验 ---------------------------------------------------------
  if (workspace_.empty()) return fail_user("workspace must not be empty");
  if (has_nul(workspace_)) return fail_user("workspace contains NUL");
  if (params.pattern.empty()) return fail_user("pattern must not be empty");
  if (has_nul(params.pattern)) return fail_user("pattern contains NUL");
  if (options_.rg_path.empty()) return fail_user("rg_path must not be empty");
  if (has_nul(options_.rg_path)) return fail_user("rg_path contains NUL");
  if (options_.timeout.count() < 0)
    return fail_user("timeout must be non-negative");
  if (options_.inline_bytes_limit == 0)
    return fail_user("inline_bytes_limit must be > 0");

  if (params.include) {
    if (params.include->empty())
      return fail_user("include must not be empty when provided");
    if (has_nul(*params.include)) return fail_user("include contains NUL");
    if (params.include->front() == '!')
      return fail_user("include does not support negation ('!' prefix)");
    if (params.include->find("..") != std::string::npos)
      return fail_user("include must not contain '..'");
  }
  if (params.path && has_nul(*params.path))
    return fail_user("path contains NUL");

  // ---- 工作区 / 目标路径 ------------------------------------------------
  std::error_code ec;
  fs::path ws = fs::weakly_canonical(fs::path(workspace_), ec);
  if (ec) return fail_user("invalid workspace: " + ec.message());
  ec.clear();
  if (!fs::is_directory(ws, ec)) {
    if (ec) return fail_sys("workspace check failed: " + ec.message());
    return fail_user("workspace is not a directory: " + ws.native());
  }

  fs::path target;
  if (params.path && !params.path->empty()) {
    fs::path p(*params.path);
    ec.clear();
    target = fs::weakly_canonical(p.is_absolute() ? p : ws / p, ec);
    if (ec)
      return fail_user("invalid path '" + *params.path + "': " + ec.message());
    if (!path_inside(ws.native(), target.native()))
      return fail_user("path '" + *params.path +
                       "' escapes the session workspace");
    ec.clear();
    if (!fs::exists(target, ec)) {
      if (ec) return fail_sys("path stat failed: " + ec.message());
      return fail_user("path does not exist: " + target.native());
    }
  } else {
    target = ws;
  }

  // ---- target 相对化（配合 addchdir_np）--------------------------------
  // rg 的 -g glob 语义是"相对 cwd 匹配"，所以必须在子进程里把 cwd 设为
  // workspace，否则 include 的行为与 path/workspace 完全错位。
  // 为了输出也一致，把 target 转成相对 workspace 的路径传给 rg。
  fs::path rel_target = target.lexically_relative(ws);
  if (rel_target.empty()) rel_target = ".";
  std::string target_arg = rel_target.native();

  // ---- argv ------------------------------------------------------------
  std::vector<std::string> argv;
  argv.reserve(20);
  argv.emplace_back(options_.rg_path);
  argv.emplace_back("--line-number");
  argv.emplace_back("--with-filename");
  argv.emplace_back("--no-heading");
  argv.emplace_back("--color=never");
  argv.emplace_back("--binary-files=without-match");
  argv.emplace_back("--max-columns");
  argv.emplace_back(std::to_string(max_line_bytes_));
  argv.emplace_back("--max-columns-preview");

  // 分组策略：
  //   --sort=path ：严格排序 + 按文件分组，但 rg 内存缓冲全部命中（大搜索 OOM
  //   风险） -j1        ：单线程按文件遍历顺序输出（分组但非字典序），内存恒定
  if (options_.sort_by_path) {
    argv.emplace_back("--sort=path");
  } else {
    argv.emplace_back("-j1");
  }

  if (params.include) {
    argv.emplace_back("-g");
    argv.emplace_back(*params.include);
  }

  argv.emplace_back("--");
  argv.emplace_back(params.pattern);
  argv.emplace_back(target_arg);

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  // ---- pipe ------------------------------------------------------------
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe2(out_pipe, O_CLOEXEC) != 0)
    return fail_sys(errno_msg("pipe2(stdout)", errno));
  if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
    int e = errno;
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return fail_sys(errno_msg("pipe2(stderr)", e));
  }
  // 若 0/1/2 被关闭，pipe2 可能返回这些 fd，随后的 addclose
  // 会误关子进程标准流。
  if (out_pipe[0] <= 2 || out_pipe[1] <= 2 || err_pipe[0] <= 2 ||
      err_pipe[1] <= 2) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    return fail_sys("standard fds 0-2 are not open; cannot spawn child safely");
  }

  auto close_pipes = [&]() {
    if (out_pipe[0] >= 0) {
      ::close(out_pipe[0]);
      out_pipe[0] = -1;
    }
    if (out_pipe[1] >= 0) {
      ::close(out_pipe[1]);
      out_pipe[1] = -1;
    }
    if (err_pipe[0] >= 0) {
      ::close(err_pipe[0]);
      err_pipe[0] = -1;
    }
    if (err_pipe[1] >= 0) {
      ::close(err_pipe[1]);
      err_pipe[1] = -1;
    }
  };

  // ---- posix_spawn_file_actions ---------------------------------------
  posix_spawn_file_actions_t fa;
  if (int r = posix_spawn_file_actions_init(&fa); r != 0) {
    close_pipes();
    return fail_sys(errno_msg("posix_spawn_file_actions_init", r));
  }

  auto cleanup_and_fail = [&](const char* op, int err) -> GrepResult {
    posix_spawn_file_actions_destroy(&fa);
    close_pipes();
    return fail_sys(errno_msg(op, err));
  };

  if (int r = posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
      r != 0)
    return cleanup_and_fail("adddup2(stdout)", r);
  if (int r = posix_spawn_file_actions_adddup2(&fa, err_pipe[1], STDERR_FILENO);
      r != 0)
    return cleanup_and_fail("adddup2(stderr)", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, out_pipe[0]); r != 0)
    return cleanup_and_fail("addclose(stdout[0])", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, out_pipe[1]); r != 0)
    return cleanup_and_fail("addclose(stdout[1])", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, err_pipe[0]); r != 0)
    return cleanup_and_fail("addclose(stderr[0])", r);
  if (int r = posix_spawn_file_actions_addclose(&fa, err_pipe[1]); r != 0)
    return cleanup_and_fail("addclose(stderr[1])", r);

#if GREPTOOL_HAS_SPAWN_CHDIR
  // 关键：把子进程 cwd 设为 workspace，让 -g glob 与 target 在同一根下解释。
  if (int r = posix_spawn_file_actions_addchdir_np(&fa, ws.c_str()); r != 0)
    return cleanup_and_fail("addchdir_np(workspace)", r);
#else
  posix_spawn_file_actions_destroy(&fa);
  close_pipes();
  return fail_sys(
      "posix_spawn_file_actions_addchdir_np not available (glibc >= 2.29 required)");
#endif

  // ---- posix_spawnattr：独立进程组 + 空信号掩码 -----------------------
  posix_spawnattr_t attr;
  if (int r = posix_spawnattr_init(&attr); r != 0) {
    posix_spawn_file_actions_destroy(&fa);
    close_pipes();
    return fail_sys(errno_msg("posix_spawnattr_init", r));
  }
  auto attr_fail = [&](const char* op, int err) -> GrepResult {
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close_pipes();
    return fail_sys(errno_msg(op, err));
  };

  // SETPGROUP 失败必须视为错误：否则孙进程不受进程组管控，超时后 SIGKILL -pid
  // 无效。
  if (int r = posix_spawnattr_setflags(
          &attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
      r != 0)
    return attr_fail("posix_spawnattr_setflags", r);
  if (int r = posix_spawnattr_setpgroup(&attr, 0); r != 0)
    return attr_fail("posix_spawnattr_setpgroup", r);

  // 清空信号掩码，避免父进程屏蔽 SIGPIPE 等导致 rg 行为异常。
  sigset_t empty_mask;
  ::sigemptyset(&empty_mask);
  if (int r = posix_spawnattr_setsigmask(&attr, &empty_mask); r != 0)
    return attr_fail("posix_spawnattr_setsigmask", r);

  pid_t pid = -1;
  int spawn_rc = ::posix_spawnp(&pid, options_.rg_path.c_str(), &fa, &attr,
                                cargv.data(), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&fa);
  ::close(out_pipe[1]);
  out_pipe[1] = -1;
  ::close(err_pipe[1]);
  err_pipe[1] = -1;

  if (spawn_rc != 0) {
    close_pipes();
    return fail_sys(errno_msg("posix_spawnp(rg)", spawn_rc));
  }

  // 从此刻起，任何异常路径都必须回收子进程
  ChildGuard guard;
  guard.adopt(pid);

  // ---- 流式读取 --------------------------------------------------------
  const std::size_t limit = inline_limit_;
  const std::size_t max_partial = max_line_bytes_ * 4 + kMaxPartialExtra;

  std::vector<std::string> head;
  head.reserve(std::min<std::size_t>(limit, 4096));

  std::size_t total = 0;
  std::size_t head_bytes = 0;
  bool truncated = false;
  bool io_error = false;
  bool spill_full = false;
  bool partial_truncated = false;
  bool any_line_truncated = false;
  TempFile spill;
  std::string partial;

  auto emit_line = [&](std::string_view line) {
    ++total;
    const bool fits_inline =
        (total <= limit) &&
        (head_bytes + line.size() + 1 <= options_.inline_bytes_limit);
    if (fits_inline) {
      head.emplace_back(line.data(), line.size());
      head_bytes += line.size() + 1;
      return;
    }

    if (!truncated) {
      truncated = true;
      if (!spill.create()) {
        LOG_ERROR("grep: failed to create spill file under $TMPDIR or /tmp");
        io_error = true;
        return;
      }
      for (auto& h : head) {
        if (spill.bytes + h.size() + 1 > options_.max_spill_bytes) {
          spill_full = true;
          break;
        }
        if (!write_all(spill.fd, h) || !write_all(spill.fd, "\n")) {
          LOG_ERROR("grep: write to spill file '{}' failed", spill.path);
          io_error = true;
          break;
        }
        spill.bytes += h.size() + 1;
      }
      if (io_error || spill_full) return;
    }
    if (io_error || spill_full) return;
    if (spill.bytes + line.size() + 1 > options_.max_spill_bytes) {
      spill_full = true;
      return;
    }
    if (!write_all(spill.fd, line) || !write_all(spill.fd, "\n")) {
      LOG_ERROR("grep: write to spill file '{}' failed", spill.path);
      io_error = true;
    } else {
      spill.bytes += line.size() + 1;
    }
  };

  auto feed = [&](const char* p, std::size_t n) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (p[i] != '\n') continue;
      std::size_t seg = i - start;
      std::size_t remaining =
          max_partial > partial.size() ? max_partial - partial.size() : 0;
      if (seg <= remaining) {
        partial.append(p + start, seg);
      } else {
        partial.append(p + start, remaining);
        partial_truncated = true;
        any_line_truncated = true;
      }
      if (partial_truncated) partial += " [...line truncated]";
      emit_line(partial);
      partial.clear();
      partial_truncated = false;
      start = i + 1;
    }
    if (start < n) {
      std::size_t seg = n - start;
      std::size_t remaining =
          max_partial > partial.size() ? max_partial - partial.size() : 0;
      if (seg <= remaining) {
        partial.append(p + start, seg);
      } else {
        partial.append(p + start, remaining);
        partial_truncated = true;
        any_line_truncated = true;
      }
    }
  };

  char buf[65536];
  std::string errbuf;
  bool out_done = false, err_done = false;
  bool timed_out = false;

  auto deadline = options_.timeout.count() > 0
                      ? std::chrono::steady_clock::now() + options_.timeout
                      : std::chrono::steady_clock::time_point::max();

  while (!out_done || !err_done) {
    int timeout_ms = -1;
    if (deadline != std::chrono::steady_clock::time_point::max()) {
      auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        timed_out = true;
        break;
      }
      auto rem =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count();
      timeout_ms = static_cast<int>(std::min<long long>(rem, INT_MAX));
    }
    struct pollfd pfds[2];
    pfds[0].fd = out_done ? -1 : out_pipe[0];
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = err_done ? -1 : err_pipe[0];
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    int pr = ::poll(pfds, 2, timeout_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) {
      timed_out = true;
      break;
    }

    if (!out_done &&
        (pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t n = ::read(out_pipe[0], buf, sizeof buf);
      if (n > 0)
        feed(buf, static_cast<std::size_t>(n));
      else if (n == 0)
        out_done = true;
      else if (errno != EINTR && errno != EAGAIN)
        out_done = true;
    }
    if (!err_done &&
        (pfds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
      ssize_t n = ::read(err_pipe[0], buf, sizeof buf);
      if (n > 0) {
        if (errbuf.size() < kErrCap) {
          std::size_t take = std::min<std::size_t>(static_cast<std::size_t>(n),
                                                   kErrCap - errbuf.size());
          errbuf.append(buf, take);
        }
      } else if (n == 0)
        err_done = true;
      else if (errno != EINTR && errno != EAGAIN)
        err_done = true;
    }
  }

  // 收尾：最后一行可能没有 '\n'。超时也把已读到的 partial 落进来（带标记）。
  if (!partial.empty()) {
    if (partial_truncated) partial += " [...line truncated]";
    if (timed_out) partial += " [...incomplete, search timed out]";
    emit_line(partial);
    partial.clear();
  }

  if (timed_out) {
    // 先按进程组 SIGKILL，再兜底单进程。
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
  }
  if (out_pipe[0] >= 0) {
    ::close(out_pipe[0]);
    out_pipe[0] = -1;
  }
  if (err_pipe[0] >= 0) {
    ::close(err_pipe[0]);
    err_pipe[0] = -1;
  }

  // ---- waitpid ---------------------------------------------------------
  int status = 0;
  pid_t w;
  do {
    w = ::waitpid(pid, &status, 0);
  } while (w < 0 && errno == EINTR);
  if (w < 0) {
    int e = errno;
    if (!spill.path.empty()) ::unlink(spill.path.c_str());
    guard.release();  // 避免析构重复 kill/wait
    return fail_sys(errno_msg("waitpid", e));
  }
  guard.release();  // 已正常回收

  int exit_code = -1;
  if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);

  // 正常路径：rg 返回 0（有命中）/ 1（无命中）；其余（非超时）视为错误。
  if (!timed_out && exit_code != 0 && exit_code != 1) {
    if (!spill.path.empty()) ::unlink(spill.path.c_str());
    std::string msg = "ripgrep failed";
    if (WIFSIGNALED(status)) {
      msg += " (terminated by signal " + std::to_string(WTERMSIG(status)) + ")";
    } else if (!errbuf.empty()) {
      msg += ": ";
      msg += trim_ws(errbuf);
    } else if (exit_code >= 0) {
      msg += " (exit code " + std::to_string(exit_code) + ")";
    }
    return fail_sys(std::move(msg));
  }

  if (io_error && !spill.path.empty()) {
    ::unlink(spill.path.c_str());
    spill.path.clear();
  }

  GrepResult r;
  r.ok = true;
  r.total_matches = total;
  r.returned_matches = head.size();
  // 超时也意味着结果不完整，统一在 truncated 上体现
  r.truncated = truncated || timed_out;

  if (timed_out) {
    LOG_WARN("grep: search timed out after {}ms; results are partial",
             options_.timeout.count());
    r.warning += "search timed out; results are partial. ";
  }
  if (any_line_truncated) {
    LOG_WARN("grep: some lines exceeded {} bytes and were truncated",
             max_line_bytes_);
    r.warning += "some lines exceeded the inline byte cap and were truncated. ";
  }
  if (io_error) {
    r.warning += "spill file write failed; complete list is incomplete. ";
  }
  if (spill_full) {
    LOG_WARN(
        "grep: spill file reached {} bytes limit; saved list is incomplete",
        options_.max_spill_bytes);
    r.warning += "spill file reached size limit; saved list is incomplete. ";
  }

  if (!errbuf.empty()) {
    std::string e = trim_ws(errbuf);
    if (!e.empty()) {
      if (!r.warning.empty()) r.warning += ' ';
      r.warning += "ripgrep stderr: " + e;
    }
  }

  if (r.truncated && !io_error && !spill.path.empty())
    r.saved_path = spill.path;

  // ---- 组装 inline 结果 ----------------------------------------------
  std::string out;
  std::size_t reserve = 0;
  for (auto& h : head) reserve += h.size() + 1;
  out.reserve(reserve + 2048);
  for (auto& h : head) {
    out += h;
    out += '\n';
  }

  if (r.truncated) {
    std::string note = "\n[truncated] showing first ";
    note += std::to_string(r.returned_matches);
    note += " of ";
    note += std::to_string(r.total_matches);
    note += timed_out ? " matched lines seen before timeout; "
                      : " matching lines; ";
    if (!r.saved_path.empty()) {
      note += "complete list saved to ";
      note += r.saved_path;
      if (spill_full) note += " (size limit reached, list is incomplete)";
    } else if (timed_out) {
      note += "search timed out before completion";
    } else if (io_error) {
      note += "complete list could not be saved";
    } else {
      note += "complete list could not be saved";
    }
    note += '\n';
    out += note;
  }
  r.output = std::move(out);
  return r;
}

#elif defined(_WIN32)

// ---------------- UTF-8 <-> UTF-16 ----------------
std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return L"";
  int n =
      ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string wideToUtf8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
  std::string s(n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
  return s;
}

// ---------------- Windows 命令行参数转义（MSVC CRT 规则） ----------------
// 规则：包含空格/引号才需要加引号；反斜杠只在引号前才需要翻倍。
std::wstring quoteArg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  if (arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;

  std::wstring out;
  out.reserve(arg.size() + 4);
  out.push_back(L'"');
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      out.push_back(c);
      backslashes = 0;
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

// ---------------- 判断一行是否为 “行号:内容” 的匹配行 ----------------
// rg --heading 输出：
//   path/to/file            <-- 文件标题行
//   12:match content        <-- 匹配行
//   15:another match
//   (空行)
bool isMatchLine(const std::string& line) {
  if (line.empty()) return false;
  size_t i = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
  return i > 0 && i < line.size() && line[i] == ':';
}

GrepResult grep(const std::string& pattern, const std::string& path,
                const std::string& include, size_t maxInline) {
  GrepResult r;

  if (pattern.empty()) {
    r.error = "pattern must not be empty";
    return r;
  }

  // ---------- 1. 构造 rg 命令行 ----------
  std::wstring cmd = L"rg";
  cmd += L" --line-number --heading --with-filename --color=never";
  cmd += L" --no-config";  // 忽略用户 .ripgreprc，输出可预期
  cmd += L" --sort path";  // 确定性顺序
  cmd += L" --max-columns 500 --max-columns-preview";

  if (!include.empty()) {
    cmd += L" --glob ";
    cmd += quoteArg(utf8ToWide(include));
  }

  cmd += L" -e ";
  cmd += quoteArg(utf8ToWide(pattern));
  cmd += L" ";
  cmd += path.empty() ? L"." : quoteArg(utf8ToWide(path));

  // ---------- 2. 管道 ----------
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;

  Handle hOutR, hOutW, hErrR, hErrW;
  if (!::CreatePipe(hOutR.addr(), hOutW.addr(), &sa, 0)) {
    r.error = "CreatePipe(stdout) failed: " + std::to_string(::GetLastError());
    return r;
  }
  if (!::CreatePipe(hErrR.addr(), hErrW.addr(), &sa, 0)) {
    r.error = "CreatePipe(stderr) failed: " + std::to_string(::GetLastError());
    return r;
  }
  ::SetHandleInformation(hOutR, HANDLE_FLAG_INHERIT, 0);
  ::SetHandleInformation(hErrR, HANDLE_FLAG_INHERIT, 0);

  Handle hNul(::CreateFileW(L"NUL", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, nullptr));

  // ---------- 3. 预创建临时文件（严格校验） ----------
  wchar_t tempDir[MAX_PATH]{};
  DWORD tdLen = ::GetTempPathW(MAX_PATH, tempDir);
  if (tdLen == 0 || tdLen >= MAX_PATH) {
    r.error = "GetTempPathW failed: " + std::to_string(::GetLastError());
    return r;
  }
  wchar_t tempFile[MAX_PATH]{};
  if (::GetTempFileNameW(tempDir, L"rg", 0, tempFile) == 0) {
    r.error = "GetTempFileNameW failed: " + std::to_string(::GetLastError());
    return r;
  }
  Handle hTemp(::CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr));
  if (!hTemp.valid()) {
    DWORD e = ::GetLastError();
    ::DeleteFileW(tempFile);
    r.error = "CreateFile(temp) failed: " + std::to_string(e);
    return r;
  }

  STARTUPINFOW si{};
  si.cb = sizeof(STARTUPINFOW);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = hOutW;
  si.hStdError = hErrW;
  si.hStdInput = hNul.valid() ? (HANDLE)hNul : ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
  cmdBuf.push_back(L'\0');

  BOOL created =
      ::CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

  // 父进程必须立即关闭写端
  hOutW = Handle{};
  hErrW = Handle{};

  if (!created) {
    DWORD err = ::GetLastError();
    hTemp = Handle{};
    ::DeleteFileW(tempFile);
    r.error = "CreateProcess(rg) failed: " + std::to_string(err) +
              " (确认 rg.exe 在 PATH 中)";
    return r;
  }
  Handle hProc(pi.hProcess);
  Handle hThread(pi.hThread);

  // ---------- 4. 并发读 stderr（防死锁） ----------
  std::string errBuf;
  std::thread errThread([&] {
    char eb[4096];
    DWORD en = 0;
    while (::ReadFile(hErrR, eb, sizeof(eb), &en, nullptr) && en > 0) {
      errBuf.append(eb, en);
    }
  });

  // ---------- 5. 主线程流式读 stdout + 落盘 + 解析 ----------
  std::string inlineOut;
  std::string lineBuf;
  std::string pendingHeader;
  size_t matchCount = 0;
  bool truncated = false;

  auto handleLine = [&](const std::string& line) {
    if (isMatchLine(line)) {
      ++matchCount;
      if (matchCount <= maxInline) {
        if (!pendingHeader.empty()) {
          inlineOut += pendingHeader;
          inlineOut += '\n';
          pendingHeader.clear();
        }
        inlineOut += line;
        inlineOut += '\n';
      } else {
        truncated = true;
      }
    } else if (!line.empty()) {
      if (matchCount < maxInline) {
        pendingHeader = line;
      }
    }
  };

  char buf[16384];
  DWORD n = 0;
  while (::ReadFile(hOutR, buf, sizeof(buf), &n, nullptr) && n > 0) {
    DWORD written = 0;
    ::WriteFile(hTemp, buf, n, &written, nullptr);
    lineBuf.append(buf, n);
    size_t pos;
    while ((pos = lineBuf.find('\n')) != std::string::npos) {
      std::string line = lineBuf.substr(0, pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      handleLine(line);
      lineBuf.erase(0, pos + 1);
    }
  }
  if (!lineBuf.empty()) {
    if (lineBuf.back() == '\r') lineBuf.pop_back();
    handleLine(lineBuf);
  }
  hOutR = Handle{};

  errThread.join();
  hErrR = Handle{};

  // ---------- 6. 收尾 ----------
  ::WaitForSingleObject(hProc, INFINITE);
  DWORD exitCode = 0;
  ::GetExitCodeProcess(hProc, &exitCode);
  r.exitCode = (int)exitCode;

  hTemp = Handle{};  // 关闭写入句柄，调用方才能读

  r.totalMatches = matchCount;
  r.ok = (exitCode == 0 || exitCode == 1);

  if (!r.ok) {
    ::DeleteFileW(tempFile);  // 出错不留垃圾
    r.error = errBuf.empty()
                  ? ("rg exited with code " + std::to_string(exitCode))
                  : errBuf;
    return r;
  }

  if (truncated) {
    r.truncated = true;
    r.fullListPath = wideToUtf8(tempFile);
  } else {
    ::DeleteFileW(tempFile);
  }

  // ---------- 7. 组装最终文本 ----------
  std::string out;
  out.reserve(inlineOut.size() + 512);
  out += inlineOut;

  if (matchCount == 0) {
    out += "未找到匹配。\n";
  } else {
    out += "\n【共 " + std::to_string(matchCount) + " 处匹配";
    if (truncated) out += "，长行已截断至 500 列";
    out += "】\n";

    if (truncated) {
      out += "仅内联显示前 " + std::to_string(maxInline) +
             " 处匹配；完整匹配列表已保存至：\n";
      out += "  " + r.fullListPath + "\n";
      out +=
          "可使用 read 工具读取该文件获取全部结果，"
          "或对上方任一文件使用 read 查看上下文。\n";
    } else {
      out += "可使用 read 工具读取上方任一文件查看完整上下文。\n";
    }
  }
  r.content = std::move(out);
  return r;
}

#endif
/*-ripgrep-------------------------------------------------------*/

};  // namespace kw
