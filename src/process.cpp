#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "buildray/core.hpp"

namespace buildray {
namespace {
volatile std::sig_atomic_t stopped = 0;
volatile std::sig_atomic_t active_group = 0;

void on_signal(int signal) {
    stopped = signal;
    if (active_group > 0) {
        ::kill(-active_group, signal);
    }
}
}  // namespace

void install_signal_handlers() {
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

bool interrupted() { return stopped != 0; }

ProcessResult run(const std::vector<std::string>& args, const fs::path& cwd, const fs::path& log) {
    if (args.empty() || args.front().empty()) throw std::runtime_error("Empty command");
    if (interrupted()) throw std::runtime_error("Interrupted");
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    const auto started = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork: " + std::string(std::strerror(errno)));
    if (pid == 0) {
        ::setpgid(0, 0);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) _exit(126);
        if (!log.empty()) {
            const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) _exit(126);
            if (::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) _exit(126);
            ::close(fd);
        }
        ::setenv("CCACHE_DISABLE", "1", 1);
        ::setenv("SCCACHE_DISABLE", "1", 1);
        ::setenv("LC_ALL", "C", 1);
        ::execvp(argv.front(), argv.data());
        const int failure = errno;
        const char message[] = "buildray: unable to execute child command\n";
        ::write(STDERR_FILENO, message, sizeof(message) - 1);
        _exit(failure == ENOENT ? 127 : 126);
    }
    ::setpgid(pid, pid);
    active_group = pid;
    if (stopped) ::kill(-pid, stopped);
    int status = 0;
    struct rusage usage{};
    while (::wait4(pid, &status, 0, &usage) < 0) {
        if (errno != EINTR) {
            active_group = 0;
            throw std::runtime_error("wait4 failed");
        }
    }
    active_group = 0;
    const auto seconds = [](const timeval& value) {
        return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1e6;
    };
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
            seconds(usage.ru_utime), seconds(usage.ru_stime), usage.ru_maxrss};
}

int launch(const std::vector<std::string>& args, bool trace, bool includes) {
    if (args.size() < 3 || args[1] != "--") {
        throw std::runtime_error("Internal launcher expects RECORD_DIR -- COMPILER [ARGS...]");
    }
    const fs::path records = args[0];
    fs::create_directories(records);
    std::vector<std::string> compiler(args.begin() + 2, args.end());
    const auto trace_file = records / (std::to_string(::getpid()) + ".trace");
    const auto include_file = records / (std::to_string(::getpid()) + ".includes");
    if (trace) {
        compiler.push_back("-ftime-trace=" + trace_file.string());
        compiler.push_back("-ftime-trace-granularity=0");
    }
    if (includes) {
        compiler.push_back("-H");
        compiler.push_back("-fshow-skipped-includes");
    }
    const auto result = run(compiler, fs::current_path(), includes ? include_file : fs::path{});
    if (includes) {
        // Keep diagnostics visible to Ninja without flooding its build log with traces.
        std::istringstream lines(read_file(include_file));
        std::string line;
        while (std::getline(lines, line))
            if (!include_line(line).first) std::cerr << line << '\n';
    }
    json::Array command;
    for (const auto& arg : compiler) command.push_back(arg);
    std::string source;
    for (std::size_t i = 0; i + 1 < compiler.size(); ++i) {
        if (compiler[i] == "-c") source = compiler[i + 1];
    }
    // One launcher process per invocation; PID is unique while Ninja is running.
    write_json(records / (std::to_string(::getpid()) + ".json"),
               json::Object{{"source", source},
                            {"directory", fs::current_path().string()},
                            {"command", std::move(command)},
                            {"trace_file", trace ? trace_file.string() : ""},
                            {"include_file", includes ? include_file.string() : ""},
                            {"wall_seconds", result.seconds},
                            {"user_seconds", result.user_seconds},
                            {"system_seconds", result.system_seconds},
                            {"peak_rss_kib", result.peak_rss_kib},
                            {"exit_code", result.code}});
    return result.code;
}
}  // namespace buildray
