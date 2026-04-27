#include "util/process.hpp"

#include "util/log.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <limits.h>
#include <vector>

namespace chiux::util {

namespace {

bool has_shell_meta(const std::string& command) {
  return command.find_first_of(" \t\n'\"$`;&|<>*?()[]{}\\") != std::string::npos;
}

std::vector<std::string> split_words(const std::string& command) {
  std::vector<std::string> words;
  std::string current;
  bool in_single = false;
  bool in_double = false;
  for (std::size_t i = 0; i < command.size(); ++i) {
    const char c = command[i];
    if (c == '\'' && !in_double) {
      in_single = !in_single;
      continue;
    }
    if (c == '"' && !in_single) {
      in_double = !in_double;
      continue;
    }
    if (!in_single && !in_double && std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    words.push_back(current);
  }
  return words;
}

std::string find_companion_executable(const std::string& name) {
  if (name.empty() || name.find('/') != std::string::npos) {
    return {};
  }
  std::array<char, PATH_MAX> buffer{};
  const ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (len <= 0) {
    return {};
  }
  buffer[static_cast<std::size_t>(len)] = '\0';
  std::filesystem::path exe_path(buffer.data());
  if (!exe_path.has_parent_path()) {
    return {};
  }
  const std::filesystem::path candidate = exe_path.parent_path() / name;
  if (access(candidate.c_str(), X_OK) == 0) {
    return candidate.string();
  }
  return {};
}

}

int spawn_command(const std::string& command) {
  const pid_t pid = fork();
  if (pid < 0) {
    chiux::log::error(std::string("fork failed: ") + std::strerror(errno));
    return -1;
  }
  if (pid == 0) {
    setsid();
    if (!has_shell_meta(command)) {
      const std::vector<std::string> words = split_words(command);
      if (!words.empty()) {
        const std::string companion = find_companion_executable(words.front());
        std::vector<char*> argv;
        argv.reserve(words.size() + 1);
        for (const auto& word : words) {
          argv.push_back(const_cast<char*>(word.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        if (!companion.empty()) {
          argv.front() = const_cast<char*>(companion.c_str());
          execv(argv.front(), argv.data());
        }
      }
    }
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    chiux::log::error(std::string("exec failed for command: ") + command + ": " + std::strerror(errno));
    _exit(127);
  }
  chiux::log::info(std::string("spawned: ") + command);
  return static_cast<int>(pid);
}

void reap_children() {
  while (true) {
    int status = 0;
    const pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0) {
      break;
    }
    if (WIFEXITED(status)) {
      chiux::log::info("child exited");
    } else if (WIFSIGNALED(status)) {
      chiux::log::warn("child terminated by signal");
    }
  }
}

}
