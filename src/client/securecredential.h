// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.

#pragma once

#include <string>

/**
 * Read a single-line credential without exposing it through process arguments
 * or logs.
 *
 * On POSIX systems the file must be a regular file owned by the effective
 * user, must not be a symlink, and must have permissions exactly 0600.
 * Throws BaseException on validation or read failure.
 */
std::string readSecureCredentialFile(const std::string &path, const char *description);
