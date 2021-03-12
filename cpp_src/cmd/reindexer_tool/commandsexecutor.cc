#include "commandsexecutor.h"
#include <iomanip>
#include "client/cororeindexer.h"
#include "core/cjson/jsonbuilder.h"
#include "core/reindexer.h"
#include "coroutine/waitgroup.h"
#include "executorscommand.h"
#include "tableviewscroller.h"
#include "tools/fsops.h"
#include "tools/jsontools.h"

namespace reindexer_tool {

using reindexer::iequals;
using reindexer::WrSerializer;
using reindexer::NamespaceDef;
using reindexer::JsonBuilder;
using reindexer::Query;

const string kConfigFile = "rxtool_settings.txt";

const string kVariableOutput = "output";
const string kOutputModeJson = "json";
const string kOutputModeTable = "table";
const string kOutputModePretty = "pretty";
const string kOutputModePrettyCollapsed = "collapsed";
const string kBenchNamespace = "rxtool_bench";
const string kBenchIndex = "id";

constexpr int kSingleThreadCoroCount = 200;
constexpr int kBenchItemsCount = 10000;
constexpr int kBenchDefaultTime = 5;
constexpr size_t k64KStack = 64 * 1024;
constexpr size_t k24KStack = 24 * 1024;
constexpr size_t k8KStack = 8 * 1024;

template <>
template <typename... Args>
Error CommandsExecutor<reindexer::Reindexer>::Run(const std::string& dsn, const Args&... args) {
	return runImpl(dsn, args...);
}

template <>
template <typename... Args>
Error CommandsExecutor<reindexer::client::CoroReindexer>::Run(const std::string& dsn, const Args&... args) {
	return runImpl(dsn, std::ref(loop_), args...);
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::GetSuggestions(const std::string& input, std::vector<std::string>& suggestions) {
	OutParamCommand<std::vector<std::string>> cmd(
		[this, &input](std::vector<std::string>& suggestions) {
			getSuggestions(input, suggestions);
			return errOK;
		},
		suggestions);
	execCommand(cmd);
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::Stop() {
	GenericCommand cmd([this] { return stop(true); });
	auto err = execCommand(cmd);
	if (err.ok() && executorThr_.joinable()) {
		executorThr_.join();
	}
	return err;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::Process(const string& command) {
	GenericCommand cmd([this, &command] { return processImpl(command); });
	return execCommand(cmd);
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::FromFile(std::istream& in) {
	GenericCommand cmd([this, &in] { return fromFileImpl(in); });
	return execCommand(cmd);
}

template <typename DBInterface>
typename CommandsExecutor<DBInterface>::Status CommandsExecutor<DBInterface>::getStatus() {
	std::lock_guard<std::mutex> lck(mtx_);
	return status_;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::fromFileImpl(std::istream& in) {
	using reindexer::coroutine::wait_group;
	using reindexer::coroutine::wait_group_guard;

	Error lastErr;
	reindexer::coroutine::channel<std::string> cmdCh(500);
	auto handleResultFn = [this, &lastErr](Error err) {
		try {
			if (!err.ok()) {
				if (err.code() == errCanceled || !db().Status().ok()) {
					if (lastErr.ok()) {
						lastErr = err;
						std::cerr << "ERROR: " << err.what() << std::endl;
					}
					return false;
				}
				lastErr = err;
				std::cerr << "ERROR: " << err.what() << std::endl;
			}
		} catch (...) {
			std::cout << "exc";
		}

		return true;
	};
	auto workerFn = [this, &cmdCh](std::function<bool(Error)> handleResult, wait_group& wg) {
		wait_group_guard wgg(wg);
		for (;;) {
			auto cmdp = cmdCh.pop();
			if (cmdp.second) {
				auto err = processImpl(cmdp.first);
				if (!handleResult(err)) {
					if (cmdCh.opened()) {
						cmdCh.close();
					}
					return;
				}
			} else {
				return;
			}
		}
	};

	wait_group wg;
	wg.add(kSingleThreadCoroCount);
	for (size_t i = 0; i < kSingleThreadCoroCount; ++i) {
		loop_.spawn(std::bind(workerFn, handleResultFn, std::ref(wg)), k64KStack);
	}

	std::string line;
	while (std::getline(in, line)) {
		if (reindexer::checkIfStartsWith("\\upsert ", line) || reindexer::checkIfStartsWith("\\delete ", line)) {
			try {
				cmdCh.push(line);
			} catch (std::exception&) {
				break;
			}
		} else {
			auto err = processImpl(line);
			if (!handleResultFn(err)) {
				break;
			}
		}
	}
	cmdCh.close();
	wg.wait();

	return lastErr;
}

template <typename DBInterface>
reindexer::Error CommandsExecutor<DBInterface>::execCommand(IExecutorsCommand& cmd) {
	std::unique_lock<std::mutex> lck_(mtx_);
	curCmd_ = &cmd;
	cmdAsync_.send();
	condVar_.wait(lck_, [&cmd] { return cmd.IsExecuted(); });
	return cmd.Status();
}

template <typename DBInterface>
template <typename... Args>
Error CommandsExecutor<DBInterface>::runImpl(const string& dsn, Args&&... args) {
	using reindexer::net::ev::sig;
	assert(!executorThr_.joinable());

	auto fn = [this](const string& dsn, Args&&... args) {
		sig sint;
		sint.set(loop_);
		sint.set([this](sig&) { cancelCtx_.Cancel(); });
		sint.start(SIGINT);

		cmdAsync_.set(loop_);
		cmdAsync_.set([this](reindexer::net::ev::async&) {
			loop_.spawn([this] {
				std::unique_lock<std::mutex> lck(mtx_);
				if (curCmd_) {
					auto cmd = curCmd_;
					curCmd_ = nullptr;
					lck.unlock();
					loop_.spawn(
						[this, cmd] {
							cmd->Execute();
							std::unique_lock<std::mutex> lck(mtx_);
							condVar_.notify_all();
						},
						k64KStack);
				}
			});
		});
		cmdAsync_.start();

		auto fn = [this](const string& dsn, Args&&... args) {
			string outputMode;
			if (reindexer::fs::ReadFile(reindexer::fs::JoinPath(reindexer::fs::GetHomeDir(), kConfigFile), outputMode) > 0) {
				gason::JsonParser jsonParser;
				gason::JsonNode value = jsonParser.Parse(reindexer::giftStr(outputMode));
				for (auto node : value) {
					WrSerializer ser;
					reindexer::jsonValueToString(node.value, ser, 0, 0, false);
					variables_[kVariableOutput] = string(ser.Slice());
				}
			}
			if (variables_.empty()) {
				variables_[kVariableOutput] = kOutputModeJson;
			}
			Error err;
			if (!uri_.parse(dsn)) {
				err = Error(errNotValid, "Cannot connect to DB: Not a valid uri");
			}
			if (err.ok()) err = db().Connect(dsn, std::forward<Args>(args)...);
			if (err.ok()) {
				loop_.spawn(
					[this] {
						// This coroutine should prevent loop from stopping for core::Reindexer
						stopCh_.pop();
					},
					k8KStack);
			}
			std::lock_guard<std::mutex> lck(mtx_);
			status_.running = err.ok();
			status_.err = std::move(err);
		};
		loop_.spawn(std::bind(fn, std::cref(dsn), std::forward<Args>(args)...));

		loop_.run();
	};

	status_ = Status();
	executorThr_ = std::thread(std::bind(fn, std::cref(dsn), std::forward<Args>(args)...));
	auto status = getStatus();
	while (!status.running && status.err.ok()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		status = getStatus();
	}
	if (!status.err.ok()) {
		executorThr_.join();
		return status.err;
	}

	auto err = output_.Status();
	if (!err.ok()) {
		std::cerr << "Output error: " << err.what() << std::endl;
	}
	return err;
}

template <typename DBInterface>
string CommandsExecutor<DBInterface>::getCurrentDsn(bool withPath) const {
	string dsn(uri_.scheme() + "://");
	if (!uri_.password().empty() && !uri_.username().empty()) {
		dsn += uri_.username() + ":" + uri_.password() + "@";
	}
	dsn += uri_.hostname() + ":" + uri_.port() + (withPath ? uri_.path() : "/");
	return dsn;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::queryResultsToJson(ostream& o, const typename DBInterface::QueryResultsT& r, bool isWALQuery,
														bool fstream) {
	if (cancelCtx_.IsCancelled()) return errOK;
	WrSerializer ser;
	size_t i = 0;
	bool scrollable = !fstream && !reindexer::isStdoutRedirected();
	reindexer::TerminalSize terminalSize;
	if (scrollable) {
		terminalSize = reindexer::getTerminalSize();
		scrollable = (int(r.Count()) > terminalSize.height);
	}
	bool prettyPrint = variables_[kVariableOutput] == kOutputModePretty;
	for (auto it : r) {
		if (cancelCtx_.IsCancelled()) break;
		if (isWALQuery) ser << '#' << int64_t(it.GetLSN()) << ' ';
		if (it.IsRaw()) {
			reindexer::WALRecord rec(it.GetRaw());
			rec.Dump(ser, [this, &r](string_view cjson) {
				auto item = db().NewItem(r.GetNamespaces()[0]);
				item.FromCJSON(cjson);
				return string(item.GetJSON());
			});
		} else {
			if (isWALQuery) ser << "WalItemUpdate ";
			Error err = it.GetJSON(ser, false);
			if (!err.ok()) return err;

			if (prettyPrint) {
				string json(ser.Slice());
				ser.Reset();
				prettyPrintJSON(reindexer::giftStr(json), ser);
			}
		}
		if ((++i != r.Count()) && !isWALQuery) ser << ',';
		ser << '\n';
		if ((ser.Len() > 0x100000) || prettyPrint || scrollable) {
			if (scrollable && (i % (terminalSize.height - 1) == 0)) {
				WaitEnterToContinue(o, terminalSize.width, [this]() -> bool { return cancelCtx_.IsCancelled(); });
			}
			o << ser.Slice();
			ser.Reset();
		}
	}
	if (!cancelCtx_.IsCancelled()) {
		o << ser.Slice();
	}
	return errOK;
}

template <>
Error CommandsExecutor<reindexer::client::CoroReindexer>::getAvailableDatabases(vector<string>& dbList) {
	return db().EnumDatabases(dbList);
}

template <>
Error CommandsExecutor<reindexer::Reindexer>::getAvailableDatabases(vector<string>&) {
	return Error();
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::addCommandsSuggestions(std::string const& cmd, std::vector<string>& suggestions) {
	LineParser parser(cmd);
	string_view token = parser.NextToken();

	if ((token == "\\upsert") || (token == "\\delete")) {
		token = parser.NextToken();
		if (parser.End()) {
			checkForNsNameMatch(token, suggestions);
		}
	} else if ((token == "\\dump") && !parser.End()) {
		while (!parser.End()) {
			checkForNsNameMatch(parser.NextToken(), suggestions);
		}
	} else if (token == "\\namespaces") {
		token = parser.NextToken();
		if (token == "drop") {
			checkForNsNameMatch(parser.NextToken(), suggestions);
		} else {
			checkForCommandNameMatch(token, {"add", "list", "drop"}, suggestions);
		}
	} else if (token == "\\meta") {
		checkForCommandNameMatch(parser.NextToken(), {"put", "list"}, suggestions);
	} else if (token == "\\set") {
		token = parser.NextToken();
		if (token == "output") {
			checkForCommandNameMatch(parser.NextToken(), {"json", "pretty", "table"}, suggestions);
		} else {
			checkForCommandNameMatch(token, {"output"}, suggestions);
		}
	} else if (token == "\\subscribe") {
		token = parser.NextToken();
		checkForCommandNameMatch(token, {"on", "off"}, suggestions);
		checkForNsNameMatch(token, suggestions);
	} else if (token == "\\databases") {
		token = parser.NextToken();
		if (token == "use") {
			vector<string> dbList;
			Error err = getAvailableDatabases(dbList);
			if (err.ok()) {
				token = parser.NextToken();
				for (const string& dbName : dbList) {
					if (token.empty() || reindexer::isBlank(token) ||
						((token.length() < dbName.length()) && reindexer::checkIfStartsWith(token, dbName))) {
						suggestions.emplace_back(dbName);
					}
				}
			}
		} else {
			checkForCommandNameMatch(token, {"use", "list"}, suggestions);
		}
	} else {
		for (const commandDefinition& cmdDef : cmds_) {
			if (token.empty() || reindexer::isBlank(token) ||
				((token.length() < cmdDef.command.length()) && reindexer::checkIfStartsWith(token, cmdDef.command))) {
				suggestions.emplace_back(cmdDef.command[0] == '\\' ? cmdDef.command.substr(1) : cmdDef.command);
			}
		}
	}
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::checkForNsNameMatch(string_view str, std::vector<string>& suggestions) {
	vector<NamespaceDef> allNsDefs;
	Error err = db().EnumNamespaces(allNsDefs, reindexer::EnumNamespacesOpts().WithClosed());
	if (!err.ok()) return;
	for (auto& ns : allNsDefs) {
		if (str.empty() || reindexer::isBlank(str) || ((str.length() < ns.name.length()) && reindexer::checkIfStartsWith(str, ns.name))) {
			suggestions.emplace_back(ns.name);
		}
	}
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::checkForCommandNameMatch(string_view str, std::initializer_list<string_view> cmds,
															 std::vector<string>& suggestions) {
	for (string_view cmd : cmds) {
		if (str.empty() || reindexer::isBlank(str) || ((str.length() < cmd.length()) && reindexer::checkIfStartsWith(str, cmd))) {
			suggestions.emplace_back(cmd);
		}
	}
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::processImpl(const std::string& command) {
	LineParser parser(command);
	auto token = parser.NextToken();

	if (!token.length() || token.substr(0, 2) == "--") return errOK;

	Error ret;
	for (auto& c : cmds_) {
		if (iequals(token, c.command)) {
			ret = (this->*(c.handler))(command);
			if (cancelCtx_.IsCancelled()) {
				ret = Error(errCanceled, "Canceled");
			}
			cancelCtx_.Reset();
			return ret;
		}
	}
	return Error(errParams, "Unknown command '%s'. Type '\\help' to list of available commands", token);
}

template <>
Error CommandsExecutor<reindexer::Reindexer>::stop(bool terminate) {
	if (terminate) {
		stopCh_.close();
	}
	return Error();
}

template <>
Error CommandsExecutor<reindexer::client::CoroReindexer>::stop(bool terminate) {
	if (terminate) {
		stopCh_.close();
	}
	return db().Stop();
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::getSuggestions(const std::string& input, std::vector<std::string>& suggestions) {
	if (!input.empty() && input[0] != '\\') db().GetSqlSuggestions(input, input.length() - 1, suggestions);
	if (suggestions.empty()) {
		addCommandsSuggestions(input, suggestions);
	}
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandSelect(const string& command) {
	typename DBInterface::QueryResultsT results(kResultsWithPayloadTypes | kResultsCJson | kResultsWithItemID | kResultsWithRaw);
	Query q;
	try {
		q.FromSQL(command);
	} catch (const Error& err) {
		return err;
	}

	auto err = db().Select(q, results);

	if (err.ok()) {
		if (results.Count()) {
			auto& outputType = variables_[kVariableOutput];
			if (outputType == kOutputModeTable) {
				auto isCanceled = [this]() -> bool { return cancelCtx_.IsCancelled(); };
				reindexer::TableViewBuilder<typename DBInterface::QueryResultsT> tableResultsBuilder(results);
				if (output_.IsCout() && !reindexer::isStdoutRedirected()) {
					TableViewScroller<typename DBInterface::QueryResultsT> resultsScroller(results, tableResultsBuilder,
																						   reindexer::getTerminalSize().height - 1);
					resultsScroller.Scroll(output_, isCanceled);
				} else {
					tableResultsBuilder.Build(output_(), isCanceled);
				}
			} else {
				output_() << "[" << std::endl;
				err = queryResultsToJson(output_(), results, q.IsWALQuery(), !output_.IsCout());
				output_() << "]" << std::endl;
			}
		}

		string explain = results.GetExplainResults();
		if (!explain.empty() && !cancelCtx_.IsCancelled()) {
			output_() << "Explain: " << std::endl;
			if (variables_[kVariableOutput] == kOutputModePretty) {
				WrSerializer ser;
				prettyPrintJSON(reindexer::giftStr(explain), ser);
				output_() << ser.Slice() << std::endl;
			} else {
				output_() << explain << std::endl;
			}
		}
		output_() << "Returned " << results.Count() << " rows";
		if (results.TotalCount()) output_() << ", total count " << results.TotalCount();
		output_() << std::endl;

		auto& aggResults = results.GetAggregationResults();
		if (aggResults.size() && !cancelCtx_.IsCancelled()) {
			output_() << "Aggregations: " << std::endl;
			for (auto& agg : aggResults) {
				switch (agg.type) {
					case AggFacet: {
						assert(!agg.fields.empty());
						reindexer::h_vector<int, 1> maxW;
						maxW.reserve(agg.fields.size());
						for (const auto& field : agg.fields) {
							maxW.push_back(field.length());
						}
						for (auto& row : agg.facets) {
							assert(row.values.size() == agg.fields.size());
							for (size_t i = 0; i < row.values.size(); ++i) {
								maxW.at(i) = std::max(maxW.at(i), int(row.values[i].length()));
							}
						}
						int rowWidth = 8 + (maxW.size() - 1) * 2;
						for (auto& mW : maxW) {
							mW += 3;
							rowWidth += mW;
						}
						for (size_t i = 0; i < agg.fields.size(); ++i) {
							if (i != 0) output_() << "| ";
							output_() << std::left << std::setw(maxW.at(i)) << agg.fields[i];
						}
						output_() << "| count" << std::endl;
						output_() << std::left << std::setw(rowWidth) << std::setfill('-') << "" << std::endl << std::setfill(' ');
						for (auto& row : agg.facets) {
							for (size_t i = 0; i < row.values.size(); ++i) {
								if (i != 0) output_() << "| ";
								output_() << std::left << std::setw(maxW.at(i)) << row.values[i];
							}
							output_() << "| " << row.count << std::endl;
						}
					} break;
					case AggDistinct:
						assert(agg.fields.size() == 1);
						output_() << "Distinct (" << agg.fields.front() << ")" << std::endl;
						for (auto& v : agg.distincts) {
							output_() << v << std::endl;
						}
						output_() << "Returned " << agg.distincts.size() << " values" << std::endl;
						break;
					default:
						assert(agg.fields.size() == 1);
						output_() << agg.aggTypeToStr(agg.type) << "(" << agg.fields.front() << ") = " << agg.value << std::endl;
				}
			}
		}
	}
	return err;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandUpsert(const string& command) {
	LineParser parser(command);
	parser.NextToken();

	string nsName = reindexer::unescapeString(parser.NextToken());

	auto item = db().NewItem(nsName);

	Error status = item.Status();
	if (!status.ok()) {
		return status;
	}

	status = item.Unsafe().FromJSON(parser.CurPtr());
	if (!status.ok()) {
		return status;
	}

	if (!parser.CurPtr().empty() && (parser.CurPtr())[0] == '[') {
		return Error(errParams, "Impossible to update entire item with array - only objects are allowed");
	}

	return db().Upsert(nsName, item);
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandUpdateSQL(const string& command) {
	typename DBInterface::QueryResultsT results;
	Query q;
	try {
		q.FromSQL(command);
	} catch (const Error& err) {
		return err;
	}

	auto err = db().Update(q, results);

	if (err.ok()) {
		output_() << "Updated " << results.Count() << " documents" << std::endl;
	}
	return err;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandDelete(const string& command) {
	LineParser parser(command);
	parser.NextToken();

	auto nsName = reindexer::unescapeString(parser.NextToken());

	auto item = db().NewItem(nsName);
	if (!item.Status().ok()) return item.Status();

	auto err = item.Unsafe().FromJSON(parser.CurPtr());
	if (!err.ok()) return err;

	return db().Delete(nsName, item);
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandDeleteSQL(const string& command) {
	typename DBInterface::QueryResultsT results;
	Query q;
	try {
		q.FromSQL(command);
	} catch (const Error& err) {
		return err;
	}
	auto err = db().Delete(q, results);

	if (err.ok()) {
		output_() << "Deleted " << results.Count() << " documents" << std::endl;
	}
	return err;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandDump(const string& command) {
	LineParser parser(command);
	parser.NextToken();

	vector<NamespaceDef> allNsDefs, doNsDefs;

	auto err = db().WithContext(&cancelCtx_).EnumNamespaces(allNsDefs, reindexer::EnumNamespacesOpts());
	if (err) return err;

	if (!parser.End()) {
		// build list of namespaces for dumped
		while (!parser.End()) {
			auto ns = parser.NextToken();
			auto nsDef = std::find_if(allNsDefs.begin(), allNsDefs.end(), [&ns](const NamespaceDef& nsDef) { return ns == nsDef.name; });
			if (nsDef != allNsDefs.end()) {
				doNsDefs.push_back(std::move(*nsDef));
				allNsDefs.erase(nsDef);
			} else {
				std::cerr << "Namespace '" << ns << "' - skipped. (not found in storage)" << std::endl;
			}
		}
	} else {
		doNsDefs = std::move(allNsDefs);
	}

	reindexer::WrSerializer wrser;

	wrser << "-- Reindexer DB backup file" << '\n';
	wrser << "-- VERSION 1.0" << '\n';

	for (auto& nsDef : doNsDefs) {
		// skip system namespaces, except #config
		if (nsDef.name.length() > 0 && nsDef.name[0] == '#' && nsDef.name != "#config") continue;

		wrser << "-- Dumping namespace '" << nsDef.name << "' ..." << '\n';

		wrser << "\\NAMESPACES ADD " << reindexer::escapeString(nsDef.name) << " ";
		nsDef.GetJSON(wrser);
		wrser << '\n';

		vector<string> meta;
		err = db().WithContext(&cancelCtx_).EnumMeta(nsDef.name, meta);
		if (err) {
			return err;
		}

		for (auto& mkey : meta) {
			string mdata;
			err = db().WithContext(&cancelCtx_).GetMeta(nsDef.name, mkey, mdata);
			if (err) {
				return err;
			}

			wrser << "\\META PUT " << reindexer::escapeString(nsDef.name) << " " << reindexer::escapeString(mkey) << " "
				  << reindexer::escapeString(mdata) << '\n';
		}

		typename DBInterface::QueryResultsT itemResults;
		err = db().WithContext(&cancelCtx_).Select(Query(nsDef.name), itemResults);

		if (!err.ok()) return err;

		for (auto it : itemResults) {
			if (!it.Status().ok()) return it.Status();
			if (cancelCtx_.IsCancelled()) {
				return Error(errCanceled, "Canceled");
			}
			wrser << "\\UPSERT " << reindexer::escapeString(nsDef.name) << ' ';
			it.GetJSON(wrser, false);
			wrser << '\n';
			if (wrser.Len() > 0x100000) {
				output_() << wrser.Slice();
				wrser.Reset();
			}
		}
	}
	output_() << wrser.Slice();

	return errOK;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandNamespaces(const string& command) {
	LineParser parser(command);
	parser.NextToken();

	string_view subCommand = parser.NextToken();

	if (iequals(subCommand, "add")) {
		auto nsName = reindexer::unescapeString(parser.NextToken());

		NamespaceDef def("");
		Error err = def.FromJSON(reindexer::giftStr(parser.CurPtr()));
		if (!err.ok()) {
			return Error(errParseJson, "Namespace structure is not valid - %s", err.what());
		}

		def.storage.DropOnFileFormatError(true);
		def.storage.CreateIfMissing(true);

		err = db().OpenNamespace(def.name);
		if (!err.ok()) {
			return err;
		}
		for (auto& idx : def.indexes) {
			err = db().AddIndex(def.name, idx);
			if (!err.ok()) {
				return err;
			}
		}
		err = db().SetSchema(def.name, def.schemaJson);
		if (!err.ok()) {
			return err;
		}
		return errOK;

	} else if (iequals(subCommand, "list")) {
		vector<NamespaceDef> allNsDefs;

		auto err = db().EnumNamespaces(allNsDefs, reindexer::EnumNamespacesOpts().WithClosed());
		for (auto& ns : allNsDefs) {
			output_() << ns.name << std::endl;
		}
		return err;

	} else if (iequals(subCommand, "drop")) {
		auto nsName = reindexer::unescapeString(parser.NextToken());
		return db().DropNamespace(nsName);
	} else if (iequals(subCommand, "truncate")) {
		auto nsName = reindexer::unescapeString(parser.NextToken());
		return db().TruncateNamespace(nsName);
	} else if (iequals(subCommand, "rename")) {
		auto nsName = reindexer::unescapeString(parser.NextToken());
		auto nsNewName = reindexer::unescapeString(parser.NextToken());
		return db().RenameNamespace(nsName, nsNewName);
	}
	return Error(errParams, "Unknown sub command '%s' of namespaces command", subCommand);
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandMeta(const string& command) {
	LineParser parser(command);
	parser.NextToken();
	string_view subCommand = parser.NextToken();

	if (iequals(subCommand, "put")) {
		string nsName = reindexer::unescapeString(parser.NextToken());
		string metaKey = reindexer::unescapeString(parser.NextToken());
		string metaData = reindexer::unescapeString(parser.NextToken());
		return db().PutMeta(nsName, metaKey, metaData);
	} else if (iequals(subCommand, "list")) {
		auto nsName = reindexer::unescapeString(parser.NextToken());
		vector<std::string> allMeta;
		auto err = db().EnumMeta(nsName, allMeta);
		for (auto& metaKey : allMeta) {
			string metaData;
			db().GetMeta(nsName, metaKey, metaData);
			output_() << metaKey << " = " << metaData << std::endl;
		}
		return err;
	}
	return Error(errParams, "Unknown sub command '%s' of meta command", subCommand);
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandHelp(const string& command) {
	LineParser parser(command);
	parser.NextToken();
	string_view subCommand = parser.NextToken();

	if (!subCommand.length()) {
		output_() << "Available commands:\n\n";
		for (auto cmd : cmds_) {
			output_() << "  " << std::left << std::setw(20) << cmd.command << "- " << cmd.description << std::endl;
		}
	} else {
		auto it = std::find_if(cmds_.begin(), cmds_.end(),
							   [&subCommand](const commandDefinition& def) { return iequals(def.command, subCommand); });

		if (it == cmds_.end()) {
			return Error(errParams, "Unknown command '%s' to help. To list of available command type '\\help'", subCommand);
		}
		output_() << it->command << " - " << it->description << ":" << std::endl << it->help << std::endl;
	}

	return errOK;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandQuit(const string&) {
	stopCh_.close();
	return errOK;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandSet(const string& command) {
	LineParser parser(command);
	parser.NextToken();

	string_view variableName = parser.NextToken();
	string_view variableValue = parser.NextToken();

	variables_[string(variableName)] = string(variableValue);

	WrSerializer wrser;
	reindexer::JsonBuilder configBuilder(wrser);
	for (auto it = variables_.begin(); it != variables_.end(); ++it) {
		configBuilder.Put(it->first, it->second);
	}
	configBuilder.End();
	reindexer::fs::WriteFile(reindexer::fs::JoinPath(reindexer::fs::GetHomeDir(), kConfigFile), wrser.Slice());

	return errOK;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandBench(const string& command) {
	LineParser parser(command);
	parser.NextToken();

	int benchTime = stoi(parser.NextToken());
	if (benchTime == 0) benchTime = kBenchDefaultTime;

	db().DropNamespace(kBenchNamespace);

	NamespaceDef nsDef(kBenchNamespace);
	nsDef.AddIndex("id", "hash", "int", IndexOpts().PK());

	auto err = db().AddNamespace(nsDef);
	if (!err.ok()) return err;

	output_() << "Seeding " << kBenchItemsCount << " documents to bench namespace..." << std::endl;
	err = seedBenchItems();
	output_() << "done." << std::endl;
	if (!err.ok()) {
		return err;
	}

	output_() << "Running " << benchTime << "s benchmark..." << std::endl;
	std::this_thread::sleep_for(std::chrono::seconds(1));

	auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(benchTime);
	std::atomic<int> count(0), errCount(0);

	auto worker = std::bind(getBenchWorkerFn(count, errCount), deadline);
	auto threads = std::unique_ptr<std::thread[]>(new std::thread[numThreads_ - 1]);
	for (int i = 0; i < numThreads_ - 1; i++) threads[i] = std::thread(worker);
	worker();
	for (int i = 0; i < numThreads_ - 1; i++) threads[i].join();

	output_() << "Done. Got " << count / benchTime << " QPS, " << errCount << " errors" << std::endl;
	return err;
}

template <typename DBInterface>
Error CommandsExecutor<DBInterface>::commandSubscribe(const string& command) {
	LineParser parser(command);
	parser.NextToken();

	reindexer::UpdatesFilters filters;
	auto token = parser.NextToken();
	if (iequals(token, "off")) {
		return db().UnsubscribeUpdates(this);
	} else if (token.empty() || iequals(token, "on")) {
		return db().SubscribeUpdates(this, filters);
	}
	std::vector<std::string> nsInSubscription;
	while (!token.empty()) {
		filters.AddFilter(token, reindexer::UpdatesFilters::Filter());
		nsInSubscription.emplace_back(token);
		token = parser.NextToken();
	}

	auto err = db().SubscribeUpdates(this, filters);
	if (!err.ok()) {
		return err;
	}
	vector<NamespaceDef> allNsDefs;
	err = db().EnumNamespaces(allNsDefs, reindexer::EnumNamespacesOpts().WithClosed());
	if (!err.ok()) {
		return err;
	}
	for (auto& ns : allNsDefs) {
		for (auto it = nsInSubscription.begin(); it != nsInSubscription.end();) {
			if (*it == ns.name) {
				it = nsInSubscription.erase(it);
			} else {
				++it;
			}
		}
	}
	if (!nsInSubscription.empty()) {
		output_() << "WARNING: You have subscribed for non-existing namespace updates: ";
		for (auto it = nsInSubscription.begin(); it != nsInSubscription.end(); ++it) {
			if (it != nsInSubscription.begin()) {
				output_() << ", ";
			}
			output_() << *it;
		}
		output_() << std::endl;
	}
	return errOK;
}

template <>
Error CommandsExecutor<reindexer::client::CoroReindexer>::commandProcessDatabases(const string& command) {
	LineParser parser(command);
	parser.NextToken();
	string_view subCommand = parser.NextToken();
	assert(uri_.scheme() == "cproto");
	if (subCommand == "list") {
		vector<string> dbList;
		Error err = getAvailableDatabases(dbList);
		if (!err.ok()) return err;
		for (const string& dbName : dbList) output_() << dbName << std::endl;
		return Error();
	} else if (subCommand == "use") {
		string currentDsn = getCurrentDsn() + std::string(parser.NextToken());
		Error err = stop(false);
		if (!err.ok()) return err;
		err = db().Connect(currentDsn, loop_);
		if (err.ok()) err = db().Status();
		if (err.ok()) output_() << "Succesfully connected to " << currentDsn << std::endl;
		return err;
	} else if (subCommand == "create") {
		auto dbName = parser.NextToken();
		string currentDsn = getCurrentDsn() + std::string(dbName);
		Error err = stop(false);
		if (!err.ok()) return err;
		output_() << "Creating database '" << dbName << "'" << std::endl;
		err = db().Connect(currentDsn, loop_, reindexer::client::ConnectOpts().CreateDBIfMissing());
		if (!err.ok()) {
			std::cerr << "Error on database '" << dbName << "' creation" << std::endl;
			return err;
		}
		std::vector<std::string> dbNames;
		err = db().EnumDatabases(dbNames);
		if (std::find(dbNames.begin(), dbNames.end(), std::string(dbName)) != dbNames.end()) {
			output_() << "Succesfully created database '" << dbName << "'" << std::endl;
		} else {
			std::cerr << "Error on database '" << dbName << "' creation" << std::endl;
		}
		return err;
	}
	return Error(errNotValid, "Invalid command");
}

template <>
Error CommandsExecutor<reindexer::Reindexer>::commandProcessDatabases(const string& command) {
	(void)command;
	return Error(errNotValid, "Database processing commands are not supported in builtin mode");
}

template <>
Error CommandsExecutor<reindexer::client::CoroReindexer>::seedBenchItems() {
	for (int i = 0; i < kBenchItemsCount; i++) {
		auto item = db().NewItem(kBenchNamespace);
		WrSerializer ser;
		JsonBuilder(ser).Put("id", i).Put("data", i);

		auto err = item.Unsafe().FromJSON(ser.Slice());
		if (!err.ok()) return err;

		err = db().Upsert(kBenchNamespace, item);
		if (!err.ok()) return err;
	}
	return errOK;
}

template <>
Error CommandsExecutor<reindexer::Reindexer>::seedBenchItems() {
	using reindexer::coroutine::wait_group;
	Error err;
	auto upsertFn = [this, &err](size_t beg, size_t end, wait_group& wg) {
		reindexer::coroutine::wait_group_guard wgg(wg);
		for (size_t i = beg; i < end; ++i) {
			auto item = db().NewItem(kBenchNamespace);
			WrSerializer ser;
			JsonBuilder(ser).Put("id", i).Put("data", i);

			auto intErr = item.Unsafe().FromJSON(ser.Slice());
			if (intErr.ok()) intErr = db().Upsert(kBenchNamespace, item);
			if (!intErr.ok()) {
				err = intErr;
				return;
			}
			if (!err.ok()) {
				return;
			}
		}
	};

	auto itemsPerCoro = kBenchItemsCount / kSingleThreadCoroCount;
	wait_group wg;
	wg.add(kSingleThreadCoroCount);
	for (int i = 0; i < kBenchItemsCount; i += itemsPerCoro) {
		loop_.spawn(std::bind(upsertFn, i, std::min(i + itemsPerCoro, kBenchItemsCount), std::ref(wg)), k24KStack);
	}
	wg.wait();
	return err;
}

template <>
std::function<void(std::chrono::system_clock::time_point)> CommandsExecutor<reindexer::client::CoroReindexer>::getBenchWorkerFn(
	std::atomic<int>& count, std::atomic<int>& errCount) {
	using reindexer::coroutine::wait_group;
	return [this, &count, &errCount](std::chrono::system_clock::time_point deadline) {
		reindexer::net::ev::dynamic_loop loop;
		loop.spawn([this, &loop, deadline, &count, &errCount] {
			reindexer::client::CoroReindexer rx;
			rx.Connect(getCurrentDsn(true), loop);
			auto selectFn = [&rx, deadline, &count, &errCount](wait_group& wg) {
				reindexer::coroutine::wait_group_guard wgg(wg);
				for (; std::chrono::system_clock::now() < deadline; ++count) {
					Query q(kBenchNamespace);
					q.Where(kBenchIndex, CondEq, count % kBenchItemsCount);
					reindexer::client::CoroReindexer::QueryResultsT results;
					auto err = rx.Select(q, results);
					if (!err.ok()) errCount++;
				}
			};
			wait_group wg;
			wg.add(kSingleThreadCoroCount);
			for (int i = 0; i < kSingleThreadCoroCount; ++i) {
				loop.spawn(std::bind(selectFn, std::ref(wg)), k24KStack);
			}
			wg.wait();
			rx.Stop();
		});

		loop.run();
	};
}

template <>
std::function<void(std::chrono::system_clock::time_point)> CommandsExecutor<reindexer::Reindexer>::getBenchWorkerFn(
	std::atomic<int>& count, std::atomic<int>& errCount) {
	return [this, &count, &errCount](std::chrono::system_clock::time_point deadline) {
		for (; (count % 1000) || std::chrono::system_clock::now() < deadline; count++) {
			Query q(kBenchNamespace);
			q.Where(kBenchIndex, CondEq, count % kBenchItemsCount);
			auto results = new typename reindexer::Reindexer::QueryResultsT;

			db().WithCompletion([results, &errCount](const Error& err) {
					delete results;
					if (!err.ok()) errCount++;
				})
				.Select(q, *results);
		}
	};
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::OnWALUpdate(reindexer::LSNPair LSNs, string_view nsName, const reindexer::WALRecord& wrec) {
	WrSerializer ser;
	ser << "# LSN " << int64_t(LSNs.upstreamLSN_) << " originLSN " << int64_t(LSNs.originLSN_) << nsName << " ";
	wrec.Dump(ser, [this, nsName](string_view cjson) {
		auto item = db().NewItem(nsName);
		item.FromCJSON(cjson);
		return string(item.GetJSON());
	});
	output_() << ser.Slice() << std::endl;
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::OnConnectionState(const Error& err) {
	if (err.ok())
		output_() << "[OnConnectionState] connected" << std::endl;
	else
		output_() << "[OnConnectionState] closed, reason: " << err.what() << std::endl;
}

template <typename DBInterface>
void CommandsExecutor<DBInterface>::OnUpdatesLost(string_view nsName) {
	output_() << "[OnUpdatesLost] " << nsName << std::endl;
}

template class CommandsExecutor<reindexer::client::CoroReindexer>;
template class CommandsExecutor<reindexer::Reindexer>;
template Error CommandsExecutor<reindexer::Reindexer>::Run(const string& dsn, const ConnectOpts& opts);
template Error CommandsExecutor<reindexer::client::CoroReindexer>::Run(const string& dsn, const reindexer::client::ConnectOpts& opts);

}  // namespace reindexer_tool
