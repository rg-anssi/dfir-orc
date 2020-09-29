//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//

#include "stdafx.h"

#include "GetThis.h"

#include "LogFileWriter.h"
#include "TableOutput.h"
#include "CsvFileWriter.h"
#include "ConfigFileReader.h"
#include "FileFind.h"
#include "ByteStream.h"
#include "FileStream.h"
#include "MemoryStream.h"
#include "TemporaryStream.h"
#include "DevNullStream.h"
#include "StringsStream.h"
#include "CryptoHashStream.h"
#include "ParameterCheck.h"
#include "ArchiveExtract.h"

#include "SnapshotVolumeReader.h"

#include "SystemDetails.h"

#include <string>
#include <memory>
#include <filesystem>
#include <sstream>

using namespace std;
namespace fs = std::filesystem;

using namespace Orc;
using namespace Orc::Command::GetThis;

namespace {

enum class CompressorFlags : uint32_t
{
    kNone = 0,
    kComputeHash = 1
};

std::shared_ptr<ArchiveCreate>
CreateCompressor(const OutputSpec& outputSpec, CompressorFlags flags, HRESULT& hr, logger& _L_)
{
    const bool computeHash = (static_cast<uint32_t>(flags) & static_cast<uint32_t>(CompressorFlags::kComputeHash));

    auto compressor = ArchiveCreate::MakeCreate(outputSpec.ArchiveFormat, _L_, computeHash);
    if (compressor == nullptr)
    {
        hr = E_POINTER;
        log::Error(_L_, hr, L"Failed calling MakeCreate for archive '%s'\r\n", outputSpec.Path.c_str());
        return nullptr;
    }

    hr = compressor->InitArchive(outputSpec.Path);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to initialize archive '%s'\r\n", outputSpec.Path.c_str());
        return nullptr;
    }

    if (!outputSpec.Password.empty())
    {
        hr = compressor->SetPassword(outputSpec.Password);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed to set password for '%s'\r\n", outputSpec.Path.c_str());
            return nullptr;
        }
    }

    hr = compressor->SetCompressionLevel(outputSpec.Compression);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to set compression level for '%s'\r\n", outputSpec.Path.c_str());
        return nullptr;
    }

    compressor->SetCallback(
        [&_L_](const OrcArchive::ArchiveItem& item) { log::Info(_L_, L"\t%s\r\n", item.Path.c_str()); });

    return compressor;
}

std::shared_ptr<TableOutput::IStreamWriter> CreateCsvWriter(
    const std::filesystem::path& out,
    const Orc::TableOutput::Schema& schema,
    const OutputSpec::Encoding& encoding,
    HRESULT& hr)
{
    auto csvStream = std::make_shared<TemporaryStream>();

    hr = csvStream->Open(out.parent_path(), out.filename(), 1 * 1024 * 1024);
    if (FAILED(hr))
    {
        Log::Error(L"Failed to create temp stream (code: {:#x})", hr);
        return nullptr;
    }

    auto options = std::make_unique<TableOutput::CSV::Options>();
    options->Encoding = encoding;

    auto csvWriter = TableOutput::CSV::Writer::MakeNew(std::move(options));
    hr = csvWriter->WriteToStream(csvStream);
    if (FAILED(hr))
    {
        Log::Error(L"Failed to initialize CSV stream (code: {:#x})", hr);
        return nullptr;
    }

    hr = csvWriter->SetSchema(schema);
    if (FAILED(hr))
    {
        Log::Error(L"Failed to set CSV schema (code: {:#x})", hr);
        return nullptr;
    }

    return csvWriter;
}

std::shared_ptr<TemporaryStream> CreateLogStream(const std::filesystem::path& out, HRESULT& hr, logger& _L_)
{
    auto logWriter = std::make_shared<LogFileWriter>(0x1000);
    logWriter->SetConsoleLog(_L_->ConsoleLog());
    logWriter->SetDebugLog(_L_->DebugLog());
    logWriter->SetVerboseLog(_L_->VerboseLog());

    auto logStream = std::make_shared<TemporaryStream>(logWriter);

    hr = logStream->Open(out.parent_path(), out.filename(), 5 * 1024 * 1024);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to create temp stream\r\n");
        return nullptr;
    }

    hr = _L_->LogToStream(logStream);
    if (FAILED(hr))
    {
        log::Error(_L_, hr, L"Failed to initialize temp logging\r\n");
        return nullptr;
    }

    return logStream;
}

}  // namespace

std::pair<HRESULT, std::shared_ptr<TableOutput::IWriter>> Main::CreateOutputDirLogFileAndCSV(const fs::path& outDir)
{
    HRESULT hr = E_FAIL;
    std::error_code ec;

    fs::create_directories(outDir, ec);
    if (ec)
    {
        hr = HRESULT_FROM_WIN32(ec.value());
        _L_->Error(hr, L"Failed to create output directory");
        return {hr, nullptr};
    }

    if (!_L_->IsLoggingToFile())
    {
        const fs::path logFile = outDir / L"GetThis.log";
        hr = _L_->LogToFile(logFile.c_str());
        if (FAILED(hr))
        {
            return {hr, nullptr};
        }
    }

    auto options = std::make_unique<TableOutput::CSV::Options>();
    options->Encoding = config.Output.OutputEncoding;

    auto csvStream = TableOutput::CSV::Writer::MakeNew(_L_, std::move(options));
    const fs::path csvPath = outDir / L"GetThis.csv";
    hr = csvStream->WriteToFile(csvPath);
    if (FAILED(hr))
    {
        return {hr, nullptr};
    }

    csvStream->SetSchema(config.Output.Schema);

    return {hr, csvStream};
}

HRESULT Main::RegFlushKeys()
{
    bool bSuccess = true;
    DWORD dwGLE = 0L;

    log::Info(_L_, L"\r\nFlushing HKEY_LOCAL_MACHINE\r\n");
    dwGLE = RegFlushKey(HKEY_LOCAL_MACHINE);
    if (dwGLE != ERROR_SUCCESS)
        bSuccess = false;

    log::Info(_L_, L"Flushing HKEY_USERS\r\n");
    dwGLE = RegFlushKey(HKEY_USERS);
    if (dwGLE != ERROR_SUCCESS)
        bSuccess = false;

    if (!bSuccess)
        return HRESULT_FROM_WIN32(dwGLE);
    return S_OK;
}

HRESULT Main::CreateSampleFileName(
    const ContentSpec& content,
    const PFILE_NAME pFileName,
    const wstring& DataName,
    DWORD idx,
    wstring& SampleFileName)
{
    if (pFileName == NULL)
        return E_POINTER;

    WCHAR tmpName[MAX_PATH];
    WCHAR* pContent = NULL;

    switch (content.Type)
    {
        case ContentType::DATA:
            pContent = L"data";
            break;
        case ContentType::STRINGS:
            pContent = L"strings";
            break;
        case ContentType::RAW:
            pContent = L"raw";
            break;
        default:
            pContent = L"";
            break;
    }

    if (idx)
    {
        if (DataName.size())
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X_%.*s_%.*s_%u_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                (UINT)DataName.size(),
                DataName.c_str(),
                idx,
                pContent);
        else
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X__%.*s_%u_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                idx,
                pContent);
    }
    else
    {
        if (DataName.size())
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X__%.*s_%.*s_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                (UINT)DataName.size(),
                DataName.c_str(),
                pContent);
        else
        {
            swprintf_s(
                tmpName,
                MAX_PATH,
                L"%*.*X%*.*X%*.*X_%.*s_%s",
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                (int)sizeof(pFileName->ParentDirectory.SequenceNumber) * 2,
                pFileName->ParentDirectory.SequenceNumber,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberHighPart) * 2,
                pFileName->ParentDirectory.SegmentNumberHighPart,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                (int)sizeof(pFileName->ParentDirectory.SegmentNumberLowPart) * 2,
                pFileName->ParentDirectory.SegmentNumberLowPart,
                pFileName->FileNameLength,
                pFileName->FileName,
                pContent);
        }
    }

    SampleFileName.assign(tmpName);

    std::for_each(SampleFileName.begin(), SampleFileName.end(), [=](WCHAR& Item) {
        if (iswspace(Item) || Item == L':' || Item == L'#')
            Item = L'_';
    });

    return S_OK;
}

HRESULT Main::ConfigureSampleStreams(SampleRef& sampleRef)
{
    HRESULT hr = E_FAIL;

    shared_ptr<ByteStream> retval;

    _ASSERT(sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream->IsOpen() == S_OK);

    if (sampleRef.SampleName.empty())
        return E_INVALIDARG;

    std::shared_ptr<ByteStream> stream;

    switch (sampleRef.Content.Type)
    {
        case ContentType::DATA:
            stream = sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream;
            break;
        case ContentType::STRINGS: {
            auto strings = std::make_shared<StringsStream>(_L_);
            if (sampleRef.Content.MaxChars == 0 && sampleRef.Content.MinChars == 0)
            {
                if (FAILED(
                        hr = strings->OpenForStrings(
                            sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream,
                            config.content.MinChars,
                            config.content.MaxChars)))
                {
                    log::Error(_L_, hr, L"Failed to initialise strings stream\r\n");
                    return hr;
                }
            }
            else
            {
                if (FAILED(
                        hr = strings->OpenForStrings(
                            sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream,
                            sampleRef.Content.MinChars,
                            sampleRef.Content.MaxChars)))
                {
                    log::Error(_L_, hr, L"Failed to initialise strings stream\r\n");
                    return hr;
                }
            }
            stream = strings;
        }
        break;
        case ContentType::RAW:
            stream = sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].RawStream;
            break;
        default:
            stream = sampleRef.Matches.front()->MatchingAttributes[sampleRef.AttributeIndex].DataStream;
            break;
    }

    std::shared_ptr<ByteStream> upstream = stream;

    CryptoHashStream::Algorithm algs = config.CryptoHashAlgs;

    if (algs != CryptoHashStream::Algorithm::Undefined)
    {
        sampleRef.HashStream = make_shared<CryptoHashStream>(_L_);
        if (FAILED(hr = sampleRef.HashStream->OpenToRead(algs, upstream)))
            return hr;
        upstream = sampleRef.HashStream;
    }
    else
    {
        upstream = stream;
    }

    FuzzyHashStream::Algorithm fuzzy_algs = config.FuzzyHashAlgs;
    if (fuzzy_algs != FuzzyHashStream::Algorithm::Undefined)
    {
        sampleRef.FuzzyHashStream = make_shared<FuzzyHashStream>(_L_);
        if (FAILED(hr = sampleRef.FuzzyHashStream->OpenToRead(fuzzy_algs, upstream)))
            return hr;
        upstream = sampleRef.FuzzyHashStream;
    }

    sampleRef.CopyStream = upstream;
    sampleRef.SampleSize = sampleRef.CopyStream->GetSize();
    return S_OK;
}

LimitStatus Main::SampleLimitStatus(const Limits& GlobalLimits, const Limits& LocalLimits, DWORDLONG DataSize)
{
    if (GlobalLimits.bIgnoreLimits)
        return NoLimits;

    // Sample count reached?

    if (GlobalLimits.dwMaxSampleCount != INFINITE)
        if (GlobalLimits.dwAccumulatedSampleCount >= GlobalLimits.dwMaxSampleCount)
            return GlobalSampleCountLimitReached;

    if (LocalLimits.dwMaxSampleCount != INFINITE)
        if (LocalLimits.dwAccumulatedSampleCount >= LocalLimits.dwMaxSampleCount)
            return LocalSampleCountLimitReached;

    //  Global limits
    if (GlobalLimits.dwlMaxBytesPerSample != INFINITE)
        if (DataSize > GlobalLimits.dwlMaxBytesPerSample)
            return GlobalMaxBytesPerSample;

    if (GlobalLimits.dwlMaxBytesTotal != INFINITE)
        if (DataSize + GlobalLimits.dwlAccumulatedBytesTotal > GlobalLimits.dwlMaxBytesTotal)
            return GlobalMaxBytesTotal;

    // Local limits  bytes are now collected?
    if (LocalLimits.dwlMaxBytesPerSample != INFINITE)
        if (DataSize > LocalLimits.dwlMaxBytesPerSample)
            return LocalMaxBytesPerSample;
    if (LocalLimits.dwlMaxBytesTotal != INFINITE)
        if (DataSize + LocalLimits.dwlAccumulatedBytesTotal > LocalLimits.dwlMaxBytesTotal)
            return LocalMaxBytesTotal;

    return SampleWithinLimits;
}

HRESULT
Main::AddSampleRefToCSV(ITableOutput& output, const std::wstring& strComputerName, const Main::SampleRef& sample)
{
    static const FlagsDefinition AttrTypeDefs[] = {
        {$UNUSED, L"$UNUSED", L"$UNUSED"},
        {$STANDARD_INFORMATION, L"$STANDARD_INFORMATION", L"$STANDARD_INFORMATION"},
        {$ATTRIBUTE_LIST, L"$ATTRIBUTE_LIST", L"$ATTRIBUTE_LIST"},
        {$FILE_NAME, L"$FILE_NAME", L"$FILE_NAME"},
        {$OBJECT_ID, L"$OBJECT_ID", L"$OBJECT_ID"},
        {$SECURITY_DESCRIPTOR, L"$SECURITY_DESCRIPTOR", L"$SECURITY_DESCRIPTOR"},
        {$VOLUME_NAME, L"$VOLUME_NAME", L"$VOLUME_NAME"},
        {$VOLUME_INFORMATION, L"$VOLUME_INFORMATION", L"$VOLUME_INFORMATION"},
        {$DATA, L"$DATA", L"$DATA"},
        {$INDEX_ROOT, L"$INDEX_ROOT", L"$INDEX_ROOT"},
        {$INDEX_ALLOCATION, L"$INDEX_ALLOCATION", L"$INDEX_ALLOCATION"},
        {$BITMAP, L"$BITMAP", L"$BITMAP"},
        {$REPARSE_POINT, L"$REPARSE_POINT", L"$REPARSE_POINT"},
        {$EA_INFORMATION, L"$EA_INFORMATION", L"$EA_INFORMATION"},
        {$EA, L"$EA", L"$EA"},
        {$LOGGED_UTILITY_STREAM, L"$LOGGED_UTILITY_STREAM", L"$LOGGED_UTILITY_STREAM"},
        {$FIRST_USER_DEFINED_ATTRIBUTE, L"$FIRST_USER_DEFINED_ATTRIBUTE", L"$FIRST_USER_DEFINED_ATTRIBUTE"},
        {$END, L"$END", L"$END"}};

    for (const auto& match : sample.Matches)
    {
        for (const auto& name : match->MatchingNames)
        {
            output.WriteString(strComputerName.c_str());

            output.WriteInteger(match->VolumeReader->VolumeSerialNumber());

            {
                LARGE_INTEGER* pLI = (LARGE_INTEGER*)&(name.FILENAME()->ParentDirectory);
                output.WriteInteger((DWORDLONG)pLI->QuadPart);
            }
            {
                LARGE_INTEGER* pLI = (LARGE_INTEGER*)&(match->FRN);
                output.WriteInteger((DWORDLONG)pLI->QuadPart);
            }

            output.WriteString(name.FullPathName);

            if (sample.OffLimits)
            {
                output.WriteNothing();
            }
            else
            {
                output.WriteString(sample.SampleName);
            }

            output.WriteFileSize(sample.SampleSize);

            output.WriteBytes(sample.MD5);

            output.WriteBytes(sample.SHA1);

            output.WriteString(match->Term->GetDescription());

            switch (sample.Content.Type)
            {
                case ContentType::DATA:
                    output.WriteString(L"data");
                    break;
                case ContentType::STRINGS:
                    output.WriteString(L"strings");
                    break;
                default:
                    output.WriteNothing();
            }

            output.WriteFileTime(sample.CollectionDate);

            output.WriteFileTime(match->StandardInformation->CreationTime);
            output.WriteFileTime(match->StandardInformation->LastModificationTime);
            output.WriteFileTime(match->StandardInformation->LastAccessTime);
            output.WriteFileTime(match->StandardInformation->LastChangeTime);

            output.WriteFileTime(name.FILENAME()->Info.CreationTime);
            output.WriteFileTime(name.FILENAME()->Info.LastModificationTime);
            output.WriteFileTime(name.FILENAME()->Info.LastAccessTime);
            output.WriteFileTime(name.FILENAME()->Info.LastChangeTime);

            output.WriteExactFlags(match->MatchingAttributes[sample.AttributeIndex].Type, AttrTypeDefs);

            output.WriteString(match->MatchingAttributes[sample.AttributeIndex].AttrName);

            output.WriteInteger((DWORD)sample.InstanceID);

            output.WriteGUID(sample.SnapshotID);

            output.WriteBytes(sample.SHA256);

            output.WriteBytes(sample.SSDeep);

            output.WriteBytes(sample.TLSH);

            const auto& rules = match->MatchingAttributes[sample.AttributeIndex].YaraRules;
            if (rules.has_value())
            {
                std::stringstream aStream;
                const char* const delim = "; ";

                std::copy(
                    std::cbegin(rules.value()),
                    std::cend(rules.value()),
                    std::ostream_iterator<std::string>(aStream, delim));

                output.WriteString(aStream.str());
            }
            else
            {
                output.WriteNothing();
            }

            output.WriteEndOfLine();
        }
    }

    return S_OK;
}

HRESULT
Main::AddSamplesForMatch(LimitStatus status, const SampleSpec& aSpec, const std::shared_ptr<FileFind::Match>& aMatch)
{
    HRESULT hr = E_FAIL;
    size_t sIndex = 0;

    for (const auto& anAttr : aMatch->MatchingAttributes)
    {
        SampleRef sampleRef;
        sampleRef.Matches.push_back(aMatch);

        sampleRef.VolumeSerial = aMatch->VolumeReader->VolumeSerialNumber();

        auto pSnapshotReader = std::dynamic_pointer_cast<SnapshotVolumeReader>(aMatch->VolumeReader);

        if (pSnapshotReader)
        {
            sampleRef.SnapshotID = pSnapshotReader->GetSnapshotID();
        }
        else
        {
            sampleRef.SnapshotID = GUID_NULL;
        }

        sampleRef.FRN = aMatch->FRN;
        sampleRef.InstanceID = anAttr.InstanceID;
        sampleRef.AttributeIndex = sIndex++;

        switch (status)
        {
            case NoLimits:
            case SampleWithinLimits:
                sampleRef.OffLimits = false;
                break;
            case GlobalSampleCountLimitReached:
            case GlobalMaxBytesPerSample:
            case GlobalMaxBytesTotal:
            case LocalSampleCountLimitReached:
            case LocalMaxBytesPerSample:
            case LocalMaxBytesTotal:
            case FailedToComputeLimits:
                sampleRef.OffLimits = true;
                break;
        }

        SampleSet::iterator prevSample = Samples.find(sampleRef);

        if (prevSample != end(Samples))
        {
            // this sample is already cabbed
            log::Verbose(
                _L_,
                L"Not adding duplicate sample %s to archive\r\n",
                aMatch->MatchingNames.front().FullPathName.c_str());
            SampleRef& item = const_cast<SampleRef&>(*prevSample);
            hr = S_FALSE;
        }
        else
        {
            for (auto& name : aMatch->MatchingNames)
            {
                log::Verbose(_L_, L"Adding sample %s to archive\r\n", name.FullPathName.c_str());

                sampleRef.Content = aSpec.Content;
                sampleRef.CollectionDate = CollectionDate;

                wstring CabSampleName;
                DWORD dwIdx = 0L;
                std::unordered_set<std::wstring>::iterator it;
                do
                {
                    if (FAILED(
                            hr = CreateSampleFileName(
                                sampleRef.Content, name.FILENAME(), anAttr.AttrName, dwIdx, CabSampleName)))
                        break;

                    if (!aSpec.Name.empty())
                    {
                        CabSampleName.insert(0, L"\\");
                        CabSampleName.insert(0, aSpec.Name);
                    }
                    it = SampleNames.find(CabSampleName);
                    dwIdx++;

                } while (it != end(SampleNames));

                SampleNames.insert(CabSampleName);
                sampleRef.SampleName = CabSampleName;
            }

            if (FAILED(hr = ConfigureSampleStreams(sampleRef)))
            {
                log::Error(_L_, hr, L"Failed to configure sample reference for %s\r\n", sampleRef.SampleName.c_str());
            }
            Samples.insert(sampleRef);
        }
    }

    if (hr == S_FALSE)
        return hr;
    return S_OK;
}

HRESULT
Main::CollectMatchingSamples(const std::shared_ptr<ArchiveCreate>& compressor, ITableOutput& output, SampleSet& Samples)
{
    HRESULT hr = E_FAIL;

    std::for_each(begin(Samples), end(Samples), [this, compressor, &hr](const SampleRef& sampleRef) {
        if (!sampleRef.OffLimits)
        {
            wstring strName;
            sampleRef.Matches.front()->GetMatchFullName(
                sampleRef.Matches.front()->MatchingNames.front(),
                sampleRef.Matches.front()->MatchingAttributes.front(),
                strName);
            if (FAILED(hr = compressor->AddStream(sampleRef.SampleName.c_str(), strName.c_str(), sampleRef.CopyStream)))
            {
                log::Error(_L_, hr, L"Failed to add sample %s\r\n", sampleRef.SampleName.c_str());
            }
        }
    });

    log::Info(_L_, L"\r\nAdding matching samples to archive:\r\n");
    compressor->SetCallback(
        [this](const OrcArchive::ArchiveItem& item) { log::Info(_L_, L"\t%s\r\n", item.Path.c_str()); });

    if (FAILED(hr = compressor->FlushQueue()))
    {
        log::Error(_L_, hr, L"Failed to flush queue to %s\r\n", config.Output.Path.c_str());
        return hr;
    }

    wstring strComputerName;
    SystemDetails::GetOrcComputerName(strComputerName);

    std::for_each(
        begin(Samples), end(Samples), [this, strComputerName, compressor, &output, &hr](const SampleRef& sampleRef) {
            if (sampleRef.HashStream)
            {
                sampleRef.HashStream->GetMD5(const_cast<CBinaryBuffer&>(sampleRef.MD5));
                sampleRef.HashStream->GetSHA1(const_cast<CBinaryBuffer&>(sampleRef.SHA1));
                sampleRef.HashStream->GetSHA256(const_cast<CBinaryBuffer&>(sampleRef.SHA256));
            }

            if (sampleRef.FuzzyHashStream)
            {
                sampleRef.FuzzyHashStream->GetSSDeep(const_cast<CBinaryBuffer&>(sampleRef.SSDeep));
                sampleRef.FuzzyHashStream->GetTLSH(const_cast<CBinaryBuffer&>(sampleRef.TLSH));
            }

            if (FAILED(hr = AddSampleRefToCSV(output, strComputerName, sampleRef)))
            {
                log::Error(
                    _L_,
                    hr,
                    L"Failed to add sample %s metadata to csv\r\n",
                    sampleRef.Matches.front()->MatchingNames.front().FullPathName.c_str());
                return;
            }
        });

    return S_OK;
}

HRESULT
Main::CollectMatchingSamples(const std::wstring& outputdir, ITableOutput& output, SampleSet& MatchingSamples)
{
    HRESULT hr = E_FAIL;

    if (MatchingSamples.empty())
        return S_OK;

    fs::path output_dir(outputdir);

    wstring strComputerName;
    SystemDetails::GetOrcComputerName(strComputerName);

    log::Info(_L_, L"\r\nCopying matching samples to %s\r\n", outputdir.c_str());

    for (const auto& sample_ref : MatchingSamples)
    {
        if (!sample_ref.OffLimits)
        {
            fs::path sampleFile = output_dir / fs::path(sample_ref.SampleName);

            FileStream outputStream(_L_);

            if (FAILED(hr = outputStream.WriteTo(sampleFile.wstring().c_str())))
            {
                log::Error(_L_, hr, L"Failed to create sample file %s\r\n", sampleFile.wstring().c_str());
                break;
            }

            ULONGLONG ullBytesWritten = 0LL;
            if (FAILED(hr = sample_ref.CopyStream->CopyTo(outputStream, &ullBytesWritten)))
            {
                log::Error(_L_, hr, L"Failed while writing to sample %s\r\n", sampleFile.string().c_str());
                break;
            }

            outputStream.Close();
            sample_ref.CopyStream->Close();

            log::Info(
                _L_, L"\t%s copied (%I64d bytes)\r\n", sample_ref.SampleName.c_str(), sample_ref.CopyStream->GetSize());
        }
    }

    for (const auto& sample_ref : MatchingSamples)
    {
        fs::path sampleFile = output_dir / fs::path(sample_ref.SampleName);

        if (sample_ref.HashStream)
        {
            sample_ref.HashStream->GetMD5(const_cast<CBinaryBuffer&>(sample_ref.MD5));
            sample_ref.HashStream->GetSHA1(const_cast<CBinaryBuffer&>(sample_ref.SHA1));
            sample_ref.HashStream->GetSHA256(const_cast<CBinaryBuffer&>(sample_ref.SHA256));
        }

        if (sample_ref.FuzzyHashStream)
        {
            sample_ref.FuzzyHashStream->GetSSDeep(const_cast<CBinaryBuffer&>(sample_ref.SSDeep));
            sample_ref.FuzzyHashStream->GetTLSH(const_cast<CBinaryBuffer&>(sample_ref.TLSH));
        }

        if (FAILED(hr = AddSampleRefToCSV(output, strComputerName, sample_ref)))
        {
            log::Error(_L_, hr, L"Failed to add sample %s metadata to csv\r\n", sampleFile.string().c_str());
            break;
        }
    }
    return S_OK;
}

HRESULT Main::CollectMatchingSamples(const OutputSpec& output, SampleSet& MatchingSamples)
{
    HRESULT hr = E_FAIL;

    switch (output.Type)
    {
        case OutputSpec::Archive: {
            const auto& archivePath = fs::path(config.Output.Path);

            auto compressor = ::CreateCompressor(config.Output, CompressorFlags::kNone, hr, _L_);
            if (compressor == nullptr)
            {
                return hr;
            }

            const fs::path tempDir = archivePath.parent_path();
            std::shared_ptr<TemporaryStream> logStream;
            logStream = ::CreateLogStream(tempDir / L"GetThisLogStream", hr, _L_);
            if (logStream == nullptr)
            {
                log::Error(_L_, hr, L"Failed to create log stream\r\n");
                return hr;
            }

            auto csvWriter = ::CreateCsvWriter(
                tempDir / L"GetThisCsvStream", config.Output.Schema, config.Output.OutputEncoding, hr, _L_);
            if (csvWriter == nullptr)
            {
                log::Error(_L_, hr, L"Failed to create log stream\r\n");
                return hr;
            }

            hr = CollectMatchingSamples(compressor, csvWriter->GetTableOutput(), MatchingSamples);
            if (FAILED(hr))
            {
                return hr;
            }

            csvWriter->Flush();

            if (csvWriter->GetStream())
            {
                auto stream = csvWriter->GetStream();
                if (stream->GetSize())
                {
                    hr = stream->SetFilePointer(0, FILE_BEGIN, nullptr);
                    if (FAILED(hr))
                    {
                        log::Error(_L_, hr, L"Failed to rewind csv stream\r\n");
                    }

                    hr = compressor->AddStream(L"GetThis.csv", L"GetThis.csv", stream);
                    if (FAILED(hr))
                    {
                        log::Error(_L_, hr, L"Failed to add GetThis.csv\r\n");
                    }
                }
            }

            auto pLogStream = _L_->GetByteStream();
            _L_->CloseLogToStream(false);

            if (pLogStream && pLogStream->GetSize() > 0LL)
            {
                if (FAILED(hr = pLogStream->SetFilePointer(0, FILE_BEGIN, nullptr)))
                {
                    log::Error(_L_, hr, L"Failed to rewind log stream\r\n");
                }
                if (FAILED(hr = compressor->AddStream(L"GetThis.log", L"GetThis.log", pLogStream)))
                {
                    log::Error(_L_, hr, L"Failed to add GetThis.log\r\n");
                }
            }

            if (FAILED(hr = compressor->Complete()))
            {
                log::Error(_L_, hr, L"Failed to complete %s\r\n", config.Output.Path.c_str());
                return hr;
            }

            csvWriter->Close();
        }
        break;
        case OutputSpec::Directory: {
            auto [hr, csvWriter] = CreateOutputDirLogFileAndCSV({config.Output.Path});
            if (csvWriter == nullptr)
            {
                return hr;
            }

            hr = CollectMatchingSamples(config.Output.Path, csvWriter->GetTableOutput(), MatchingSamples);
            if (FAILED(hr))
            {
                return hr;
            }

            csvWriter->Close();
        }
        break;
        default:
            return E_NOTIMPL;
    }

    return S_OK;
}

HRESULT Main::HashOffLimitSamples(SampleSet& samples) const
{
    auto devnull = std::make_shared<DevNullStream>(_L_);

    log::Info(_L_, L"\r\nComputing hash of off limit samples\r\n");

    for (auto& sample : samples)
    {
        if (!sample.OffLimits)
        {
            continue;
        }

        ULONGLONG ullBytesWritten = 0LL;
        HRESULT hr = sample.CopyStream->CopyTo(devnull, &ullBytesWritten);
        if (FAILED(hr))
        {
            log::Error(_L_, hr, L"Failed while computing hash of sample\r\n");
            break;
        }

        sample.CopyStream->Close();
    }

    return S_OK;
}

HRESULT Main::FindMatchingSamples()
{
    HRESULT hr = E_FAIL;

    if (FAILED(hr = FileFinder.InitializeYara(config.Yara)))
    {
        log::Error(_L_, hr, L"Failed to initialize Yara scan\r\n");
    }

    if (FAILED(
            hr = FileFinder.Find(
                config.Locations,
                [this, &hr](const std::shared_ptr<FileFind::Match>& aMatch, bool& bStop) {
                    if (aMatch == nullptr)
                        return;

                    // finding the corresponding Sample Spec (for limits)
                    auto aSpecIt = std::find_if(
                        begin(config.listofSpecs), end(config.listofSpecs), [aMatch](const SampleSpec& aSpec) -> bool {
                            auto filespecIt = std::find(begin(aSpec.Terms), end(aSpec.Terms), aMatch->Term);
                            return filespecIt != end(aSpec.Terms);
                        });

                    if (aSpecIt == end(config.listofSpecs))
                    {
                        log::Error(
                            _L_,
                            hr = E_FAIL,
                            L"Could not find sample spec for match %s\r\n",
                            aMatch->Term->GetDescription().c_str());
                        return;
                    }

                    const wstring& strFullFileName = aMatch->MatchingNames.front().FullPathName;

                    if (aMatch->MatchingAttributes.empty())
                    {
                        log::Warning(
                            _L_,
                            E_FAIL,
                            L"\"%s\" matched \"%s\" but no data related attribute was associated\r\n",
                            strFullFileName.c_str(),
                            aMatch->Term->GetDescription().c_str());
                        return;
                    }

                    for (const auto& attr : aMatch->MatchingAttributes)
                    {
                        wstring strName;

                        aMatch->GetMatchFullName(aMatch->MatchingNames.front(), attr, strName);

                        DWORDLONG dwlDataSize = attr.DataStream->GetSize();
                        LimitStatus status = SampleLimitStatus(GlobalLimits, aSpecIt->PerSampleLimits, dwlDataSize);

                        if (FAILED(hr = AddSamplesForMatch(status, *aSpecIt, aMatch)))
                        {
                            log::Error(_L_, hr, L"\tFailed to add %s\r\n", strName.c_str());
                        }

                        switch (status)
                        {
                            case NoLimits:
                            case SampleWithinLimits: {
                                if (hr == S_FALSE)
                                {
                                    log::Info(_L_, L"\t%s is already collected\r\n", strName.c_str());
                                }
                                else
                                {
                                    log::Info(_L_, L"\t%s matched (%d bytes)\r\n", strName.c_str(), dwlDataSize);
                                    aSpecIt->PerSampleLimits.dwlAccumulatedBytesTotal += dwlDataSize;
                                    aSpecIt->PerSampleLimits.dwAccumulatedSampleCount++;

                                    GlobalLimits.dwlAccumulatedBytesTotal += dwlDataSize;
                                    GlobalLimits.dwAccumulatedSampleCount++;
                                }
                            }
                            break;
                            case GlobalSampleCountLimitReached:
                                log::Info(
                                    _L_,
                                    L"\t%s : Global sample count reached (%d)\r\n",
                                    strName.c_str(),
                                    GlobalLimits.dwMaxSampleCount);
                                GlobalLimits.bMaxSampleCountReached = true;
                                break;
                            case GlobalMaxBytesPerSample:
                                log::Info(
                                    _L_,
                                    L"\t%s : Exceeds global per sample size limit (%I64d)\r\n",
                                    strName.c_str(),
                                    GlobalLimits.dwlMaxBytesPerSample);
                                GlobalLimits.bMaxBytesPerSampleReached = true;
                                break;
                            case GlobalMaxBytesTotal:
                                log::Info(
                                    _L_,
                                    L"\t%s : Global total sample size limit reached (%I64d)\r\n",
                                    strName.c_str(),
                                    GlobalLimits.dwlMaxBytesTotal);
                                GlobalLimits.bMaxBytesTotalReached = true;
                                break;
                            case LocalSampleCountLimitReached:
                                log::Info(
                                    _L_,
                                    L"\t%s : sample count reached (%d)\r\n",
                                    strName.c_str(),
                                    aSpecIt->PerSampleLimits.dwMaxSampleCount);
                                aSpecIt->PerSampleLimits.bMaxSampleCountReached = true;
                                break;
                            case LocalMaxBytesPerSample:
                                log::Info(
                                    _L_,
                                    L"\t%s : Exceeds per sample size limit (%I64d)\r\n",
                                    strName.c_str(),
                                    aSpecIt->PerSampleLimits.dwlMaxBytesPerSample);
                                aSpecIt->PerSampleLimits.bMaxBytesPerSampleReached = true;
                                break;
                            case LocalMaxBytesTotal:
                                log::Info(
                                    _L_,
                                    L"\t%s : total sample size limit reached (%I64d)\r\n",
                                    strName.c_str(),
                                    aSpecIt->PerSampleLimits.dwlMaxBytesTotal);
                                aSpecIt->PerSampleLimits.bMaxBytesTotalReached = true;
                                break;
                            case FailedToComputeLimits:
                                break;
                        }
                    }
                    return;
                },
                false)))
    {
        log::Error(_L_, hr, L"Failed while parsing locations\r\n");
    }

    return S_OK;
}

HRESULT Main::Run()
{
    HRESULT hr = E_FAIL;
    LoadWinTrust();

    GetSystemTimeAsFileTime(&CollectionDate);

    try
    {
        if (config.bFlushRegistry)
        {
            if (FAILED(hr = RegFlushKeys()))
                log::Info(_L_, L"Failed to flush keys (hr = 0x%lx)\r\n", hr);
        }
    }
    catch (...)
    {
        log::Error(_L_, E_FAIL, L"GetThis failed during output setup, parameter output, RegistryFlush, exiting\r\n");
        return E_FAIL;
    }

    try
    {
        if (FAILED(hr = FindMatchingSamples()))
        {
            log::Error(_L_, hr, L"\r\nGetThis failed while matching samples\r\n");
            return hr;
        }

        if (config.bReportAll && config.CryptoHashAlgs != CryptoHashStream::Algorithm::Undefined)
        {
            hr = HashOffLimitSamples(Samples);
            if (FAILED(hr))
            {
                return hr;
            }
        }

        if (FAILED(hr = CollectMatchingSamples(config.Output, Samples)))
        {
            log::Error(_L_, hr, L"\r\nGetThis failed while collecting samples\r\n");
            return hr;
        }

        hr = CloseOutput();
        if (FAILED(hr))
        {
            Log::Error(L"Failed to close output (code: {:#x})", hr);
        }
    }
    catch (...)
    {
        log::Error(_L_, E_ABORT, L"\r\nGetThis failed during sample collection, terminating archive\r\n");
        _L_->CloseLogFile();

        return E_ABORT;
    }

    return S_OK;
}
