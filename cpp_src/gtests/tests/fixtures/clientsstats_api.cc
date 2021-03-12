#include "clientsstats_api.h"
#include "coroutine/waitgroup.h"

using reindexer::net::ev::dynamic_loop;
using reindexer::client::CoroReindexer;
using reindexer::client::CoroQueryResults;
using reindexer::coroutine::wait_group;
using reindexer::coroutine::wait_group_guard;

void ClientsStatsApi::SetUp() {}

void ClientsStatsApi::RunServerInThread(bool statEnable) {
	reindexer::fs::RmDirAll(kdbPath);
	// clang-format off
	std::string yaml =
		"storage:\n"
		"    path:" + kdbPath +"\n"
		"metrics:\n"
		"   clientsstats: " + (statEnable ? "true" : "false") + "\n"
		"logger:\n"
		"   loglevel: none\n"
		"   rpclog: \n"
		"   serverlog: \n"
		"net:\n"
		"   rpcaddr: " + kipaddress + ":" + kRPCPort + "\n"
		"   httpaddr: " + kipaddress + ":" + kClusterPort + "\n"
		"   clusteraddr: " + kipaddress + ":" + kHttpPort + "\n"
		"   security: true\n";
	// clang-format on

	auto err = server_.InitFromYAML(yaml);
	EXPECT_TRUE(err.ok()) << err.what();

	serverThread_ = std::unique_ptr<std::thread>(new std::thread([this]() {
		auto res = this->server_.Start();
		(void)res;
		assert(res == EXIT_SUCCESS);
	}));
	while (!server_.IsRunning()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void ClientsStatsApi::TearDown() {
	if (server_.IsRunning()) {
		server_.Stop();
		serverThread_->join();
	}
}

std::string ClientsStatsApi::GetConnectionString() {
	std::string ret = "cproto://" + kUserName + ":" + kPassword + "@" + kipaddress + ":" + kRPCPort + "/" + kdbName;
	return ret;
}

void ClientsStatsApi::SetProfilingFlag(bool val, const std::string& column, CoroReindexer& c) {
	reindexer::Query qup = std::move(reindexer::Query("#config").Where("type", CondEq, "profiling").Set(column, val));
	CoroQueryResults result;
	auto err = c.Update(qup, result);
	ASSERT_TRUE(err.ok()) << err.what();
}

void ClientsStatsApi::ClientLoopReconnect() {
	dynamic_loop loop;
	bool finished = false;
	loop.spawn([this, &finished, &loop] {
		while (!stop_) {
			int dt = rand() % 100;
			loop.sleep(std::chrono::milliseconds(dt));
			CoroReindexer rx;
			auto err = rx.Connect(GetConnectionString(), loop);
			ASSERT_TRUE(err.ok()) << err.what();
			CoroQueryResults result;
			err = rx.Select(reindexer::Query("#namespaces"), result);
			ASSERT_TRUE(err.ok()) << err.what();
			std::string resString;
			for (auto it = result.begin(); it != result.end(); ++it) {
				reindexer::WrSerializer sr;
				err = it.GetJSON(sr, false);
				ASSERT_TRUE(err.ok()) << err.what();
				reindexer::string_view sv = sr.Slice();
				resString += std::string(sv.data(), sv.size());
			}
		}
		finished = true;
	});
	loop.run();
	ASSERT_TRUE(finished);
}

uint32_t ClientsStatsApi::StatsTxCount(CoroReindexer& rx) {
	CoroQueryResults resultCs;
	auto err = rx.Select("SELECT * FROM #clientsstats", resultCs);
	EXPECT_TRUE(err.ok()) << err.what();
	EXPECT_EQ(resultCs.Count(), 1);
	auto it = resultCs.begin();
	reindexer::WrSerializer wrser;
	err = it.GetJSON(wrser, false);
	EXPECT_TRUE(err.ok()) << err.what();
	try {
		gason::JsonParser parser;
		gason::JsonNode clientsStats = parser.Parse(wrser.Slice());
		return clientsStats["tx_count"].As<uint32_t>();
	} catch (...) {
		assert(false);
	}
	EXPECT_TRUE(false);
	return 0;
}

void ClientsStatsApi::ClientSelectLoop(size_t coroutines) {
	dynamic_loop loop;
	bool finished = false;
	loop.spawn([this, &loop, &finished, coroutines] {
		CoroReindexer rx;
		auto err = rx.Connect(GetConnectionString(), loop);
		ASSERT_TRUE(err.ok()) << err.what();
		wait_group wg;
		wg.add(coroutines);
		for (size_t i = 0; i < coroutines; ++i) {
			loop.spawn([this, &rx, &wg] {
				wait_group_guard wgg(wg);
				while (!stop_) {
					reindexer::client::CoroQueryResults result;
					auto err = rx.Select(reindexer::Query("#clientsstats"), result);
					ASSERT_TRUE(err.ok()) << err.what();
					std::string resString;
					for (auto it = result.begin(); it != result.end(); ++it) {
						reindexer::WrSerializer sr;
						err = it.GetJSON(sr, false);
						ASSERT_TRUE(err.ok()) << err.what();
						reindexer::string_view sv = sr.Slice();
						resString += std::string(sv.data(), sv.size());
					}
				}
			});
		}
		wg.wait();
		finished = true;
	});
	loop.run();
	ASSERT_TRUE(finished);
}

void ClientsStatsApi::RunNSelectThread(size_t threads, size_t coroutines) {
	for (size_t i = 0; i < threads; i++) {
		auto thread_ = std::unique_ptr<std::thread>(new std::thread([this, coroutines]() { this->ClientSelectLoop(coroutines); }));
		clientThreads_.push_back(std::move(thread_));
	}
}

void ClientsStatsApi::RunNReconnectThread(size_t N) {
	for (size_t i = 0; i < N; i++) {
		auto thread_ = std::unique_ptr<std::thread>(new std::thread([this]() { this->ClientLoopReconnect(); }));
		reconnectThreads_.push_back(std::move(thread_));
	}
}

void ClientsStatsApi::StopThreads() {
	stop_ = true;
	for (auto& t : clientThreads_) {
		if (t->joinable()) {
			t->join();
		}
	}
	clientThreads_.clear();
	for (auto& t : reconnectThreads_) {
		if (t->joinable()) {
			t->join();
		}
	}
	reconnectThreads_.clear();
}
