#ifndef __GALAY_SALT_H__
#define __GALAY_SALT_H__

#include <string>

namespace galay::algorithm
{
    /**
     * @brief 盐值生成工具类
     * @details 用于生成随机盐值，常用于密码哈希等安全场景
     */
    class Salt{
    public:
        /**
         * @brief 创建指定长度范围的随机盐值
         * @param SaltLenMin 盐值最小长度
         * @param SaltLenMax 盐值最大长度
         * @return 生成的随机盐值字符串，长度在[SaltLenMin, SaltLenMax]之间
         */
        static std::string create(int SaltLenMin,int SaltLenMax);

    };
    
}


#endif