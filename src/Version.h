#pragma once

#define DT_VERSION_MAJOR 1
#define DT_VERSION_MINOR 1
#define DT_VERSION_PATCH 0
#define DT_VERSION_BUILD 0

#define DT_VERSION_FILE DT_VERSION_MAJOR,DT_VERSION_MINOR,DT_VERSION_PATCH,DT_VERSION_BUILD
#define DT_VERSION_STRING "1.1.0"

#ifdef __cplusplus
namespace version {
inline constexpr wchar_t APP_VERSION[] = L"1.1.0";
inline constexpr int PROTOCOL_VERSION = 2;
inline constexpr wchar_t PROTOCOL_MAGIC[] = L"DirectTransfer";
}
#endif
