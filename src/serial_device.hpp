#pragma once

#include <string>
#include <vector>

// Port série USB (CDC-ACM) non bloquant, découpé en lignes.
class SerialDevice {
public:
    SerialDevice() = default;
    ~SerialDevice();

    SerialDevice(const SerialDevice&) = delete;
    SerialDevice& operator=(const SerialDevice&) = delete;

    // Retourne false (et remplit error) en cas d'échec.
    bool open(const std::string& path, int baudrate, std::string& error);
    void close();

    bool isOpen() const { return m_fd >= 0; }
    int fd() const { return m_fd; }

    // Lit tout ce qui est disponible et retourne les lignes complètes.
    // disconnected passe à true si l'appareil a été débranché.
    std::vector<std::string> readLines(bool& disconnected);

private:
    int m_fd = -1;
    std::string m_buffer;
};
