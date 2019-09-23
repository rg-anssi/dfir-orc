//
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Copyright © 2011-2019 ANSSI. All Rights Reserved.
//
// Author(s): Jean Gautier (ANSSI)
//
#include "stdafx.h"

#include <memory>
#include <iostream>
#include <iomanip>

#include "EmbeddedResource.h"

#include "LogFileWriter.h"

#include "Convert.h"

using namespace std;

using namespace std::string_literals;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Orc;
using namespace Orc::Test;

namespace Orc::Test {
TEST_CLASS(EmbeddedResourceTest)
{
private:
    logger _L_;
    UnitTestHelper helper;

public:
    TEST_METHOD_INITIALIZE(Initialize)
    {
        _L_ = std::make_shared<LogFileWriter>();
        helper.InitLogFileWriter(_L_);
    }

    TEST_METHOD_CLEANUP(Finalize) { helper.FinalizeLogFileWriter(_L_); }

    TEST_METHOD(SimpleValue)
    {
        using namespace std::string_literals;

        std::wstring strValue;
        Assert::IsTrue(SUCCEEDED(Orc::EmbeddedResource::ExtractValue(_L_, L""s, L"TEST_7Z_DLL"s, strValue)));
        Assert::AreEqual(L"7z:#TEST_7Z_DLL_BIN|OrcLibTest.dll"s, strValue);
    }
    TEST_METHOD(Uncompressed)
    {
        using namespace std::string_literals;

        Orc::CBinaryBuffer buffer;
        Assert::IsTrue(SUCCEEDED(Orc::EmbeddedResource::ExtractBuffer(_L_, L""s, L"TEST_7Z_DLL_BIN"s, buffer)));
        Assert::IsTrue(buffer.GetCount() > 0);
    }

#ifdef WORK_IN_PROGRESS
    TEST_METHOD(ArchiveToMemory)
    {
        using namespace std::string_literals;

        Orc::CBinaryBuffer buffer;
        Assert::IsTrue(
            SUCCEEDED(Orc::EmbeddedResource::ExtractToBuffer(_L_, L"7z:#TEST_7Z_DLL_BIN|OrcLibTest.dll"s, buffer)));
        Assert::IsTrue(buffer.GetCount() > 0);
    }
#endif  // WORK_IN_PROGRESS
};
}  // namespace Orc::Test
