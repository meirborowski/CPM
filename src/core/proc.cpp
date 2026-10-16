#include "core/proc.hpp"

#include <algorithm>
#include <format>
#include <thread>

#include "core/env.hpp"
#include "core/error.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <map>
#else
#include <cerrno>
#include <cstring>
#include <iterator>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace cpm::proc {

// ---------------------------------------------------------------------------------------------
// Quoting and lookup (portable)

std::string quote_windows(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) return std::string(arg);
    std::string out = "\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += arg[i];
        }
    }
    out += '"';
    return out;
}

std::string quote_posix(std::string_view arg) {
    auto safe = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               std::string_view("_@%+=:,./-").find(c) != std::string_view::npos;
    };
    if (!arg.empty() && std::all_of(arg.begin(), arg.end(), safe)) return std::string(arg);
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

std::string quote(std::string_view arg) {
#ifdef _WIN32
    return quote_windows(arg);
#else
    return quote_posix(arg);
#endif
}

std::string quote_cmd(std::string_view arg) {
    std::string quoted = quote_windows(arg);
    std::string out;
    for (char c : quoted) {
        if (std::string_view("()!^\"<>&|").find(c) != std::string_view::npos) out += '^';
        out += c;
    }
    return out;
}

std::string command_line(const std::vector<std::string>& argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) out += ' ';
        out += quote(argv[i]);
    }
    return out;
}

std::optional<fs::path> find_program(std::string_view name) {
    if (name.empty()) return std::nullopt;
    std::error_code ec;
#ifdef _WIN32
    constexpr char list_sep = ';';
    bool has_sep = name.find_first_of("/\\") != std::string_view::npos;
#else
    constexpr char list_sep = ':';
    bool has_sep = name.find('/') != std::string_view::npos;
#endif
    fs::path given = paths::from_utf8(name);

#ifdef _WIN32
    // Only what CreateProcess can start directly. Scripts (.bat, .cmd, .py, ...) need an interpreter, and
    // probing every PATHEXT entry in every PATH directory makes lookups slow.
    std::vector<std::string> exts;
    if (given.has_extension()) exts = {""};
    else exts = {".exe", ".com"};
    auto try_path = [&](const fs::path& base) -> std::optional<fs::path> {
        for (const auto& ext : exts) {
            fs::path candidate = base;
            if (!ext.empty()) candidate += paths::from_utf8(ext);
            if (fs::is_regular_file(candidate, ec)) return fs::absolute(candidate).lexically_normal();
        }
        return std::nullopt;
    };
#else
    auto try_path = [&](const fs::path& candidate) -> std::optional<fs::path> {
        if (fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0)
            return fs::absolute(candidate).lexically_normal();
        return std::nullopt;
    };
#endif

    if (has_sep || given.is_absolute()) return try_path(given);
    for (const auto& dir : str::split(env::get("PATH").value_or(""), list_sep)) {
        if (dir.empty()) continue;
        std::string d = dir;
        if (d.size() >= 2 && d.front() == '"' && d.back() == '"') d = d.substr(1, d.size() - 2);
        if (auto found = try_path(paths::from_utf8(d) / given)) return found;
    }
    return std::nullopt;
}

namespace {

fs::path resolve_program(const Command& cmd) {
    if (cmd.argv.empty()) throw Error("internal error: empty command");
    auto found = find_program(cmd.argv[0]);
    if (!found) throw Error(std::format("program not found: {}", cmd.argv[0]), "check that it is installed and on PATH");
    return *found;
}

}  // namespace

#ifdef _WIN32
// ---------------------------------------------------------------------------------------------
// Windows

namespace {

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Handle& operator=(Handle&& o) noexcept {
        reset();
        h_ = std::exchange(o.h_, nullptr);
        return *this;
    }
    ~Handle() { reset(); }
    void reset() {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = nullptr;
    }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

std::string win_error(DWORD code = GetLastError()) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text = buf ? str::narrow(buf) : std::string();
    if (buf) LocalFree(buf);
    return std::format("{} (code {})", std::string(str::trim(text)), code);
}

Handle open_nul() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    return Handle(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, nullptr));
}

Handle dup_inheritable(HANDLE h) {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return open_nul();
    HANDLE out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return open_nul();
    return Handle(out);
}

std::wstring build_command_line(const fs::path& program, const std::vector<std::string>& argv, bool verbatim) {
    // argv[0] must use backslashes: cmd.exe scans its whole command line for switches, so a path like
    // "C:/windows/system32/cmd.exe" would be read as "/c md.exe ...".
    fs::path native = program.lexically_normal();
    native.make_preferred();
    std::string line = quote_windows(str::narrow(native.native()));
    for (std::size_t i = 1; i < argv.size(); ++i) {
        line += ' ';
        line += verbatim ? argv[i] : quote_windows(argv[i]);
    }
    return str::widen(line);
}

// A UTF-16 environment block with the command's changes applied, or empty to inherit unchanged.
std::wstring build_environment(const Command& cmd) {
    if (cmd.env_set.empty() && cmd.env_unset.empty()) return {};
    auto upper = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
        return s;
    };
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;  // UPPER(name) -> (name, value)
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; p && *p; p += wcslen(p) + 1) {
        std::wstring entry(p);
        std::size_t eq = entry.find(L'=', 1);  // names like "=C:" start with '='
        if (eq == std::wstring::npos) continue;
        std::wstring name = entry.substr(0, eq);
        vars[upper(name)] = {name, entry.substr(eq + 1)};
    }
    if (block) FreeEnvironmentStringsW(block);
    for (const auto& name : cmd.env_unset) vars.erase(upper(str::widen(name)));
    for (const auto& [name, value] : cmd.env_set) {
        std::wstring wname = str::widen(name);
        vars[upper(wname)] = {wname, str::widen(value)};
    }
    std::wstring out;
    for (const auto& [key, nv] : vars) {
        out += nv.first;
        out += L'=';
        out += nv.second;
        out += L'\0';
    }
    out += L'\0';
    return out;
}

struct Spawned {
    Handle process;
    Handle job;
};

// Starts the command with the given (inheritable) standard handles. Only these handles are inherited.
Spawned spawn(const Command& cmd, HANDLE in, HANDLE out, HANDLE err, bool use_job) {
    fs::path program = resolve_program(cmd);
    std::wstring cmdline = build_command_line(program, cmd.argv, cmd.verbatim_args);
    std::wstring env_block = build_environment(cmd);
    std::wstring cwd = cmd.cwd.empty() ? std::wstring() : cmd.cwd.wstring();

    HANDLE inherit[3];
    DWORD count = 0;
    for (HANDLE h : {in, out, err}) {
        if (std::find(inherit, inherit + count, h) == inherit + count) inherit[count++] = h;
    }

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size))
        throw Error(std::format("cannot start {}: {}", cmd.argv[0], win_error()));
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, count * sizeof(HANDLE), nullptr,
                              nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = attrs;

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (use_job) flags |= CREATE_SUSPENDED;

    PROCESS_INFORMATION pi{};
    std::wstring app = program.wstring();
    BOOL ok = CreateProcessW(app.c_str(), cmdline.data(), nullptr, nullptr, TRUE, flags,
                             env_block.empty() ? nullptr : env_block.data(), cwd.empty() ? nullptr : cwd.c_str(),
                             &si.StartupInfo, &pi);
    DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!ok) throw Error(std::format("cannot start {}: {}", paths::to_utf8(program), win_error(create_error)));

    Spawned s;
    s.process = Handle(pi.hProcess);
    Handle thread(pi.hThread);
    if (use_job) {
        s.job = Handle(CreateJobObjectW(nullptr, nullptr));
        if (s.job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(s.job.get(), JobObjectExtendedLimitInformation, &info, sizeof(info));
            if (!AssignProcessToJobObject(s.job.get(), s.process.get())) s.job.reset();
        }
        ResumeThread(thread.get());
    }
    return s;
}

// Waits for the process; on timeout kills it (and its job). Returns the exit code, or -1 on timeout.
int wait_for(Spawned& s, std::optional<std::chrono::milliseconds> timeout, bool& timed_out) {
    DWORD ms = timeout ? static_cast<DWORD>(timeout->count()) : INFINITE;
    DWORD r = WaitForSingleObject(s.process.get(), ms);
    if (r == WAIT_TIMEOUT) {
        timed_out = true;
        if (s.job) TerminateJobObject(s.job.get(), 1);
        else TerminateProcess(s.process.get(), 1);
        WaitForSingleObject(s.process.get(), INFINITE);
        return -1;
    }
    DWORD code = 0;
    GetExitCodeProcess(s.process.get(), &code);
    return static_cast<int>(code);
}

BOOL WINAPI ignore_ctrl_c(DWORD type) { return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT; }

// Ignores Ctrl-C in this process for its lifetime (the child, sharing the console, still receives it).
class InterruptGuard {
public:
    explicit InterruptGuard(bool active) : active_(active) {
        if (active_) SetConsoleCtrlHandler(ignore_ctrl_c, TRUE);
    }
    ~InterruptGuard() {
        if (active_) SetConsoleCtrlHandler(ignore_ctrl_c, FALSE);
    }
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

private:
    bool active_;
};

constexpr DWORD kStatusControlCExit = 0xC000013A;

void read_all(HANDLE h, std::string* out) {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) out->append(buf, n);
}

}  // namespace

Captured capture(const Command& cmd, std::optional<std::chrono::milliseconds> timeout, bool null_stdin) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0))
        throw Error(std::format("cannot create a pipe: {}", win_error()));
    Handle hout_r(out_r), hout_w(out_w), herr_r(err_r), herr_w(err_w);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    Handle in = null_stdin ? open_nul() : dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));

    log::trace(command_line(cmd.argv));
    Spawned s = spawn(cmd, in.get(), out_w, err_w, timeout.has_value());
    hout_w.reset();
    herr_w.reset();
    in.reset();

    Captured result;
    std::thread t_out(read_all, out_r, &result.out);
    std::thread t_err(read_all, err_r, &result.err);
    result.exit_code = wait_for(s, timeout, result.timed_out);
    t_out.join();
    t_err.join();
    return result;
}

int run(const Command& cmd, const RunOptions& options) {
    Handle in = options.null_stdin ? open_nul() : dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));
    Handle err = dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
    Handle out = dup_inheritable(
        GetStdHandle(options.stdout_mode == Stdout::ToStderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE));
    log::trace(command_line(cmd.argv));
    InterruptGuard guard(options.forward_interrupts);
    Spawned s = spawn(cmd, in.get(), out.get(), err.get(), options.timeout.has_value());
    in.reset();
    out.reset();
    err.reset();
    bool timed_out = false;
    int code = wait_for(s, options.timeout, timed_out);
    if (static_cast<DWORD>(code) == kStatusControlCExit) return 130;
    return code;
}

#else
// ---------------------------------------------------------------------------------------------
// POSIX

namespace {

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    ~Fd() { reset(); }
    void reset() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }
    int get() const { return fd_; }

private:
    int fd_ = -1;
};

void set_cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

std::pair<Fd, Fd> make_pipe() {
    int fds[2];
    if (pipe(fds) != 0) throw Error(std::format("cannot create a pipe: {}", std::strerror(errno)));
    set_cloexec(fds[0]);
    set_cloexec(fds[1]);
    return {Fd(fds[0]), Fd(fds[1])};
}

Fd open_null() {
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) set_cloexec(fd);
    return Fd(fd);
}

std::vector<std::string> build_environment(const Command& cmd) {
    std::vector<std::string> out;
    auto removed = [&](std::string_view entry) {
        std::string_view name = entry.substr(0, entry.find('='));
        for (const auto& n : cmd.env_unset) {
            if (n == name) return true;
        }
        for (const auto& [n, v] : cmd.env_set) {
            if (n == name) return true;
        }
        return false;
    };
    for (char** e = environ; e && *e; ++e) {
        if (!removed(*e)) out.emplace_back(*e);
    }
    for (const auto& [n, v] : cmd.env_set) out.push_back(n + "=" + v);
    return out;
}

// Forks and execs; in/out/err are file descriptors to install as 0/1/2, or -1 to inherit. With
// `own_group` the child leads a new process group, so a timeout can kill everything it started (the
// POSIX counterpart of the Windows job object).
pid_t spawn(const Command& cmd, int in, int out, int err, bool own_group) {
    fs::path program = resolve_program(cmd);
    std::vector<std::string> env_strings = build_environment(cmd);
    std::vector<char*> argv, envp;
    std::string program_str = program.string();
    argv.push_back(program_str.data());
    std::vector<std::string> args(cmd.argv.begin() + 1, cmd.argv.end());
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (auto& e : env_strings) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::string cwd = cmd.cwd.string();

    auto [err_r, err_w] = make_pipe();
    pid_t pid = fork();
    if (pid < 0) throw Error(std::format("cannot start {}: {}", cmd.argv[0], std::strerror(errno)));
    if (pid == 0) {
        if (own_group) setpgid(0, 0);
        // Ignored signal dispositions survive exec; the child must start with the defaults.
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) dup2(out, 1);
        if (err >= 0) dup2(err, 2);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            int e = errno;
            (void)!write(err_w.get(), &e, sizeof e);
            _exit(127);
        }
        execve(program_str.c_str(), argv.data(), envp.data());
        int e = errno;
        (void)!write(err_w.get(), &e, sizeof e);
        _exit(127);
    }
    // Both sides set the group, so it exists before either one relies on it (EACCES after the child's exec
    // means the child already did it).
    if (own_group) setpgid(pid, pid);
    err_w.reset();
    int child_errno = 0;
    ssize_t n;
    do {
        n = read(err_r.get(), &child_errno, sizeof child_errno);
    } while (n < 0 && errno == EINTR);
    if (n == static_cast<ssize_t>(sizeof child_errno)) {
        int status;
        waitpid(pid, &status, 0);
        throw Error(std::format("cannot start {}: {}", program_str, std::strerror(child_errno)));
    }
    return pid;
}

int decode_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// A child in its own process group no longer receives the terminal's Ctrl-C, so while one runs, SIGINT,
// SIGTERM and SIGHUP sent to CPM are passed on to its group. Construct the guard before spawning (so no
// signal is lost in between), attach the child, and re-raise forwarded() once the child is gone.
volatile sig_atomic_t g_group = 0;
volatile sig_atomic_t g_forwarded = 0;

extern "C" void forward_to_group(int sig) {
    g_forwarded = sig;
    if (g_group > 0) kill(-static_cast<pid_t>(g_group), sig);
}

class GroupSignals {
public:
    explicit GroupSignals(bool active) : active_(active) {
        if (!active_) return;
        g_group = 0;
        g_forwarded = 0;
        struct sigaction forward {};
        forward.sa_handler = forward_to_group;
        sigemptyset(&forward.sa_mask);
        for (std::size_t i = 0; i < std::size(kSignals); ++i) sigaction(kSignals[i], &forward, &old_[i]);
    }
    ~GroupSignals() { restore(); }
    GroupSignals(const GroupSignals&) = delete;
    GroupSignals& operator=(const GroupSignals&) = delete;

    void attach(pid_t group) {
        if (!active_) return;
        g_group = group;
        if (int sig = g_forwarded) kill(-group, sig);  // arrived while the child was being started
    }
    // Restores CPM's own handlers and returns the signal that was forwarded, if any.
    int restore() {
        if (!active_) return 0;
        active_ = false;
        for (std::size_t i = 0; i < std::size(kSignals); ++i) sigaction(kSignals[i], &old_[i], nullptr);
        g_group = 0;
        return g_forwarded;
    }

private:
    static constexpr int kSignals[] = {SIGINT, SIGTERM, SIGHUP};
    bool active_;
    struct sigaction old_[std::size(kSignals)] {};
};

// `group`: the child leads its own process group, and a timeout kills the whole group.
int wait_child(pid_t pid, std::optional<std::chrono::milliseconds> timeout, bool& timed_out, bool group = false) {
    int status = 0;
    if (!timeout) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return decode_status(status);
    }
    auto deadline = std::chrono::steady_clock::now() + *timeout;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return decode_status(status);
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            kill(group ? -pid : pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

Captured capture(const Command& cmd, std::optional<std::chrono::milliseconds> timeout, bool null_stdin) {
    auto [out_r, out_w] = make_pipe();
    auto [err_r, err_w] = make_pipe();
    Fd in = null_stdin ? open_null() : Fd();
    log::trace(command_line(cmd.argv));
    const bool own_group = timeout.has_value();  // with a timeout, the child gets its own process group
    GroupSignals signals(own_group);
    pid_t pid = spawn(cmd, in.get(), out_w.get(), err_w.get(), own_group);
    signals.attach(pid);
    out_w.reset();
    err_w.reset();

    Captured result;
    // A plain time_point rather than std::optional: GCC 13 warns (wrongly) that the optional's value may be
    // used uninitialized.
    const auto deadline = std::chrono::steady_clock::now() + timeout.value_or(std::chrono::milliseconds(0));
    pollfd fds[2] = {{out_r.get(), POLLIN, 0}, {err_r.get(), POLLIN, 0}};
    std::string* sinks[2] = {&result.out, &result.err};
    int open_count = 2;
    bool killed = false;
    while (open_count > 0) {
        int wait_ms = -1;
        if (own_group && !killed) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            wait_ms = static_cast<int>(std::max<long long>(0, left.count()));
        }
        int r = poll(fds, 2, wait_ms);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0 && own_group && !killed) {
            kill(-pid, SIGKILL);  // the whole group: grandchildren could keep the pipes open
            killed = true;
            result.timed_out = true;
            continue;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[4096];
            ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                fds[i].fd = -1;
                --open_count;
            }
        }
    }
    bool ignored = false;
    result.exit_code = wait_child(pid, std::nullopt, ignored);
    if (result.timed_out) result.exit_code = -1;
    if (int sig = signals.restore()) raise(sig);  // e.g. Ctrl-C during `cpm test` still stops CPM
    return result;
}

int run(const Command& cmd, const RunOptions& options) {
    Fd in = options.null_stdin ? open_null() : Fd();
    int out = options.stdout_mode == Stdout::ToStderr ? 2 : -1;
    log::trace(command_line(cmd.argv));
    if (options.timeout) {
        GroupSignals signals(true);
        pid_t pid = spawn(cmd, in.get(), out, -1, true);
        signals.attach(pid);
        bool timed_out = false;
        int code = wait_child(pid, options.timeout, timed_out, true);
        int sig = signals.restore();
        // With forward_interrupts the child decides what Ctrl-C means and CPM carries on.
        if (sig && !(options.forward_interrupts && (sig == SIGINT))) raise(sig);
        return code;
    }
    pid_t pid = spawn(cmd, in.get(), out, -1, false);
    struct sigaction ignore {}, old_int{}, old_quit{};
    if (options.forward_interrupts) {
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        sigaction(SIGINT, &ignore, &old_int);
        sigaction(SIGQUIT, &ignore, &old_quit);
    }
    bool timed_out = false;
    int code = wait_child(pid, options.timeout, timed_out);
    if (options.forward_interrupts) {
        sigaction(SIGINT, &old_int, nullptr);
        sigaction(SIGQUIT, &old_quit, nullptr);
    }
    return code;
}

#endif

}  // namespace cpm::proc
