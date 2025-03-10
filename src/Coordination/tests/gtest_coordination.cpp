#include <chrono>
#include <gtest/gtest.h>
#include "Common/ZooKeeper/IKeeper.h"

#include "Core/Defines.h"
#include "config.h"

#if USE_NURAFT
#include <filesystem>
#include <thread>
#include <Coordination/Changelog.h>
#include <Coordination/InMemoryLogStore.h>
#include <Coordination/KeeperContext.h>
#include <Coordination/KeeperConstants.h>
#include <Coordination/KeeperFeatureFlags.h>
#include <Coordination/KeeperLogStore.h>
#include <Coordination/KeeperSnapshotManager.h>
#include <Coordination/KeeperStateMachine.h>
#include <Coordination/KeeperStateManager.h>
#include <Coordination/KeeperStorage.h>
#include <Coordination/LoggerWrapper.h>
#include <Coordination/ReadBufferFromNuraftBuffer.h>
#include <Coordination/SummingStateMachine.h>
#include <Coordination/WriteBufferFromNuraftBuffer.h>
#include <Coordination/pathUtils.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <libnuraft/nuraft.hxx>
#include <Poco/ConsoleChannel.h>
#include <Poco/Logger.h>
#include <Common/Exception.h>
#include <Common/SipHash.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Common/ZooKeeper/ZooKeeperIO.h>
#include <Common/logger_useful.h>

#include <Disks/DiskLocal.h>

#include <Coordination/SnapshotableHashTable.h>

namespace fs = std::filesystem;
struct ChangelogDirTest
{
    std::string path;
    bool drop;
    explicit ChangelogDirTest(std::string path_, bool drop_ = true) : path(path_), drop(drop_)
    {
        if (fs::exists(path))
        {
            EXPECT_TRUE(false) << "Path " << path << " already exists, remove it to run test";
        }
        fs::create_directory(path);
    }

    ~ChangelogDirTest()
    {
        if (fs::exists(path) && drop)
            fs::remove_all(path);
    }
};

struct CompressionParam
{
    bool enable_compression;
    std::string extension;
};

class CoordinationTest : public ::testing::TestWithParam<CompressionParam>
{
protected:
    DB::KeeperContextPtr keeper_context = std::make_shared<DB::KeeperContext>(true);
    Poco::Logger * log{&Poco::Logger::get("CoordinationTest")};

    void SetUp() override
    {
        Poco::AutoPtr<Poco::ConsoleChannel> channel(new Poco::ConsoleChannel(std::cerr));
        Poco::Logger::root().setChannel(channel);
        Poco::Logger::root().setLevel("trace");

        keeper_context->local_logs_preprocessed = true;
    }

    void setLogDirectory(const std::string & path) { keeper_context->setLogDisk(std::make_shared<DB::DiskLocal>("LogDisk", path)); }

    void setSnapshotDirectory(const std::string & path)
    {
        keeper_context->setSnapshotDisk(std::make_shared<DB::DiskLocal>("SnapshotDisk", path));
    }

    void setStateFileDirectory(const std::string & path)
    {
        keeper_context->setStateFileDisk(std::make_shared<DB::DiskLocal>("StateFile", path));
    }
};

TEST_P(CoordinationTest, RaftServerConfigParse)
{
    auto parse = Coordination::RaftServerConfig::parse;
    using Cfg = std::optional<DB::RaftServerConfig>;

    EXPECT_EQ(parse(""), std::nullopt);
    EXPECT_EQ(parse("="), std::nullopt);
    EXPECT_EQ(parse("=;"), std::nullopt);
    EXPECT_EQ(parse("=;;"), std::nullopt);
    EXPECT_EQ(parse("=:80"), std::nullopt);
    EXPECT_EQ(parse("server."), std::nullopt);
    EXPECT_EQ(parse("server.=:80"), std::nullopt);
    EXPECT_EQ(parse("server.-5=1:2"), std::nullopt);
    EXPECT_EQ(parse("server.1=host;-123"), std::nullopt);
    EXPECT_EQ(parse("server.1=host:999"), (Cfg{{1, "host:999"}}));
    EXPECT_EQ(parse("server.1=host:999;learner"), (Cfg{{1, "host:999", true}}));
    EXPECT_EQ(parse("server.1=host:999;participant"), (Cfg{{1, "host:999", false}}));
    EXPECT_EQ(parse("server.1=host:999;learner;25"), (Cfg{{1, "host:999", true, 25}}));

    EXPECT_EQ(parse("server.1=127.0.0.1:80"), (Cfg{{1, "127.0.0.1:80"}}));
    EXPECT_EQ(
        parse("server.1=2001:0db8:85a3:0000:0000:8a2e:0370:7334:80"),
        (Cfg{{1, "2001:0db8:85a3:0000:0000:8a2e:0370:7334:80"}}));
}

TEST_P(CoordinationTest, RaftServerClusterConfigParse)
{
    auto parse = Coordination::parseRaftServers;
    using Cfg = DB::RaftServerConfig;
    using Servers = DB::RaftServers;

    EXPECT_EQ(parse(""), Servers{});
    EXPECT_EQ(parse(","), Servers{});
    EXPECT_EQ(parse("1,2"), Servers{});
    EXPECT_EQ(parse("server.1=host:80,server.1=host2:80"), Servers{});
    EXPECT_EQ(parse("server.1=host:80,server.2=host:80"), Servers{});
    EXPECT_EQ(
        parse("server.1=host:80,server.2=host:81"),
        (Servers{Cfg{1, "host:80"}, Cfg{2, "host:81"}}));
}

TEST_P(CoordinationTest, BuildTest)
{
    DB::InMemoryLogStore store;
    DB::SummingStateMachine machine;
    EXPECT_EQ(1, 1);
}

TEST_P(CoordinationTest, BufferSerde)
{
    Coordination::ZooKeeperRequestPtr request = Coordination::ZooKeeperRequestFactory::instance().get(Coordination::OpNum::Get);
    request->xid = 3;
    dynamic_cast<Coordination::ZooKeeperGetRequest &>(*request).path = "/path/value";

    DB::WriteBufferFromNuraftBuffer wbuf;
    request->write(wbuf);
    auto nuraft_buffer = wbuf.getBuffer();
    EXPECT_EQ(nuraft_buffer->size(), 28);

    DB::ReadBufferFromNuraftBuffer rbuf(nuraft_buffer);

    int32_t length;
    Coordination::read(length, rbuf);
    EXPECT_EQ(length + sizeof(length), nuraft_buffer->size());

    int32_t xid;
    Coordination::read(xid, rbuf);
    EXPECT_EQ(xid, request->xid);

    Coordination::OpNum opnum;
    Coordination::read(opnum, rbuf);

    Coordination::ZooKeeperRequestPtr request_read = Coordination::ZooKeeperRequestFactory::instance().get(opnum);
    request_read->xid = xid;
    request_read->readImpl(rbuf);

    EXPECT_EQ(request_read->getOpNum(), Coordination::OpNum::Get);
    EXPECT_EQ(request_read->xid, 3);
    EXPECT_EQ(dynamic_cast<Coordination::ZooKeeperGetRequest &>(*request_read).path, "/path/value");
}

template <typename StateMachine>
struct SimpliestRaftServer
{
    SimpliestRaftServer(
        int server_id_, const std::string & hostname_, int port_, DB::KeeperContextPtr keeper_context)
        : server_id(server_id_)
        , hostname(hostname_)
        , port(port_)
        , endpoint(hostname + ":" + std::to_string(port))
        , state_machine(nuraft::cs_new<StateMachine>())
        , state_manager(nuraft::cs_new<DB::KeeperStateManager>(server_id, hostname, port, keeper_context))
    {
        state_manager->loadLogStore(1, 0);
        nuraft::raft_params params;
        params.heart_beat_interval_ = 100;
        params.election_timeout_lower_bound_ = 200;
        params.election_timeout_upper_bound_ = 400;
        params.reserved_log_items_ = 5;
        params.snapshot_distance_ = 1; /// forcefully send snapshots
        params.client_req_timeout_ = 3000;
        params.return_method_ = nuraft::raft_params::blocking;
        params.parallel_log_appending_ = true;

        nuraft::raft_server::init_options opts;
        opts.start_server_in_constructor_ = false;
        raft_instance = launcher.init(
            state_machine,
            state_manager,
            nuraft::cs_new<DB::LoggerWrapper>("ToyRaftLogger", DB::LogsLevel::trace),
            port,
            nuraft::asio_service::options{},
            params,
            opts);

        if (!raft_instance)
        {
            std::cerr << "Failed to initialize launcher" << std::endl;
            _exit(1);
        }

        state_manager->getLogStore()->setRaftServer(raft_instance);

        raft_instance->start_server(false);

        std::cout << "init Raft instance " << server_id;
        for (size_t ii = 0; ii < 20; ++ii)
        {
            if (raft_instance->is_initialized())
            {
                std::cout << " done" << std::endl;
                break;
            }
            std::cout << "." << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Server ID.
    int server_id;

    // Server address.
    std::string hostname;

    // Server port.
    int port;

    std::string endpoint;

    // State machine.
    nuraft::ptr<StateMachine> state_machine;

    // State manager.
    nuraft::ptr<DB::KeeperStateManager> state_manager;

    // Raft launcher.
    nuraft::raft_launcher launcher;

    // Raft server instance.
    nuraft::ptr<nuraft::raft_server> raft_instance;
};

using SummingRaftServer = SimpliestRaftServer<DB::SummingStateMachine>;

nuraft::ptr<nuraft::buffer> getBuffer(int64_t number)
{
    nuraft::ptr<nuraft::buffer> ret = nuraft::buffer::alloc(sizeof(number));
    nuraft::buffer_serializer bs(ret);
    bs.put_raw(&number, sizeof(number));
    return ret;
}

TEST_P(CoordinationTest, TestSummingRaft1)
{
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");
    setStateFileDirectory(".");

    SummingRaftServer s1(1, "localhost", 44444, keeper_context);
    SCOPE_EXIT(if (std::filesystem::exists("./state")) std::filesystem::remove("./state"););

    /// Single node is leader
    EXPECT_EQ(s1.raft_instance->get_leader(), 1);

    auto entry1 = getBuffer(143);
    auto ret = s1.raft_instance->append_entries({entry1});
    EXPECT_TRUE(ret->get_accepted()) << "failed to replicate: entry 1" << ret->get_result_code();
    EXPECT_EQ(ret->get_result_code(), nuraft::cmd_result_code::OK) << "failed to replicate: entry 1" << ret->get_result_code();

    while (s1.state_machine->getValue() != 143)
    {
        LOG_INFO(log, "Waiting s1 to apply entry");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_EQ(s1.state_machine->getValue(), 143);

    s1.launcher.shutdown(5);
}

DB::LogEntryPtr getLogEntry(const std::string & s, size_t term)
{
    DB::WriteBufferFromNuraftBuffer bufwriter;
    writeText(s, bufwriter);
    return nuraft::cs_new<nuraft::log_entry>(term, bufwriter.getBuffer());
}

TEST_P(CoordinationTest, ChangelogTestSimple)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);
    auto entry = getLogEntry("hello world", 77);
    changelog.append(entry);
    changelog.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog.next_slot(), 2);
    EXPECT_EQ(changelog.start_index(), 1);
    EXPECT_EQ(changelog.last_entry()->get_term(), 77);
    EXPECT_EQ(changelog.entry_at(1)->get_term(), 77);
    EXPECT_EQ(changelog.log_entries(1, 2)->size(), 1);
}

namespace
{
void waitDurableLogs(nuraft::log_store & log_store)
{
    while (log_store.last_durable_index() != log_store.next_slot() - 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

}

TEST_P(CoordinationTest, ChangelogTestFile)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);
    auto entry = getLogEntry("hello world", 77);
    changelog.append(entry);
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    for (const auto & p : fs::directory_iterator("./logs"))
        EXPECT_EQ(p.path(), "./logs/changelog_1_5.bin" + params.extension);

    changelog.append(entry);
    changelog.append(entry);
    changelog.append(entry);
    changelog.append(entry);
    changelog.append(entry);
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
}

TEST_P(CoordinationTest, ChangelogReadWrite)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 1000},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 10; ++i)
    {
        auto entry = getLogEntry("hello world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog.size(), 10);

    waitDurableLogs(changelog);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 1000},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(1, 0);
    EXPECT_EQ(changelog_reader.size(), 10);
    EXPECT_EQ(changelog_reader.last_entry()->get_term(), changelog.last_entry()->get_term());
    EXPECT_EQ(changelog_reader.start_index(), changelog.start_index());
    EXPECT_EQ(changelog_reader.next_slot(), changelog.next_slot());

    for (size_t i = 0; i < 10; ++i)
        EXPECT_EQ(changelog_reader.entry_at(i + 1)->get_term(), changelog.entry_at(i + 1)->get_term());

    auto entries_from_range_read = changelog_reader.log_entries(1, 11);
    auto entries_from_range = changelog.log_entries(1, 11);
    EXPECT_EQ(entries_from_range_read->size(), entries_from_range->size());
    EXPECT_EQ(10, entries_from_range->size());
}

TEST_P(CoordinationTest, ChangelogWriteAt)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 1000},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);
    for (size_t i = 0; i < 10; ++i)
    {
        auto entry = getLogEntry("hello world", i * 10);
        changelog.append(entry);
    }

    changelog.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog.size(), 10);

    auto entry = getLogEntry("writer", 77);
    changelog.write_at(7, entry);
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_EQ(changelog.size(), 7);
    EXPECT_EQ(changelog.last_entry()->get_term(), 77);
    EXPECT_EQ(changelog.entry_at(7)->get_term(), 77);
    EXPECT_EQ(changelog.next_slot(), 8);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 1000},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(1, 0);

    EXPECT_EQ(changelog_reader.size(), changelog.size());
    EXPECT_EQ(changelog_reader.last_entry()->get_term(), changelog.last_entry()->get_term());
    EXPECT_EQ(changelog_reader.start_index(), changelog.start_index());
    EXPECT_EQ(changelog_reader.next_slot(), changelog.next_slot());
}


TEST_P(CoordinationTest, ChangelogTestAppendAfterRead)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);
    for (size_t i = 0; i < 7; ++i)
    {
        auto entry = getLogEntry("hello world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog.size(), 7);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(1, 0);

    EXPECT_EQ(changelog_reader.size(), 7);
    for (size_t i = 7; i < 10; ++i)
    {
        auto entry = getLogEntry("hello world", i * 10);
        changelog_reader.append(entry);
    }
    changelog_reader.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog_reader.size(), 10);

    waitDurableLogs(changelog_reader);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));

    size_t logs_count = 0;
    for (const auto & _ [[maybe_unused]] : fs::directory_iterator("./logs"))
        logs_count++;

    EXPECT_EQ(logs_count, 2);

    auto entry = getLogEntry("someentry", 77);
    changelog_reader.append(entry);
    changelog_reader.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog_reader.size(), 11);

    waitDurableLogs(changelog_reader);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));

    logs_count = 0;
    for (const auto & _ [[maybe_unused]] : fs::directory_iterator("./logs"))
        logs_count++;

    EXPECT_EQ(logs_count, 3);
}

namespace
{

void assertFileDeleted(std::string path)
{
    for (size_t i = 0; i < 100; ++i)
    {
        if (!fs::exists(path))
            return;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    FAIL() << "File " << path << " was not removed";
}

}

TEST_P(CoordinationTest, ChangelogTestCompaction)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 3; ++i)
    {
        auto entry = getLogEntry("hello world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_EQ(changelog.size(), 3);

    changelog.compact(2);

    EXPECT_EQ(changelog.size(), 1);
    EXPECT_EQ(changelog.start_index(), 3);
    EXPECT_EQ(changelog.next_slot(), 4);
    EXPECT_EQ(changelog.last_entry()->get_term(), 20);
    // nothing should be deleted
    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));

    auto e1 = getLogEntry("hello world", 30);
    changelog.append(e1);
    auto e2 = getLogEntry("hello world", 40);
    changelog.append(e2);
    auto e3 = getLogEntry("hello world", 50);
    changelog.append(e3);
    auto e4 = getLogEntry("hello world", 60);
    changelog.append(e4);
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));

    changelog.compact(6);
    std::this_thread::sleep_for(std::chrono::microseconds(1000));

    assertFileDeleted("./logs/changelog_1_5.bin" + params.extension);
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));

    EXPECT_EQ(changelog.size(), 1);
    EXPECT_EQ(changelog.start_index(), 7);
    EXPECT_EQ(changelog.next_slot(), 8);
    EXPECT_EQ(changelog.last_entry()->get_term(), 60);
    /// And we able to read it
    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(7, 0);

    EXPECT_EQ(changelog_reader.size(), 1);
    EXPECT_EQ(changelog_reader.start_index(), 7);
    EXPECT_EQ(changelog_reader.next_slot(), 8);
    EXPECT_EQ(changelog_reader.last_entry()->get_term(), 60);
}

TEST_P(CoordinationTest, ChangelogTestBatchOperations)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);
    for (size_t i = 0; i < 10; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog.size(), 10);

    waitDurableLogs(changelog);

    auto entries = changelog.pack(1, 5);

    DB::KeeperLogStore apply_changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    apply_changelog.init(1, 0);

    for (size_t i = 0; i < 10; ++i)
    {
        EXPECT_EQ(apply_changelog.entry_at(i + 1)->get_term(), i * 10);
    }
    EXPECT_EQ(apply_changelog.size(), 10);

    apply_changelog.apply_pack(8, *entries);
    apply_changelog.end_of_append_batch(0, 0);

    EXPECT_EQ(apply_changelog.size(), 12);
    EXPECT_EQ(apply_changelog.start_index(), 1);
    EXPECT_EQ(apply_changelog.next_slot(), 13);

    for (size_t i = 0; i < 7; ++i)
    {
        EXPECT_EQ(apply_changelog.entry_at(i + 1)->get_term(), i * 10);
    }

    EXPECT_EQ(apply_changelog.entry_at(8)->get_term(), 0);
    EXPECT_EQ(apply_changelog.entry_at(9)->get_term(), 10);
    EXPECT_EQ(apply_changelog.entry_at(10)->get_term(), 20);
    EXPECT_EQ(apply_changelog.entry_at(11)->get_term(), 30);
    EXPECT_EQ(apply_changelog.entry_at(12)->get_term(), 40);
}

TEST_P(CoordinationTest, ChangelogTestBatchOperationsEmpty)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    nuraft::ptr<nuraft::buffer> entries;
    {
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);
        for (size_t i = 0; i < 10; ++i)
        {
            auto entry = getLogEntry(std::to_string(i) + "_hello_world", i * 10);
            changelog.append(entry);
        }
        changelog.end_of_append_batch(0, 0);

        EXPECT_EQ(changelog.size(), 10);

        waitDurableLogs(changelog);

        entries = changelog.pack(5, 5);
    }

    ChangelogDirTest test1("./logs1");
    setLogDirectory("./logs1");
    DB::KeeperLogStore changelog_new(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    changelog_new.init(1, 0);
    EXPECT_EQ(changelog_new.size(), 0);

    changelog_new.apply_pack(5, *entries);
    changelog_new.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog_new.size(), 5);
    EXPECT_EQ(changelog_new.start_index(), 5);
    EXPECT_EQ(changelog_new.next_slot(), 10);

    for (size_t i = 4; i < 9; ++i)
        EXPECT_EQ(changelog_new.entry_at(i + 1)->get_term(), i * 10);

    auto e = getLogEntry("hello_world", 110);
    changelog_new.append(e);
    changelog_new.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog_new.size(), 6);
    EXPECT_EQ(changelog_new.start_index(), 5);
    EXPECT_EQ(changelog_new.next_slot(), 11);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(5, 0);
}


TEST_P(CoordinationTest, ChangelogTestWriteAtPreviousFile)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 33; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_31_35.bin" + params.extension));

    EXPECT_EQ(changelog.size(), 33);

    auto e1 = getLogEntry("helloworld", 5555);
    changelog.write_at(7, e1);
    changelog.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog.size(), 7);
    EXPECT_EQ(changelog.start_index(), 1);
    EXPECT_EQ(changelog.next_slot(), 8);
    EXPECT_EQ(changelog.last_entry()->get_term(), 5555);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));

    EXPECT_FALSE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_31_35.bin" + params.extension));

    DB::KeeperLogStore changelog_read(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_read.init(1, 0);
    EXPECT_EQ(changelog_read.size(), 7);
    EXPECT_EQ(changelog_read.start_index(), 1);
    EXPECT_EQ(changelog_read.next_slot(), 8);
    EXPECT_EQ(changelog_read.last_entry()->get_term(), 5555);
}

TEST_P(CoordinationTest, ChangelogTestWriteAtFileBorder)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 33; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_31_35.bin" + params.extension));

    EXPECT_EQ(changelog.size(), 33);

    auto e1 = getLogEntry("helloworld", 5555);
    changelog.write_at(11, e1);
    changelog.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog.size(), 11);
    EXPECT_EQ(changelog.start_index(), 1);
    EXPECT_EQ(changelog.next_slot(), 12);
    EXPECT_EQ(changelog.last_entry()->get_term(), 5555);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));

    EXPECT_FALSE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_31_35.bin" + params.extension));

    DB::KeeperLogStore changelog_read(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_read.init(1, 0);
    EXPECT_EQ(changelog_read.size(), 11);
    EXPECT_EQ(changelog_read.start_index(), 1);
    EXPECT_EQ(changelog_read.next_slot(), 12);
    EXPECT_EQ(changelog_read.last_entry()->get_term(), 5555);
}

TEST_P(CoordinationTest, ChangelogTestWriteAtAllFiles)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);
    for (size_t i = 0; i < 33; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_31_35.bin" + params.extension));

    EXPECT_EQ(changelog.size(), 33);

    auto e1 = getLogEntry("helloworld", 5555);
    changelog.write_at(1, e1);
    changelog.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog.size(), 1);
    EXPECT_EQ(changelog.start_index(), 1);
    EXPECT_EQ(changelog.next_slot(), 2);
    EXPECT_EQ(changelog.last_entry()->get_term(), 5555);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));

    EXPECT_FALSE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_31_35.bin" + params.extension));
}

TEST_P(CoordinationTest, ChangelogTestStartNewLogAfterRead)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 35; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog.size(), 35);

    waitDurableLogs(changelog);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_31_35.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./logs/changelog_36_40.bin" + params.extension));

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(1, 0);

    auto entry = getLogEntry("36_hello_world", 360);
    changelog_reader.append(entry);
    changelog_reader.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog_reader.size(), 36);

    waitDurableLogs(changelog_reader);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_31_35.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_36_40.bin" + params.extension));
}

namespace
{
void assertBrokenLogRemoved(const fs::path & log_folder, const fs::path & filename)
{
    EXPECT_FALSE(fs::exists(log_folder / filename));
    // broken logs are sent to the detached/{timestamp} folder
    // we don't know timestamp so we iterate all of them
    for (const auto & dir_entry : fs::recursive_directory_iterator(log_folder / "detached"))
    {
        if (dir_entry.path().filename() == filename)
            return;
    }

    FAIL() << "Broken log " << filename << " was not moved to the detached folder";
}

}

TEST_P(CoordinationTest, ChangelogTestReadAfterBrokenTruncate)
{
    static const fs::path log_folder{"./logs"};

    auto params = GetParam();
    ChangelogDirTest test(log_folder);
    setLogDirectory(log_folder);

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 35; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", i * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog.size(), 35);

    waitDurableLogs(changelog);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_16_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_25.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_26_30.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_31_35.bin" + params.extension));

    DB::WriteBufferFromFile plain_buf(
        "./logs/changelog_11_15.bin" + params.extension, DB::DBMS_DEFAULT_BUFFER_SIZE, O_APPEND | O_CREAT | O_WRONLY);
    plain_buf.truncate(0);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(1, 0);
    changelog_reader.end_of_append_batch(0, 0);

    EXPECT_EQ(changelog_reader.size(), 10);
    EXPECT_EQ(changelog_reader.last_entry()->get_term(), 90);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));

    assertBrokenLogRemoved(log_folder, "changelog_16_20.bin" + params.extension);
    assertBrokenLogRemoved(log_folder, "changelog_21_25.bin" + params.extension);
    assertBrokenLogRemoved(log_folder, "changelog_26_30.bin" + params.extension);
    assertBrokenLogRemoved(log_folder, "changelog_31_35.bin" + params.extension);

    auto entry = getLogEntry("h", 7777);
    changelog_reader.append(entry);
    changelog_reader.end_of_append_batch(0, 0);
    EXPECT_EQ(changelog_reader.size(), 11);
    EXPECT_EQ(changelog_reader.last_entry()->get_term(), 7777);

    waitDurableLogs(changelog_reader);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_5.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_6_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_15.bin" + params.extension));

    assertBrokenLogRemoved(log_folder, "changelog_16_20.bin" + params.extension);
    assertBrokenLogRemoved(log_folder, "changelog_21_25.bin" + params.extension);
    assertBrokenLogRemoved(log_folder, "changelog_26_30.bin" + params.extension);
    assertBrokenLogRemoved(log_folder, "changelog_31_35.bin" + params.extension);

    DB::KeeperLogStore changelog_reader2(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader2.init(1, 0);
    EXPECT_EQ(changelog_reader2.size(), 11);
    EXPECT_EQ(changelog_reader2.last_entry()->get_term(), 7777);
}

/// Truncating all entries
TEST_P(CoordinationTest, ChangelogTestReadAfterBrokenTruncate2)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 20},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 35; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", (i + 44) * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_40.bin" + params.extension));

    DB::WriteBufferFromFile plain_buf(
        "./logs/changelog_1_20.bin" + params.extension, DB::DBMS_DEFAULT_BUFFER_SIZE, O_APPEND | O_CREAT | O_WRONLY);
    plain_buf.truncate(30);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 20},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(1, 0);

    EXPECT_EQ(changelog_reader.size(), 0);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_20.bin" + params.extension));
    assertBrokenLogRemoved("./logs", "changelog_21_40.bin" + params.extension);
    auto entry = getLogEntry("hello_world", 7777);
    changelog_reader.append(entry);
    changelog_reader.end_of_append_batch(0, 0);

    waitDurableLogs(changelog_reader);

    EXPECT_EQ(changelog_reader.size(), 1);
    EXPECT_EQ(changelog_reader.last_entry()->get_term(), 7777);

    DB::KeeperLogStore changelog_reader2(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 1},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader2.init(1, 0);
    EXPECT_EQ(changelog_reader2.size(), 1);
    EXPECT_EQ(changelog_reader2.last_entry()->get_term(), 7777);
}

/// Truncating only some entries from the end
/// For compressed logs we have no reliable way of knowing how many log entries were lost 
/// after we truncate some bytes from the end
TEST_F(CoordinationTest, ChangelogTestReadAfterBrokenTruncate3)
{
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 20},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 35; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", (i + 44) * 10);
        changelog.append(entry);
    }

    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_20.bin"));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_40.bin"));

    DB::WriteBufferFromFile plain_buf(
        "./logs/changelog_1_20.bin", DB::DBMS_DEFAULT_BUFFER_SIZE, O_APPEND | O_CREAT | O_WRONLY);
    plain_buf.truncate(plain_buf.size() - 30);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 20},
        DB::FlushSettings(),
        keeper_context);
    changelog_reader.init(1, 0);

    EXPECT_EQ(changelog_reader.size(), 19);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_20.bin"));
    assertBrokenLogRemoved("./logs", "changelog_21_40.bin");
    EXPECT_TRUE(fs::exists("./logs/changelog_20_39.bin"));
    auto entry = getLogEntry("hello_world", 7777);
    changelog_reader.append(entry);
    changelog_reader.end_of_append_batch(0, 0);

    waitDurableLogs(changelog_reader);

    EXPECT_EQ(changelog_reader.size(), 20);
    EXPECT_EQ(changelog_reader.last_entry()->get_term(), 7777);
}

TEST_F(CoordinationTest, ChangelogTestMixedLogTypes)
{
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    std::vector<std::string> changelog_files;

    const auto verify_changelog_files = [&]
    {
        for (const auto & log_file : changelog_files)
            EXPECT_TRUE(fs::exists(log_file)) << "File " << log_file << " not found";
    };

    size_t last_term = 0;
    size_t log_size = 0;

    const auto append_log = [&](auto & changelog, const std::string & data, uint64_t term)
    {
        last_term = term;
        ++log_size;
        auto entry = getLogEntry(data, last_term);
        changelog.append(entry);
    };

    const auto verify_log_content = [&](const auto & changelog)
    {
        EXPECT_EQ(changelog.size(), log_size);
        EXPECT_EQ(changelog.last_entry()->get_term(), last_term);
    };

    {
        SCOPED_TRACE("Initial uncompressed log");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 20},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);

        for (size_t i = 0; i < 35; ++i)
            append_log(changelog, std::to_string(i) + "_hello_world", (i+ 44) * 10);

        changelog.end_of_append_batch(0, 0);

        waitDurableLogs(changelog);
        changelog_files.push_back("./logs/changelog_1_20.bin");
        changelog_files.push_back("./logs/changelog_21_40.bin");
        verify_changelog_files();

        verify_log_content(changelog);
    }

    {
        SCOPED_TRACE("Compressed log");
        DB::KeeperLogStore changelog_compressed(
            DB::LogFileSettings{.force_sync = true, .compress_logs = true, .rotate_interval = 20},
            DB::FlushSettings(),
            keeper_context);
        changelog_compressed.init(1, 0);

        verify_changelog_files();
        verify_log_content(changelog_compressed);

        append_log(changelog_compressed, "hello_world", 7777);
        changelog_compressed.end_of_append_batch(0, 0);

        waitDurableLogs(changelog_compressed);

        verify_log_content(changelog_compressed);

        changelog_files.push_back("./logs/changelog_36_55.bin.zstd");
        verify_changelog_files();
    }

    {
        SCOPED_TRACE("Final uncompressed log");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 20},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);

        verify_changelog_files();
        verify_log_content(changelog);

        append_log(changelog, "hello_world", 7778);
        changelog.end_of_append_batch(0, 0);

        waitDurableLogs(changelog);

        verify_log_content(changelog);

        changelog_files.push_back("./logs/changelog_37_56.bin");
        verify_changelog_files();
    }
}

TEST_P(CoordinationTest, ChangelogTestLostFiles)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 20},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 35; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", (i + 44) * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_40.bin" + params.extension));

    fs::remove("./logs/changelog_1_20.bin" + params.extension);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 20},
        DB::FlushSettings(),
        keeper_context);
    /// It should print error message, but still able to start
    changelog_reader.init(5, 0);
    assertBrokenLogRemoved("./logs", "changelog_21_40.bin" + params.extension);
}

TEST_P(CoordinationTest, ChangelogTestLostFiles2)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 10},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);

    for (size_t i = 0; i < 35; ++i)
    {
        auto entry = getLogEntry(std::to_string(i) + "_hello_world", (i + 44) * 10);
        changelog.append(entry);
    }
    changelog.end_of_append_batch(0, 0);

    waitDurableLogs(changelog);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_20.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_21_30.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_31_40.bin" + params.extension));

    // we have a gap in our logs, we need to remove all the logs after the gap
    fs::remove("./logs/changelog_21_30.bin" + params.extension);

    DB::KeeperLogStore changelog_reader(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 10},
        DB::FlushSettings(),
        keeper_context);
    /// It should print error message, but still able to start
    changelog_reader.init(5, 0);
    EXPECT_TRUE(fs::exists("./logs/changelog_1_10.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_11_20.bin" + params.extension));

    assertBrokenLogRemoved("./logs", "changelog_31_40.bin" + params.extension);
}
struct IntNode
{
    int value;
    IntNode(int value_) : value(value_) { } // NOLINT(google-explicit-constructor)
    UInt64 sizeInBytes() const { return sizeof value; }
    IntNode & operator=(int rhs)
    {
        this->value = rhs;
        return *this;
    }
    bool operator==(const int & rhs) const { return value == rhs; }
    bool operator!=(const int & rhs) const { return rhs != this->value; }
};

TEST_P(CoordinationTest, SnapshotableHashMapSimple)
{
    DB::SnapshotableHashTable<IntNode> hello;
    EXPECT_TRUE(hello.insert("hello", 5).second);
    EXPECT_TRUE(hello.contains("hello"));
    EXPECT_EQ(hello.getValue("hello"), 5);
    EXPECT_FALSE(hello.insert("hello", 145).second);
    EXPECT_EQ(hello.getValue("hello"), 5);
    hello.updateValue("hello", [](IntNode & value) { value = 7; });
    EXPECT_EQ(hello.getValue("hello"), 7);
    EXPECT_EQ(hello.size(), 1);
    EXPECT_TRUE(hello.erase("hello"));
    EXPECT_EQ(hello.size(), 0);
}

TEST_P(CoordinationTest, SnapshotableHashMapTrySnapshot)
{
    DB::SnapshotableHashTable<IntNode> map_snp;
    EXPECT_TRUE(map_snp.insert("/hello", 7).second);
    EXPECT_FALSE(map_snp.insert("/hello", 145).second);
    map_snp.enableSnapshotMode(100000);
    EXPECT_FALSE(map_snp.insert("/hello", 145).second);
    map_snp.updateValue("/hello", [](IntNode & value) { value = 554; });
    EXPECT_EQ(map_snp.getValue("/hello"), 554);
    EXPECT_EQ(map_snp.snapshotSizeWithVersion().first, 2);
    EXPECT_EQ(map_snp.size(), 1);

    auto itr = map_snp.begin();
    EXPECT_EQ(itr->key, "/hello");
    EXPECT_EQ(itr->value, 7);
    EXPECT_EQ(itr->active_in_map, false);
    itr = std::next(itr);
    EXPECT_EQ(itr->key, "/hello");
    EXPECT_EQ(itr->value, 554);
    EXPECT_EQ(itr->active_in_map, true);
    itr = std::next(itr);
    EXPECT_EQ(itr, map_snp.end());
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_TRUE(map_snp.insert("/hello" + std::to_string(i), i).second);
    }
    EXPECT_EQ(map_snp.getValue("/hello3"), 3);

    EXPECT_EQ(map_snp.snapshotSizeWithVersion().first, 7);
    EXPECT_EQ(map_snp.size(), 6);
    itr = std::next(map_snp.begin(), 2);
    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(itr->key, "/hello" + std::to_string(i));
        EXPECT_EQ(itr->value, i);
        EXPECT_EQ(itr->active_in_map, true);
        itr = std::next(itr);
    }

    EXPECT_TRUE(map_snp.erase("/hello3"));
    EXPECT_TRUE(map_snp.erase("/hello2"));

    EXPECT_EQ(map_snp.snapshotSizeWithVersion().first, 7);
    EXPECT_EQ(map_snp.size(), 4);
    itr = std::next(map_snp.begin(), 2);
    for (size_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(itr->key, "/hello" + std::to_string(i));
        EXPECT_EQ(itr->value, i);
        EXPECT_EQ(itr->active_in_map, i != 3 && i != 2);
        itr = std::next(itr);
    }
    map_snp.clearOutdatedNodes();

    EXPECT_EQ(map_snp.snapshotSizeWithVersion().first, 4);
    EXPECT_EQ(map_snp.size(), 4);
    itr = map_snp.begin();
    EXPECT_EQ(itr->key, "/hello");
    EXPECT_EQ(itr->value, 554);
    EXPECT_EQ(itr->active_in_map, true);
    itr = std::next(itr);
    EXPECT_EQ(itr->key, "/hello0");
    EXPECT_EQ(itr->value, 0);
    EXPECT_EQ(itr->active_in_map, true);
    itr = std::next(itr);
    EXPECT_EQ(itr->key, "/hello1");
    EXPECT_EQ(itr->value, 1);
    EXPECT_EQ(itr->active_in_map, true);
    itr = std::next(itr);
    EXPECT_EQ(itr->key, "/hello4");
    EXPECT_EQ(itr->value, 4);
    EXPECT_EQ(itr->active_in_map, true);
    itr = std::next(itr);
    EXPECT_EQ(itr, map_snp.end());
    map_snp.disableSnapshotMode();
}

TEST_P(CoordinationTest, SnapshotableHashMapDataSize)
{
    /// int
    DB::SnapshotableHashTable<IntNode> hello;
    hello.disableSnapshotMode();
    EXPECT_EQ(hello.getApproximateDataSize(), 0);

    hello.insert("hello", 1);
    EXPECT_EQ(hello.getApproximateDataSize(), 9);
    hello.updateValue("hello", [](IntNode & value) { value = 2; });
    EXPECT_EQ(hello.getApproximateDataSize(), 9);
    hello.insertOrReplace("hello", 3);
    EXPECT_EQ(hello.getApproximateDataSize(), 9);

    hello.erase("hello");
    EXPECT_EQ(hello.getApproximateDataSize(), 0);

    hello.clear();
    EXPECT_EQ(hello.getApproximateDataSize(), 0);

    hello.enableSnapshotMode(10000);
    hello.insert("hello", 1);
    EXPECT_EQ(hello.getApproximateDataSize(), 9);
    hello.updateValue("hello", [](IntNode & value) { value = 2; });
    EXPECT_EQ(hello.getApproximateDataSize(), 18);
    hello.insertOrReplace("hello", 1);
    EXPECT_EQ(hello.getApproximateDataSize(), 27);

    hello.clearOutdatedNodes();
    EXPECT_EQ(hello.getApproximateDataSize(), 9);

    hello.erase("hello");
    EXPECT_EQ(hello.getApproximateDataSize(), 9);

    hello.clearOutdatedNodes();
    EXPECT_EQ(hello.getApproximateDataSize(), 0);

    /// Node
    using Node = DB::KeeperStorage::Node;
    DB::SnapshotableHashTable<Node> world;
    Node n1;
    n1.setData("1234");
    Node n2;
    n2.setData("123456");
    n2.addChild("");

    /// Note: Below, we check in many cases only that getApproximateDataSize() > 0. This is because
    ///       the SnapshotableHashTable's approximate data size includes Node's sizeInBytes(). The
    ///       latter includes sizeof(absl::flat_hash_set) which is surprisingly not constant across
    ///       different runs. The approximate size is only used for statistics accounting, so this
    ///       should be okay.

    world.disableSnapshotMode();
    world.insert("world", n1);
    EXPECT_GT(world.getApproximateDataSize(), 0);
    world.updateValue("world", [&](Node & value) { value = n2; });
    EXPECT_GT(world.getApproximateDataSize(), 0);

    world.erase("world");
    EXPECT_EQ(world.getApproximateDataSize(), 0);

    world.enableSnapshotMode(100000);
    world.insert("world", n1);
    EXPECT_GT(world.getApproximateDataSize(), 0);
    world.updateValue("world", [&](Node & value) { value = n2; });
    EXPECT_GT(world.getApproximateDataSize(), 0);

    world.clearOutdatedNodes();
    EXPECT_GT(world.getApproximateDataSize(), 0);

    world.erase("world");
    EXPECT_GT(world.getApproximateDataSize(), 0);

    world.clear();
    EXPECT_EQ(world.getApproximateDataSize(), 0);
}

void addNode(DB::KeeperStorage & storage, const std::string & path, const std::string & data, int64_t ephemeral_owner = 0)
{
    using Node = DB::KeeperStorage::Node;
    Node node{};
    node.setData(data);
    node.stat.ephemeralOwner = ephemeral_owner;
    storage.container.insertOrReplace(path, node);
    auto child_it = storage.container.find(path);
    auto child_path = DB::getBaseNodeName(child_it->key);
    storage.container.updateValue(
        DB::parentNodePath(StringRef{path}),
        [&](auto & parent)
        {
            parent.addChild(child_path);
            parent.stat.numChildren++;
        });
}

TEST_P(CoordinationTest, TestStorageSnapshotSimple)
{
    auto params = GetParam();
    ChangelogDirTest test("./snapshots");
    setSnapshotDirectory("./snapshots");

    DB::KeeperSnapshotManager manager(3, keeper_context, params.enable_compression);

    DB::KeeperStorage storage(500, "", keeper_context);
    addNode(storage, "/hello", "world", 1);
    addNode(storage, "/hello/somepath", "somedata", 3);
    storage.session_id_counter = 5;
    storage.zxid = 2;
    storage.ephemerals[3] = {"/hello"};
    storage.ephemerals[1] = {"/hello/somepath"};
    storage.getSessionID(130);
    storage.getSessionID(130);

    DB::KeeperStorageSnapshot snapshot(&storage, 2);

    EXPECT_EQ(snapshot.snapshot_meta->get_last_log_idx(), 2);
    EXPECT_EQ(snapshot.session_id, 7);
    EXPECT_EQ(snapshot.snapshot_container_size, 6);
    EXPECT_EQ(snapshot.session_and_timeout.size(), 2);

    auto buf = manager.serializeSnapshotToBuffer(snapshot);
    manager.serializeSnapshotBufferToDisk(*buf, 2);
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_2.bin" + params.extension));


    auto debuf = manager.deserializeSnapshotBufferFromDisk(2);

    auto [restored_storage, snapshot_meta, _] = manager.deserializeSnapshotFromBuffer(debuf);

    EXPECT_EQ(restored_storage->container.size(), 6);
    EXPECT_EQ(restored_storage->container.getValue("/").getChildren().size(), 2);
    EXPECT_EQ(restored_storage->container.getValue("/hello").getChildren().size(), 1);
    EXPECT_EQ(restored_storage->container.getValue("/hello/somepath").getChildren().size(), 0);

    EXPECT_EQ(restored_storage->container.getValue("/").getData(), "");
    EXPECT_EQ(restored_storage->container.getValue("/hello").getData(), "world");
    EXPECT_EQ(restored_storage->container.getValue("/hello/somepath").getData(), "somedata");
    EXPECT_EQ(restored_storage->session_id_counter, 7);
    EXPECT_EQ(restored_storage->zxid, 2);
    EXPECT_EQ(restored_storage->ephemerals.size(), 2);
    EXPECT_EQ(restored_storage->ephemerals[3].size(), 1);
    EXPECT_EQ(restored_storage->ephemerals[1].size(), 1);
    EXPECT_EQ(restored_storage->session_and_timeout.size(), 2);
}

TEST_P(CoordinationTest, TestStorageSnapshotMoreWrites)
{
    auto params = GetParam();
    ChangelogDirTest test("./snapshots");
    setSnapshotDirectory("./snapshots");

    DB::KeeperSnapshotManager manager(3, keeper_context, params.enable_compression);

    DB::KeeperStorage storage(500, "", keeper_context);
    storage.getSessionID(130);

    for (size_t i = 0; i < 50; ++i)
    {
        addNode(storage, "/hello_" + std::to_string(i), "world_" + std::to_string(i));
    }

    DB::KeeperStorageSnapshot snapshot(&storage, 50);
    EXPECT_EQ(snapshot.snapshot_meta->get_last_log_idx(), 50);
    EXPECT_EQ(snapshot.snapshot_container_size, 54);

    for (size_t i = 50; i < 100; ++i)
    {
        addNode(storage, "/hello_" + std::to_string(i), "world_" + std::to_string(i));
    }

    EXPECT_EQ(storage.container.size(), 104);

    auto buf = manager.serializeSnapshotToBuffer(snapshot);
    manager.serializeSnapshotBufferToDisk(*buf, 50);
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_50.bin" + params.extension));


    auto debuf = manager.deserializeSnapshotBufferFromDisk(50);
    auto [restored_storage, meta, _] = manager.deserializeSnapshotFromBuffer(debuf);

    EXPECT_EQ(restored_storage->container.size(), 54);
    for (size_t i = 0; i < 50; ++i)
    {
        EXPECT_EQ(restored_storage->container.getValue("/hello_" + std::to_string(i)).getData(), "world_" + std::to_string(i));
    }
}


TEST_P(CoordinationTest, TestStorageSnapshotManySnapshots)
{
    auto params = GetParam();
    ChangelogDirTest test("./snapshots");
    setSnapshotDirectory("./snapshots");

    DB::KeeperSnapshotManager manager(3, keeper_context, params.enable_compression);

    DB::KeeperStorage storage(500, "", keeper_context);
    storage.getSessionID(130);

    for (size_t j = 1; j <= 5; ++j)
    {
        for (size_t i = (j - 1) * 50; i < j * 50; ++i)
        {
            addNode(storage, "/hello_" + std::to_string(i), "world_" + std::to_string(i));
        }

        DB::KeeperStorageSnapshot snapshot(&storage, j * 50);
        auto buf = manager.serializeSnapshotToBuffer(snapshot);
        manager.serializeSnapshotBufferToDisk(*buf, j * 50);
        EXPECT_TRUE(fs::exists(std::string{"./snapshots/snapshot_"} + std::to_string(j * 50) + ".bin" + params.extension));
    }

    EXPECT_FALSE(fs::exists("./snapshots/snapshot_50.bin" + params.extension));
    EXPECT_FALSE(fs::exists("./snapshots/snapshot_100.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_150.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_200.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_250.bin" + params.extension));


    auto [restored_storage, meta, _] = manager.restoreFromLatestSnapshot();

    EXPECT_EQ(restored_storage->container.size(), 254);

    for (size_t i = 0; i < 250; ++i)
    {
        EXPECT_EQ(restored_storage->container.getValue("/hello_" + std::to_string(i)).getData(), "world_" + std::to_string(i));
    }
}

TEST_P(CoordinationTest, TestStorageSnapshotMode)
{
    auto params = GetParam();
    ChangelogDirTest test("./snapshots");
    setSnapshotDirectory("./snapshots");

    DB::KeeperSnapshotManager manager(3, keeper_context, params.enable_compression);
    DB::KeeperStorage storage(500, "", keeper_context);
    for (size_t i = 0; i < 50; ++i)
    {
        addNode(storage, "/hello_" + std::to_string(i), "world_" + std::to_string(i));
    }

    {
        DB::KeeperStorageSnapshot snapshot(&storage, 50);
        for (size_t i = 0; i < 50; ++i)
        {
            addNode(storage, "/hello_" + std::to_string(i), "wlrd_" + std::to_string(i));
        }
        for (size_t i = 0; i < 50; ++i)
        {
            EXPECT_EQ(storage.container.getValue("/hello_" + std::to_string(i)).getData(), "wlrd_" + std::to_string(i));
        }
        for (size_t i = 0; i < 50; ++i)
        {
            if (i % 2 == 0)
                storage.container.erase("/hello_" + std::to_string(i));
        }
        EXPECT_EQ(storage.container.size(), 29);
        EXPECT_EQ(storage.container.snapshotSizeWithVersion().first, 105);
        EXPECT_EQ(storage.container.snapshotSizeWithVersion().second, 1);
        auto buf = manager.serializeSnapshotToBuffer(snapshot);
        manager.serializeSnapshotBufferToDisk(*buf, 50);
    }
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_50.bin" + params.extension));
    EXPECT_EQ(storage.container.size(), 29);
    storage.clearGarbageAfterSnapshot();
    EXPECT_EQ(storage.container.snapshotSizeWithVersion().first, 29);
    for (size_t i = 0; i < 50; ++i)
    {
        if (i % 2 != 0)
            EXPECT_EQ(storage.container.getValue("/hello_" + std::to_string(i)).getData(), "wlrd_" + std::to_string(i));
        else
            EXPECT_FALSE(storage.container.contains("/hello_" + std::to_string(i)));
    }

    auto [restored_storage, meta, _] = manager.restoreFromLatestSnapshot();

    for (size_t i = 0; i < 50; ++i)
    {
        EXPECT_EQ(restored_storage->container.getValue("/hello_" + std::to_string(i)).getData(), "world_" + std::to_string(i));
    }
}

TEST_P(CoordinationTest, TestStorageSnapshotBroken)
{
    auto params = GetParam();
    ChangelogDirTest test("./snapshots");
    setSnapshotDirectory("./snapshots");

    DB::KeeperSnapshotManager manager(3, keeper_context, params.enable_compression);
    DB::KeeperStorage storage(500, "", keeper_context);
    for (size_t i = 0; i < 50; ++i)
    {
        addNode(storage, "/hello_" + std::to_string(i), "world_" + std::to_string(i));
    }
    {
        DB::KeeperStorageSnapshot snapshot(&storage, 50);
        auto buf = manager.serializeSnapshotToBuffer(snapshot);
        manager.serializeSnapshotBufferToDisk(*buf, 50);
    }
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_50.bin" + params.extension));

    /// Let's corrupt file
    DB::WriteBufferFromFile plain_buf(
        "./snapshots/snapshot_50.bin" + params.extension, DB::DBMS_DEFAULT_BUFFER_SIZE, O_APPEND | O_CREAT | O_WRONLY);
    plain_buf.truncate(34);
    plain_buf.sync();

    EXPECT_THROW(manager.restoreFromLatestSnapshot(), DB::Exception);
}

nuraft::ptr<nuraft::buffer> getBufferFromZKRequest(int64_t session_id, int64_t zxid, const Coordination::ZooKeeperRequestPtr & request)
{
    DB::WriteBufferFromNuraftBuffer buf;
    DB::writeIntBinary(session_id, buf);
    request->write(buf);
    using namespace std::chrono;
    auto time = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    DB::writeIntBinary(time, buf);
    DB::writeIntBinary(zxid, buf);
    DB::writeIntBinary(DB::KeeperStorage::DigestVersion::NO_DIGEST, buf);
    return buf.getBuffer();
}

nuraft::ptr<nuraft::log_entry>
getLogEntryFromZKRequest(size_t term, int64_t session_id, int64_t zxid, const Coordination::ZooKeeperRequestPtr & request)
{
    auto buffer = getBufferFromZKRequest(session_id, zxid, request);
    return nuraft::cs_new<nuraft::log_entry>(term, buffer);
}

void testLogAndStateMachine(
    Coordination::CoordinationSettingsPtr settings,
    uint64_t total_logs,
    bool enable_compression,
    Coordination::KeeperContextPtr keeper_context)
{
    using namespace Coordination;
    using namespace DB;

    ChangelogDirTest snapshots("./snapshots");
    keeper_context->setSnapshotDisk(std::make_shared<DiskLocal>("SnapshotDisk", "./snapshots"));
    ChangelogDirTest logs("./logs");
    keeper_context->setLogDisk(std::make_shared<DiskLocal>("LogDisk", "./logs"));

    ResponsesQueue queue(std::numeric_limits<size_t>::max());
    SnapshotsQueue snapshots_queue{1};
    auto state_machine = std::make_shared<KeeperStateMachine>(queue, snapshots_queue, settings, keeper_context, nullptr);
    state_machine->init();
    DB::KeeperLogStore changelog(
        DB::LogFileSettings{
            .force_sync = true, .compress_logs = enable_compression, .rotate_interval = settings->rotate_log_storage_interval},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(state_machine->last_commit_index() + 1, settings->reserved_log_items);
    for (size_t i = 1; i < total_logs + 1; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog.append(entry);
        changelog.end_of_append_batch(0, 0);

        waitDurableLogs(changelog);

        state_machine->pre_commit(i, changelog.entry_at(i)->get_buf());
        state_machine->commit(i, changelog.entry_at(i)->get_buf());
        bool snapshot_created = false;
        if (i % settings->snapshot_distance == 0)
        {
            nuraft::snapshot s(i, 0, std::make_shared<nuraft::cluster_config>());
            nuraft::async_result<bool>::handler_type when_done
                = [&snapshot_created](bool & ret, nuraft::ptr<std::exception> & /*exception*/)
            {
                snapshot_created = ret;
                LOG_INFO(&Poco::Logger::get("CoordinationTest"), "Snapshot finished");
            };

            state_machine->create_snapshot(s, when_done);
            CreateSnapshotTask snapshot_task;
            bool pop_result = snapshots_queue.pop(snapshot_task);
            EXPECT_TRUE(pop_result);

            snapshot_task.create_snapshot(std::move(snapshot_task.snapshot));
        }
        if (snapshot_created && changelog.size() > settings->reserved_log_items)
            changelog.compact(i - settings->reserved_log_items);
    }

    SnapshotsQueue snapshots_queue1{1};
    auto restore_machine = std::make_shared<KeeperStateMachine>(queue, snapshots_queue1, settings, keeper_context, nullptr);
    restore_machine->init();
    EXPECT_EQ(restore_machine->last_commit_index(), total_logs - total_logs % settings->snapshot_distance);

    DB::KeeperLogStore restore_changelog(
        DB::LogFileSettings{
            .force_sync = true, .compress_logs = enable_compression, .rotate_interval = settings->rotate_log_storage_interval},
        DB::FlushSettings(),
        keeper_context);
    restore_changelog.init(restore_machine->last_commit_index() + 1, settings->reserved_log_items);

    EXPECT_EQ(restore_changelog.size(), std::min(settings->reserved_log_items + total_logs % settings->snapshot_distance, total_logs));
    EXPECT_EQ(restore_changelog.next_slot(), total_logs + 1);
    if (total_logs > settings->reserved_log_items + 1)
        EXPECT_EQ(
            restore_changelog.start_index(), total_logs - total_logs % settings->snapshot_distance - settings->reserved_log_items + 1);
    else
        EXPECT_EQ(restore_changelog.start_index(), 1);

    for (size_t i = restore_machine->last_commit_index() + 1; i < restore_changelog.next_slot(); ++i)
    {
        restore_machine->pre_commit(i, changelog.entry_at(i)->get_buf());
        restore_machine->commit(i, changelog.entry_at(i)->get_buf());
    }

    auto & source_storage = state_machine->getStorageUnsafe();
    auto & restored_storage = restore_machine->getStorageUnsafe();

    EXPECT_EQ(source_storage.container.size(), restored_storage.container.size());
    for (size_t i = 1; i < total_logs + 1; ++i)
    {
        auto path = "/hello_" + std::to_string(i);
        EXPECT_EQ(source_storage.container.getValue(path).getData(), restored_storage.container.getValue(path).getData());
    }
}

TEST_P(CoordinationTest, TestStateMachineAndLogStore)
{
    using namespace Coordination;
    using namespace DB;
    auto params = GetParam();

    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 10;
        settings->reserved_log_items = 10;
        settings->rotate_log_storage_interval = 10;
        testLogAndStateMachine(settings, 37, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 10;
        settings->reserved_log_items = 10;
        settings->rotate_log_storage_interval = 10;
        testLogAndStateMachine(settings, 11, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 10;
        settings->reserved_log_items = 10;
        settings->rotate_log_storage_interval = 10;
        testLogAndStateMachine(settings, 40, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 10;
        settings->reserved_log_items = 20;
        settings->rotate_log_storage_interval = 30;
        testLogAndStateMachine(settings, 40, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 10;
        settings->reserved_log_items = 0;
        settings->rotate_log_storage_interval = 10;
        testLogAndStateMachine(settings, 40, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 1;
        settings->reserved_log_items = 1;
        settings->rotate_log_storage_interval = 32;
        testLogAndStateMachine(settings, 32, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 10;
        settings->reserved_log_items = 7;
        settings->rotate_log_storage_interval = 1;
        testLogAndStateMachine(settings, 33, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 37;
        settings->reserved_log_items = 1000;
        settings->rotate_log_storage_interval = 5000;
        testLogAndStateMachine(settings, 33, params.enable_compression, keeper_context);
    }
    {
        CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
        settings->snapshot_distance = 37;
        settings->reserved_log_items = 1000;
        settings->rotate_log_storage_interval = 5000;
        testLogAndStateMachine(settings, 45, params.enable_compression, keeper_context);
    }
}

TEST_P(CoordinationTest, TestEphemeralNodeRemove)
{
    using namespace Coordination;
    using namespace DB;

    ChangelogDirTest snapshots("./snapshots");
    setSnapshotDirectory("./snapshots");

    CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();

    ResponsesQueue queue(std::numeric_limits<size_t>::max());
    SnapshotsQueue snapshots_queue{1};
    auto state_machine = std::make_shared<KeeperStateMachine>(queue, snapshots_queue, settings, keeper_context, nullptr);
    state_machine->init();

    std::shared_ptr<ZooKeeperCreateRequest> request_c = std::make_shared<ZooKeeperCreateRequest>();
    request_c->path = "/hello";
    request_c->is_ephemeral = true;
    auto entry_c = getLogEntryFromZKRequest(0, 1, state_machine->getNextZxid(), request_c);
    state_machine->pre_commit(1, entry_c->get_buf());
    state_machine->commit(1, entry_c->get_buf());
    const auto & storage = state_machine->getStorageUnsafe();

    EXPECT_EQ(storage.ephemerals.size(), 1);
    std::shared_ptr<ZooKeeperRemoveRequest> request_d = std::make_shared<ZooKeeperRemoveRequest>();
    request_d->path = "/hello";
    /// Delete from other session
    auto entry_d = getLogEntryFromZKRequest(0, 2, state_machine->getNextZxid(), request_d);
    state_machine->pre_commit(2, entry_d->get_buf());
    state_machine->commit(2, entry_d->get_buf());

    EXPECT_EQ(storage.ephemerals.size(), 0);
}


TEST_P(CoordinationTest, TestCreateNodeWithAuthSchemeForAclWhenAuthIsPrecommitted)
{
    using namespace Coordination;
    using namespace DB;

    ChangelogDirTest snapshots("./snapshots");
    setSnapshotDirectory("./snapshots");
    CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
    ResponsesQueue queue(std::numeric_limits<size_t>::max());
    SnapshotsQueue snapshots_queue{1};

    auto state_machine = std::make_shared<KeeperStateMachine>(queue, snapshots_queue, settings, keeper_context, nullptr);
    state_machine->init();

    String user_auth_data = "test_user:test_password";
    String digest = KeeperStorage::generateDigest(user_auth_data);

    std::shared_ptr<ZooKeeperAuthRequest> auth_req = std::make_shared<ZooKeeperAuthRequest>();
    auth_req->scheme = "digest";
    auth_req->data = user_auth_data;

    // Add auth data to the session
    auto auth_entry = getLogEntryFromZKRequest(0, 1, state_machine->getNextZxid(), auth_req);
    state_machine->pre_commit(1, auth_entry->get_buf());

    // Create a node with 'auth' scheme for ACL
    String node_path = "/hello";
    std::shared_ptr<ZooKeeperCreateRequest> create_req = std::make_shared<ZooKeeperCreateRequest>();
    create_req->path = node_path;
    // When 'auth' scheme is used the creator must have been authenticated by the server (for example, using 'digest' scheme) before it can
    // create nodes with this ACL.
    create_req->acls = {{.permissions = 31, .scheme = "auth", .id = ""}};
    auto create_entry = getLogEntryFromZKRequest(0, 1, state_machine->getNextZxid(), create_req);
    state_machine->pre_commit(2, create_entry->get_buf());

    const auto & uncommitted_state = state_machine->getStorageUnsafe().uncommitted_state;
    ASSERT_TRUE(uncommitted_state.nodes.contains(node_path));

    // commit log entries
    state_machine->commit(1, auth_entry->get_buf());
    state_machine->commit(2, create_entry->get_buf());

    auto node = uncommitted_state.getNode(node_path);
    ASSERT_NE(node, nullptr);
    auto acls = uncommitted_state.getACLs(node_path);
    ASSERT_EQ(acls.size(), 1);
    EXPECT_EQ(acls[0].scheme, "digest");
    EXPECT_EQ(acls[0].id, digest);
    EXPECT_EQ(acls[0].permissions, 31);
}

TEST_P(CoordinationTest, TestSetACLWithAuthSchemeForAclWhenAuthIsPrecommitted)
{
    using namespace Coordination;
    using namespace DB;

    ChangelogDirTest snapshots("./snapshots");
    setSnapshotDirectory("./snapshots");

    CoordinationSettingsPtr settings = std::make_shared<CoordinationSettings>();
    ResponsesQueue queue(std::numeric_limits<size_t>::max());
    SnapshotsQueue snapshots_queue{1};

    auto state_machine = std::make_shared<KeeperStateMachine>(queue, snapshots_queue, settings, keeper_context, nullptr);
    state_machine->init();

    String user_auth_data = "test_user:test_password";
    String digest = KeeperStorage::generateDigest(user_auth_data);

    std::shared_ptr<ZooKeeperAuthRequest> auth_req = std::make_shared<ZooKeeperAuthRequest>();
    auth_req->scheme = "digest";
    auth_req->data = user_auth_data;

    // Add auth data to the session
    auto auth_entry = getLogEntryFromZKRequest(0, 1, state_machine->getNextZxid(), auth_req);
    state_machine->pre_commit(1, auth_entry->get_buf());

    // Create a node
    String node_path = "/hello";
    std::shared_ptr<ZooKeeperCreateRequest> create_req = std::make_shared<ZooKeeperCreateRequest>();
    create_req->path = node_path;
    auto create_entry = getLogEntryFromZKRequest(0, 1, state_machine->getNextZxid(), create_req);
    state_machine->pre_commit(2, create_entry->get_buf());

    // Set ACL with 'auth' scheme for ACL
    std::shared_ptr<ZooKeeperSetACLRequest> set_acl_req = std::make_shared<ZooKeeperSetACLRequest>();
    set_acl_req->path = node_path;
    // When 'auth' scheme is used the creator must have been authenticated by the server (for example, using 'digest' scheme) before it can
    // set this ACL.
    set_acl_req->acls = {{.permissions = 31, .scheme = "auth", .id = ""}};
    auto set_acl_entry = getLogEntryFromZKRequest(0, 1, state_machine->getNextZxid(), set_acl_req);
    state_machine->pre_commit(3, set_acl_entry->get_buf());

    // commit all entries
    state_machine->commit(1, auth_entry->get_buf());
    state_machine->commit(2, create_entry->get_buf());
    state_machine->commit(3, set_acl_entry->get_buf());

    const auto & uncommitted_state = state_machine->getStorageUnsafe().uncommitted_state;
    auto node = uncommitted_state.getNode(node_path);

    ASSERT_NE(node, nullptr);
    auto acls = uncommitted_state.getACLs(node_path);
    ASSERT_EQ(acls.size(), 1);
    EXPECT_EQ(acls[0].scheme, "digest");
    EXPECT_EQ(acls[0].id, digest);
    EXPECT_EQ(acls[0].permissions, 31);
}


TEST_P(CoordinationTest, TestRotateIntervalChanges)
{
    using namespace Coordination;
    auto params = GetParam();
    ChangelogDirTest snapshots("./logs");
    setLogDirectory("./logs");
    {
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);

        changelog.init(0, 3);
        for (size_t i = 1; i < 55; ++i)
        {
            std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
            request->path = "/hello_" + std::to_string(i);
            auto entry = getLogEntryFromZKRequest(0, 1, i, request);
            changelog.append(entry);
            changelog.end_of_append_batch(0, 0);
        }

        waitDurableLogs(changelog);
    }


    EXPECT_TRUE(fs::exists("./logs/changelog_1_100.bin" + params.extension));

    DB::KeeperLogStore changelog_1(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 10},
        DB::FlushSettings(),
        keeper_context);
    changelog_1.init(0, 50);
    for (size_t i = 0; i < 55; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(100 + i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog_1.append(entry);
        changelog_1.end_of_append_batch(0, 0);
    }

    waitDurableLogs(changelog_1);

    EXPECT_TRUE(fs::exists("./logs/changelog_1_100.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_101_110.bin" + params.extension));

    DB::KeeperLogStore changelog_2(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 7},
        DB::FlushSettings(),
        keeper_context);
    changelog_2.init(98, 55);

    for (size_t i = 0; i < 17; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(200 + i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog_2.append(entry);
        changelog_2.end_of_append_batch(0, 0);
    }

    waitDurableLogs(changelog_2);

    changelog_2.compact(105);
    std::this_thread::sleep_for(std::chrono::microseconds(1000));

    assertFileDeleted("./logs/changelog_1_100.bin" + params.extension);
    EXPECT_TRUE(fs::exists("./logs/changelog_101_110.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_111_117.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_118_124.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_125_131.bin" + params.extension));

    DB::KeeperLogStore changelog_3(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 5},
        DB::FlushSettings(),
        keeper_context);
    changelog_3.init(116, 3);
    for (size_t i = 0; i < 17; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(300 + i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog_3.append(entry);
        changelog_3.end_of_append_batch(0, 0);
    }

    waitDurableLogs(changelog_3);

    changelog_3.compact(125);
    std::this_thread::sleep_for(std::chrono::microseconds(1000));
    assertFileDeleted("./logs/changelog_101_110.bin" + params.extension);
    assertFileDeleted("./logs/changelog_111_117.bin" + params.extension);
    assertFileDeleted("./logs/changelog_118_124.bin" + params.extension);

    EXPECT_TRUE(fs::exists("./logs/changelog_125_131.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_132_136.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_137_141.bin" + params.extension));
    EXPECT_TRUE(fs::exists("./logs/changelog_142_146.bin" + params.extension));
}

TEST_P(CoordinationTest, TestSessionExpiryQueue)
{
    using namespace Coordination;
    SessionExpiryQueue queue(500);

    queue.addNewSessionOrUpdate(1, 1000);

    for (size_t i = 0; i < 2; ++i)
    {
        EXPECT_EQ(queue.getExpiredSessions(), std::vector<int64_t>({}));
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_EQ(queue.getExpiredSessions(), std::vector<int64_t>({1}));
}


TEST_P(CoordinationTest, TestCompressedLogsMultipleRewrite)
{
    using namespace Coordination;
    auto test_params = GetParam();
    ChangelogDirTest logs("./logs");
    setLogDirectory("./logs");
    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = test_params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);

    changelog.init(0, 3);
    for (size_t i = 1; i < 55; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog.append(entry);
        changelog.end_of_append_batch(0, 0);
    }

    waitDurableLogs(changelog);

    DB::KeeperLogStore changelog1(
        DB::LogFileSettings{.force_sync = true, .compress_logs = test_params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    changelog1.init(0, 3);
    for (size_t i = 55; i < 70; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog1.append(entry);
        changelog1.end_of_append_batch(0, 0);
    }

    waitDurableLogs(changelog1);

    DB::KeeperLogStore changelog2(
        DB::LogFileSettings{.force_sync = true, .compress_logs = test_params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    changelog2.init(0, 3);
    for (size_t i = 70; i < 80; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog2.append(entry);
        changelog2.end_of_append_batch(0, 0);
    }
}

TEST_P(CoordinationTest, TestStorageSnapshotDifferentCompressions)
{
    auto params = GetParam();

    ChangelogDirTest test("./snapshots");
    setSnapshotDirectory("./snapshots");

    DB::KeeperSnapshotManager manager(3, keeper_context, params.enable_compression);

    DB::KeeperStorage storage(500, "", keeper_context);
    addNode(storage, "/hello", "world", 1);
    addNode(storage, "/hello/somepath", "somedata", 3);
    storage.session_id_counter = 5;
    storage.zxid = 2;
    storage.ephemerals[3] = {"/hello"};
    storage.ephemerals[1] = {"/hello/somepath"};
    storage.getSessionID(130);
    storage.getSessionID(130);

    DB::KeeperStorageSnapshot snapshot(&storage, 2);

    auto buf = manager.serializeSnapshotToBuffer(snapshot);
    manager.serializeSnapshotBufferToDisk(*buf, 2);
    EXPECT_TRUE(fs::exists("./snapshots/snapshot_2.bin" + params.extension));

    DB::KeeperSnapshotManager new_manager(3, keeper_context, !params.enable_compression);

    auto debuf = new_manager.deserializeSnapshotBufferFromDisk(2);

    auto [restored_storage, snapshot_meta, _] = new_manager.deserializeSnapshotFromBuffer(debuf);

    EXPECT_EQ(restored_storage->container.size(), 6);
    EXPECT_EQ(restored_storage->container.getValue("/").getChildren().size(), 2);
    EXPECT_EQ(restored_storage->container.getValue("/hello").getChildren().size(), 1);
    EXPECT_EQ(restored_storage->container.getValue("/hello/somepath").getChildren().size(), 0);

    EXPECT_EQ(restored_storage->container.getValue("/").getData(), "");
    EXPECT_EQ(restored_storage->container.getValue("/hello").getData(), "world");
    EXPECT_EQ(restored_storage->container.getValue("/hello/somepath").getData(), "somedata");
    EXPECT_EQ(restored_storage->session_id_counter, 7);
    EXPECT_EQ(restored_storage->zxid, 2);
    EXPECT_EQ(restored_storage->ephemerals.size(), 2);
    EXPECT_EQ(restored_storage->ephemerals[3].size(), 1);
    EXPECT_EQ(restored_storage->ephemerals[1].size(), 1);
    EXPECT_EQ(restored_storage->session_and_timeout.size(), 2);
}

TEST_P(CoordinationTest, ChangelogInsertThreeTimesSmooth)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");
    {
        LOG_INFO(log, "================First time=====================");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog.append(entry);
        changelog.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog.next_slot(), 2);
        waitDurableLogs(changelog);
    }

    {
        LOG_INFO(log, "================Second time=====================");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog.append(entry);
        changelog.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog.next_slot(), 3);
        waitDurableLogs(changelog);
    }

    {
        LOG_INFO(log, "================Third time=====================");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog.append(entry);
        changelog.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog.next_slot(), 4);
        waitDurableLogs(changelog);
    }

    {
        LOG_INFO(log, "================Fourth time=====================");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog.append(entry);
        changelog.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog.next_slot(), 5);
        waitDurableLogs(changelog);
    }
}


TEST_P(CoordinationTest, ChangelogInsertMultipleTimesSmooth)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");
    for (size_t i = 0; i < 36; ++i)
    {
        LOG_INFO(log, "================First time=====================");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);
        for (size_t j = 0; j < 7; ++j)
        {
            auto entry = getLogEntry("hello_world", 7);
            changelog.append(entry);
        }
        changelog.end_of_append_batch(0, 0);
        waitDurableLogs(changelog);
    }

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    changelog.init(1, 0);
    EXPECT_EQ(changelog.next_slot(), 36 * 7 + 1);
}

TEST_P(CoordinationTest, ChangelogInsertThreeTimesHard)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");
    {
        LOG_INFO(log, "================First time=====================");
        DB::KeeperLogStore changelog1(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog1.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog1.append(entry);
        changelog1.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog1.next_slot(), 2);
        waitDurableLogs(changelog1);
    }

    {
        LOG_INFO(log, "================Second time=====================");
        DB::KeeperLogStore changelog2(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog2.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog2.append(entry);
        changelog2.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog2.next_slot(), 3);
        waitDurableLogs(changelog2);
    }

    {
        LOG_INFO(log, "================Third time=====================");
        DB::KeeperLogStore changelog3(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog3.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog3.append(entry);
        changelog3.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog3.next_slot(), 4);
        waitDurableLogs(changelog3);
    }

    {
        LOG_INFO(log, "================Fourth time=====================");
        DB::KeeperLogStore changelog4(
            DB::LogFileSettings{.force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100},
            DB::FlushSettings(),
            keeper_context);
        changelog4.init(1, 0);
        auto entry = getLogEntry("hello_world", 1000);
        changelog4.append(entry);
        changelog4.end_of_append_batch(0, 0);
        EXPECT_EQ(changelog4.next_slot(), 5);
        waitDurableLogs(changelog4);
    }
}

TEST_P(CoordinationTest, TestStorageSnapshotEqual)
{
    auto params = GetParam();
    ChangelogDirTest test("./snapshots");
    setSnapshotDirectory("./snapshots");

    std::optional<UInt128> snapshot_hash;
    for (size_t i = 0; i < 15; ++i)
    {
        DB::KeeperSnapshotManager manager(3, keeper_context, params.enable_compression);

        DB::KeeperStorage storage(500, "", keeper_context);
        addNode(storage, "/hello", "");
        for (size_t j = 0; j < 5000; ++j)
        {
            addNode(storage, "/hello_" + std::to_string(j), "world", 1);
            addNode(storage, "/hello/somepath_" + std::to_string(j), "somedata", 3);
        }

        storage.session_id_counter = 5;

        storage.ephemerals[3] = {"/hello"};
        storage.ephemerals[1] = {"/hello/somepath"};

        for (size_t j = 0; j < 3333; ++j)
            storage.getSessionID(130 * j);

        DB::KeeperStorageSnapshot snapshot(&storage, storage.zxid);

        auto buf = manager.serializeSnapshotToBuffer(snapshot);

        auto new_hash = sipHash128(reinterpret_cast<char *>(buf->data()), buf->size());
        if (!snapshot_hash.has_value())
        {
            snapshot_hash = new_hash;
        }
        else
        {
            EXPECT_EQ(*snapshot_hash, new_hash);
        }
    }
}


TEST_P(CoordinationTest, TestLogGap)
{
    using namespace Coordination;
    auto test_params = GetParam();
    ChangelogDirTest logs("./logs");
    setLogDirectory("./logs");

    DB::KeeperLogStore changelog(
        DB::LogFileSettings{.force_sync = true, .compress_logs = test_params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);

    changelog.init(0, 3);
    for (size_t i = 1; i < 55; ++i)
    {
        std::shared_ptr<ZooKeeperCreateRequest> request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = "/hello_" + std::to_string(i);
        auto entry = getLogEntryFromZKRequest(0, 1, i, request);
        changelog.append(entry);
        changelog.end_of_append_batch(0, 0);
    }

    DB::KeeperLogStore changelog1(
        DB::LogFileSettings{.force_sync = true, .compress_logs = test_params.enable_compression, .rotate_interval = 100},
        DB::FlushSettings(),
        keeper_context);
    changelog1.init(61, 3);

    /// Logs discarded
    EXPECT_FALSE(fs::exists("./logs/changelog_1_100.bin" + test_params.extension));
    EXPECT_EQ(changelog1.start_index(), 61);
    EXPECT_EQ(changelog1.next_slot(), 61);
}

template <typename ResponseType>
ResponseType getSingleResponse(const auto & responses)
{
    EXPECT_FALSE(responses.empty());
    return dynamic_cast<ResponseType &>(*responses[0].response);
}

TEST_P(CoordinationTest, TestUncommittedStateBasicCrud)
{
    using namespace DB;
    using namespace Coordination;

    DB::KeeperStorage storage{500, "", keeper_context};

    constexpr std::string_view path = "/test";

    const auto get_committed_data = [&]() -> std::optional<String>
    {
        auto request = std::make_shared<ZooKeeperGetRequest>();
        request->path = path;
        auto responses = storage.processRequest(request, 0, std::nullopt, true, true);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);

        if (get_response.error != Error::ZOK)
            return std::nullopt;

        return get_response.data;
    };

    const auto preprocess_get = [&](int64_t zxid)
    {
        auto get_request = std::make_shared<ZooKeeperGetRequest>();
        get_request->path = path;
        storage.preprocessRequest(get_request, 0, 0, zxid);
        return get_request;
    };

    const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
    create_request->path = path;
    create_request->data = "initial_data";
    storage.preprocessRequest(create_request, 0, 0, 1);
    storage.preprocessRequest(create_request, 0, 0, 2);

    ASSERT_FALSE(get_committed_data());

    const auto after_create_get = preprocess_get(3);

    ASSERT_FALSE(get_committed_data());

    const auto set_request = std::make_shared<ZooKeeperSetRequest>();
    set_request->path = path;
    set_request->data = "new_data";
    storage.preprocessRequest(set_request, 0, 0, 4);

    const auto after_set_get = preprocess_get(5);

    ASSERT_FALSE(get_committed_data());

    const auto remove_request = std::make_shared<ZooKeeperRemoveRequest>();
    remove_request->path = path;
    storage.preprocessRequest(remove_request, 0, 0, 6);
    storage.preprocessRequest(remove_request, 0, 0, 7);

    const auto after_remove_get = preprocess_get(8);

    ASSERT_FALSE(get_committed_data());

    {
        const auto responses = storage.processRequest(create_request, 0, 1);
        const auto & create_response = getSingleResponse<ZooKeeperCreateResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZOK);
    }

    {
        const auto responses = storage.processRequest(create_request, 0, 2);
        const auto & create_response = getSingleResponse<ZooKeeperCreateResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZNODEEXISTS);
    }

    {
        const auto responses = storage.processRequest(after_create_get, 0, 3);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);
        ASSERT_EQ(get_response.error, Error::ZOK);
        ASSERT_EQ(get_response.data, "initial_data");
    }

    ASSERT_EQ(get_committed_data(), "initial_data");

    {
        const auto responses = storage.processRequest(set_request, 0, 4);
        const auto & create_response = getSingleResponse<ZooKeeperSetResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZOK);
    }

    {
        const auto responses = storage.processRequest(after_set_get, 0, 5);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);
        ASSERT_EQ(get_response.error, Error::ZOK);
        ASSERT_EQ(get_response.data, "new_data");
    }

    ASSERT_EQ(get_committed_data(), "new_data");

    {
        const auto responses = storage.processRequest(remove_request, 0, 6);
        const auto & create_response = getSingleResponse<ZooKeeperRemoveResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZOK);
    }

    {
        const auto responses = storage.processRequest(remove_request, 0, 7);
        const auto & create_response = getSingleResponse<ZooKeeperRemoveResponse>(responses);
        ASSERT_EQ(create_response.error, Error::ZNONODE);
    }

    {
        const auto responses = storage.processRequest(after_remove_get, 0, 8);
        const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);
        ASSERT_EQ(get_response.error, Error::ZNONODE);
    }

    ASSERT_FALSE(get_committed_data());
}

TEST_P(CoordinationTest, TestListRequestTypes)
{
    using namespace DB;
    using namespace Coordination;

    KeeperStorage storage{500, "", keeper_context};

    int32_t zxid = 0;

    static constexpr std::string_view test_path = "/list_request_type/node";

    const auto create_path = [&](const auto & path, bool is_ephemeral, bool is_sequential = true)
    {
        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        int new_zxid = ++zxid;
        create_request->path = path;
        create_request->is_sequential = is_sequential;
        create_request->is_ephemeral = is_ephemeral;
        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);

        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to create " << path;
        const auto & create_response = dynamic_cast<ZooKeeperCreateResponse &>(*responses[0].response);
        return create_response.path_created;
    };

    create_path(parentNodePath(StringRef{test_path}).toString(), false, false);

    static constexpr size_t persistent_num = 5;
    std::unordered_set<std::string> expected_persistent_children;
    for (size_t i = 0; i < persistent_num; ++i)
    {
        expected_persistent_children.insert(getBaseNodeName(create_path(test_path, false)).toString());
    }
    ASSERT_EQ(expected_persistent_children.size(), persistent_num);

    static constexpr size_t ephemeral_num = 5;
    std::unordered_set<std::string> expected_ephemeral_children;
    for (size_t i = 0; i < ephemeral_num; ++i)
    {
        expected_ephemeral_children.insert(getBaseNodeName(create_path(test_path, true)).toString());
    }
    ASSERT_EQ(expected_ephemeral_children.size(), ephemeral_num);

    const auto get_children = [&](const auto list_request_type)
    {
        const auto list_request = std::make_shared<ZooKeeperFilteredListRequest>();
        int new_zxid = ++zxid;
        list_request->path = parentNodePath(StringRef{test_path}).toString();
        list_request->list_request_type = list_request_type;
        storage.preprocessRequest(list_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(list_request, 1, new_zxid);

        EXPECT_GE(responses.size(), 1);
        const auto & list_response = dynamic_cast<ZooKeeperListResponse &>(*responses[0].response);
        return list_response.names;
    };

    const auto persistent_children = get_children(ListRequestType::PERSISTENT_ONLY);
    EXPECT_EQ(persistent_children.size(), persistent_num);
    for (const auto & child : persistent_children)
    {
        EXPECT_TRUE(expected_persistent_children.contains(child)) << "Missing persistent child " << child;
    }

    const auto ephemeral_children = get_children(ListRequestType::EPHEMERAL_ONLY);
    EXPECT_EQ(ephemeral_children.size(), ephemeral_num);
    for (const auto & child : ephemeral_children)
    {
        EXPECT_TRUE(expected_ephemeral_children.contains(child)) << "Missing ephemeral child " << child;
    }

    const auto all_children = get_children(ListRequestType::ALL);
    EXPECT_EQ(all_children.size(), ephemeral_num + persistent_num);
    for (const auto & child : all_children)
    {
        EXPECT_TRUE(expected_ephemeral_children.contains(child) || expected_persistent_children.contains(child))
            << "Missing child " << child;
    }
}

TEST_P(CoordinationTest, TestDurableState)
{
    ChangelogDirTest logs("./logs");
    setLogDirectory("./logs");
    setStateFileDirectory(".");

    auto state = nuraft::cs_new<nuraft::srv_state>();
    std::optional<DB::KeeperStateManager> state_manager;

    const auto reload_state_manager = [&]
    {
        state_manager.emplace(1, "localhost", 9181, keeper_context);
        state_manager->loadLogStore(1, 0);
    };

    reload_state_manager();
    ASSERT_EQ(state_manager->read_state(), nullptr);

    state->set_term(1);
    state->set_voted_for(2);
    state->allow_election_timer(true);
    state_manager->save_state(*state);

    const auto assert_read_state = [&]
    {
        auto read_state = state_manager->read_state();
        ASSERT_NE(read_state, nullptr);
        ASSERT_EQ(read_state->get_term(), state->get_term());
        ASSERT_EQ(read_state->get_voted_for(), state->get_voted_for());
        ASSERT_EQ(read_state->is_election_timer_allowed(), state->is_election_timer_allowed());
    };

    assert_read_state();

    reload_state_manager();
    assert_read_state();

    {
        SCOPED_TRACE("Read from corrupted file");
        state_manager.reset();
        DB::WriteBufferFromFile write_buf("./state", DB::DBMS_DEFAULT_BUFFER_SIZE, O_WRONLY);
        write_buf.seek(20, SEEK_SET);
        DB::writeIntBinary(31, write_buf);
        write_buf.sync();
        write_buf.close();
        reload_state_manager();
#    ifdef NDEBUG
        ASSERT_EQ(state_manager->read_state(), nullptr);
#    else
        ASSERT_THROW(state_manager->read_state(), DB::Exception);
#    endif
    }

    {
        SCOPED_TRACE("Read from file with invalid size");
        state_manager.reset();

        DB::WriteBufferFromFile write_buf("./state", DB::DBMS_DEFAULT_BUFFER_SIZE, O_TRUNC | O_CREAT | O_WRONLY);
        DB::writeIntBinary(20, write_buf);
        write_buf.sync();
        write_buf.close();
        reload_state_manager();
        ASSERT_EQ(state_manager->read_state(), nullptr);
    }

    {
        SCOPED_TRACE("State file is missing");
        state_manager.reset();
        std::filesystem::remove("./state");
        reload_state_manager();
        ASSERT_EQ(state_manager->read_state(), nullptr);
    }
}

TEST_P(CoordinationTest, TestFeatureFlags)
{
    using namespace Coordination;
    KeeperStorage storage{500, "", keeper_context};
    auto request = std::make_shared<ZooKeeperGetRequest>();
    request->path = DB::keeper_api_feature_flags_path;
    auto responses = storage.processRequest(request, 0, std::nullopt, true, true);
    const auto & get_response = getSingleResponse<ZooKeeperGetResponse>(responses);
    DB::KeeperFeatureFlags feature_flags;
    feature_flags.setFeatureFlags(get_response.data);
    ASSERT_TRUE(feature_flags.isEnabled(KeeperFeatureFlag::FILTERED_LIST));
    ASSERT_TRUE(feature_flags.isEnabled(KeeperFeatureFlag::MULTI_READ));
    ASSERT_FALSE(feature_flags.isEnabled(KeeperFeatureFlag::CHECK_NOT_EXISTS));
}

TEST_P(CoordinationTest, TestSystemNodeModify)
{
    using namespace Coordination;
    int64_t zxid{0};

    // On INIT we abort when a system path is modified
    keeper_context->setServerState(KeeperContext::Phase::RUNNING);
    KeeperStorage storage{500, "", keeper_context};
    const auto assert_create = [&](const std::string_view path, const auto expected_code)
    {
        auto request = std::make_shared<ZooKeeperCreateRequest>();
        request->path = path;
        storage.preprocessRequest(request, 0, 0, zxid);
        auto responses = storage.processRequest(request, 0, zxid);
        ASSERT_FALSE(responses.empty());

        const auto & response = responses[0];
        ASSERT_EQ(response.response->error, expected_code) << "Unexpected error for path " << path;

        ++zxid;
    };

    assert_create("/keeper", Error::ZBADARGUMENTS);
    assert_create("/keeper/with_child", Error::ZBADARGUMENTS);
    assert_create(DB::keeper_api_version_path, Error::ZBADARGUMENTS);

    assert_create("/keeper_map", Error::ZOK);
    assert_create("/keeper1", Error::ZOK);
    assert_create("/keepe", Error::ZOK);
    assert_create("/keeper1/test", Error::ZOK);
}

TEST_P(CoordinationTest, ChangelogTestMaxLogSize)
{
    auto params = GetParam();
    ChangelogDirTest test("./logs");
    setLogDirectory("./logs");

    uint64_t last_entry_index{0};
    size_t i{0};
    {
        SCOPED_TRACE("Small rotation interval, big size limit");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{
                .force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 20, .max_size = 50 * 1024 * 1024},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);

        for (; i < 100; ++i)
        {
            auto entry = getLogEntry(std::to_string(i) + "_hello_world", (i + 44) * 10);
            last_entry_index = changelog.append(entry);
        }
        changelog.end_of_append_batch(0, 0);

        waitDurableLogs(changelog);

        ASSERT_EQ(changelog.entry_at(last_entry_index)->get_term(), (i - 1 + 44) * 10);
    }
    {
        SCOPED_TRACE("Large rotation interval, small size limit");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{
                .force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100'000, .max_size = 4000},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);

        ASSERT_EQ(changelog.entry_at(last_entry_index)->get_term(), (i - 1 + 44) * 10);

        for (; i < 500; ++i)
        {
            auto entry = getLogEntry(std::to_string(i) + "_hello_world", (i + 44) * 10);
            last_entry_index = changelog.append(entry);
        }
        changelog.end_of_append_batch(0, 0);

        waitDurableLogs(changelog);

        ASSERT_EQ(changelog.entry_at(last_entry_index)->get_term(), (i - 1 + 44) * 10);
    }
    {
        SCOPED_TRACE("Final verify all logs");
        DB::KeeperLogStore changelog(
            DB::LogFileSettings{
                .force_sync = true, .compress_logs = params.enable_compression, .rotate_interval = 100'000, .max_size = 4000},
            DB::FlushSettings(),
            keeper_context);
        changelog.init(1, 0);
        ASSERT_EQ(changelog.entry_at(last_entry_index)->get_term(), (i - 1 + 44) * 10);
    }
}

TEST_P(CoordinationTest, TestCheckNotExistsRequest)
{
    using namespace DB;
    using namespace Coordination;

    KeeperStorage storage{500, "", keeper_context};

    int32_t zxid = 0;

    const auto create_path = [&](const auto & path)
    {
        const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
        int new_zxid = ++zxid;
        create_request->path = path;
        storage.preprocessRequest(create_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(create_request, 1, new_zxid);

        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Coordination::Error::ZOK) << "Failed to create " << path;
    };

    const auto check_request = std::make_shared<ZooKeeperCheckRequest>();
    check_request->path = "/test_node";
    check_request->not_exists = true;

    {
        SCOPED_TRACE("CheckNotExists returns ZOK");
        int new_zxid = ++zxid;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZOK) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }

    create_path("/test_node");
    auto node_it = storage.container.find("/test_node");
    ASSERT_NE(node_it, storage.container.end());
    auto node_version = node_it->value.stat.version;

    {
        SCOPED_TRACE("CheckNotExists returns ZNODEEXISTS");
        int new_zxid = ++zxid;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZNODEEXISTS) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }

    {
        SCOPED_TRACE("CheckNotExists returns ZNODEEXISTS for same version");
        int new_zxid = ++zxid;
        check_request->version = node_version;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZNODEEXISTS) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }

    {
        SCOPED_TRACE("CheckNotExists returns ZOK for different version");
        int new_zxid = ++zxid;
        check_request->version = node_version + 1;
        storage.preprocessRequest(check_request, 1, 0, new_zxid);
        auto responses = storage.processRequest(check_request, 1, new_zxid);
        EXPECT_GE(responses.size(), 1);
        auto error = responses[0].response->error;
        EXPECT_EQ(error, Coordination::Error::ZOK) << "CheckNotExists returned invalid result: " << errorMessage(error);
    }
}

TEST_P(CoordinationTest, TestReapplyingDeltas)
{
    using namespace DB;
    using namespace Coordination;

    static constexpr int64_t initial_zxid = 100;

    const auto create_request = std::make_shared<ZooKeeperCreateRequest>();
    create_request->path = "/test/data";
    create_request->is_sequential = true;

    const auto process_create = [](KeeperStorage & storage, const auto & request, int64_t zxid)
    {
        storage.preprocessRequest(request, 1, 0, zxid);
        auto responses = storage.processRequest(request, 1, zxid);
        EXPECT_GE(responses.size(), 1);
        EXPECT_EQ(responses[0].response->error, Error::ZOK);
    };

    const auto commit_initial_data = [&](auto & storage)
    {
        int64_t zxid = 1;

        const auto root_create = std::make_shared<ZooKeeperCreateRequest>();
        root_create->path = "/test";
        process_create(storage, root_create, zxid);
        ++zxid;

        for (; zxid <= initial_zxid; ++zxid)
            process_create(storage, create_request, zxid);
    };

    KeeperStorage storage1{500, "", keeper_context};
    commit_initial_data(storage1);

    for (int64_t zxid = initial_zxid + 1; zxid < initial_zxid + 50; ++zxid)
        storage1.preprocessRequest(create_request, 1, 0, zxid, /*check_acl=*/true, /*digest=*/std::nullopt, /*log_idx=*/zxid);

    /// create identical new storage
    KeeperStorage storage2{500, "", keeper_context};
    commit_initial_data(storage2);

    storage1.applyUncommittedState(storage2, initial_zxid);

    const auto commit_unprocessed = [&](KeeperStorage & storage)
    {
        for (int64_t zxid = initial_zxid + 1; zxid < initial_zxid + 50; ++zxid)
        {
            auto responses = storage.processRequest(create_request, 1, zxid);
            EXPECT_GE(responses.size(), 1);
            EXPECT_EQ(responses[0].response->error, Error::ZOK);
        }
    };

    commit_unprocessed(storage1);
    commit_unprocessed(storage2);

    const auto get_children = [&](KeeperStorage & storage)
    {
        const auto list_request = std::make_shared<ZooKeeperListRequest>();
        list_request->path = "/test";
        auto responses = storage.processRequest(list_request, 1, std::nullopt, /*check_acl=*/true, /*is_local=*/true);
        EXPECT_EQ(responses.size(), 1);
        const auto * list_response = dynamic_cast<const ListResponse *>(responses[0].response.get());
        EXPECT_TRUE(list_response);
        return list_response->names;
    };

    auto children1 = get_children(storage1);
    std::unordered_set<std::string> children1_set(children1.begin(), children1.end());

    auto children2 = get_children(storage2);
    std::unordered_set<std::string> children2_set(children2.begin(), children2.end());

    ASSERT_TRUE(children1_set == children2_set);
}

INSTANTIATE_TEST_SUITE_P(CoordinationTestSuite,
    CoordinationTest,
    ::testing::ValuesIn(std::initializer_list<CompressionParam>{CompressionParam{true, ".zstd"}, CompressionParam{false, ""}}));

#endif
