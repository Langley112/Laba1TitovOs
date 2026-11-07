#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct Parameters {
    std::string text = "Hello";
    std::string color = "green";
    int size = 1;
};

constexpr const char* fifo_path = "/tmp/program_a_fifo";
constexpr const char* terminal_path = "/usr/bin/ptyxis";

bool is_supported_color(const std::string& color) {
    return color == "red" || color == "green" || color == "blue";
}

bool parse_size(const std::string& value, int& size) {
    if (value.empty()) {
        return false;
    }
    for (char digit : value) {
        if (digit < '0' || digit > '9') {
            return false;
        }
    }

    char* end = nullptr;
    errno = 0;
    const long number = std::strtol(value.c_str(), &end, 10);
    if (errno == ERANGE || *end != '\0' || number < 1 || number > INT_MAX) {
        return false;
    }
    size = static_cast<int>(number);
    return true;
}

bool read_parameters(Parameters& parameters) {
    std::string text;
    std::string color;
    std::string size_text;

    std::cout << "Текст: " << std::flush;
    if (!std::getline(std::cin, text)) {
        return false;
    }
    std::cout << "Цвет (red, green, blue): " << std::flush;
    if (!std::getline(std::cin, color)) {
        return false;
    }
    std::cout << "Количество повторений: " << std::flush;
    if (!std::getline(std::cin, size_text)) {
        return false;
    }

    int size = 0;
    if (!is_supported_color(color) || !parse_size(size_text, size)) {
        std::cout << "Некорректный цвет или размер. Параметры не изменены.\n";
        return true;
    }

    parameters.text = text;
    parameters.color = color;
    parameters.size = size;
    std::cout << "Параметры сохранены.\n";
    return true;
}

bool find_program_a(std::string& path) {
    char executable[PATH_MAX + 1];
    const ssize_t length = readlink("/proc/self/exe", executable, PATH_MAX);
    if (length < 0 || length == PATH_MAX) {
        std::cerr << "Не удалось определить путь к программе B.\n";
        return false;
    }
    executable[length] = '\0';

    path = executable;
    const std::size_t separator = path.find_last_of('/');
    path.erase(separator + 1);
    path += "program_a";
    if (access(path.c_str(), X_OK) != 0) {
        std::cerr << "Не найден исполняемый файл программы A рядом с программой B: "
                  << path << '\n';
        return false;
    }
    return true;
}

void reap_terminals(pid_t& current_terminal_pid) {
    int status = 0;
    pid_t finished_pid;
    while ((finished_pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (finished_pid == current_terminal_pid) {
            current_terminal_pid = -1;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            std::cout << "Терминал завершился с ошибкой. Проверьте запуск программы A.\n";
        } else if (WIFSIGNALED(status)) {
            std::cout << "Окно терминала было прервано сигналом.\n";
        }
    }
}

bool program_a_is_running() {
    struct stat info {};
    if (lstat(fifo_path, &info) != 0 || !S_ISFIFO(info.st_mode)) {
        return false;
    }

    const int fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        return false;
    }
    close(fifo_fd);
    return true;
}

void launch_program_a(const Parameters& parameters, pid_t& terminal_pid,
                      bool& a_was_running) {
    reap_terminals(terminal_pid);
    if (program_a_is_running()) {
        a_was_running = true;
        std::cout << "Программа A уже запущена или её окно ещё открывается.\n";
        return;
    }
    if (terminal_pid > 0 && !a_was_running) {
        std::cout << "Окно программы A ещё открывается.\n";
        return;
    }

    std::string program_path;
    if (!find_program_a(program_path)) {
        return;
    }
    if (access(terminal_path, X_OK) != 0) {
        std::cerr << "Не найден терминал Ptyxis: " << terminal_path << '\n';
        return;
    }

    const std::string size_text = std::to_string(parameters.size);
    const pid_t child_pid = fork();
    if (child_pid < 0) {
        std::perror("Не удалось создать процесс");
        return;
    }
    if (child_pid == 0) {
        // SIGPIPE игнорируется только в B; терминал и A запускаются с обычным поведением сигнала.
        std::signal(SIGPIPE, SIG_DFL);
        char* const arguments[] = {
            const_cast<char*>("ptyxis"),
            const_cast<char*>("--"),
            const_cast<char*>(program_path.c_str()),
            const_cast<char*>("--text"),
            const_cast<char*>(parameters.text.c_str()),
            const_cast<char*>("--color"),
            const_cast<char*>(parameters.color.c_str()),
            const_cast<char*>("--size"),
            const_cast<char*>(size_text.c_str()),
            nullptr
        };
        execvp(terminal_path, arguments);
        std::perror("Не удалось запустить терминал");
        _exit(127);
    }

    terminal_pid = child_pid;
    a_was_running = false;
    std::cout << "Открывается отдельное окно программы A.\n";

    // Коротко ждём появления читателя FIFO, чтобы отличить запуск A от оставшегося окна терминала.
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (program_a_is_running()) {
            a_was_running = true;
            break;
        }
        reap_terminals(terminal_pid);
        if (terminal_pid < 0) {
            break;
        }
        usleep(50 * 1000);
    }
}

bool is_supported_command(const std::string& command) {
    if (command.size() == 5 && command.compare(0, 4, "key=") == 0) {
        const char key = command[4];
        return key == 'r' || key == 'g' || key == 'b' ||
               key == '+' || key == 'q';
    }

    const std::string prefix = "cmd=color:";
    if (command.compare(0, prefix.size(), prefix) == 0) {
        return is_supported_color(command.substr(prefix.size()));
    }
    return false;
}

void send_command(const std::string& command) {
    struct stat info {};
    if (lstat(fifo_path, &info) != 0) {
        if (errno == ENOENT) {
            std::cout << "FIFO не существует. Запустите программу A.\n";
        } else {
            std::perror("Не удалось проверить FIFO");
        }
        return;
    }
    if (!S_ISFIFO(info.st_mode)) {
        std::cout << "Путь " << fifo_path << " не является FIFO.\n";
        return;
    }

    const int fifo_fd = open(fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        if (errno == ENOENT || errno == ENXIO) {
            std::cout << "Программа A не запущена или уже завершилась.\n";
        } else {
            std::perror("Не удалось открыть FIFO для записи");
        }
        return;
    }

    const std::string line = command + '\n';
    ssize_t written;
    do {
        written = write(fifo_fd, line.data(), line.size());
    } while (written < 0 && errno == EINTR);
    const int write_error = errno;
    close(fifo_fd);

    if (written == static_cast<ssize_t>(line.size())) {
        std::cout << "Команда отправлена.\n";
    } else if (written < 0 && write_error == EPIPE) {
        std::cout << "Программа A завершилась до отправки команды.\n";
    } else if (written < 0) {
        errno = write_error;
        std::perror("Не удалось отправить команду");
    } else {
        std::cout << "Не удалось отправить команду полностью.\n";
    }
}

bool prompt_and_send_command() {
    std::cout << "Команда (например, key=r или cmd=color:blue): " << std::flush;
    std::string command;
    if (!std::getline(std::cin, command)) {
        return false;
    }
    if (!is_supported_command(command)) {
        std::cout << "Некорректная команда.\n";
        return true;
    }
    send_command(command);
    return true;
}

void show_menu(const Parameters& parameters) {
    std::cout << "\nПрограмма B. Параметры A: текст=\"" << parameters.text
              << "\", цвет=" << parameters.color << ", размер=" << parameters.size
              << "\n1. Ввести параметры A\n"
              << "2. Запустить A\n"
              << "3. Отправить команду A\n"
              << "0. Выйти\n"
              << "Выберите пункт: " << std::flush;
}

int main() {
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::perror("Не удалось настроить SIGPIPE");
        return 1;
    }

    Parameters parameters;
    pid_t terminal_pid = -1;
    bool a_was_running = false;
    std::string choice;
    while (true) {
        reap_terminals(terminal_pid);
        if (program_a_is_running()) {
            a_was_running = true;
        }
        show_menu(parameters);
        if (!std::getline(std::cin, choice)) {
            break;
        }

        if (choice == "1") {
            if (!read_parameters(parameters)) {
                break;
            }
        } else if (choice == "2") {
            launch_program_a(parameters, terminal_pid, a_was_running);
        } else if (choice == "3") {
            if (!prompt_and_send_command()) {
                break;
            }
        } else if (choice == "0") {
            break;
        } else {
            std::cout << "Некорректный пункт меню.\n";
        }
    }

    reap_terminals(terminal_pid);
    std::cout << "Программа B завершена.\n";
    return 0;
}
