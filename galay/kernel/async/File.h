#ifndef GALAY_FILE_H
#define GALAY_FILE_H 

#ifndef __WIN32__ || __WIN64__
#include <unistd.h>
#include <fcntl.h>
#endif

#include "FileEvent.h"

namespace galay 
{
    class OpenFlags
    {
    public:
        OpenFlags& append() { m_flags |= O_APPEND; return *this; }
        OpenFlags& create() { m_flags |= O_CREAT; return *this; }
        OpenFlags& truncate() { m_flags |= O_TRUNC; return *this; }
        OpenFlags& read() { m_flags |= O_RDONLY; return *this; }
        OpenFlags& write() { m_flags |= O_WRONLY; return *this; }
        OpenFlags& readWrite() { m_flags |= O_RDWR; return *this; }
        OpenFlags& noBlock() { m_flags |= O_NONBLOCK; return *this; }
        OpenFlags& cloExec() { m_flags |= O_CLOEXEC; return *this; }

        int getFlags() const { return m_flags; }
    private:
        int m_flags = 0;
    };

    class FileModes 
    {
    public:
        FileModes() = default;  // Default creates mode 0644
        // Builder pattern for permission flags
        FileModes& userRead()      { m_mode |= S_IRUSR; return *this; }
        FileModes& userWrite()     { m_mode |= S_IWUSR; return *this; }
        FileModes& userExecute()   { m_mode |= S_IXUSR; return *this; }
        FileModes& groupRead()     { m_mode |= S_IRGRP; return *this; }
        FileModes& groupWrite()    { m_mode |= S_IWGRP; return *this; }
        FileModes& groupExecute()  { m_mode |= S_IXGRP; return *this; }
        FileModes& otherRead()     { m_mode |= S_IROTH; return *this; }
        FileModes& otherWrite()    { m_mode |= S_IWOTH; return *this; }
        FileModes& otherExecute()  { m_mode |= S_IXOTH; return *this; }
        // Higher-level permission presets
        FileModes& readOnly() { m_mode = 0; return userRead().groupRead().otherRead(); }
        FileModes& readWrite() { m_mode = 0; return userRead().userWrite().groupRead().otherRead(); }
        FileModes& allRead() { m_mode = S_IRUSR | S_IRGRP | S_IROTH; return *this; }
        FileModes& privateMode() { m_mode = S_IRUSR | S_IWUSR; return *this; }
        // Get the mode value
        mode_t getModes() const { return m_mode; }
    private:
        mode_t m_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  // Default: 0644
    };

    class File
    {
    public:
        ValueWrapper<bool> open(const std::string& path, OpenFlags flags, FileModes modes);
        ValueWrapper<bool> close();
    private:
        GHandle m_handle {};
    };

}


#endif