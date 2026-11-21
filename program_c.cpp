#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

struct AInfo {
    pid_t process_id = -1;
    std::string log_path;
    std::streamoff log_pos = 0;
    bool start_saved = false;
    bool exit_saved = false;
    bool log_error_shown = false;
};

std::string time_now() {
    const std::time_t now = std::time(nullptr);
    std::tm local_time {};
    localtime_r(&now, &local_time);

    char time[20];
    std::strftime(time, sizeof(time), "%Y-%m-%d %H:%M:%S", &local_time);
    return time;
}

void write_event(std::ofstream& log_file, const std::string& time,
                 const std::string& event) {
    log_file << time << ' ' << event << '\n';
    log_file.flush();
}

bool read_link(const std::string& link_path, std::string& link_target) {
    char buffer[PATH_MAX + 1];
    const ssize_t length = readlink(link_path.c_str(), buffer, PATH_MAX);
    if (length < 0 || length == PATH_MAX) {
        return false;
    }
    buffer[length] = '\0';
    link_target = buffer;
    return true;
}

std::string get_a_path() {
    std::string own_path;
    if (!read_link("/proc/self/exe", own_path)) {
        return {};
    }
    const std::size_t slash = own_path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    return own_path.substr(0, slash + 1) + "program_a";
}

bool is_a_process(pid_t process_id, const struct stat& a_file_info) {
    const std::string proc_path = "/proc/" + std::to_string(process_id);

    std::ifstream comm_file(proc_path + "/comm");
    std::string comm;
    if (!std::getline(comm_file, comm) || comm != "program_a") {
        return false;
    }

    std::ifstream cmdline_file(proc_path + "/cmdline", std::ios::binary);
    std::string first_arg;
    if (!std::getline(cmdline_file, first_arg, '\0')) {
        return false;
    }
    const std::size_t slash = first_arg.find_last_of('/');
    if (first_arg.substr(slash + 1) != "program_a") {
        return false;
    }

    struct stat process_file {};
    if (stat((proc_path + "/exe").c_str(), &process_file) != 0) {
        return false;
    }
    return process_file.st_dev == a_file_info.st_dev &&
           process_file.st_ino == a_file_info.st_ino;
}

std::vector<pid_t> find_a_processes(const std::string& a_path) {
    std::vector<pid_t> a_pids;
    struct stat a_file_info {};
    if (stat(a_path.c_str(), &a_file_info) != 0) {
        return a_pids;
    }

    DIR* proc_dir = opendir("/proc");
    if (proc_dir == nullptr) {
        std::perror("Не удалось открыть /proc");
        return a_pids;
    }

    dirent* entry;
    while ((entry = readdir(proc_dir)) != nullptr) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) {
            continue;
        }
        const pid_t process_id = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
        if (is_a_process(process_id, a_file_info)) {
            a_pids.push_back(process_id);
        }
    }
    closedir(proc_dir);
    return a_pids;
}

bool split_log_line(const std::string& line, std::string& time,
                    std::string& event) {
    if (line.size() < 21 || line[19] != ' ') {
        return false;
    }
    time = line.substr(0, 19);
    event = line.substr(20);
    return true;
}

void read_old_start(AInfo& a_info, std::ofstream& c_log_file) {
    std::ifstream input(a_info.log_path, std::ios::binary);
    if (!input) {
        return;
    }

    std::string last_start;
    std::string line;
    while (std::getline(input, line)) {
        if (input.eof()) {
            break; // Незавершённую строку прочитаем после появления символа новой строки.
        }
        a_info.log_pos = static_cast<std::streamoff>(input.tellg());

        std::string time;
        std::string event;
        if (!split_log_line(line, time, event)) {
            continue;
        }
        if (event.compare(0, 6, "start ") == 0) {
            last_start = time;
        } else if (event == "exit") {
            last_start.clear();
        }
    }

    if (!last_start.empty()) {
        write_event(c_log_file, last_start,
                    "program_a started pid=" + std::to_string(a_info.process_id));
        a_info.start_saved = true;
    }
}

bool watch_a(pid_t process_id, AInfo& a_info, std::ofstream& c_log_file) {
    std::string work_dir;
    if (!read_link("/proc/" + std::to_string(process_id) + "/cwd", work_dir)) {
        return false;
    }

    a_info = AInfo {};
    a_info.process_id = process_id;
    a_info.log_path = work_dir + "/program_a.log";
    read_old_start(a_info, c_log_file);
    std::cout << "Найдена программа A, PID " << process_id << ".\n";
    return true;
}

void save_a_event(const std::string& line, AInfo& a_info,
                  std::ofstream& c_log_file) {
    std::string time;
    std::string event;
    if (!split_log_line(line, time, event)) {
        return;
    }

    if (event.compare(0, 6, "start ") == 0) {
        if (!a_info.start_saved) {
            write_event(c_log_file, time,
                        "program_a started pid=" + std::to_string(a_info.process_id));
            a_info.start_saved = true;
        }
        return;
    }

    if (event == "exit") {
        if (!a_info.exit_saved) {
            write_event(c_log_file, time,
                        "program_a exited pid=" + std::to_string(a_info.process_id));
            a_info.exit_saved = true;
        }
        return;
    }

    if (a_info.exit_saved) {
        return;
    }

    const bool key_event = event.size() >= 5 && event.compare(0, 4, "key=") == 0 &&
                           (event.size() == 5 || event[5] == ' ');
    const bool color_event = event.compare(0, 10, "cmd=color:") == 0;
    if (key_event || color_event) {
        write_event(c_log_file, time, event + " pid=" + std::to_string(a_info.process_id));
    }
}

void read_new_log(AInfo& a_info, std::ofstream& c_log_file) {
    struct stat log_info {};
    if (stat(a_info.log_path.c_str(), &log_info) != 0) {
        if (!a_info.log_error_shown) {
            std::cout << "Ожидание журнала " << a_info.log_path << ".\n";
            a_info.log_error_shown = true;
        }
        return;
    }
    a_info.log_error_shown = false;
    if (log_info.st_size < a_info.log_pos) {
        a_info.log_pos = 0;
    }

    std::ifstream input(a_info.log_path, std::ios::binary);
    if (!input) {
        return;
    }
    input.seekg(a_info.log_pos);

    std::string line;
    while (std::getline(input, line)) {
        if (input.eof()) {
            break; // Не обрабатываем строку, пока A не допишет перевод строки.
        }
        a_info.log_pos = static_cast<std::streamoff>(input.tellg());
        save_a_event(line, a_info, c_log_file);
    }
}

void stop_watch(AInfo& a_info, std::ofstream& c_log_file) {
    read_new_log(a_info, c_log_file);
    if (!a_info.exit_saved) {
        write_event(c_log_file, time_now(),
                    "program_a disappearance detected pid=" + std::to_string(a_info.process_id) +
                    " (no exit record)");
    }
    std::cout << "Программа A завершилась. Ожидание следующего запуска.\n";
    a_info = AInfo {};
}

bool wants_exit() {
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
    const std::string a_path = get_a_path();
    if (a_path.empty()) {
        std::cerr << "Не удалось определить путь к программе A.\n";
        return 1;
    }

    std::ofstream c_log_file("program_c.log", std::ios::app);
    if (!c_log_file) {
        std::cerr << "Не удалось открыть program_c.log.\n";
        return 1;
    }

    AInfo a_info;
    bool many_a_shown = false;
    bool wait_shown = false;
    std::cout << "Программа C запущена. Для выхода введите q и нажмите Enter.\n";

    while (true) {
        if (wants_exit()) {
            break;
        }

        const std::vector<pid_t> a_pids = find_a_processes(a_path);
        if (a_pids.size() > 1) {
            if (!many_a_shown) {
                std::cerr << "Найдено несколько программ A. Мониторинг приостановлен.\n";
                write_event(c_log_file, time_now(),
                            "multiple program_a processes; monitoring paused");
                many_a_shown = true;
            }
            a_info = AInfo {};
            wait_shown = false;
        } else if (a_pids.empty()) {
            many_a_shown = false;
            if (a_info.process_id > 0) {
                stop_watch(a_info, c_log_file);
            } else if (!wait_shown) {
                std::cout << "Ожидание программы A.\n";
                wait_shown = true;
            }
        } else {
            many_a_shown = false;
            wait_shown = false;
            if (a_info.process_id > 0 && a_info.process_id != a_pids[0]) {
                stop_watch(a_info, c_log_file);
            }
            if (a_info.process_id < 0) {
                watch_a(a_pids[0], a_info, c_log_file);
            }
            if (a_info.process_id == a_pids[0]) {
                read_new_log(a_info, c_log_file);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "Программа C завершена.\n";
    return 0;
}
