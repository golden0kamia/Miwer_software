#pragma once

#include <string>
#include <vector>

// Port série USB (CDC-ACM) non bloquant, découpé en lignes, avec tampon d'écriture.
class SerialDevice {
public:
    // Valeur spéciale de « device » : détection automatique du port
    static constexpr const char* kAuto = "auto";

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

    // Ajoute line + '\n' au tampon de sortie et écrit ce qui peut l'être sans bloquer.
    // Retourne false si l'écriture a échoué (appareil débranché).
    bool writeLine(const std::string& line);
    // Écrit la suite du tampon de sortie (à appeler quand le fd est inscriptible).
    bool flush();
    bool hasPendingOutput() const { return !m_outBuffer.empty(); }

    // Cherche dans /dev/serial/by-id/ une entrée contenant « Miwer » (insensible à la casse),
    // sinon retourne /dev/ttyACM0.
    static std::string autoDetect();

private:
    int m_fd = -1;
    std::string m_buffer;
    std::string m_outBuffer;
};
