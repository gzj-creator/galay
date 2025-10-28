#include "Md5.h"
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>

namespace galay::algorithm
{
    std::string 
    Md5Util::encode(std::string const& str)
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_len;
        
        const EVP_MD* md = EVP_md5();
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        
        EVP_DigestInit_ex(ctx, md, nullptr);
        EVP_DigestUpdate(ctx, str.c_str(), str.length());
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        
        EVP_MD_CTX_free(ctx);
        
        std::stringstream ss;
        for (unsigned int i = 0; i < digest_len; i++)
        {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        }
        return ss.str();
    }

    #if __cplusplus >= 201703L
    std::string 
    Md5Util::encode(std::string_view str)
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_len;
        
        const EVP_MD* md = EVP_md5();
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        
        EVP_DigestInit_ex(ctx, md, nullptr);
        EVP_DigestUpdate(ctx, str.data(), str.length());
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        
        EVP_MD_CTX_free(ctx);
        
        std::stringstream ss;
        for (unsigned int i = 0; i < digest_len; i++)
        {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        }
        return ss.str();
    }
    #endif
}
