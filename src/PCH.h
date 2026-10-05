#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <format>
#include <algorithm>
#include <limits>
#include <vector>

using namespace std::literals;
using json = nlohmann::json;

namespace logger = SKSE::log;
