#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "gui/file_import_dialog.hpp"

namespace {

signallab::repository::CsvImportOptions csvOptions()
{
    signallab::repository::CsvImportOptions options;
    options.delimiter = signallab::repository::CsvDelimiter::Comma;
    options.hasHeader = true;
    options.valueColumn = 1U;
    options.timeColumn = 0U;
    options.sampleRateIfNoTimeColumn = 48'000.0;
    options.metadata = {"CSV signal", "V", "input.csv"};
    return options;
}

signallab::repository::ImportPreview csvPreview()
{
    return signallab::repository::ImportPreview{
        std::filesystem::path("input.csv"),
        signallab::repository::ImportFormat::Csv,
        8U,
        2U,
        1U,
        1'000.0,
        {"time", "value"},
        {{"0.000", "1.0"}, {"0.001", "2.0"}}};
}

signallab::repository::ImportPreview wavPreview(std::size_t channels)
{
    signallab::repository::ImportPreview preview;
    preview.sourcePath = "input.wav";
    preview.format = signallab::repository::ImportFormat::Wav;
    preview.sampleCount = 16U;
    preview.columnCount = channels;
    preview.channelCount = channels;
    preview.sampleRate = 48'000.0;
    preview.columnNames = channels == 1U
        ? std::vector<std::string>{"Sample"}
        : std::vector<std::string>{"Left", "Right"};
    preview.rows = channels == 1U
        ? std::vector<std::vector<std::string>>{{"0.25"}}
        : std::vector<std::vector<std::string>>{{"0.25", "-0.25"}};
    return preview;
}

TEST(FileImportDialogTest, CollectsCsvOptionsInOneDialog)
{
    auto initial = csvOptions();
    initial.delimiter = signallab::repository::CsvDelimiter::Tab;
    initial.hasHeader = false;
    initial.valueColumn = 4U;
    initial.timeColumn.reset();
    initial.sampleRateIfNoTimeColumn = 1'234.5;

    signallab::gui::FileImportDialog dialog(
        std::filesystem::path("input.tsv"), initial);

    EXPECT_EQ(dialog.mode(), signallab::gui::FileImportDialog::Mode::Csv);
    auto* delimiter = dialog.findChild<QComboBox*>(
        QStringLiteral("importDelimiterCombo"));
    auto* header = dialog.findChild<QCheckBox*>(
        QStringLiteral("importHasHeaderCheck"));
    auto* valueColumn = dialog.findChild<QSpinBox*>(
        QStringLiteral("importValueColumnSpin"));
    auto* timeSource = dialog.findChild<QComboBox*>(
        QStringLiteral("importTimeSourceCombo"));
    auto* timeColumn = dialog.findChild<QSpinBox*>(
        QStringLiteral("importTimeColumnSpin"));
    auto* sampleRate = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("importSampleRateSpin"));
    auto* name = dialog.findChild<QLineEdit*>(
        QStringLiteral("importSignalNameEdit"));
    ASSERT_NE(delimiter, nullptr);
    ASSERT_NE(header, nullptr);
    ASSERT_NE(valueColumn, nullptr);
    ASSERT_NE(timeSource, nullptr);
    ASSERT_NE(timeColumn, nullptr);
    ASSERT_NE(sampleRate, nullptr);
    ASSERT_NE(name, nullptr);

    EXPECT_FALSE(timeColumn->isEnabled());
    EXPECT_TRUE(sampleRate->isEnabled());
    auto options = dialog.csvOptions();
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->delimiter, signallab::repository::CsvDelimiter::Tab);
    EXPECT_FALSE(options->hasHeader);
    EXPECT_EQ(options->valueColumn, 4U);
    EXPECT_FALSE(options->timeColumn.has_value());
    EXPECT_DOUBLE_EQ(options->sampleRateIfNoTimeColumn, 1'234.5);

    const std::uint64_t originalRevision = dialog.revision();
    timeSource->setCurrentIndex(0);
    timeColumn->setValue(3);
    valueColumn->setValue(5);
    name->setText(QStringLiteral("Configured once"));

    EXPECT_GT(dialog.revision(), originalRevision);
    EXPECT_TRUE(timeColumn->isEnabled());
    EXPECT_FALSE(sampleRate->isEnabled());
    options = dialog.csvOptions();
    ASSERT_TRUE(options.has_value());
    ASSERT_TRUE(options->timeColumn.has_value());
    EXPECT_EQ(*options->timeColumn, 2U);
    EXPECT_EQ(options->valueColumn, 4U);
    EXPECT_EQ(options->metadata.name, "Configured once");
    EXPECT_EQ(options->metadata.source, "input.csv");
    EXPECT_FALSE(dialog.wavOptions().has_value());
}

TEST(FileImportDialogTest, InvalidatesPreviewAndIgnoresStaleResults)
{
    signallab::gui::FileImportDialog dialog(
        std::filesystem::path("input.csv"), csvOptions());
    std::vector<std::uint64_t> requestedRevisions;
    dialog.setPreviewRequestCallback(
        [&requestedRevisions](std::uint64_t revision) {
            requestedRevisions.push_back(revision);
        });

    const std::uint64_t firstRevision = dialog.revision();
    EXPECT_TRUE(dialog.requestPreview());
    EXPECT_TRUE(dialog.isPreviewPending());
    ASSERT_EQ(requestedRevisions.size(), 1U);
    EXPECT_EQ(requestedRevisions.front(), firstRevision);
    EXPECT_TRUE(dialog.setPreview(firstRevision, csvPreview()));
    EXPECT_TRUE(dialog.isPreviewCurrent());

    auto* importButton = dialog.findChild<QPushButton*>(
        QStringLiteral("importAcceptButton"));
    auto* previewTable = dialog.findChild<QTableWidget*>(
        QStringLiteral("importPreviewTable"));
    auto* valueColumn = dialog.findChild<QSpinBox*>(
        QStringLiteral("importValueColumnSpin"));
    ASSERT_NE(importButton, nullptr);
    ASSERT_NE(previewTable, nullptr);
    ASSERT_NE(valueColumn, nullptr);
    EXPECT_TRUE(importButton->isEnabled());
    EXPECT_EQ(previewTable->rowCount(), 2);
    EXPECT_EQ(previewTable->columnCount(), 2);

    valueColumn->setValue(3);
    const std::uint64_t secondRevision = dialog.revision();
    EXPECT_GT(secondRevision, firstRevision);
    EXPECT_FALSE(dialog.isPreviewCurrent());
    EXPECT_FALSE(importButton->isEnabled());
    EXPECT_FALSE(dialog.setPreview(firstRevision, csvPreview()));
    EXPECT_FALSE(dialog.isPreviewCurrent());

    EXPECT_TRUE(dialog.requestPreview());
    ASSERT_EQ(requestedRevisions.size(), 2U);
    EXPECT_EQ(requestedRevisions.back(), secondRevision);
    EXPECT_TRUE(dialog.setPreviewError(
        secondRevision, QStringLiteral("bad CSV")));
    auto* errorLabel = dialog.findChild<QLabel*>(
        QStringLiteral("importPreviewErrorLabel"));
    ASSERT_NE(errorLabel, nullptr);
    EXPECT_TRUE(errorLabel->text().contains(QStringLiteral("bad CSV")));
    EXPECT_FALSE(importButton->isEnabled());

    EXPECT_TRUE(dialog.requestPreview());
    EXPECT_TRUE(dialog.setPreview(secondRevision, csvPreview()));
    EXPECT_TRUE(errorLabel->text().isEmpty());
    EXPECT_TRUE(importButton->isEnabled());
}

TEST(FileImportDialogTest, RejectsEqualCsvTimeAndValueColumnsInline)
{
    signallab::gui::FileImportDialog dialog(
        std::filesystem::path("input.csv"), csvOptions());
    int requestCount = 0;
    dialog.setPreviewRequestCallback(
        [&requestCount](std::uint64_t) { ++requestCount; });

    auto* valueColumn = dialog.findChild<QSpinBox*>(
        QStringLiteral("importValueColumnSpin"));
    auto* errorLabel = dialog.findChild<QLabel*>(
        QStringLiteral("importPreviewErrorLabel"));
    ASSERT_NE(valueColumn, nullptr);
    ASSERT_NE(errorLabel, nullptr);
    valueColumn->setValue(1);

    EXPECT_FALSE(dialog.requestPreview());
    EXPECT_EQ(requestCount, 0);
    EXPECT_FALSE(dialog.isPreviewPending());
    EXPECT_FALSE(errorLabel->text().isEmpty());
}

TEST(FileImportDialogTest, EnablesWavChannelsOnlyAfterCompatiblePreview)
{
    signallab::repository::WavImportOptions initial;
    initial.channel = signallab::repository::StereoChannel::Right;
    initial.metadata = {"WAV signal", "V", "input.wav"};
    signallab::gui::FileImportDialog dialog(
        std::filesystem::path("input.wav"), initial);
    dialog.setPreviewRequestCallback([](std::uint64_t) {});

    auto* channel = dialog.findChild<QComboBox*>(
        QStringLiteral("importWavChannelCombo"));
    auto* importButton = dialog.findChild<QPushButton*>(
        QStringLiteral("importAcceptButton"));
    ASSERT_NE(channel, nullptr);
    ASSERT_NE(importButton, nullptr);
    EXPECT_FALSE(channel->isEnabled());

    const std::uint64_t stereoRevision = dialog.revision();
    EXPECT_TRUE(dialog.requestPreview());
    EXPECT_TRUE(dialog.setPreview(stereoRevision, wavPreview(2U)));
    EXPECT_TRUE(channel->isEnabled());
    EXPECT_TRUE(importButton->isEnabled());
    auto options = dialog.wavOptions();
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(options->channel, signallab::repository::StereoChannel::Right);

    channel->setCurrentIndex(channel->findData(
        static_cast<int>(signallab::repository::StereoChannel::Left)));
    const std::uint64_t monoRevision = dialog.revision();
    EXPECT_GT(monoRevision, stereoRevision);
    EXPECT_FALSE(importButton->isEnabled());
    EXPECT_TRUE(dialog.requestPreview());
    EXPECT_TRUE(dialog.setPreview(monoRevision, wavPreview(1U)));
    EXPECT_FALSE(channel->isEnabled());
    EXPECT_TRUE(importButton->isEnabled());
    options = dialog.wavOptions();
    ASSERT_TRUE(options.has_value());
    EXPECT_EQ(
        options->channel, signallab::repository::StereoChannel::Average);

    EXPECT_FALSE(dialog.setPreview(monoRevision, wavPreview(3U)));
    EXPECT_FALSE(importButton->isEnabled());
}

}  // namespace
