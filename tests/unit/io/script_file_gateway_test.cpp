#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

#include "signallab/core/error.hpp"
#include "signallab/io/filesystem_script_file_gateway.hpp"
#include "signallab/repository/script_file_gateway.hpp"

namespace signallab::io {
namespace {

std::error_code createTestDirectoryLink(
    const std::filesystem::path& target,
    const std::filesystem::path& link)
{
#ifdef _WIN32
    // Directory junctions are mount-point reparse records and do not require
    // the symbolic-link privilege, so this test also runs on locked-down CI.
    struct MountPointReparseData {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
        WCHAR pathBuffer[1];
    };

    if (CreateDirectoryW(link.c_str(), nullptr) == 0) {
        return std::error_code(
            static_cast<int>(GetLastError()),
            std::system_category());
    }

    const std::wstring printName =
        std::filesystem::absolute(target).native();
    const std::wstring substituteName = L"\\??\\" + printName;
    const std::size_t substituteBytes =
        substituteName.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    const std::size_t bufferBytes =
        offsetof(MountPointReparseData, pathBuffer)
        + substituteBytes + sizeof(wchar_t)
        + printBytes + sizeof(wchar_t);
    if (bufferBytes - 8U > (std::numeric_limits<WORD>::max)()) {
        RemoveDirectoryW(link.c_str());
        return std::make_error_code(std::errc::filename_too_long);
    }

    std::vector<unsigned char> storage(bufferBytes, 0U);
    auto* data = reinterpret_cast<MountPointReparseData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength = static_cast<WORD>(bufferBytes - 8U);
    data->substituteNameOffset = 0;
    data->substituteNameLength = static_cast<WORD>(substituteBytes);
    data->printNameOffset = static_cast<WORD>(
        substituteBytes + sizeof(wchar_t));
    data->printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(data->pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(
        reinterpret_cast<unsigned char*>(data->pathBuffer)
            + data->printNameOffset,
        printName.data(),
        printBytes);

    HANDLE handle = CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = std::error_code(
            static_cast<int>(GetLastError()),
            std::system_category());
        RemoveDirectoryW(link.c_str());
        return error;
    }

    DWORD returnedBytes = 0;
    const BOOL result = DeviceIoControl(
        handle,
        FSCTL_SET_REPARSE_POINT,
        data,
        static_cast<DWORD>(bufferBytes),
        nullptr,
        0,
        &returnedBytes,
        nullptr);
    const DWORD lastError = result != 0 ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (result == 0) {
        RemoveDirectoryW(link.c_str());
        return std::error_code(
            static_cast<int>(lastError),
            std::system_category());
    }
    return {};
#else
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    return error;
#endif
}

class TestDirectoryLinkGuard {
public:
    explicit TestDirectoryLinkGuard(std::filesystem::path path)
        : path_(std::move(path))
    {
    }

    ~TestDirectoryLinkGuard()
    {
#ifdef _WIN32
        RemoveDirectoryW(path_.c_str());
#else
        std::error_code ignoredError;
        std::filesystem::remove(path_, ignoredError);
#endif
    }

    TestDirectoryLinkGuard(const TestDirectoryLinkGuard&) = delete;
    TestDirectoryLinkGuard& operator=(const TestDirectoryLinkGuard&) = delete;

private:
    std::filesystem::path path_;
};

class ScriptFileGatewayTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto timestamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
        root_ = std::filesystem::temp_directory_path()
            / ("signallab-script-file-gateway-"
               + std::to_string(timestamp) + "-"
               + std::to_string(nextId_.fetch_add(1)));
        readRoot_ = root_ / "inputs";
        outputRoot_ = root_ / "outputs";
        outsideRoot_ = root_ / "outside";
        std::filesystem::create_directories(readRoot_);
        std::filesystem::create_directories(outputRoot_);
        std::filesystem::create_directories(outsideRoot_);
        gateway_ = std::make_unique<FilesystemScriptFileGateway>(
            readRoot_,
            outputRoot_);
    }

    void TearDown() override
    {
        gateway_.reset();
        std::error_code ignoredError;
        std::filesystem::remove_all(root_, ignoredError);
    }

    static void writeFile(
        const std::filesystem::path& path,
        std::string_view contents)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output) << path;
        output.write(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
        ASSERT_TRUE(output) << path;
    }

    static std::string readFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input) << path;
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    [[nodiscard]] bool outputHasTemporaryFile() const
    {
        std::error_code error;
        std::filesystem::recursive_directory_iterator item(
            outputRoot_,
            error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && item != end) {
            const std::string name = item->path().filename().string();
            if (name.starts_with(".signallab-script-output-")
                && name.ends_with(".tmp")) {
                return true;
            }
            item.increment(error);
        }
        return false;
    }

    inline static std::atomic<unsigned int> nextId_{0};
    std::filesystem::path root_;
    std::filesystem::path readRoot_;
    std::filesystem::path outputRoot_;
    std::filesystem::path outsideRoot_;
    std::unique_ptr<FilesystemScriptFileGateway> gateway_;
};

TEST_F(ScriptFileGatewayTest, ResolvesOnlyRegularFilesInsideTheReadRoot)
{
    const std::filesystem::path input = readRoot_ / "nested" / "signal.csv";
    writeFile(input, "time,value\n0,1\n");

    repository::ScriptFileGateway& abstractGateway = *gateway_;
    const auto resolved = abstractGateway.resolveInput(
        std::filesystem::path("nested") / "signal.csv");

    ASSERT_TRUE(resolved) << resolved.error().message;
    EXPECT_EQ(resolved.value(), std::filesystem::canonical(input));
    EXPECT_TRUE(std::filesystem::is_regular_file(resolved.value()));
}

TEST_F(ScriptFileGatewayTest, RejectsUnsafeAndNonRegularInputPaths)
{
    const std::filesystem::path outsideFile = outsideRoot_ / "secret.csv";
    writeFile(outsideFile, "secret");
    std::filesystem::create_directories(readRoot_ / "folder");

    const auto empty = gateway_->resolveInput({});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, core::ErrorCode::InvalidParameter);

    const auto absolute = gateway_->resolveInput(outsideFile);
    ASSERT_FALSE(absolute);
    EXPECT_EQ(absolute.error().code, core::ErrorCode::InvalidParameter);

    const auto traversal = gateway_->resolveInput(
        std::filesystem::path("..") / "outside" / "secret.csv");
    ASSERT_FALSE(traversal);
    EXPECT_EQ(traversal.error().code, core::ErrorCode::InvalidParameter);

    const auto missing = gateway_->resolveInput("missing.csv");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, core::ErrorCode::FileNotFound);

    const auto directory = gateway_->resolveInput("folder");
    ASSERT_FALSE(directory);
    EXPECT_EQ(directory.error().code, core::ErrorCode::FileOpenFailed);
}

TEST_F(ScriptFileGatewayTest, RejectsAnInputReparsePointThatEscapesTheReadRoot)
{
    const std::filesystem::path outsideFile = outsideRoot_ / "secret.csv";
    const std::filesystem::path link = readRoot_ / "linked";
    writeFile(outsideFile, "secret");

    const std::error_code error = createTestDirectoryLink(outsideRoot_, link);
    if (error) {
        GTEST_SKIP() << "Directory links are unavailable: " << error.message();
    }
    TestDirectoryLinkGuard linkGuard(link);

    const auto result = gateway_->resolveInput(
        std::filesystem::path("linked") / "secret.csv");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
}

TEST_F(ScriptFileGatewayTest, WritesExactUtf8AndNeverOverwrites)
{
    const std::string contents =
        "frequency_hz,amplitude\n1000,1.0\n中文,有效\n";
    const std::filesystem::path relative =
        std::filesystem::path("reports") / "spectrum.csv";

    const auto first = gateway_->writeOutput(relative, contents);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first.value(), std::filesystem::canonical(first.value()));
    EXPECT_EQ(readFile(first.value()), contents);
    EXPECT_FALSE(outputHasTemporaryFile());

    const auto second = gateway_->writeOutput(relative, "replacement");
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, core::ErrorCode::FileWriteFailed);
    EXPECT_EQ(readFile(first.value()), contents);
    EXPECT_FALSE(outputHasTemporaryFile());
}

TEST_F(ScriptFileGatewayTest, RejectsUnsafeOutputPathsAndInvalidUtf8)
{
    const auto empty = gateway_->writeOutput({}, "text");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, core::ErrorCode::InvalidParameter);

    const auto absolute = gateway_->writeOutput(
        outsideRoot_ / "written.txt",
        "text");
    ASSERT_FALSE(absolute);
    EXPECT_EQ(absolute.error().code, core::ErrorCode::InvalidParameter);

    const auto traversal = gateway_->writeOutput(
        std::filesystem::path("..") / "outside" / "written.txt",
        "text");
    ASSERT_FALSE(traversal);
    EXPECT_EQ(traversal.error().code, core::ErrorCode::InvalidParameter);

    const std::string invalidUtf8{"\xc0\xaf", 2};
    const auto invalidText = gateway_->writeOutput(
        "invalid.txt",
        invalidUtf8);
    ASSERT_FALSE(invalidText);
    EXPECT_EQ(invalidText.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_FALSE(std::filesystem::exists(outputRoot_ / "invalid.txt"));
    EXPECT_FALSE(std::filesystem::exists(outsideRoot_ / "written.txt"));
    EXPECT_FALSE(outputHasTemporaryFile());
}

TEST_F(ScriptFileGatewayTest, RejectsAnOutputReparsePointThatEscapesItsRoot)
{
    const std::filesystem::path link = outputRoot_ / "escaped";
    const std::error_code error = createTestDirectoryLink(outsideRoot_, link);
    if (error) {
        GTEST_SKIP() << "Directory links are unavailable: "
                     << error.message();
    }
    TestDirectoryLinkGuard linkGuard(link);

    const auto result = gateway_->writeOutput(
        std::filesystem::path("escaped") / "result.csv",
        "value\n1\n");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, core::ErrorCode::InvalidParameter);
    EXPECT_FALSE(std::filesystem::exists(outsideRoot_ / "result.csv"));
    EXPECT_FALSE(outputHasTemporaryFile());
}

TEST_F(ScriptFileGatewayTest, ConcurrentWritersPublishExactlyOneFile)
{
    std::atomic<bool> start{false};
    std::atomic<int> successCount{0};
    std::atomic<int> failureCount{0};

    auto writer = [this, &start, &successCount, &failureCount](
                      std::string contents) {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto result = gateway_->writeOutput("race.txt", contents);
        if (result) {
            successCount.fetch_add(1, std::memory_order_relaxed);
        } else {
            EXPECT_EQ(result.error().code, core::ErrorCode::FileWriteFailed);
            failureCount.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread first(writer, "first");
    std::thread second(writer, "second");
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    EXPECT_EQ(successCount.load(), 1);
    EXPECT_EQ(failureCount.load(), 1);
    const std::string published = readFile(outputRoot_ / "race.txt");
    EXPECT_TRUE(published == "first" || published == "second");
    EXPECT_FALSE(outputHasTemporaryFile());
}

TEST_F(ScriptFileGatewayTest, CancellationLeavesNoOutputOrTemporaryFile)
{
    std::stop_source beforeStart;
    beforeStart.request_stop();
    const auto immediatelyCancelled = gateway_->writeOutput(
        "cancelled-before.txt",
        "text",
        beforeStart.get_token());
    ASSERT_FALSE(immediatelyCancelled);
    EXPECT_EQ(
        immediatelyCancelled.error().code,
        core::ErrorCode::Cancelled);

    // A large payload and a watcher synchronize cancellation with the
    // existence of the temporary file, exercising cleanup during block I/O.
    const std::string largeText(32U * 1024U * 1024U, 'x');
    std::stop_source duringWrite;
    std::atomic<bool> writingFinished{false};
    std::atomic<bool> sawTemporaryFile{false};
    std::thread watcher([this, &duringWrite, &writingFinished,
                         &sawTemporaryFile]() {
        while (!writingFinished.load(std::memory_order_acquire)) {
            if (outputHasTemporaryFile()) {
                sawTemporaryFile.store(true, std::memory_order_release);
                duringWrite.request_stop();
                return;
            }
            std::this_thread::yield();
        }
    });

    const auto cancelled = gateway_->writeOutput(
        "cancelled-during.txt",
        largeText,
        duringWrite.get_token());
    writingFinished.store(true, std::memory_order_release);
    watcher.join();

    ASSERT_TRUE(sawTemporaryFile.load(std::memory_order_acquire))
        << "The test could not observe the temporary output.";
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, core::ErrorCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(
        outputRoot_ / "cancelled-before.txt"));
    EXPECT_FALSE(std::filesystem::exists(
        outputRoot_ / "cancelled-during.txt"));
    EXPECT_FALSE(outputHasTemporaryFile());
}

}  // namespace
}  // namespace signallab::io
