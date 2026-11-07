#include <cerrno>
#include <climits>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

enum class Color {
    Red,
    Green,
    Blue
};

struct ProgramState {
    std::string text = "Hello";
    int repeat_count = 1;
    Color color = Color::Green;
    bool running = true;
};

constexpr const char* fifo_path = "/tmp/program_a_fifo";
volatile sig_atomic_t stop_requested = 0;

void request_stop(int) {
    stop_requested = 1;
}

bool parse_color(const std::string& name, Color& color) {
    if (name == "red") {
        color = Color::Red;
    } else if (name == "green") {
        color = Color::Green;
    } else if (name == "blue") {
        color = Color::Blue;
    } else {
        return false;
    }
    return true;
}

const char* color_name(Color color) {
    switch (color) {
        case Color::Red: return "red";
        case Color::Green: return "green";
        case Color::Blue: return "blue";
    }
    return "green";
}

const char* color_code(Color color) {
    switch (color) {
        case Color::Red: return "\033[31m";
        case Color::Green: return "\033[32m";
        case Color::Blue: return "\033[34m";
    }
    return "\033[32m";
}

Color next_color(Color color) {
    switch (color) {
        case Color::Red: return Color::Green;
        case Color::Green: return Color::Blue;
        case Color::Blue: return Color::Red;
    }
    return Color::Green;
}

bool parse_arguments(int argc, char* argv[], ProgramState& state) {
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            std::cerr << "Отсутствует значение для " << argv[i] << '\n';
            return false;
        }

        const std::string option = argv[i];
        const std::string value = argv[i + 1];

        if (option == "--text") {
            state.text = value;
        } else if (option == "--color") {
            if (!parse_color(value, state.color)) {
                std::cerr << "Неизвестный цвет: " << value << '\n';
                return false;
            }
        } else if (option == "--size") {
            char* end = nullptr;
            errno = 0;
            const long number = std::strtol(value.c_str(), &end, 10);
            if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
                number < 1 || number > INT_MAX) {
                std::cerr << "--size должен быть положительным целым числом\n";
                return false;
            }
            state.repeat_count = static_cast<int>(number);
        } else {
            std::cerr << "Неизвестный параметр: " << option << '\n';
            return false;
        }
    }
    return true;
}

void log_event(std::ofstream& log, const std::string& event) {
    std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);

    char timestamp[20];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    log << timestamp << ' ' << event << '\n';
    log.flush();
}

void draw_text(const ProgramState& state) {
    std::cout << "\033[2J\033[H" << color_code(state.color);
    for (int i = 0; i < state.repeat_count; ++i) {
        if (i > 0) {
            std::cout << ' ';
        }
        std::cout << state.text;
    }
    std::cout << "\033[0m\n" << std::flush;
}

bool apply_key(char key, ProgramState& state) {
    switch (key) {
        case 'r': state.color = Color::Red; break;
        case 'g': state.color = Color::Green; break;
        case 'b': state.color = Color::Blue; break;
        case '+': state.color = next_color(state.color); break;
        case 'q': state.running = false; break;
        default: return false;
    }
    return true;
}

void handle_key(char key, ProgramState& state, std::ofstream& log) {
    const Color previous_color = state.color;
    if (!apply_key(key, state)) {
        return;
    }

    log_event(log, std::string("key=") + key + " color=" + color_name(state.color));
    if (state.running && state.color != previous_color) {
        draw_text(state);
    }
}

void handle_fifo_line(std::string line, ProgramState& state, std::ofstream& log) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    if (line.size() == 5 && line.compare(0, 4, "key=") == 0) {
        handle_key(line[4], state, log);
        return;
    }

    const std::string prefix = "cmd=color:";
    if (line.compare(0, prefix.size(), prefix) == 0) {
        Color color;
        if (parse_color(line.substr(prefix.size()), color)) {
            const Color previous_color = state.color;
            state.color = color;
            log_event(log, std::string("cmd=color:") + color_name(color) +
                           " color=" + color_name(state.color));
            if (state.color != previous_color) {
                draw_text(state);
            }
        }
    }
}

bool prepare_fifo(bool& created) {
    struct stat info {};
    if (lstat(fifo_path, &info) == 0) {
        if (!S_ISFIFO(info.st_mode)) {
            std::cerr << fifo_path << " существует, но не является FIFO\n";
            return false;
        }
        return true;
    }
    if (errno != ENOENT) {
        std::perror("lstat FIFO");
        return false;
    }

    if (mkfifo(fifo_path, 0600) == 0) {
        created = true;
        return true;
    }
    if (errno == EEXIST && lstat(fifo_path, &info) == 0 && S_ISFIFO(info.st_mode)) {
        return true;
    }
    std::perror("mkfifo");
    return false;
}

int open_fifo(bool& created) {
    if (!prepare_fifo(created)) {
        return -1;
    }

    // Свой открытый конец записи предотвращает EOF и постоянные пробуждения poll без отправителя.
    const int fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fifo_fd < 0) {
        std::perror("open FIFO");
        if (created) {
            unlink(fifo_path);
        }
    }
    return fifo_fd;
}

void close_fifo(int fifo_fd, bool created) {
    close(fifo_fd);
    if (created) {
        unlink(fifo_path);
    }
}

bool enable_keyboard_mode(termios& original) {
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        std::perror("tcgetattr");
        return false;
    }

    termios modified = original;
    // Оставляем ISIG включённым: Ctrl+C вызывает обработчик, а не попадает в поток клавиш.
    modified.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    modified.c_cc[VMIN] = 1;
    modified.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &modified) != 0) {
        std::perror("tcsetattr");
        tcsetattr(STDIN_FILENO, TCSANOW, &original);
        return false;
    }
    return true;
}

bool restore_keyboard_mode(const termios& original) {
    if (tcsetattr(STDIN_FILENO, TCSANOW, &original) != 0) {
        std::perror("restore terminal");
        return false;
    }
    return true;
}

bool install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) {
        std::perror("sigaction");
        return false;
    }
    return true;
}

bool read_keyboard(ProgramState& state, std::ofstream& log) {
    char key;
    const ssize_t count = read(STDIN_FILENO, &key, 1);
    if (count == 1) {
        handle_key(key, state, log);
        return true;
    }
    if (count < 0 && errno == EINTR) {
        return true;
    }
    if (count == 0) {
        std::cerr << "Стандартный ввод закрыт\n";
    } else {
        std::perror("read keyboard");
    }
    return false;
}

bool read_fifo(int fifo_fd, std::string& pending_line,
               ProgramState& state, std::ofstream& log) {
    char buffer[256];
    while (state.running && !stop_requested) {
        const ssize_t count = read(fifo_fd, buffer, sizeof(buffer));
        if (count > 0) {
            pending_line.append(buffer, static_cast<std::size_t>(count));
            std::size_t end = pending_line.find('\n');
            while (end != std::string::npos && state.running) {
                handle_fifo_line(pending_line.substr(0, end), state, log);
                pending_line.erase(0, end + 1);
                end = pending_line.find('\n');
            }
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else {
            if (count == 0) {
                std::cerr << "FIFO неожиданно закрыт\n";
            } else {
                std::perror("read FIFO");
            }
            return false;
        }
    }
    return true;
}

bool run_event_loop(int fifo_fd, ProgramState& state, std::ofstream& log) {
    std::string pending_line;
    pollfd descriptors[2] = {
        {STDIN_FILENO, POLLIN, 0},
        {fifo_fd, POLLIN, 0}
    };

    while (state.running && !stop_requested) {
        // poll(-1) спит до появления события на клавиатуре или в FIFO.
        const int ready = poll(descriptors, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            return false;
        }

        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Ошибка ожидания клавиатуры\n";
            return false;
        }
        if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Ошибка ожидания FIFO\n";
            return false;
        }
        if ((descriptors[0].revents & POLLIN) && !read_keyboard(state, log)) {
            return false;
        }
        if (state.running && (descriptors[1].revents & POLLIN) &&
            !read_fifo(fifo_fd, pending_line, state, log)) {
            return false;
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    ProgramState state;
    if (!parse_arguments(argc, argv, state)) {
        return 1;
    }

    std::ofstream log("program_a.log", std::ios::app);
    if (!log) {
        std::cerr << "Не удалось открыть program_a.log\n";
        return 1;
    }

    bool created_fifo = false;
    const int fifo_fd = open_fifo(created_fifo);
    if (fifo_fd < 0) {
        return 1;
    }

    termios original_terminal{};
    if (!install_signal_handlers() || !enable_keyboard_mode(original_terminal)) {
        close_fifo(fifo_fd, created_fifo);
        return 1;
    }

    log_event(log, std::string("start color=") + color_name(state.color));
    draw_text(state);
    const bool success = run_event_loop(fifo_fd, state, log);
    log_event(log, "exit");

    std::cout << "\033[0m" << std::flush;
    const bool terminal_restored = restore_keyboard_mode(original_terminal);
    close_fifo(fifo_fd, created_fifo);
    return success && terminal_restored ? 0 : 1;
}
