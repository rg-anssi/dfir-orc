//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//
#include "StdAfx.h"

#include <regex>
#include <sstream>

#include "Location.h"

#include "ConfigFileReader.h"

#include "PhysicalDiskReader.h"
#include "InterfaceReader.h"
#include "SystemStorageReader.h"
#include "SnapshotVolumeReader.h"
#include "ImageReader.h"
#include "MountedVolumeReader.h"
#include "OfflineMFTReader.h"

using namespace std;

using namespace Orc;

Location::Location(logger pLog, const std::wstring& Location, Location::Type type)
    : _L_(std::move(pLog))
    , m_Location(Location)
    , m_Type(type)
{
}

std::shared_ptr<VolumeReader> Location::GetReader()
{
    if (m_Reader != nullptr)
        return m_Reader;

    if (m_Type == Undetermined)
    {
        // Cannot instantiate a reader for a Location we fail to determine
        return nullptr;
    }

    switch (m_Type)
    {
        case MountedVolume:
        case PartitionVolume:
            m_Reader = make_shared<MountedVolumeReader>(_L_, m_Location.c_str());
            break;
        case Snapshot:
            if (m_Shadow != nullptr)
            {
                m_Reader = make_shared<SnapshotVolumeReader>(_L_, *m_Shadow);
            }
            break;
        case PhysicalDrive:
        case PhysicalDriveVolume:
            m_Reader = make_shared<PhysicalDiskReader>(_L_, m_Location.c_str());
            break;
        case DiskInterface:
        case DiskInterfaceVolume:
            m_Reader = make_shared<InterfaceReader>(_L_, m_Location.c_str());
            break;
        case SystemStorage:
        case SystemStorageVolume:
            m_Reader = make_shared<SystemStorageReader>(_L_, m_Location.c_str());
            break;
        case ImageFileVolume:
        case ImageFileDisk:
            m_Reader = make_shared<ImageReader>(_L_, m_Location.c_str());
            break;
        case OfflineMFT:
            m_Reader = make_shared<OfflineMFTReader>(_L_, m_Location.c_str());
            break;
        default:
            break;
    }
    return m_Reader;
}

void Location::MakeIdentifier()
{
    if (m_Type == Undetermined)
    {
        // Cannot instantiate a reader for a Location we fail to determine
        m_Identifier = L"Undetermined";
        return;
    }

    auto replace_reserved_chars = [](wstring& in) -> wstring {
        wstring retval = in;
        std::replace_if(
            begin(retval),
            end(retval),
            [](const WCHAR inchar) -> bool {
                return (
                    inchar == L'<' ||  //< (less than)
                    inchar == L'>' ||  //> (greater than)
                    inchar == L':' ||  //: (colon)
                    inchar == L'\"' ||  //" (double quote)
                    inchar == L'/' ||  /// (forward slash)
                    inchar == L'\\' ||  //\ (backslash)
                    inchar == L'|' ||  //| (vertical bar or pipe)
                    inchar == L'?' ||  //? (question mark)
                    inchar == L'*');  //* (asterisk)
            },
            L'_');

        return move(retval);
    };

    m_Identifier.clear();

    switch (m_Type)
    {
        case MountedVolume:
        {
            static wregex r1(REGEX_MOUNTED_DRIVE);
            wsmatch s;

            if (regex_match(m_Location, s, r1))
            {
                if (s[REGEX_MOUNTED_DRIVE_SUBDIR].matched && s[REGEX_MOUNTED_DRIVE_SUBDIR].compare(L"\\") == 0)
                {
                    // no subdir specified, not a mount point
                    m_Identifier = L"Volume_" + s[REGEX_MOUNTED_DRIVE_LETTER].str();
                    break;
                }
            }
            if (!m_Paths.empty() && m_Identifier.empty())
            {
                for_each(begin(m_Paths), end(m_Paths), [&](const wstring& item) {
                    if (regex_match(item, s, r1))
                    {
                        if (s[REGEX_MOUNTED_DRIVE_LETTER].matched)
                        {
                            // no subdir specified, not a mount point
                            m_Identifier = L"Volume_" + s[REGEX_MOUNTED_DRIVE_LETTER].str();
                            if (s[REGEX_MOUNTED_DRIVE_SUBDIR].matched && s[REGEX_MOUNTED_DRIVE_SUBDIR].compare(L"\\"))
                            {
                                wstring str = s[REGEX_MOUNTED_DRIVE_SUBDIR].str();
                                m_Identifier += replace_reserved_chars(str);
                            }
                        }
                    }
                });
                if (!m_Identifier.empty())
                    break;
            }

            wregex r2(REGEX_MOUNTED_VOLUME);
            if (regex_match(m_Location, s, r2))
            {
                if (s[REGEX_MOUNTED_VOLUME_ID].matched)
                {
                    m_Identifier = L"Volume" + s[REGEX_MOUNTED_VOLUME_ID].str();
                    break;
                }
            }

            wregex r3(REGEX_MOUNTED_HARDDISKVOLUME, regex_constants::icase);
            if (regex_match(m_Location, s, r3))
            {
                if (s[REGEX_MOUNTED_HARDDISKVOLUME_ID].matched)
                {
                    m_Identifier = L"HarddiskVolume" + s[REGEX_MOUNTED_HARDDISKVOLUME_ID].str();
                    break;
                }
            }

            m_Identifier = replace_reserved_chars(m_Location);
        }
        break;
        case Snapshot:
        {
            wregex r(REGEX_SNAPSHOT, regex_constants::icase);
            wsmatch s;

            if (regex_match(m_Location, s, r))
            {

                m_Identifier = L"Snapshot_" + s[REGEX_SNAPSHOT_NUM].str();
            }
        }
        break;
        case PhysicalDrive:
        case PhysicalDriveVolume:
        {
            wregex r_physical(REGEX_PHYSICALDRIVE, std::regex_constants::icase);
            wregex r_disk(REGEX_DISK, std::regex_constants::icase);
            wsmatch s;

            if (regex_match(m_Location, s, r_physical))
            {
                if (s[REGEX_PHYSICALDRIVE_PARTITION_SPEC].matched
                    && *s[REGEX_PHYSICALDRIVE_PARTITION_SPEC].first == L'*')
                {
                    m_Identifier = L"PhysicalDrive_" + s[REGEX_PHYSICALDRIVE_NUM].str() + L"_ActivePartition";
                    break;
                }
                else if (s[REGEX_PHYSICALDRIVE_PARTITION_NUM].matched)
                {
                    m_Identifier = L"PhysicalDrive_" + s[REGEX_PHYSICALDRIVE_NUM].str() + L"_Partition_"
                        + s[REGEX_PHYSICALDRIVE_PARTITION_NUM].str();
                    break;
                }
                else if (s[REGEX_PHYSICALDRIVE_OFFSET].matched)
                {
                    m_Identifier = L"PhysicalDrive_" + s[REGEX_PHYSICALDRIVE_NUM].str() + L"_Offset_"
                        + s[REGEX_PHYSICALDRIVE_OFFSET].str();
                    break;
                }
            }
            else if (regex_match(m_Location, s, r_disk))
            {
                if (s[REGEX_DISK_PARTITION_SPEC].matched && *s[REGEX_DISK_PARTITION_SPEC].first == L'*')
                {
                    m_Identifier = L"Disk_" + s[REGEX_DISK_GUID].str() + L"_ActivePartition";
                    break;
                }
                else if (s[REGEX_DISK_PARTITION_NUM].matched)
                {
                    m_Identifier =
                        L"Disk_" + s[REGEX_DISK_GUID].str() + L"_Partition_" + s[REGEX_DISK_PARTITION_NUM].str();
                    break;
                }
                else if (s[REGEX_DISK_OFFSET].matched)
                {
                    m_Identifier = L"Disk_" + s[REGEX_DISK_GUID].str() + L"_Offset_" + s[REGEX_DISK_OFFSET].str();
                    break;
                }
            }
            m_Identifier = wstring(L"Disk_") + replace_reserved_chars(m_Location);
        }
        break;
        case PartitionVolume:
        {
            m_Identifier = wstring(L"PartitionVolume_") + replace_reserved_chars(m_Location);
        }
        break;
        case DiskInterface:
        case DiskInterfaceVolume:
        {
            std::wstringstream ss;
            ss << L"0x" << std::hex << SerialNumber();

            m_Identifier = wstring(L"DiskInterface_") + ss.str();
        }
        break;
        case SystemStorage:
        case SystemStorageVolume:
        {
            std::wstringstream ss;
            ss << L"0x" << std::hex << SerialNumber();

            m_Identifier = wstring(L"SystemStorage_") + ss.str();
        }
        break;
        case ImageFileVolume:
        {
            wregex r(REGEX_IMAGE, regex_constants::icase);
            wsmatch s;

            if (regex_match(m_Location, s, r))
            {

                WCHAR szImageName[MAX_PATH];
                if (FAILED(GetFileNameForFile(s[REGEX_IMAGE_SPEC].str().c_str(), szImageName, MAX_PATH)))
                    return;

                m_Identifier = wstring(L"VolumeImage_") + szImageName;

                if (s[REGEX_IMAGE_PARTITION_NUM].matched)
                {
                    m_Identifier += L"_partition_";
                    m_Identifier += s[REGEX_IMAGE_PARTITION_NUM].str();
                }
                else
                {
                    if (s[REGEX_IMAGE_OFFSET].matched)
                    {
                        m_Identifier += L"_offset_";
                        m_Identifier += s[REGEX_IMAGE_OFFSET].str();
                    }
                    if (s[REGEX_IMAGE_SIZE].matched)
                    {
                        m_Identifier += L"_size_";
                        m_Identifier += s[REGEX_IMAGE_SIZE].str();
                    }
                    if (s[REGEX_IMAGE_SECTOR].matched)
                    {
                        m_Identifier += L"_sector_";
                        m_Identifier += s[REGEX_IMAGE_SECTOR].str();
                    }
                }
            }
        }
        break;
        case ImageFileDisk:
        {
            wregex r(REGEX_IMAGE, regex_constants::icase);
            wsmatch s;

            if (regex_match(m_Location, s, r))
            {

                WCHAR szImageName[MAX_PATH];
                if (FAILED(GetFileNameForFile(s[REGEX_IMAGE_SPEC].str().c_str(), szImageName, MAX_PATH)))
                {
                    return;
                }

                m_Identifier = L"DiskImage_" + wstring(szImageName);

                if (s[REGEX_IMAGE_PARTITION_SPEC].matched && *s[REGEX_IMAGE_PARTITION_SPEC].first == L'*')
                {
                    m_Identifier += L"_ActivePartition";
                    break;
                }
                else if (s[REGEX_IMAGE_PARTITION_NUM].matched)
                {
                    m_Identifier += L"_Partition_" + s[REGEX_IMAGE_PARTITION_NUM].str();
                    break;
                }
            }
        }
        break;
        case OfflineMFT:
        {
            WCHAR szImageName[MAX_PATH];
            if (SUCCEEDED(GetFileNameForFile(m_Location.c_str(), szImageName, MAX_PATH)))
            {
                m_Identifier = wstring(L"OfflineMFT_") + szImageName;
            }
            else
            {
                m_Identifier = wstring(L"OfflineMFT_") + replace_reserved_chars(m_Location);
            }
        }
        break;
        default:
            break;
    }
}

ULONGLONG Location::SerialNumber() const
{
    if (m_Reader != nullptr)
    {
        return m_Reader->VolumeSerialNumber();
    }

    return 0LL;
}

FSVBR::FSType Location::GetFSType() const
{
    if (m_Reader != nullptr)
    {
        return m_Reader->GetFSType();
    }

    return FSVBR::FSType::UNKNOWN;
}

std::wostream& Orc::operator<<(std::wostream& o, const Location& l)
{
    std::wstringstream ss;
    bool isAMountedVolume = false;

    switch (l.GetType())
    {
        case Location::Type::OfflineMFT:
            ss << L"OfflineMFT           ";
            break;
        case Location::Type::ImageFileDisk:
            ss << L"ImageFileDisk        ";
            break;
        case Location::Type::ImageFileVolume:
            ss << L"ImageFileVolume      ";
            break;
        case Location::Type::DiskInterface:
            ss << L"DiskInterface        ";
            break;
        case Location::Type::DiskInterfaceVolume:
            ss << L"DiskInterfaceVolume  ";
            break;
        case Location::Type::PhysicalDrive:
            ss << L"PhysicalDrive        ";
            break;
        case Location::Type::PhysicalDriveVolume:
            ss << L"PhysicalDriveVolume  ";
            break;
        case Location::Type::SystemStorage:
            ss << L"SystemStorage        ";
            break;
        case Location::Type::SystemStorageVolume:
            ss << L"SystemStorageVolume  ";
            break;
        case Location::Type::PartitionVolume:
            ss << L"PartitionVolume      ";
            break;
        case Location::Type::MountedStorageVolume:
            ss << L"MountedStorageVolume ";
            break;
        case Location::Type::MountedVolume:
            ss << L"MountedVolume        ";
            isAMountedVolume = true;
            break;
        case Location::Type::Snapshot:
            ss << L"Snapshot             ";
            break;

        default:
            ss << L"Undetermined         ";
    }

    ss << L" : " << l.GetLocation();

    if (isAMountedVolume)
    {
        if (!l.GetPaths().empty())
        {
            ss << L" -";
            for (auto& path : l.GetPaths())
                ss << L" " << path;
        }
    }

    FSVBR::FSType fsType = l.GetFSType();
    ss << " - " << FSVBR::GetFSName(fsType);

    if (l.IsValid())
    {
        ss << L" - Valid (serial : 0x" << std::hex << l.SerialNumber() << L")";
    }
    else
    {
        ss << L" - Invalid";
    }

    if (l.GetParse())
    {
        ss << L" *";
    }

    o << ss.str();

    return o;
}