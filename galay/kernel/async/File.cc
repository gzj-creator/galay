#include "File.h"

namespace galay 
{
    ValueWrapper<bool> File::open(const std::string &path, OpenFlags flags, FileModes modes)
    {
        using namespace error;
        ValueWrapper<bool> wrapper;
        SystemError::ptr error = nullptr;
#if defined(__linux__) || defined(__APPLE__) 
        const int fd = ::open(path.c_str(), flags.getFlags(), modes.getModes());
        if( fd < 0 ) {
            error = std::make_shared<SystemError>(ErrorCode::CallOpenError, errno);
            makeValue(wrapper, false, error);
            return wrapper;
        }
#endif
        m_handle.fd = fd;
        HandleOption option(GHandle{fd});
        option.handleNonBlock();
        makeValue(wrapper, true, error);
        return wrapper;
    }

    ValueWrapper<bool> File::close()
    {

    }
}