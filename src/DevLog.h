#pragma once

#define DEV_LOG(fmt, ...) \
    do { DevLogWrite("+", fmt, ##__VA_ARGS__); } while (0)

#define DEV_WARN(fmt, ...) \
    do { DevLogWrite("!", fmt, ##__VA_ARGS__); } while (0)

#define DEV_ERR(fmt, ...) \
    do { DevLogWrite("-", fmt, ##__VA_ARGS__); } while (0)

void DevLogWrite(const char* level, const char* fmt, ...);
