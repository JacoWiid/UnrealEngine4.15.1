// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "UnrealEd.h"
#include "UnrealAudioModule.h"
#include "UnrealAudioTests.h"

DEFINE_LOG_CATEGORY_STATIC(AudioTestCommandlet, Log, All);

#if ENABLE_UNREAL_AUDIO

/** Unreal audio module ptr */
static UAudio::IUnrealAudioModule* UnrealAudioModule = nullptr;

static bool UnrealAudioLoad(const FString* DeviceApi = nullptr)
{
	UnrealAudioModule = FModuleManager::LoadModulePtr<UAudio::IUnrealAudioModule>(FName("UnrealAudio"));
	if (DeviceApi)
	{
		UnrealAudioModule->Initialize(*DeviceApi);
	}
	else
	{
		UnrealAudioModule->Initialize();
	}
	return UnrealAudioModule != nullptr;
}

static bool UnrealAudioUnload()
{
	if (UnrealAudioModule)
	{
		UnrealAudioModule->Shutdown();
		return true;
	}
	return false;
}

/**
* Test static functions which call into module test code
*/

static bool TestAudioDeviceAll(const TArray<FString>& Args)
{
	if (!UAudio::TestDeviceQuery())
	{
		return false;
	}

	if (!UAudio::TestDeviceOutputSimple(10))
	{
		return false;
	}

	if (!UAudio::TestDeviceOutputRandomizedFm(10))
	{
		return false;
	}

	if (!UAudio::TestDeviceOutputNoisePan(10))
	{
		return false;
	}

	return true;
}

static bool TestAudioDeviceQuery(const TArray<FString>& Args)
{
	return UAudio::TestDeviceQuery();
}

static bool TestAudioDeviceOutputSimple(const TArray<FString>& Args)
{
	return UAudio::TestDeviceOutputSimple(-1);
}

static bool TestAudioDeviceOutputFm(const TArray<FString>& Args)
{
	return UAudio::TestDeviceOutputRandomizedFm(-1);
}

static bool TestAudioDeviceOutputPan(const TArray<FString>& Args)
{
	return UAudio::TestDeviceOutputNoisePan(-1);
}

static bool TestAudioSourceImport(const TArray<FString>& Args)
{
	if (Args.Num() != 1)
	{
		return false;
	}

	UAudio::FSoundFileImportSettings SoundFileImportSettings;
	SoundFileImportSettings.SoundFilePath = Args[0];
	SoundFileImportSettings.Format = UAudio::ESoundFileFormat::OGG | UAudio::ESoundFileFormat::VORBIS;
	SoundFileImportSettings.SampleRate = 48000;
	SoundFileImportSettings.EncodingQuality = 0.5;
	SoundFileImportSettings.bPerformPeakNormalization = true;

	return UAudio::TestSourceImport(SoundFileImportSettings);
}

static bool TestAudioSourceImportExport(const TArray<FString>& Args)
{
	if (Args.Num() != 1)
	{
		return false;
	}

	UAudio::FSoundFileImportSettings SoundFileImportSettings;
	SoundFileImportSettings.SoundFilePath = Args[0];
	SoundFileImportSettings.Format = UAudio::ESoundFileFormat::OGG | UAudio::ESoundFileFormat::VORBIS;
	SoundFileImportSettings.SampleRate = 48000;
	SoundFileImportSettings.EncodingQuality = 0.5;
	SoundFileImportSettings.bPerformPeakNormalization = false;

	return UAudio::TestSourceImportExport(SoundFileImportSettings);
}

/**
* Setting up commandlet code
*/

typedef bool AudioTestFunction();

struct AudioTestInfo
{
	FString CategoryName;
	FString TestName;
	FString ArgDescription;
	int32 NumArgs;
	bool (*TestFunction)(const TArray<FString>& Args);
};

enum EAudioTests
{
	AUDIO_TEST_DEVICE_ALL,
	AUDIO_TEST_DEVICE_QUERY,
	AUDIO_TEST_DEVICE_OUTPUT_SIMPLE,
	AUDIO_TEST_DEVICE_OUTPUT_FM,
	AUDIO_TEST_DEVICE_OUTPUT_PAN,
	AUDIO_TEST_SOURCE_IMPORT,
	AUDIO_TEST_SOURCE_IMPORT_EXPORT,
	AUDIO_TESTS
};

static AudioTestInfo AudioTestInfoList[] =
{
	{ "device", "all",		"None",						0, TestAudioDeviceAll			}, // AUDIO_TEST_DEVICE_ALL
	{ "device", "query",	"None",						0, TestAudioDeviceQuery			}, // AUDIO_TEST_DEVICE_QUERY
	{ "device", "out",		"None",						0, TestAudioDeviceOutputSimple	}, // AUDIO_TEST_DEVICE_OUTPUT_SIMPLE
	{ "device", "out_fm",	"None",						0, TestAudioDeviceOutputFm		}, // AUDIO_TEST_DEVICE_OUTPUT_FM
	{ "device", "out_pan",	"None",						0, TestAudioDeviceOutputPan		}, // AUDIO_TEST_DEVICE_OUTPUT_PAN
	{ "source", "import",	"SourcePath",				1, TestAudioSourceImport		}, // AUDIO_TEST_SOURCE_IMPORT
	{ "source", "export",	"SourcePath or Directory",	1, TestAudioSourceImportExport	}, // AUDIO_TEST_SOURCE_IMPORT_EXPORT
};
static_assert(ARRAY_COUNT(AudioTestInfoList) == AUDIO_TESTS, "Mismatched info list and test enumeration");

static void PrintUsage()
{
	UE_LOG(AudioTestCommandlet, Display, TEXT("AudioTestCommandlet Usage: {Editor}.exe UnrealEd.AudioTestCommandlet {testcategory} {test} {arglist}"));
	UE_LOG(AudioTestCommandlet, Display, TEXT("Possible Tests:"));
	UE_LOG(AudioTestCommandlet, Display, TEXT("CategoryName | TestName | Arguments"));
	for (uint32 Index = 0; Index < AUDIO_TESTS; ++Index)
	{
		AudioTestInfo& Info = AudioTestInfoList[Index];
		UE_LOG(AudioTestCommandlet, Display, TEXT("%s, %s, %s"), *Info.CategoryName, *Info.TestName, *Info.ArgDescription);
	}
}

#endif // #if ENABLE_UNREAL_AUDIO

// -- UAudioTestCommandlet Functions -------------------

UAudioTestCommandlet::UAudioTestCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

int32 UAudioTestCommandlet::Main(const FString& InParams)
{
#if ENABLE_UNREAL_AUDIO
    
    // Mac stupidly is adds "-NSDocumentRevisionsDebugMode YES" to command line args so lets remove that
    FString Params;
#if PLATFORM_MAC
    FString BadString(TEXT("-NSDocumentRevisionsDebugMode YES"));
    int32 DebugModeIndex = InParams.Find(BadString);
    if (DebugModeIndex != INDEX_NONE)
    {
        Params = InParams.LeftChop(BadString.Len());
    }
    else
#endif // #if PLATFORM_MAC
    {
        Params = InParams;
    }
   
	// Parse command line.
	TArray<FString> Tokens;
	TArray<FString> Switches;
	UCommandlet::ParseCommandLine(*Params, Tokens, Switches);

	// Check if we're switching to a different device API
	if (Switches.Num() == 1)
	{
		FString DeviceApiName(Switches[1]);
		if (!UnrealAudioLoad(&DeviceApiName))
		{
			UE_LOG(AudioTestCommandlet, Display, TEXT("Failed to load unreal audio module. Exiting."));
			return 0;
		}
	}
	else
	{
		if (!UnrealAudioLoad())
		{
			UE_LOG(AudioTestCommandlet, Display, TEXT("Failed to load unreal audio module. Exiting."));
			return 0;
		}
	}

	check(UnrealAudioModule != nullptr);

	if (Tokens.Num() < 3)
	{
		PrintUsage();
		return 0;
	}

	const int32 CategoryNameIndex = 1;
	const int32 TestNameIndex = 2;
	const int32 ArgStartIndex = 3;
	bool FoundTest = false;
	for (uint32 Index = 0; Index < AUDIO_TESTS; ++Index)
	{
		AudioTestInfo& Info = AudioTestInfoList[Index];
		if (Info.CategoryName == Tokens[CategoryNameIndex])
		{
			if (Info.TestName == Tokens[TestNameIndex])
			{
				FoundTest = true;
				TArray<FString> Args;
				for (int32 ArgIndex = ArgStartIndex; ArgIndex < Tokens.Num(); ++ArgIndex)
				{
					Args.Add(Tokens[ArgIndex]);
				}
				if (AudioTestInfoList[Index].TestFunction(Args))
				{
					UE_LOG(AudioTestCommandlet, Display, TEXT("Test %s succeeded."), *Info.TestName);
				}
				else
				{
					UE_LOG(AudioTestCommandlet, Display, TEXT("Test %s failed."), *Info.TestName);
				}
				break;
			}
		}
	}

	if (!FoundTest)
	{
		UE_LOG(AudioTestCommandlet, Display, TEXT("Unknown category or test. Exiting."));
		return 0;
	}

	UnrealAudioUnload();
#else // #if ENABLE_UNREAL_AUDIO
	UE_LOG(AudioTestCommandlet, Display, TEXT("Unreal Audio Module Not Enabled For This Platform"));
#endif // #else // #if ENABLE_UNREAL_AUDIO


	return 0;
}
