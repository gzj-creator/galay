#include "Salt.h"
#include "galay/utils/Random.h"
#include <string.h>
#include <openssl/rand.h>

namespace galay::algorithm
{
std::string 
Salt::create(int SaltLenMin,int SaltLenMax)
{
    int saltlen = utils::Randomizer::randomInt(SaltLenMin,SaltLenMax);
    unsigned char* salt = new unsigned char[saltlen];
    bzero(salt,saltlen);
    RAND_bytes(salt,saltlen);
    std::string res(reinterpret_cast<char*>(salt),saltlen);
    delete[] salt;
    return res;
}
}

