#include "SerialSource.h"

#if JUCE_MAC || JUCE_LINUX

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>

namespace bateria
{

namespace
{
    // Mapeia baud rate inteiro para a constante speed_t do termios.
    speed_t toSpeedT (int baud) noexcept
    {
        switch (baud)
        {
            case 9600:   return B9600;
            case 19200:  return B19200;
            case 38400:  return B38400;
            case 57600:  return B57600;
            case 115200: return B115200;
            case 230400: return B230400;
            default:     return B115200;
        }
    }
}

bool PosixSerialSource::open()
{
    if (fd >= 0)
        return true;

    // O_RDWR: leitura (MIDI do Arduino) + escrita (config pro Arduino).
    // O_NONBLOCK garante que open() não trave aguardando DCD.
    fd = ::open (path.toRawUTF8(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return false;

    termios tty {};
    if (::tcgetattr (fd, &tty) != 0)
    {
        close();
        return false;
    }

    const speed_t spd = toSpeedT (baud);
    ::cfsetispeed (&tty, spd);
    ::cfsetospeed (&tty, spd);

    // 8N1, sem controle de fluxo, modo RAW.
    tty.c_cflag &= ~static_cast<tcflag_t> (PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= static_cast<tcflag_t> (CS8 | CREAD | CLOCAL);
    tty.c_lflag &= ~static_cast<tcflag_t> (ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~static_cast<tcflag_t> (IXON | IXOFF | IXANY | INLCR | ICRNL);
    tty.c_oflag &= ~static_cast<tcflag_t> (OPOST);

    // Leitura não bloqueante com timeout curto: retorna assim que houver
    // qualquer byte, ou após 100ms, permitindo checar threadShouldExit().
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1; // décimos de segundo

    if (::tcsetattr (fd, TCSANOW, &tty) != 0)
    {
        close();
        return false;
    }

    // Tira o O_NONBLOCK agora que VMIN/VTIME controlam o bloqueio da read().
    const int flags = ::fcntl (fd, F_GETFL, 0);
    ::fcntl (fd, F_SETFL, flags & ~O_NONBLOCK);

    return true;
}

void PosixSerialSource::close()
{
    if (fd >= 0)
    {
        ::close (fd);
        fd = -1;
    }
}

int PosixSerialSource::read (char* dst, int maxBytes)
{
    if (fd < 0)
        return -1;

    const ssize_t n = ::read (fd, dst, static_cast<size_t> (maxBytes));

    if (n < 0)
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;

    return static_cast<int> (n);
}

int PosixSerialSource::write (const char* data, int len)
{
    if (fd < 0)
        return -1;

    const ssize_t n = ::write (fd, data, static_cast<size_t> (len));

    if (n < 0)
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;

    return static_cast<int> (n);
}

} // namespace bateria

#endif // JUCE_MAC || JUCE_LINUX
