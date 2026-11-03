#include "project_files.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

int ProjectFiles_ListFolders(const char* dir, char*** names) {
	*names = nullptr;
	std::error_code          error;
	std::vector<std::string> found;
	for (fs::directory_iterator it(fs::u8path(dir), error), end; !error && it != end; it.increment(error)) {
		const std::string name = it->path().filename().u8string();
		if (!name.empty() && name[0] != '.' && it->is_directory(error))
			found.push_back(name);
	}
	if (error)
		return -1;
	std::sort(found.begin(), found.end());
	char** list = static_cast<char**>(std::calloc(found.size() ? found.size() : 1, sizeof *list));
	if (!list)
		return -1;
	for (size_t i = 0; i < found.size(); ++i) {
		list[i] = static_cast<char*>(std::malloc(found[i].size() + 1));
		if (!list[i]) {
			ProjectFiles_FreeNames(list, static_cast<int>(i));
			return -1;
		}
		std::memcpy(list[i], found[i].c_str(), found[i].size() + 1);
	}
	*names = list;
	return static_cast<int>(found.size());
}

void ProjectFiles_FreeNames(char** names, int count) {
	for (int i = 0; names && i < count; ++i)
		std::free(names[i]);
	std::free(names);
}

bool ProjectFiles_IsFile(const char* path) {
	std::error_code error;
	return fs::is_regular_file(fs::u8path(path), error);
}

bool ProjectFiles_IsFolder(const char* path) {
	std::error_code error;
	return fs::is_directory(fs::u8path(path), error);
}

bool ProjectFiles_MakeFolders(const char* path) {
	std::error_code error;
	fs::create_directories(fs::u8path(path), error);
	return ProjectFiles_IsFolder(path);
}

bool ProjectFiles_CopyFile(const char* from, const char* to) {
	std::error_code error;
	fs::copy_file(fs::u8path(from), fs::u8path(to), fs::copy_options::overwrite_existing, error);
	return !error;
}

int ProjectFiles_HardwareThreads(void) {
	const unsigned threads = std::thread::hardware_concurrency();
	return threads ? static_cast<int>(threads) : 1;
}

void ProjectFiles_RunTasks(int count, int jobs, void (*task)(void* context, int index), void* context) {
	std::atomic<int> next(0);
	auto             work = [&]() {
		for (int index; (index = next.fetch_add(1)) < count;)
			task(context, index);
	};
	std::vector<std::thread> threads;
	for (int i = 1; i < jobs && i < count; ++i)
		threads.emplace_back(work);
	work();
	for (std::thread& thread : threads)
		thread.join();
}
