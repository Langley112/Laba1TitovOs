#include <cerrno>
#include <cctype>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct MonitoredProgram {
    pid_t pid = -1;
    std::string log_path;
    std::streamoff next_offset = 0;
    bool start_logged = false;
    bool exit_logged = false;
    bool missing_log_reported = false;
};

constexpr const char* fifo_path = "/tmp/program_a_fifo";

std::string current_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time {};
    localtime_r(&now, &local_time);

    char timestamp[20];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    return timestamp;
}

void write_event(std::ofstream& log, const std::string& timestamp,
                 const std::string& event) {
    log << timestamp << ' ' << event << '\n';
    log.flush();
}

bool read_link(const std::string& path, std::string& target) {
    char buffer[PATH_MAX + 1];
    const ssize_t length = readlink(path.c_str(), buffer, PATH_MAX);
    if (length < 0 || length == PATH_MAX) {
        return false;
    }
    buffer[length] = '\0';
    target = buffer;
    return true;
}

std::string companion_program_a_path() {
    std::string self_path;
    if (!read_link("/proc/self/exe", self_path)) {
        return {};
    }
    const std::size_t separator = self_path.find_last_of('/');
    if (separator == std::string::npos) {
        return {};
    }
    return self_path.substr(0, separator + 1) + "program_a";
}

bool is_program_a_process(pid_t pid, const struct stat& expected_binary) {
    const std::string proc_path = "/proc/" + std::to_string(pid);

    std::ifstream comm_file(proc_path + "/comm");
    std::string comm;
    if (!std::getline(comm_file, comm) || comm != "program_a") {
        return false;
    }

    std::ifstream cmdline_file(proc_path + "/cmdline", std::ios::binary);
    std::string first_argument;
    if (!std::getline(cmdline_file, first_argument, '\0')) {
        return false;
    }
    const std::size_t separator = first_argument.find_last_of('/');
    if (first_argument.substr(separator + 1) != "program_a") {
        return false;
    }

    struct stat actual_binary {};
    if (stat((proc_path + "/exe").c_str(), &actual_binary) != 0) {
        return false;
    }
    return actual_binary.st_dev == expected_binary.st_dev &&
           actual_binary.st_ino == expected_binary.st_ino;
}

std::vector<pid_t> find_program_a_processes(const std::string& program_path) {
    std::vector<pid_t> matches;
    struct stat expected_binary {};
    if (stat(program_path.c_str(), &expected_binary) != 0) {
        return matches;
    }

    DIR* proc_directory = opendir("/proc");
    if (proc_directory == nullptr) {
        std::perror("Не удалось открыть /proc");
        return matches;
    }

    dirent* entry;
    while ((entry = readdir(proc_directory)) != nullptr) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) {
            continue;
        }
        const pid_t pid = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
        if (is_program_a_process(pid, expected_binary)) {
            matches.push_back(pid);
        }
    }
    closedir(proc_directory);
    return matches;
}

bool split_a_log_line(const std::string& line, std::string& timestamp,
                      std::string& event) {
    if (line.size() < 21 || line[19] != ' ') {
        return false;
    }
    timestamp = line.substr(0, 19);
    event = line.substr(20);
    return true;
}

void read_existing_start(MonitoredProgram& program, std::ofstream& own_log) {
    std::ifstream input(program.log_path, std::ios::binary);
    if (!input) {
        return;
    }

    std::string last_start;
    std::string line;
    while (std::getline(input, line)) {
        if (input.eof()) {
            break; // Незавершённую строку прочитаем после появления символа новой строки.
        }
        program.next_offset = static_cast<std::streamoff>(input.tellg());

        std::string timestamp;
        std::string event;
        if (!split_a_log_line(line, timestamp, event)) {
            continue;
        }
        if (event.compare(0, 6, "start ") == 0) {
            last_start = timestamp;
        } else if (event == "exit") {
            last_start.clear();
        }
    }

    if (!last_start.empty()) {
        write_event(own_log, last_start,
                    "program_a started pid=" + std::to_string(program.pid));
        program.start_logged = true;
    }
}

bool attach_to_program_a(pid_t pid, MonitoredProgram& program,
                         std::ofstream& own_log) {
    std::string working_directory;
    if (!read_link("/proc/" + std::to_string(pid) + "/cwd", working_directory)) {
        return false;
    }

    program = MonitoredProgram {};
    program.pid = pid;
    program.log_path = working_directory + "/program_a.log";
    read_existing_start(program, own_log);
    std::cout << "Найдена программа A, PID " << pid << ".\n";
    return true;
}

bool send_key_t(const std::string& program_path, pid_t expected_pid,
                std::string& error) {
    const std::vector<pid_t> matches = find_program_a_processes(program_path);
    if (matches.size() != 1 || matches[0] != expected_pid) {
        error = "программа A завершилась или найдено несколько экземпляров";
        return false;
    }

    struct stat fifo_info {};
    if (lstat(fifo_path, &fifo_info) != 0 || !S_ISFIFO(fifo_info.st_mode)) {
        error = "FIFO отсутствует или недоступен";
        return false;
    }

    const int fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        error = std::string("не удалось открыть FIFO: ") + std::strerror(errno);
        return false;
    }

    constexpr char command[] = "key=t\n";
    ssize_t written;
    do {
        written = write(fifo_fd, command, sizeof(command) - 1);
    } while (written < 0 && errno == EINTR);
    const int write_error = errno;
    close(fifo_fd);

    if (written != static_cast<ssize_t>(sizeof(command) - 1)) {
        error = written < 0 ? std::strerror(write_error) : "неполная запись в FIFO";
        return false;
    }
    return true;
}

void handle_a_log_line(const std::string& line, MonitoredProgram& program,
                       const std::string& program_path, std::ofstream& own_log) {
    std::string timestamp;
    std::string event;
    if (!split_a_log_line(line, timestamp, event)) {
        return;
    }

    if (event.compare(0, 6, "start ") == 0) {
        if (!program.start_logged) {
            write_event(own_log, timestamp,
                        "program_a started pid=" + std::to_string(program.pid));
            program.start_logged = true;
        }
        return;
    }

    if (event == "exit") {
        if (!program.exit_logged) {
            write_event(own_log, timestamp,
                        "program_a exited pid=" + std::to_string(program.pid));
            program.exit_logged = true;
        }
        return;
    }

    if (program.exit_logged) {
        return;
    }

    if (event.size() >= 5 && event.compare(0, 4, "key=") == 0 &&
        (event.size() == 5 || event[5] == ' ')) {
        write_event(own_log, timestamp, event + " pid=" + std::to_string(program.pid));
        if (event[4] == '+') {
            std::string error;
            if (send_key_t(program_path, program.pid, error)) {
                write_event(own_log, current_timestamp(),
                            "injected key=t pid=" + std::to_string(program.pid));
            } else {
                write_event(own_log, current_timestamp(),
                            "failed to inject key=t pid=" + std::to_string(program.pid) +
                            " reason=" + error);
                std::cerr << "Не удалось отправить key=t: " << error << '\n';
            }
        }
    } else if (event.compare(0, 10, "cmd=color:") == 0) {
        write_event(own_log, timestamp, event + " pid=" + std::to_string(program.pid));
    }
}

void read_new_a_log_lines(MonitoredProgram& program,
                          const std::string& program_path, std::ofstream& own_log) {
    struct stat log_info {};
    if (stat(program.log_path.c_str(), &log_info) != 0) {
        if (!program.missing_log_reported) {
            std::cout << "Ожидание журнала " << program.log_path << ".\n";
            program.missing_log_reported = true;
        }
        return;
    }
    program.missing_log_reported = false;
    if (log_info.st_size < program.next_offset) {
        program.next_offset = 0;
    }

    std::ifstream input(program.log_path, std::ios::binary);
    if (!input) {
        return;
    }
    input.seekg(program.next_offset);

    std::string line;
    while (std::getline(input, line)) {
        if (input.eof()) {
            break; // Не обрабатываем строку, пока A не допишет перевод строки.
        }
        program.next_offset = static_cast<std::streamoff>(input.tellg());
        handle_a_log_line(line, program, program_path, own_log);
    }
}

void finish_monitoring(MonitoredProgram& program,
                       const std::string& program_path, std::ofstream& own_log) {
    read_new_a_log_lines(program, program_path, own_log);
    if (!program.exit_logged) {
        write_event(own_log, current_timestamp(),
                    "program_a disappearance detected pid=" + std::to_string(program.pid) +
                    " (no exit record)");
    }
    std::cout << "Программа A завершилась. Ожидание следующего запуска.\n";
    program = MonitoredProgram {};
}

bool user_requested_exit() {
    pollfd input {STDIN_FILENO, POLLIN, 0};
    const int result = poll(&input, 1, 0);
    if (result <= 0) {
        return false;
    }
    if (input.revents & (POLLHUP | POLLERR | POLLNVAL)) {
        return true;
    }
    if (input.revents & POLLIN) {
        std::string command;
        if (!std::getline(std::cin, command)) {
            return true;
        }
        if (command == "q") {
            return true;
        }
        std::cout << "Для выхода введите q и нажмите Enter.\n";
    }
    return false;
}

int main() {
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::perror("Не удалось настроить SIGPIPE");
        return 1;
    }

    const std::string program_path = companion_program_a_path();
    if (program_path.empty()) {
        std::cerr << "Не удалось определить путь к программе A.\n";
        return 1;
    }

    std::ofstream own_log("program_c.log", std::ios::app);
    if (!own_log) {
        std::cerr << "Не удалось открыть program_c.log.\n";
        return 1;
    }

    MonitoredProgram program;
    bool ambiguity_reported = false;
    bool waiting_reported = false;
    std::cout << "Программа C запущена. Для выхода введите q и нажмите Enter.\n";

    while (true) {
        if (user_requested_exit()) {
            break;
        }

        const std::vector<pid_t> matches = find_program_a_processes(program_path);
        if (matches.size() > 1) {
            if (!ambiguity_reported) {
                std::cerr << "Найдено несколько программ A. Мониторинг приостановлен.\n";
                write_event(own_log, current_timestamp(),
                            "multiple program_a processes; monitoring paused");
                ambiguity_reported = true;
            }
            program = MonitoredProgram {};
            waiting_reported = false;
        } else if (matches.empty()) {
            ambiguity_reported = false;
            if (program.pid > 0) {
                finish_monitoring(program, program_path, own_log);
            } else if (!waiting_reported) {
                std::cout << "Ожидание программы A.\n";
                waiting_reported = true;
            }
        } else {
            ambiguity_reported = false;
            waiting_reported = false;
            if (program.pid > 0 && program.pid != matches[0]) {
                finish_monitoring(program, program_path, own_log);
            }
            if (program.pid < 0) {
                attach_to_program_a(matches[0], program, own_log);
            }
            if (program.pid == matches[0]) {
                read_new_a_log_lines(program, program_path, own_log);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "Программа C завершена.\n";
    return 0;
}
