// Automation tests for CLISystem. Run headless:
//   UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests CLISystem; Quit" -unattended -nullrhi -nopause
// Process tests use the platform shell (cmd.exe / /bin/sh), so no test binary is needed.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CLIFrameCodec.h"
#include "SubProcess.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <atomic>

namespace CLISystemTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;
	static const FCLIFrameMagic TestMagic('T', 'S', 'T', 1);

	struct FCapture
	{
		FCriticalSection Lock;
		TArray<uint8> Bytes;
		std::atomic<bool> bBegan{ false };
		std::atomic<bool> bBeganOk{ false };
		std::atomic<bool> bEnded{ false };
		std::atomic<int32> ExitCode{ -999 };
		std::atomic<double> EndTime{ 0.0 };
		double StartTime = 0.0;

		FString Text()
		{
			FScopeLock L(&Lock);
			FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
			return FString(Converted.Length(), Converted.Get());
		}

		int64 NumBytes()
		{
			FScopeLock L(&Lock);
			return Bytes.Num();
		}
	};

	static TSharedPtr<FSubProcessHandler> MakeHandler(const TSharedPtr<FCapture>& C)
	{
		TSharedPtr<FSubProcessHandler> H = MakeShared<FSubProcessHandler>();
		H->OnProcessBegin = [C](int32, bool bOk) { C->bBeganOk = bOk; C->bBegan = true; };
		H->OnProcessEnd = [C](int32, int32 Code) { C->ExitCode = Code; C->EndTime = FPlatformTime::Seconds(); C->bEnded = true; };
		H->OnProcessOutput = [C](int32, const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			FScopeLock L(&C->Lock);
			C->Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		};
		H->OnProcessOutputBytes = [C](int32, const TArray<uint8>& Bytes)
		{
			FScopeLock L(&C->Lock);
			C->Bytes.Append(Bytes);
		};
		return H;
	}

	static FString ShellUrl()
	{
#if PLATFORM_WINDOWS
		const FString ComSpec = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
		return ComSpec.IsEmpty() ? FString(TEXT("C:\\Windows\\System32\\cmd.exe")) : ComSpec;
#else
		return TEXT("/bin/sh");
#endif
	}

	/** Shell that reads commands from stdin until stopped. */
	static FString InteractiveArgs()
	{
#if PLATFORM_WINDOWS
		return TEXT("/q /k");
#else
		return TEXT("");
#endif
	}

	static FString RunArgs(const FString& Command)
	{
#if PLATFORM_WINDOWS
		return TEXT("/c ") + Command;
#else
		return FString::Printf(TEXT("-c \"%s\""), *Command);
#endif
	}

	/** Echo a token without the token appearing in the command text (in case the shell echoes commands). */
	static FString EchoCommand(const FString& Token)
	{
#if PLATFORM_WINDOWS
		return FString::Printf(TEXT("echo %c^%s"), Token[0], *Token.Mid(1));
#else
		return FString::Printf(TEXT("echo %c''%s"), Token[0], *Token.Mid(1));
#endif
	}

	static FProcessParams ShellParams(const FString& Args)
	{
		FProcessParams P;
		P.Url = ShellUrl();
		P.Params = Args;
		P.OptionalWorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		return P;
	}

	/** Polls Pred each frame until true or Timeout; reports Failure on timeout. */
	static void WaitFor(FAutomationTestBase* Test, double Timeout, const FString& Failure, TFunction<bool()> Pred)
	{
		const double Deadline = FPlatformTime::Seconds() + Timeout;
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Deadline, Failure, Pred]() -> bool
		{
			if (Pred())
			{
				return true;
			}
			if (FPlatformTime::Seconds() > Deadline)
			{
				Test->AddError(Failure);
				return true;
			}
			return false;
		}));
	}

	static void Then(TFunction<void()> Fn)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Fn]() -> bool
		{
			Fn();
			return true;
		}));
	}

	static bool ContainsInOrder(const FString& Text, const FString& Prefix, int32 Count)
	{
		int32 From = 0;
		for (int32 i = 0; i < Count; ++i)
		{
			From = Text.Find(FString::Printf(TEXT("%s%d\n"), *Prefix, i), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (From == INDEX_NONE)
			{
				//cmd ends lines with \r\n
				return false;
			}
		}
		return true;
	}

	static FString NormalizeNewlines(const FString& Text)
	{
		return Text.Replace(TEXT("\r\n"), TEXT("\n"));
	}
}

using namespace CLISystemTests;

// ------------------------------------------------------------------------------------------------- codec

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIFrameCodecRoundTripTest, "CLISystem.FrameCodec.RoundTrip", CLISystemTests::Flags)
bool FCLIFrameCodecRoundTripTest::RunTest(const FString&)
{
	TArray<uint8> Big;
	Big.SetNumUninitialized(300000);
	for (int32 i = 0; i < Big.Num(); ++i) { Big[i] = (uint8)(i * 7); }

	TArray<uint8> Stream;
	Stream.Append(FCLIFrameCodec::Encode(TestMagic, 1, TEXT("{\"a\":1}")));
	const FString Utf8Header = TEXT("{\"name\":\"bin\",\"ü\":\"✓\"}");
	Stream.Append(FCLIFrameCodec::Encode(TestMagic, 3, Utf8Header, Big));
	Stream.Append(FCLIFrameCodec::Encode(TestMagic, 5, TEXT("")));

	//Feed in awkward chunk sizes, including single bytes across the markers
	for (const int32 Chunk : { 1, 3, 4096, Stream.Num() })
	{
		FCLIFrameCodec Codec(TestMagic);
		TArray<FCLIFrame> Frames;
		int64 StrayBytes = 0;
		for (int32 Pos = 0; Pos < Stream.Num(); Pos += Chunk)
		{
			Codec.Feed(TConstArrayView<uint8>(Stream.GetData() + Pos, FMath::Min(Chunk, Stream.Num() - Pos)),
				[&](FCLIFrame& F) { Frames.Add(MoveTemp(F)); },
				[&](TConstArrayView<uint8> S) { StrayBytes += S.Num(); });
		}
		const FString Ctx = FString::Printf(TEXT("chunk %d"), Chunk);
		if (!TestEqual(Ctx + TEXT(": frame count"), Frames.Num(), 3)) { continue; }
		TestEqual(Ctx + TEXT(": no stray bytes"), StrayBytes, (int64)0);
		TestEqual(Ctx + TEXT(": types"), FString::Printf(TEXT("%d%d%d"), Frames[0].Type, Frames[1].Type, Frames[2].Type), FString(TEXT("135")));
		TestEqual(Ctx + TEXT(": header 0"), Frames[0].HeaderAsString(), FString(TEXT("{\"a\":1}")));
		TestEqual(Ctx + TEXT(": utf8 header"), Frames[1].HeaderAsString(), Utf8Header);
		TestTrue(Ctx + TEXT(": binary intact"), Frames[1].Binary == Big);
		TestEqual(Ctx + TEXT(": empty frame"), Frames[2].Header.Num() + Frames[2].Binary.Num(), 0);
		TestEqual(Ctx + TEXT(": nothing left buffered"), Codec.NumBufferedBytes(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIFrameCodecStrayTest, "CLISystem.FrameCodec.StrayAndResync", CLISystemTests::Flags)
bool FCLIFrameCodecStrayTest::RunTest(const FString&)
{
	auto Bytes = [](const ANSICHAR* S) { return TArray<uint8>(reinterpret_cast<const uint8*>(S), FCStringAnsi::Strlen(S)); };

	TArray<uint8> Stream = Bytes("Traceback: boom\n");
	Stream.Append(FCLIFrameCodec::Encode(TestMagic, 1, TEXT("one")));
	Stream.Append(Bytes("TS"));	//partial marker that never completes
	Stream.Append(Bytes(" native log line\n"));
	//A marker followed by an absurd header length: must be skipped, not waited on forever
	Stream.Append(TestMagic.Bytes, 4);
	Stream.Add(1);
	Stream.Append({ 0xFF, 0xFF, 0xFF, 0x7F });
	Stream.Append(FCLIFrameCodec::Encode(TestMagic, 2, TEXT("two")));
	Stream.Append(Bytes("tail"));

	FCLIFrameCodec Codec(TestMagic, 1024 * 1024);
	TArray<FString> Headers;
	TArray<uint8> Stray;
	for (int32 Pos = 0; Pos < Stream.Num(); Pos += 5)
	{
		Codec.Feed(TConstArrayView<uint8>(Stream.GetData() + Pos, FMath::Min(5, Stream.Num() - Pos)),
			[&](FCLIFrame& F) { Headers.Add(F.HeaderAsString()); },
			[&](TConstArrayView<uint8> S) { Stray.Append(S.GetData(), S.Num()); });
	}
	Codec.Flush([&](TConstArrayView<uint8> S) { Stray.Append(S.GetData(), S.Num()); });

	TestEqual(TEXT("both real frames decoded"), FString::Join(Headers, TEXT(",")), FString(TEXT("one,two")));
	//Compare raw bytes: the skipped fake frame left invalid UTF-8 (0xFF) in the stray stream
	auto Find = [&Stray](const ANSICHAR* Needle) -> int32
	{
		const int32 Len = FCStringAnsi::Strlen(Needle);
		for (int32 i = 0; i + Len <= Stray.Num(); ++i)
		{
			if (FMemory::Memcmp(Stray.GetData() + i, Needle, Len) == 0) { return i; }
		}
		return INDEX_NONE;
	};
	TestEqual(TEXT("leading text surfaced first"), Find("Traceback: boom\n"), 0);
	TestTrue(TEXT("text between frames surfaced"), Find("TS native log line\n") != INDEX_NONE);
	TestEqual(TEXT("trailing text surfaced last"), Find("tail"), Stray.Num() - 4);
	TestTrue(TEXT("skipped fake marker surfaced as stray"), Find("ST\x01\x01") != INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIFrameCodecBinaryTableTest, "CLISystem.FrameCodec.BinaryTable", CLISystemTests::Flags)
bool FCLIFrameCodecBinaryTableTest::RunTest(const FString&)
{
	TArray<TArray<uint8>> In;
	In.Add({ 1, 2, 3 });
	In.Add({});
	In.Add(TArray<uint8>());
	In[2].Init(9, 70000);

	TArray<uint8> Table;
	FCLIFrameCodec::BuildBinaryTable(In, Table);
	TArray<TArray<uint8>> Out;
	TestTrue(TEXT("parses"), FCLIFrameCodec::ParseBinaryTable(Table, Out));
	TestTrue(TEXT("round trip"), Out == In);

	Table.SetNum(Table.Num() - 1);
	TestFalse(TEXT("truncated table rejected"), FCLIFrameCodec::ParseBinaryTable(Table, Out));
	return true;
}

// ------------------------------------------------------------------------------------------------- process

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIEmptyWorkingDirectoryTest, "CLISystem.SubProcess.EmptyWorkingDirectory", CLISystemTests::Flags)
bool FCLIEmptyWorkingDirectoryTest::RunTest(const FString&)
{
	//Default (polling, string) mode with no working directory: used to fail with ERROR_INVALID_NAME on Windows
	TSharedPtr<FCapture> C = MakeShared<FCapture>();
	TSharedPtr<FSubProcessHandler> H = MakeHandler(C);
	FProcessParams P = ShellParams(RunArgs(EchoCommand(TEXT("cli_ok"))));
	P.OptionalWorkingDirectory.Empty();
	H->StartProcess(P);

	WaitFor(this, 10.0, TEXT("process never ended"), [C] { return C->bEnded.load(); });
	Then([this, C, H]
	{
		TestTrue(TEXT("launched"), C->bBeganOk.load());
		TestTrue(TEXT("output received"), C->Text().Contains(TEXT("cli_ok")));
		FSubProcessHandler::Retire(H, 1.f);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIBlockingAsyncEchoTest, "CLISystem.SubProcess.BlockingReadAsyncWrites", CLISystemTests::Flags)
bool FCLIBlockingAsyncEchoTest::RunTest(const FString&)
{
	constexpr int32 Lines = 100;
	TSharedPtr<FCapture> C = MakeShared<FCapture>();
	TSharedPtr<FSubProcessHandler> H = MakeHandler(C);
	FProcessParams P = ShellParams(InteractiveArgs());
	P.bProcessInBytes = true;
	P.bOutputToGameThread = false;
	P.bBlockingRead = true;
	P.bAsyncStdinWrites = true;
	H->StartProcess(P);

	//Sent before the process exists: async writes keep them until it starts
	for (int32 i = 0; i < Lines; ++i)
	{
		H->SendInput(EchoCommand(FString::Printf(TEXT("line_%d"), i)));
	}

	WaitFor(this, 20.0, TEXT("not every echoed line arrived in order"), [C] { return ContainsInOrder(NormalizeNewlines(C->Text()), TEXT("line_"), Lines); });
	Then([this, C, H]
	{
		TestTrue(TEXT("launched"), C->bBeganOk.load());
		C->StartTime = FPlatformTime::Seconds();
		H->StopProcess();
	});
	WaitFor(this, 5.0, TEXT("blocking reader did not finish after StopProcess"), [C] { return C->bEnded.load(); });
	Then([this, C, H]() mutable
	{
		AddInfo(FString::Printf(TEXT("StopProcess -> OnProcessEnd: %.0f ms"), (C->EndTime.load() - C->StartTime) * 1000.0));
		const double T0 = FPlatformTime::Seconds();
		H.Reset(); //Joins the threads
		TestTrue(TEXT("destroying a stopped handler is quick"), FPlatformTime::Seconds() - T0 < 1.0);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIDrainOnExitTest, "CLISystem.SubProcess.DrainOnExit", CLISystemTests::Flags)
bool FCLIDrainOnExitTest::RunTest(const FString&)
{
	//2 MB of printable text written by a process that exits right after: every byte must arrive
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("CLISystemDrain.txt"));
	FString Content;
	const FString Line = TEXT("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTU\n");
	while (Content.Len() < 2 * 1024 * 1024) { Content += Line; }
	FFileHelper::SaveStringToFile(Content, *File, FFileHelper::EEncodingOptions::ForceAnsi);
	const int64 Expected = IFileManager::Get().FileSize(*File);

#if PLATFORM_WINDOWS
	const FString Command = FString::Printf(TEXT("type \"%s\""), *FPaths::ConvertRelativePathToFull(File).Replace(TEXT("/"), TEXT("\\")));
#else
	const FString Command = FString::Printf(TEXT("cat '%s'"), *File);
#endif

	struct FCase { const TCHAR* Name; bool bBlocking; };
	for (const FCase Case : { FCase{ TEXT("polling+bDrainOutputOnExit"), false }, FCase{ TEXT("bBlockingRead"), true } })
	{
		TSharedPtr<FCapture> C = MakeShared<FCapture>();
		TSharedPtr<FSubProcessHandler> H = MakeHandler(C);
		FProcessParams P = ShellParams(RunArgs(Command));
		P.bProcessInBytes = true;
		P.bOutputToGameThread = false;
		P.bDrainOutputOnExit = !Case.bBlocking;
		P.bBlockingRead = Case.bBlocking;
		const FString Name = Case.Name;
		Then([H, P] { H->StartProcess(P); });
		WaitFor(this, 20.0, Name + TEXT(": process never ended"), [C] { return C->bEnded.load(); });
		Then([this, C, H, Name, Expected]
		{
			TestEqual(Name + TEXT(": bytes received"), C->NumBytes(), Expected);
			FSubProcessHandler::Retire(H, 1.f);
		});
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIGrandchildHoldsPipeTest, "CLISystem.SubProcess.GrandchildHoldsPipe", CLISystemTests::Flags)
bool FCLIGrandchildHoldsPipeTest::RunTest(const FString&)
{
	//The shell exits at once but leaves a background child holding the output pipe for ~3s. The blocking
	//reader must notice the exit instead of waiting for EOF.
	TSharedPtr<FCapture> C = MakeShared<FCapture>();
	TSharedPtr<FSubProcessHandler> H = MakeHandler(C);
#if PLATFORM_WINDOWS
	FProcessParams P = ShellParams(RunArgs(TEXT("start /b ping -n 4 127.0.0.1 & echo parent_done")));
#else
	FProcessParams P = ShellParams(RunArgs(TEXT("sleep 3 & echo parent_done")));
#endif
	P.bProcessInBytes = true;
	P.bOutputToGameThread = false;
	P.bBlockingRead = true;
	C->StartTime = FPlatformTime::Seconds();
	H->StartProcess(P);

	WaitFor(this, 10.0, TEXT("process never ended"), [C] { return C->bEnded.load(); });
	Then([this, C, H]() mutable
	{
		const double Seconds = C->EndTime.load() - C->StartTime;
		AddInfo(FString::Printf(TEXT("ended after %.2f s"), Seconds));
		TestTrue(TEXT("output received"), C->Text().Contains(TEXT("parent_done")));
		TestTrue(TEXT("ended without waiting for the background child"), Seconds < 2.5);
		H.Reset();
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCLIDestroyWhileRunningTest, "CLISystem.SubProcess.DestroyWhileRunning", CLISystemTests::Flags)
bool FCLIDestroyWhileRunningTest::RunTest(const FString&)
{
	//Threaded modes join on destruction: releasing a handler mid-run must neither crash nor hang
	TSharedPtr<FCapture> C = MakeShared<FCapture>();
	TSharedPtr<FSubProcessHandler> H = MakeHandler(C);
	FProcessParams P = ShellParams(InteractiveArgs());
	P.bProcessInBytes = true;
	P.bOutputToGameThread = true; //Exercises the game-thread hand-off during shutdown
	P.bBlockingRead = true;
	P.bAsyncStdinWrites = true;
	H->StartProcess(P);
	H->SendInput(EchoCommand(TEXT("alive")));

	WaitFor(this, 10.0, TEXT("no output before destroy"), [C] { return C->Text().Contains(TEXT("alive")); });
	Then([this, H]() mutable
	{
		const double T0 = FPlatformTime::Seconds();
		H->SendInput(EchoCommand(TEXT("more")));
		H.Reset();
		const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		AddInfo(FString::Printf(TEXT("destructor joined in %.1f ms"), Ms));
		TestTrue(TEXT("destructor joined quickly"), Ms < 2000.0);
	});
	//Let any game-thread tasks queued before the destroy run: they must see the handler is gone
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, C] { TestFalse(TEXT("no end event after destroy"), C->bEnded.load()); }, 0.5f));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
