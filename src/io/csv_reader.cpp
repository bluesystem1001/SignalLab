#include "signallab/io/csv_reader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace signallab::io {
namespace {

struct ParsedCsv {
    std::size_t columnCount{0};
    std::size_t sampleCount{0};
    double sampleRate{0.0};
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> previewRows;
    std::vector<double> samples;
};

struct TimeValidationState {
    std::size_t count{0};
    std::size_t firstLineNumber{0};
    std::size_t lastLineNumber{0};
    double firstTime{0.0};
    double previousTime{0.0};
    double averageInterval{0.0};
};

std::string errorMessage(
    const std::filesystem::path& path,
    std::size_t row,
    std::size_t column,
    const std::string& detail)
{
    return path.string()
        + " (row " + std::to_string(row)
        + ", column " + std::to_string(column)
        + "): " + detail;
}

bool isBlankLine(const std::string& line)
{
    return std::all_of(
        line.begin(),
        line.end(),
        [](unsigned char character) {
            return std::isspace(character) != 0;
        });
}

std::string trim(const std::string& text)
{
    const auto first = std::find_if_not(
        text.begin(),
        text.end(),
        [](unsigned char character) {
            return std::isspace(character) != 0;
        });

    const auto last = std::find_if_not(
        text.rbegin(),
        text.rend(),
        [](unsigned char character) {
            return std::isspace(character) != 0;
        }).base();

    if (first >= last) {
        return {};
    }

    return {first, last};
}

char delimiterCharacter(CsvDelimiter delimiter)
{
    if (delimiter == CsvDelimiter::Tab) {
        return '\t';
    }

    return ',';
}

core::Result<std::vector<std::string>> parseLine(
    const std::string& line,
    char delimiter,
    const std::filesystem::path& path,
    std::size_t row)
{
    std::vector<std::string> fields;
    std::string field;
    bool insideQuotes = false;
    bool closedQuote = false;
    std::size_t column = 1;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];

        if (insideQuotes) {
            if (character == '"') {
                const bool hasEscapedQuote =
                    index + 1 < line.size() && line[index + 1] == '"';

                if (hasEscapedQuote) {
                    field.push_back('"');
                    ++index;
                } else {
                    insideQuotes = false;
                    closedQuote = true;
                }
            } else {
                field.push_back(character);
            }

            continue;
        }

        if (character == delimiter) {
            fields.push_back(field);
            field.clear();
            closedQuote = false;
            ++column;
            continue;
        }

        if (character == '"') {
            if (!field.empty() || closedQuote) {
                return core::Result<std::vector<std::string>>::failure(
                    core::ErrorCode::InvalidCsv,
                    errorMessage(path, row, column, "unexpected quote."));
            }

            insideQuotes = true;
            continue;
        }

        if (closedQuote) {
            if (std::isspace(static_cast<unsigned char>(character)) != 0) {
                continue;
            }

            return core::Result<std::vector<std::string>>::failure(
                core::ErrorCode::InvalidCsv,
                errorMessage(
                    path,
                    row,
                    column,
                    "unexpected character after a quoted field."));
        }

        field.push_back(character);
    }

    if (insideQuotes) {
        return core::Result<std::vector<std::string>>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(path, row, column, "unterminated quoted field."));
    }

    fields.push_back(field);
    return core::Result<std::vector<std::string>>::success(std::move(fields));
}

core::Result<double> parseNumber(
    const std::string& field,
    const std::filesystem::path& path,
    std::size_t row,
    std::size_t column)
{
    const std::string cleaned = trim(field);
    if (cleaned.empty()) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(path, row, column, "expected a number, but the field is empty."));
    }

    try {
        std::size_t parsedCharacterCount = 0;
        const double value = std::stod(cleaned, &parsedCharacterCount);

        if (parsedCharacterCount != cleaned.size() || !std::isfinite(value)) {
            return core::Result<double>::failure(
                core::ErrorCode::InvalidCsv,
                errorMessage(path, row, column, "expected a finite number."));
        }

        return core::Result<double>::success(value);
    } catch (...) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(path, row, column, "expected a finite number."));
    }
}

core::Result<bool> validateColumnOptions(
    const CsvImportOptions& options,
    std::size_t columnCount,
    const std::filesystem::path& path,
    std::size_t row)
{
    if (options.valueColumn >= columnCount) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(
                path,
                row,
                options.valueColumn + 1,
                "the selected value column does not exist."));
    }

    if (options.timeColumn && *options.timeColumn >= columnCount) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(
                path,
                row,
                *options.timeColumn + 1,
                "the selected time column does not exist."));
    }

    if (options.timeColumn && *options.timeColumn == options.valueColumn) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(
                path,
                row,
                options.valueColumn + 1,
                "the time and value columns must be different."));
    }

    if (!options.timeColumn
        && (!std::isfinite(options.sampleRateIfNoTimeColumn)
            || options.sampleRateIfNoTimeColumn <= 0.0)) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidSampleRate,
            errorMessage(
                path,
                row,
                options.valueColumn + 1,
                "the explicit sample rate must be finite and greater than zero."));
    }

    return core::Result<bool>::success(true);
}

core::Result<bool> addTimeValue(
    TimeValidationState& state,
    double time,
    const std::filesystem::path& path,
    std::size_t row,
    std::size_t column)
{
    if (state.count == 0) {
        state.count = 1;
        state.firstLineNumber = row;
        state.lastLineNumber = row;
        state.firstTime = time;
        state.previousTime = time;
        return core::Result<bool>::success(true);
    }

    const double interval = time - state.previousTime;
    if (interval <= 0.0) {
        return core::Result<bool>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(path, row, column, "time values must be strictly increasing."));
    }

    if (state.count == 1) {
        state.averageInterval = interval;
    } else {
        // Six-decimal 44.1 kHz timestamps alternate between 22 and 23 us.
        // A 5% tolerance accepts that rounding while rejecting clear gaps.
        constexpr double relativeTolerance = 0.05;
        const double intervalScale =
            std::max(std::abs(state.averageInterval), std::abs(interval));
        const double tolerance =
            std::max(intervalScale * relativeTolerance, 1.0e-12);

        if (std::abs(interval - state.averageInterval) > tolerance) {
            return core::Result<bool>::failure(
                core::ErrorCode::InvalidCsv,
                errorMessage(
                    path,
                    row,
                    column,
                    "time values must be approximately equally spaced."));
        }

        const double intervalCount = static_cast<double>(state.count);
        state.averageInterval +=
            (interval - state.averageInterval) / intervalCount;
    }

    ++state.count;
    state.lastLineNumber = row;
    state.previousTime = time;
    return core::Result<bool>::success(true);
}

core::Result<double> inferSampleRate(
    const TimeValidationState& state,
    const std::filesystem::path& path,
    std::size_t column)
{
    if (state.count < 2) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(
                path,
                state.firstLineNumber,
                column,
                "at least two time values are required to infer a sample rate."));
    }

    const double averageInterval =
        (state.previousTime - state.firstTime)
        / static_cast<double>(state.count - 1);
    const double sampleRate = 1.0 / averageInterval;

    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return core::Result<double>::failure(
            core::ErrorCode::InvalidSampleRate,
            errorMessage(
                path,
                state.lastLineNumber,
                column,
                "the time values do not produce a valid sample rate."));
    }

    return core::Result<double>::success(sampleRate);
}

core::Result<ParsedCsv> parseCsv(
    const std::filesystem::path& path,
    const CsvImportOptions& options,
    bool keepSamples,
    std::size_t previewRowLimit)
{
    std::error_code fileSystemError;
    const bool fileExists = std::filesystem::exists(path, fileSystemError);

    if (fileSystemError) {
        return core::Result<ParsedCsv>::failure(
            core::ErrorCode::FileOpenFailed,
            errorMessage(path, 1, 1, "could not inspect the file."));
    }

    if (!fileExists) {
        return core::Result<ParsedCsv>::failure(
            core::ErrorCode::FileNotFound,
            errorMessage(path, 1, 1, "file does not exist."));
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<ParsedCsv>::failure(
            core::ErrorCode::FileOpenFailed,
            errorMessage(path, 1, 1, "could not open the file."));
    }

    ParsedCsv parsed;
    TimeValidationState timeState;
    std::string line;
    std::size_t lineNumber = 0;
    std::size_t headerLineNumber = 0;
    bool hasColumnLayout = false;
    bool optionsValidated = false;
    const char delimiter = delimiterCharacter(options.delimiter);

    while (std::getline(input, line)) {
        ++lineNumber;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (lineNumber == 1 && line.starts_with("\xEF\xBB\xBF")) {
            line.erase(0, 3);
        }

        if (isBlankLine(line)) {
            continue;
        }

        auto fieldsResult = parseLine(line, delimiter, path, lineNumber);
        if (!fieldsResult) {
            return core::Result<ParsedCsv>::failure(fieldsResult.error());
        }

        std::vector<std::string> fields = std::move(fieldsResult).value();

        if (!hasColumnLayout) {
            hasColumnLayout = true;
            parsed.columnCount = fields.size();

            if (options.hasHeader) {
                headerLineNumber = lineNumber;
                parsed.columnNames.reserve(parsed.columnCount);
                for (const std::string& name : fields) {
                    parsed.columnNames.push_back(trim(name));
                }
                continue;
            }

            parsed.columnNames.reserve(parsed.columnCount);
            for (std::size_t index = 0; index < parsed.columnCount; ++index) {
                parsed.columnNames.push_back("Column " + std::to_string(index + 1));
            }
        }

        if (fields.size() != parsed.columnCount) {
            const std::size_t errorColumn =
                std::min(fields.size(), parsed.columnCount) + 1;
            return core::Result<ParsedCsv>::failure(
                core::ErrorCode::InvalidCsv,
                errorMessage(
                    path,
                    lineNumber,
                    errorColumn,
                    "the row has a different number of columns."));
        }

        if (!optionsValidated) {
            auto optionsResult = validateColumnOptions(
                options,
                parsed.columnCount,
                path,
                lineNumber);
            if (!optionsResult) {
                return core::Result<ParsedCsv>::failure(optionsResult.error());
            }
            optionsValidated = true;
        }

        auto sampleResult = parseNumber(
            fields[options.valueColumn],
            path,
            lineNumber,
            options.valueColumn + 1);
        if (!sampleResult) {
            return core::Result<ParsedCsv>::failure(sampleResult.error());
        }

        if (options.timeColumn) {
            auto timeResult = parseNumber(
                fields[*options.timeColumn],
                path,
                lineNumber,
                *options.timeColumn + 1);
            if (!timeResult) {
                return core::Result<ParsedCsv>::failure(timeResult.error());
            }

            auto timeValidationResult = addTimeValue(
                timeState,
                timeResult.value(),
                path,
                lineNumber,
                *options.timeColumn + 1);
            if (!timeValidationResult) {
                return core::Result<ParsedCsv>::failure(
                    timeValidationResult.error());
            }
        }

        if (keepSamples) {
            parsed.samples.push_back(sampleResult.value());
        }

        if (parsed.previewRows.size() < previewRowLimit) {
            parsed.previewRows.push_back(std::move(fields));
        }

        ++parsed.sampleCount;
    }

    if (input.bad()) {
        return core::Result<ParsedCsv>::failure(
            core::ErrorCode::FileOpenFailed,
            errorMessage(path, lineNumber + 1, 1, "failed while reading the file."));
    }

    if (!hasColumnLayout) {
        return core::Result<ParsedCsv>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(path, 1, 1, "the CSV file contains no data."));
    }

    if (parsed.sampleCount == 0) {
        return core::Result<ParsedCsv>::failure(
            core::ErrorCode::InvalidCsv,
            errorMessage(
                path,
                headerLineNumber + 1,
                1,
                "the CSV file contains no sample rows."));
    }

    if (options.timeColumn) {
        auto sampleRateResult = inferSampleRate(
            timeState,
            path,
            *options.timeColumn + 1);
        if (!sampleRateResult) {
            return core::Result<ParsedCsv>::failure(sampleRateResult.error());
        }
        parsed.sampleRate = sampleRateResult.value();
    } else {
        parsed.sampleRate = options.sampleRateIfNoTimeColumn;
    }

    return core::Result<ParsedCsv>::success(std::move(parsed));
}

}  // namespace

core::Result<ImportPreview> previewCsv(
    const std::filesystem::path& path,
    const CsvImportOptions& options,
    std::size_t maxRows)
{
    auto parsedResult = parseCsv(path, options, false, maxRows);
    if (!parsedResult) {
        return core::Result<ImportPreview>::failure(parsedResult.error());
    }

    ParsedCsv parsed = std::move(parsedResult).value();

    ImportPreview preview;
    preview.sourcePath = path;
    preview.format = ImportFormat::Csv;
    preview.sampleCount = parsed.sampleCount;
    preview.columnCount = parsed.columnCount;
    preview.channelCount = 1;
    preview.sampleRate = parsed.sampleRate;
    preview.columnNames = std::move(parsed.columnNames);

    preview.rows = std::move(parsed.previewRows);

    return core::Result<ImportPreview>::success(std::move(preview));
}

core::Result<core::Signal> readCsv(
    const std::filesystem::path& path,
    const CsvImportOptions& options)
{
    auto parsedResult = parseCsv(path, options, true, 0);
    if (!parsedResult) {
        return core::Result<core::Signal>::failure(parsedResult.error());
    }

    ParsedCsv parsed = std::move(parsedResult).value();
    auto signalResult = core::Signal::create(
        std::move(parsed.samples),
        parsed.sampleRate,
        options.metadata);

    if (!signalResult) {
        return core::Result<core::Signal>::failure(
            signalResult.error().code,
            errorMessage(path, 1, 1, signalResult.error().message));
    }

    return signalResult;
}

}  // namespace signallab::io
