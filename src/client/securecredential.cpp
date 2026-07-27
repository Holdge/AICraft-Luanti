// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.

#include "securecredential.h"

#include "exceptions.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef _WIN32

std::string readSecureCredentialFile(const std::string &, const char *)
{
	throw BaseException("Secure credential files require POSIX 0600 permissions");
}

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr size_t MAX_CREDENTIAL_SIZE = 4096;

class ScopedFd
{
public:
	explicit ScopedFd(int fd) : m_fd(fd) {}
	~ScopedFd()
	{
		if (m_fd >= 0)
			close(m_fd);
	}

	ScopedFd(const ScopedFd &) = delete;
	ScopedFd &operator=(const ScopedFd &) = delete;

	int get() const { return m_fd; }

private:
	int m_fd;
};

std::string credentialError(const char *description, const std::string &path,
		const char *reason)
{
	return std::string("Invalid ") + description + " file \"" + path + "\": " + reason;
}

} // namespace

std::string readSecureCredentialFile(const std::string &path, const char *description)
{
	if (path.empty())
		throw BaseException(credentialError(description, path, "path is empty"));

	int flags = O_RDONLY;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	const int raw_fd = open(path.c_str(), flags);
	if (raw_fd < 0) {
		throw BaseException(credentialError(description, path,
				std::strerror(errno)));
	}
	ScopedFd fd(raw_fd);

	struct stat info {};
	if (fstat(fd.get(), &info) != 0) {
		throw BaseException(credentialError(description, path,
				std::strerror(errno)));
	}
	if (!S_ISREG(info.st_mode))
		throw BaseException(credentialError(description, path, "not a regular file"));
	if (info.st_uid != geteuid())
		throw BaseException(credentialError(description, path, "not owned by the current user"));
	if ((info.st_mode & 0777) != 0600)
		throw BaseException(credentialError(description, path, "permissions must be exactly 0600"));
	if (info.st_size > static_cast<off_t>(MAX_CREDENTIAL_SIZE))
		throw BaseException(credentialError(description, path, "credential exceeds 4096 bytes"));

	std::string credential;
	char buffer[512];
	while (true) {
		const ssize_t length = read(fd.get(), buffer, sizeof(buffer));
		if (length > 0) {
			credential.append(buffer, static_cast<size_t>(length));
			if (credential.size() > MAX_CREDENTIAL_SIZE) {
				std::fill(credential.begin(), credential.end(), '\0');
				throw BaseException(credentialError(description, path,
						"credential exceeds 4096 bytes"));
			}
			continue;
		}
		if (length == 0)
			break;
		if (errno == EINTR)
			continue;
		std::fill(credential.begin(), credential.end(), '\0');
		throw BaseException(credentialError(description, path,
				std::strerror(errno)));
	}

	if (!credential.empty() && credential.back() == '\n') {
		credential.pop_back();
		if (!credential.empty() && credential.back() == '\r')
			credential.pop_back();
	}
	if (credential.empty()) {
		throw BaseException(credentialError(description, path,
				"credential must not be empty"));
	}
	if (credential.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
		std::fill(credential.begin(), credential.end(), '\0');
		throw BaseException(credentialError(description, path,
				"credential must contain exactly one line"));
	}

	return credential;
}

#endif
