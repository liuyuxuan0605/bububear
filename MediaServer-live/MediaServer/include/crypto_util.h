#ifndef _CRYPTO_UTIL_H
#define _CRYPTO_UTIL_H
#include <string>

//生成16字节随机盐，返回32位十六进制字符串
std::string genSalt();

//对输入字符串做SHA256，返回64位十六进制字符串
std::string sha256Hex(const std::string& input);

#endif
