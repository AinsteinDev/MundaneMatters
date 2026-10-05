#include "Log.h"

namespace MundaneMatters::Log
{
	void Init()
	{
		auto dir = logger::log_directory();
		if (!dir) {
			SKSE::stl::report_and_fail("MundaneMatters: SKSE log directory not found");
		}

		auto path = *dir / "MundaneMatters.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
		auto log = std::make_shared<spdlog::logger>("MundaneMatters", std::move(sink));

		log->set_level(spdlog::level::debug);
		log->flush_on(spdlog::level::debug);
		log->set_pattern("[%H:%M:%S.%e] [%l] [t%t] %v");

		spdlog::set_default_logger(std::move(log));
	}
}
