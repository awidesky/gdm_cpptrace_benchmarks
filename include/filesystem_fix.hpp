// Workaround for libc++ bug: iomanip uses ios_base without std:: prefix
// This header must be included BEFORE any header that includes <filesystem>
// or <iomanip> to ensure ios_base is visible in the global namespace.

#ifndef FILESYSTEM_FIX_HPP
#define FILESYSTEM_FIX_HPP

// Include ios first to get ios_base declared in std namespace
#include <ios>

// Bring ios_base into global namespace so that iomanip (included by filesystem)
// can find it without std:: prefix
using std::ios_base;

#endif // FILESYSTEM_FIX_HPP