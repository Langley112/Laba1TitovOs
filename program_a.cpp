#include <cerrno>
#include <cstdio>
#include <csignal>
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
    Color color = Color::Green;
    bool running = true;
};

constexpr const char* fifo_path = "/tmp/program_a_fifo";
volatile sig_atomic_t stop_requested = 0;

void stop_program(int /* signal_number */) {
    stop_requested = 1;
}

bool read_color(const std::string& color_text, Color& color) {
    if (color_text == "red") {
        color = Color::Red;
    } else if (color_text == "green") {
        color = Color::Green;
    } else if (color_text == "blue") {
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

bool read_args(int argument_count, char* arguments[], ProgramState& program_state) {
    for (int argument_index = 1; argument_index < argument_count; argument_index += 2) {
        if (argument_index + 1 >= argument_count) {
            std::cerr << "Отсутствует значение для " << arguments[argument_index] << '\n';
            return false;
        }

        const std::string option = arguments[argument_index];
        const std::string value = arguments[argument_index + 1];

        if (option == "--text") {
            program_state.text = value;
        } else if (option == "--color") {
            if (!read_color(value, program_state.color)) {
                std::cerr << "Неизвестный цвет: " << value << '\n';
                return false;
            }
        } else {
            std::cerr << "Неизвестный параметр: " << option << '\n';
            return false;
        }
    }
    return true;
}

void write_log(std::ofstream& log_file, const std::string& event) {
    std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);

    char timestamp[20];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    log_file << timestamp << ' ' << event << '\n';
    log_file.flush();
}

void draw_text(const ProgramState& program_state) {
    std::cout << "\033[2J\033[H" << color_code(program_state.color);
    std::cout << program_state.text;
    std::cout << "\033[0m\n" << std::flush;
}

bool use_key(char key, ProgramState& program_state) {
    switch (key) {
        case 'r': program_state.color = Color::Red; break;
        case 'g': program_state.color = Color::Green; break;
        case 'b': program_state.color = Color::Blue; break;
        case 'q': program_state.running = false; break;
        default: return false;
    }
    return true;
}

void run_key(char key, ProgramState& program_state, std::ofstream& log_file) {
    const Color old_color = program_state.color;
    if (!use_key(key, program_state)) {
        return;
    }

    write_log(log_file, std::string("key=") + key + " color=" + color_name(program_state.color));
    if (program_state.running && program_state.color != old_color) {
        draw_text(program_state);
    }
}

void run_fifo_command(std::string line, ProgramState& program_state, std::ofstream& log_file) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    if (line.size() == 5 && line.compare(0, 4, "key=") == 0) {
        run_key(line[4], program_state, log_file);
        return;
    }

    const std::string prefix = "cmd=color:";
    if (line.compare(0, prefix.size(), prefix) != 0) {
        return;
    }

    Color color;
    if (!read_color(line.substr(prefix.size()), color)) {
        return;
    }

    const Color old_color = program_state.color;
    program_state.color = color;
    write_log(log_file, std::string("cmd=color:") + color_name(color) +
                   " color=" + color_name(program_state.color));
    if (program_state.color != old_color) {
        draw_text(program_state);
    }
}

bool make_fifo(bool& fifo_created) {
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
        fifo_created = true;
        return true;
    }
    if (errno == EEXIST && lstat(fifo_path, &info) == 0 && S_ISFIFO(info.st_mode)) {
        return true;
    }
    std::perror("mkfifo");
    return false;
}

int open_fifo(bool& fifo_created) {
    if (!make_fifo(fifo_created)) {
        return -1;
    }

    // O_RDWR не даёт FIFO закрыться, пока нет внешнего отправителя.
    // O_NONBLOCK позволяет читать без ожидания следующей команды.
    const int fifo_file_descriptor = open(fifo_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fifo_file_descriptor < 0) {
        std::perror("open FIFO");
        if (fifo_created) {
            unlink(fifo_path);
        }
    }
    return fifo_file_descriptor;
}

void close_fifo(int fifo_file_descriptor, bool fifo_created) {
    close(fifo_file_descriptor);
    if (fifo_created) {
        unlink(fifo_path);
    }
}

bool start_keyboard(termios& old_settings) {
    if (tcgetattr(STDIN_FILENO, &old_settings) != 0) {
        std::perror("tcgetattr");
        return false;
    }

    termios new_settings = old_settings;
    // Оставляем ISIG включённым: Ctrl+C вызывает обработчик, а не попадает в поток клавиш.
    new_settings.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    new_settings.c_cc[VMIN] = 1;
    new_settings.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_settings) != 0) {
        std::perror("tcsetattr");
        tcsetattr(STDIN_FILENO, TCSANOW, &old_settings);
        return false;
    }
    return true;
}

bool restore_keyboard(const termios& old_settings) {
    if (tcsetattr(STDIN_FILENO, TCSANOW, &old_settings) != 0) {
        std::perror("restore terminal");
        return false;
    }
    return true;
}

bool set_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = stop_program;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) {
        std::perror("sigaction");
        return false;
    }
    return true;
}

bool read_key(ProgramState& program_state, std::ofstream& log_file) {
    char key;
    const ssize_t count = read(STDIN_FILENO, &key, 1);
    if (count == 1) {
        run_key(key, program_state, log_file);
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

void run_fifo_lines(std::string& line_part, ProgramState& program_state, std::ofstream& log_file) {
    // FIFO может передать несколько строк сразу или только часть одной строки.
    std::size_t line_end = line_part.find('\n');
    while (line_end != std::string::npos && program_state.running) {
        const std::string line = line_part.substr(0, line_end);
        line_part.erase(0, line_end + 1);
        run_fifo_command(line, program_state, log_file);
        line_end = line_part.find('\n');
    }
}

bool read_fifo(int fifo_file_descriptor, std::string& line_part,
               ProgramState& program_state, std::ofstream& log_file) {
    char buffer[256];
    while (program_state.running && !stop_requested) {
        const ssize_t bytes_read = read(fifo_file_descriptor, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            line_part.append(buffer, static_cast<std::size_t>(bytes_read));
            run_fifo_lines(line_part, program_state, log_file);
            continue;
        }
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }
        // Неблокирующий FIFO прочитан полностью; ждём следующие данные в poll().
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (bytes_read == 0) {
            std::cerr << "FIFO неожиданно закрыт\n";
        } else {
            std::perror("read FIFO");
        }
        return false;
    }
    return true;
}

bool run_program(int fifo_file_descriptor, ProgramState& program_state, std::ofstream& log_file) {
    std::string line_part;
    pollfd inputs[2] = {
        {STDIN_FILENO, POLLIN, 0},
        {fifo_file_descriptor, POLLIN, 0}
    };

    while (program_state.running && !stop_requested) {
        // poll(-1) спит до появления события на клавиатуре или в FIFO.
        const int ready = poll(inputs, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            return false;
        }

        if (inputs[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Ошибка ожидания клавиатуры\n";
            return false;
        }
        if (inputs[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::cerr << "Ошибка ожидания FIFO\n";
            return false;
        }
        if ((inputs[0].revents & POLLIN) && !read_key(program_state, log_file)) {
            return false;
        }
        if (program_state.running && (inputs[1].revents & POLLIN) &&
            !read_fifo(fifo_file_descriptor, line_part, program_state, log_file)) {
            return false;
        }
    }
    return true;
}

int main(int argument_count, char* arguments[]) {
    ProgramState program_state;
    if (!read_args(argument_count, arguments, program_state)) {
        return 1;
    }

    std::ofstream log_file("program_a.log", std::ios::app);
    if (!log_file) {
        std::cerr << "Не удалось открыть program_a.log\n";
        return 1;
    }

    bool fifo_created = false;
    const int fifo_file_descriptor = open_fifo(fifo_created);
    if (fifo_file_descriptor < 0) {
        return 1;
    }

    termios old_terminal{};
    if (!set_signal_handlers() || !start_keyboard(old_terminal)) {
        close_fifo(fifo_file_descriptor, fifo_created);
        return 1;
    }

    write_log(log_file, std::string("start color=") + color_name(program_state.color));
    draw_text(program_state);
    const bool success = run_program(fifo_file_descriptor, program_state, log_file);
    write_log(log_file, "exit");

    std::cout << "\033[0m" << std::flush;
    const bool terminal_restored = restore_keyboard(old_terminal);
    close_fifo(fifo_file_descriptor, fifo_created);
    return success && terminal_restored ? 0 : 1;
}
