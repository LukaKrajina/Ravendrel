#include "../include/qhal/r/ThermalController.hpp"

#include <chrono>
#include <thread>
#include <stdexcept>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace qhal
{
    #ifndef _WIN32

    class PosixSerialPort : public ISerialPort
    {
    private:
        int fd = -1;

    public:
        ~PosixSerialPort() override { close(); }

        bool open(const std::string &device) override
        {
            fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
            if (fd < 0)
                return false;

            struct termios tty;
            std::memset(&tty, 0, sizeof(tty));
            if (tcgetattr(fd, &tty) != 0)
            {
                ::close(fd);
                fd = -1;
                return false;
            }

            cfsetospeed(&tty, B9600);
            cfsetispeed(&tty, B9600);
            tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
            tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
            tty.c_cflag |= (CLOCAL | CREAD);
            tty.c_iflag &= ~(IXON | IXOFF | IXANY);
            tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
            tty.c_oflag &= ~OPOST;
            tty.c_cc[VMIN] = 0;
            tty.c_cc[VTIME] = 0;

            if (tcsetattr(fd, TCSANOW, &tty) != 0)
            {
                ::close(fd);
                fd = -1;
                return false;
            }
            return true;
        }

        void close() override
        {
            if (fd >= 0)
            {
                ::close(fd);
                fd = -1;
            }
        }

        bool is_open() const override { return fd >= 0; }

        int write(const std::string &data) override
        {
            if (fd < 0)
                return -1;
            return static_cast<int>(::write(fd, data.data(), data.size()));
        }

        std::string read_line(double timeout_ms) override
        {
            std::string line;
            if (fd < 0)
                return line;

            auto start = std::chrono::steady_clock::now();
            char c;
            while (true)
            {
                ssize_t n = ::read(fd, &c, 1);
                if (n > 0)
                {
                    if (c == '\n')
                        break;
                    if (c != '\r')
                        line += c;
                }
                else
                {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
                    if (elapsed >= timeout_ms)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            return line;
        }
    };

    #else

    class WinSerialPort : public ISerialPort
    {
    private:
        HANDLE h = INVALID_HANDLE_VALUE;

    public:
        ~WinSerialPort() override { close(); }

        bool open(const std::string &device) override
        {
            h = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return false;

            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (!GetCommState(h, &dcb))
            {
                close();
                return false;
            }
            dcb.BaudRate = CBR_9600;
            dcb.ByteSize = 8;
            dcb.Parity = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            if (!SetCommState(h, &dcb))
            {
                close();
                return false;
            }

            COMMTIMEOUTS timeouts{};
            timeouts.ReadIntervalTimeout = 5;
            timeouts.ReadTotalTimeoutConstant = 5;
            timeouts.ReadTotalTimeoutMultiplier = 5;
            timeouts.WriteTotalTimeoutConstant = 500;
            timeouts.WriteTotalTimeoutMultiplier = 5;
            SetCommTimeouts(h, &timeouts);
            return true;
        }

        void close() override
        {
            if (h != INVALID_HANDLE_VALUE)
            {
                CloseHandle(h);
                h = INVALID_HANDLE_VALUE;
            }
        }

        bool is_open() const override { return h != INVALID_HANDLE_VALUE; }

        int write(const std::string &data) override
        {
            if (h == INVALID_HANDLE_VALUE)
                return -1;
            DWORD written = 0;
            if (!WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr))
                return -1;
            return static_cast<int>(written);
        }

        std::string read_line(double timeout_ms) override
        {
            std::string line;
            if (h == INVALID_HANDLE_VALUE)
                return line;

            auto start = std::chrono::steady_clock::now();
            char c;
            DWORD read = 0;
            while (true)
            {
                if (ReadFile(h, &c, 1, &read, nullptr) && read > 0)
                {
                    if (c == '\n')
                        break;
                    if (c != '\r')
                        line += c;
                }
                else
                {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
                    if (elapsed >= timeout_ms)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            return line;
        }
    };

    #endif

    SerialCommandSensor::SerialCommandSensor(std::unique_ptr<ISerialPort> p,
                                            const std::string &device_path,
                                            int gpib_addr)
        : port(std::move(p)), device(device_path), gpib_address(gpib_addr) {}

    SerialCommandSensor::~SerialCommandSensor()
    {
        disconnect();
    }

    bool SerialCommandSensor::connect()
    {
        if (!port || !port->open(device))
            return false;

        if (gpib_address > 0)
        {
            port->write("++mode 1\n");
            port->write("++addr " + std::to_string(gpib_address) + "\n");
            port->write("++auto 1\n");
        }
        return true;
    }

    void SerialCommandSensor::disconnect()
    {
        if (port)
            port->close();
    }

    bool SerialCommandSensor::is_connected() const
    {
        return port && port->is_open();
    }

    Lakeshore336Sensor::Lakeshore336Sensor(std::unique_ptr<ISerialPort> p,
                                        const std::string &device_path,
                                        char input_channel,
                                        int gpib_addr)
        : SerialCommandSensor(std::move(p), device_path, gpib_addr), channel(input_channel) {}

    const char *Lakeshore336Sensor::name() const
    {
        return "Lakeshore336";
    }

    double Lakeshore336Sensor::read_kelvin()
    {
        if (!is_connected() && !connect())
            throw std::runtime_error("[Lakeshore336] Failed to open serial device: " + device);

        std::string cmd = std::string("KRDG? ") + channel + "\n";
        if (port->write(cmd) < 0)
            throw std::runtime_error("[Lakeshore336] Serial write failed on " + device);

        std::string reply = port->read_line();
        try
        {
            return std::stod(reply);
        }
        catch (const std::exception &)
        {
            throw std::runtime_error("[Lakeshore336] Malformed temperature reply: '" + reply + "'");
        }
    }

    namespace
    {
        double parse_oxford_temperature(const std::string &reply)
        {
            size_t colon = reply.rfind(':');
            std::string token = (colon == std::string::npos) ? reply : reply.substr(colon + 1);

            bool millikelvin = token.find("mK") != std::string::npos;

            std::string num;
            for (char c : token)
            {
                if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E')
                    num += c;
            }
            if (num.empty())
                throw std::runtime_error("[OxfordMercury] Malformed temperature reply: '" + reply + "'");

            try
            {
                double v = std::stod(num);
                return millikelvin ? v / 1000.0 : v;
            }
            catch (const std::exception &)
            {
                throw std::runtime_error("[OxfordMercury] Malformed temperature reply: '" + reply + "'");
            }
        }
    }

    OxfordMercurySensor::OxfordMercurySensor(std::unique_ptr<ISerialPort> p,
                                            const std::string &device_path,
                                            const std::string &temp_read_cmd,
                                            int gpib_addr)
        : SerialCommandSensor(std::move(p), device_path, gpib_addr), read_cmd(temp_read_cmd) {}

    const char *OxfordMercurySensor::name() const
    {
        return "OxfordMercury";
    }

    double OxfordMercurySensor::read_kelvin()
    {
        if (!is_connected() && !connect())
            throw std::runtime_error("[OxfordMercury] Failed to open serial device: " + device);

        std::string cmd = read_cmd;
        if (cmd.empty() || cmd.back() != '\n')
            cmd += '\n';

        if (port->write(cmd) < 0)
            throw std::runtime_error("[OxfordMercury] Serial write failed on " + device);

        return parse_oxford_temperature(port->read_line());
    }

    std::unique_ptr<Lakeshore336Sensor> make_lakeshore336_sensor(
        const std::string &device_path, char channel, int gpib_addr)
    {
    #ifdef _WIN32
        auto port = std::make_unique<WinSerialPort>();
    #else
        auto port = std::make_unique<PosixSerialPort>();
    #endif
        return std::make_unique<Lakeshore336Sensor>(std::move(port), device_path, channel, gpib_addr);
    }

    std::unique_ptr<OxfordMercurySensor> make_oxford_mercuryitc_sensor(
        const std::string &device_path, int thermometer_id, int gpib_addr)
    {
    #ifdef _WIN32
        auto port = std::make_unique<WinSerialPort>();
    #else
        auto port = std::make_unique<PosixSerialPort>();
    #endif
        std::string cmd = "READ:DEV:MB1.T" + std::to_string(thermometer_id) + ":TEMP:SIG:TEMP";
        return std::make_unique<OxfordMercurySensor>(std::move(port), device_path, cmd, gpib_addr);
    }
}
