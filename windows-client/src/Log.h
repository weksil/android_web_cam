#pragma once

namespace awc {

/** Opens <folder>\<name> for appending. folder is created if missing. */
void logInit(const wchar_t* folder, const wchar_t* name);
void logf(const char* format, ...);
void logClose();

} // namespace awc
