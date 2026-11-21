#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

constexpr const char* fifo_path = "/tmp/program_a_fifo";
constexpr const char* terminal_path = "/usr/bin/ptyxis";

bool a_is_running() {
    struct stat info {};
    if (lstat(fifo_path, &info) != 0 || !S_ISFIFO(info.st_mode)) {
        return false;
    }

    const int fifo_file_descriptor = open(fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fifo_file_descriptor < 0) {
        return false;
    }
    ::close(fifo_file_descriptor);
    return true;
}

class ProgramBWindow : public QWidget {
public:
    ProgramBWindow() {
        setWindowTitle("Программа B");

        text_input = new QLineEdit("Hello", this);
        color_input = new QComboBox(this);
        color_input->addItems({"red", "green", "blue"});
        color_input->setCurrentText("green");
        status = new QLabel("Задайте параметры и запустите программу A.", this);
        status->setWordWrap(true);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto* form = new QFormLayout;
        form->addRow("Текст:", text_input);
        form->addRow("Цвет:", color_input);

        auto* start_button = new QPushButton("Запустить A", this);
        auto* red_button = new QPushButton("Красный", this);
        auto* green_button = new QPushButton("Зелёный", this);
        auto* blue_button = new QPushButton("Синий", this);
        auto* stop_button = new QPushButton("Завершить A", this);
        auto* color_buttons = new QHBoxLayout;
        color_buttons->addWidget(red_button);
        color_buttons->addWidget(green_button);
        color_buttons->addWidget(blue_button);
        auto* layout = new QVBoxLayout(this);
        layout->addLayout(form);
        layout->addWidget(start_button);
        layout->addLayout(color_buttons);
        layout->addWidget(stop_button);
        layout->addWidget(status);

        connect(start_button, &QPushButton::clicked, this, [this] { start_a(); });
        connect(red_button, &QPushButton::clicked, this, [this] { send_command("cmd=color:red"); });
        connect(green_button, &QPushButton::clicked, this, [this] { send_command("cmd=color:green"); });
        connect(blue_button, &QPushButton::clicked, this, [this] { send_command("cmd=color:blue"); });
        connect(stop_button, &QPushButton::clicked, this, [this] { send_command("key=q"); });

        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { check_start(); });
        timer->start(100);
    }

private:
    void start_a() {
        if (a_is_running()) {
            status->setText("Программа A уже запущена.");
            return;
        }
        if (checks_left > 0) {
            status->setText("Окно программы A ещё открывается.");
            return;
        }

        const QString a_path = QCoreApplication::applicationDirPath() + "/program_a";
        const QByteArray program = a_path.toLocal8Bit();
        if (access(program.constData(), X_OK) != 0) {
            status->setText("Не найден исполняемый файл программы A: " + a_path);
            return;
        }
        if (access(terminal_path, X_OK) != 0) {
            status->setText("Не найден терминал Ptyxis: /usr/bin/ptyxis");
            return;
        }

        const QByteArray text = text_input->text().toUtf8();
        const QByteArray color = color_input->currentText().toUtf8();
        const pid_t child = fork();
        if (child < 0) {
            status->setText("Не удалось создать процесс: " + QString::fromLocal8Bit(std::strerror(errno)));
            return;
        }
        if (child == 0) {
            std::signal(SIGPIPE, SIG_DFL);
            execl(terminal_path, "ptyxis", "--", program.constData(),
                  "--text", text.constData(), "--color", color.constData(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }

        terminal_pid = child;
        checks_left = 20;
        status->setText("Открывается окно программы A.");
    }

    void check_start() {
        int child_status = 0;
        pid_t done_pid;
        while ((done_pid = waitpid(-1, &child_status, WNOHANG)) > 0) {
            if (done_pid == terminal_pid) {
                terminal_pid = -1;
                if (checks_left > 0 &&
                    (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)) {
                    checks_left = 0;
                    status->setText("Не удалось запустить терминал Ptyxis.");
                    return;
                }
            }
        }
        if (checks_left == 0) {
            return;
        }
        if (a_is_running()) {
            checks_left = 0;
            status->setText("Программа A запущена.");
        } else if (--checks_left == 0) {
            status->setText("Программа A не ответила. Проверьте окно терминала.");
        }
    }

    void send_command(const QByteArray& command) {
        struct stat file_info {};
        if (lstat(fifo_path, &file_info) != 0) {
            if (errno == ENOENT) {
                status->setText("FIFO не существует. Запустите программу A.");
            } else {
                status->setText("Не удалось проверить FIFO.");
            }
            return;
        }
        if (!S_ISFIFO(file_info.st_mode)) {
            status->setText("Путь /tmp/program_a_fifo не является FIFO.");
            return;
        }

        // O_NONBLOCK не позволяет окну зависнуть, если A не читает канал.
        const int fifo_file_descriptor = open(fifo_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fifo_file_descriptor < 0) {
            if (errno == ENOENT || errno == ENXIO) {
                status->setText("Программа A не запущена или уже завершилась.");
            } else {
                status->setText("Не удалось открыть FIFO.");
            }
            return;
        }

        // Перевод строки показывает A, где заканчивается команда.
        const QByteArray line = command + '\n';
        ssize_t bytes_written;
        do {
            bytes_written = write(fifo_file_descriptor, line.constData(), static_cast<size_t>(line.size()));
        } while (bytes_written < 0 && errno == EINTR);
        ::close(fifo_file_descriptor);

        if (bytes_written == line.size()) {
            status->setText("Команда отправлена.");
        } else {
            status->setText("Не удалось отправить команду. Проверьте программу A.");
        }
    }

    QLineEdit* text_input;
    QComboBox* color_input;
    QLabel* status;
    pid_t terminal_pid = -1;
    int checks_left = 0;
};

int main(int argument_count, char* arguments[]) {
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return 1;
    }

    QApplication app(argument_count, arguments);
    ProgramBWindow window;
    window.show();
    return app.exec();
}
