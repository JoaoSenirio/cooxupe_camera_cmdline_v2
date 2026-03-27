#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include <iostream>
#include <string>
#include <vector>

#include "app_config.h"

#define TEST_ASSERT(cond, msg)                                      \
    do {                                                            \
        if (!(cond)) {                                              \
            std::cerr << "[FAIL] " << msg << "\n";                  \
            return 1;                                               \
        }                                                           \
    } while (0)

struct TestCase {
    const char* name;
    int (*fn)();
};

int RunSuite(const char* suite_name, const std::vector<TestCase>& tests);

std::string MakeDayStamp();
std::string ResolveLogFilePath(const std::string& configured_path);
std::string ReadTextFile(const std::string& path);
bool StringContains(const std::string& haystack, const std::string& needle);
AppConfig MakeTestConfig(const std::string& log_tag);
void RemoveLogFileIfExists(const std::string& configured_path);
std::string GetArtifactsLogDir();
bool EnsureTestLogWorkingDirectory();

#endif
