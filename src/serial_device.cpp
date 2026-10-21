#include "serial_device.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxLineLength = 1024;

speed_t toSpeed(int baudrate)
{
    switch (baudrate) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

}  // namespace

SerialDevice::~SerialDevice()
{
    close();
}

bool SerialDevice::open(const std::string& path, int baudrate, std::string& error)
{
    close();

    const speed_t speed = toSpeed(baudrate);
    if (speed == 0) {
        error = "baudrate non supporté : " + std::to_string(baudrate);
        return false;
    }

    m_fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        error = path + " : " + std::strerror(errno);
        return false;
    }

    termios tty{};
    if (tcgetattr(m_fd, &tty) != 0) {
        error = "tcgetattr : " + std::string(std::strerror(errno));
        close();
        return false;
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(m_fd, TCSANOW, &tty) != 0) {
        error = "tcsetattr : " + std::string(std::strerror(errno));
        close();
        return false;
    }

    tcflush(m_fd, TCIFLUSH);
    m_buffer.clear();
    return true;
}

void SerialDevice::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_buffer.clear();
}

std::vector<std::string> SerialDevice::readLines(bool& disconnected)
{
    std::vector<std::string> lines;
    disconnected = false;

    char chunk[256];
    for (;;) {
        const ssize_t n = ::read(m_fd, chunk, sizeof(chunk));
        if (n > 0) {
            m_buffer.append(chunk, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            disconnected = true;  // EIO typiquement : appareil débranché
        break;
    }

    size_t pos;
    while ((pos = m_buffer.find('\n')) != std::string::npos) {
        std::string line = m_buffer.substr(0, pos);
        m_buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
    }

    // Protection contre un flux sans fin de ligne
    if (m_buffer.size() > kMaxLineLength)
        m_buffer.clear();

    return lines;
}
