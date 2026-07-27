// AICraft-Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.

#include "test.h"

#if defined(AICRAFT_AGENT_CLIENT) && !defined(_WIN32)

#include "client/securecredential.h"
#include "exceptions.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

class TestSecureCredential : public TestBase
{
public:
	TestSecureCredential() { TestManager::registerTestModule(this); }
	const char *getName() override { return "TestSecureCredential"; }

	void runTests(IGameDef *) override
	{
		TEST(testValidCredential);
		TEST(testRejectsUnsafePermissions);
		TEST(testRejectsMultipleLines);
		TEST(testRejectsSymlink);
	}

	void testValidCredential();
	void testRejectsUnsafePermissions();
	void testRejectsMultipleLines();
	void testRejectsSymlink();

private:
	void writeFile(const std::string &path, const std::string &content, mode_t mode);
};

static TestSecureCredential g_test_instance;

void TestSecureCredential::writeFile(const std::string &path,
		const std::string &content, mode_t mode)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	UASSERT(fd >= 0);
	const ssize_t written = write(fd, content.data(), content.size());
	UASSERTEQ(ssize_t, written, static_cast<ssize_t>(content.size()));
	UASSERTEQ(int, close(fd), 0);
	UASSERTEQ(int, chmod(path.c_str(), mode), 0);
}

void TestSecureCredential::testValidCredential()
{
	const std::string path = getTestTempFile();
	writeFile(path, "correct horse battery staple\n", 0600);
	UASSERTEQ(std::string, readSecureCredentialFile(path, "test"),
			"correct horse battery staple");
}

void TestSecureCredential::testRejectsUnsafePermissions()
{
	const std::string path = getTestTempFile();
	writeFile(path, "secret\n", 0644);
	EXCEPTION_CHECK(BaseException, readSecureCredentialFile(path, "test"));
}

void TestSecureCredential::testRejectsMultipleLines()
{
	const std::string path = getTestTempFile();
	writeFile(path, "first\nsecond\n", 0600);
	EXCEPTION_CHECK(BaseException, readSecureCredentialFile(path, "test"));
}

void TestSecureCredential::testRejectsSymlink()
{
	const std::string target = getTestTempFile();
	const std::string link = getTestTempFile();
	writeFile(target, "secret\n", 0600);
	UASSERTEQ(int, symlink(target.c_str(), link.c_str()), 0);
	EXCEPTION_CHECK(BaseException, readSecureCredentialFile(link, "test"));
}

#endif
