#include "serial_device.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxLineLength = 1024;
// Au-delà, l'appareil ne lit manifestement plus : on abandonne ce qui n'a pas été écrit
constexpr size_t kMaxOutBuffer = 64 * 1024;

constexpr const char* kByIdDir = "/dev/serial/by-id/";
constexpr const char* kDefaultPort = "/dev/ttyACM0";

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
    m_outBuffer.clear();
    return true;
}

void SerialDevice::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_buffer.clear();
    m_outBuffer.clear();
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

bool SerialDevice::writeLine(const std::string& line)
{
    if (m_fd < 0)
        return false;
    if (m_outBuffer.size() + line.size() + 1 > kMaxOutBuffer)
        m_outBuffer.clear();
    m_outBuffer += line;
    m_outBuffer += '\n';
    return flush();
}

bool SerialDevice::flush()
{
    while (m_fd >= 0 && !m_outBuffer.empty()) {
        const ssize_t n = ::write(m_fd, m_outBuffer.data(), m_outBuffer.size());
        if (n > 0) {
            m_outBuffer.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;  // le reste attendra que le fd soit inscriptible
        return false;
    }
    return m_fd >= 0;
}

std::string SerialDevice::autoDetect()
{
    std::string found;
    if (DIR* dir = opendir(kByIdDir)) {
        while (const dirent* entry = readdir(dir)) {
            std::string name = entry->d_name;
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // Ordre stable si plusieurs appareils : le premier par ordre alphabétique
            if (lower.find("miwer") != std::string::npos && (found.empty() || name < found))
                found = name;
        }
        closedir(dir);
    }
    return found.empty() ? kDefaultPort : kByIdDir + found;
}
