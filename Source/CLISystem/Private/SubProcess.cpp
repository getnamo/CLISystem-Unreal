#include "SubProcess.h"
#include "Async/Async.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include <atomic>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_UNIX
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogCLISystem, Log, All);

namespace CLISubProcess
{
	constexpr int32 ReadChunkBytes = 1024 * 1024;
	//Once the process has exited, stop reading after the pipe has been quiet this long (something the
	//process started may still hold the pipe open, so EOF might never come).
	constexpr int32 DrainQuietMs = 100;
	constexpr int32 PollIntervalMs = 50;
	constexpr int32 JoinTimeoutMs = 5000;
	constexpr int32 MaxDrainReads = 100000;

	static const TCHAR* WorkingDirectoryOrNull(const FString& Dir)
	{
		//An empty string makes CreateProcess fail with ERROR_INVALID_NAME; null means "inherit ours".
		return Dir.IsEmpty() ? nullptr : *Dir;
	}

	static bool WriteAll(void* Pipe, const TArray<uint8>& Bytes)
	{
		const uint8* Data = Bytes.GetData();
		int32 Left = Bytes.Num();
		while (Left > 0)
		{
			int32 Written = 0;
			if (!FPlatformProcess::WritePipe(Pipe, Data, Left, &Written) || Written <= 0)
			{
				return false;
			}
			Data += Written;
			Left -= Written;
		}
		return true;
	}

	static TArray<uint8> LineToUtf8(const FString& Text)
	{
		//Same bytes WritePipe(FString) sends: UTF-8 plus a newline
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Reserve(Utf8.Length() + 1);
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Bytes.Add('\n');
		return Bytes;
	}

#if PLATFORM_WINDOWS
	//Anonymous pipes can't be waited on, so the blocking reader uses an overlapped named pipe instead: the
	//reader waits on data, a stop request and process exit at once. 1MB buffer (anonymous pipes get 4KB).
	static bool CreateOverlappedOutputPipe(void*& OutRead, void*& OutWrite)
	{
		static std::atomic<uint32> Counter{ 0 };
		const FString Name = FString::Printf(TEXT("\\\\.\\pipe\\CLISystem-%u-%u-%s"),
			::GetCurrentProcessId(), ++Counter, *FGuid::NewGuid().ToString());

		HANDLE Read = ::CreateNamedPipeW(*Name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, ReadChunkBytes, 0, nullptr);
		if (Read == INVALID_HANDLE_VALUE)
		{
			UE_LOG(LogCLISystem, Warning, TEXT("CreateNamedPipe failed (%u), falling back to polling"), ::GetLastError());
			return false;
		}

		//The child's end: inheritable, synchronous
		SECURITY_ATTRIBUTES Attr = { sizeof(SECURITY_ATTRIBUTES), nullptr, 1 };
		HANDLE Write = ::CreateFileW(*Name, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, &Attr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (Write == INVALID_HANDLE_VALUE)
		{
			UE_LOG(LogCLISystem, Warning, TEXT("Opening pipe client failed (%u), falling back to polling"), ::GetLastError());
			::CloseHandle(Read);
			return false;
		}
		OutRead = Read;
		OutWrite = Write;
		return true;
	}

	//Blocks until output arrives. Returns on EOF, on a stop request, or once the process has exited and the
	//pipe stays quiet for DrainQuietMs.
	static void ReadBlocking(void* ReadPipe, FProcHandle& Process, void* StopEvent, const std::atomic<bool>& bStop,
		TArray<uint8>& Buffer, TFunctionRef<void(const TArray<uint8>&)> Deliver)
	{
		OVERLAPPED Ov = {};
		Ov.hEvent = ::CreateEventW(nullptr, 1, 0, nullptr);
		bool bProcessExited = false;

		while (!bStop)
		{
			::ResetEvent(Ov.hEvent);
			Buffer.SetNumUninitialized(ReadChunkBytes, EAllowShrinking::No);
			::DWORD Got = 0;
			::DWORD Err = ERROR_SUCCESS;

			if (::ReadFile(ReadPipe, Buffer.GetData(), ReadChunkBytes, nullptr, &Ov))
			{
				::GetOverlappedResult(ReadPipe, &Ov, &Got, 0);
			}
			else
			{
				Err = ::GetLastError();
				if (Err == ERROR_IO_PENDING)
				{
					HANDLE Waits[3] = { Ov.hEvent, StopEvent, Process.Get() };
					::DWORD Wait = ::WaitForMultipleObjects(bProcessExited ? 2 : 3, Waits, 0, bProcessExited ? DrainQuietMs : INFINITE);
					if (Wait == WAIT_OBJECT_0 + 2)
					{
						//Exited while a read is pending: give it a moment to complete with more data or EOF
						bProcessExited = true;
						Wait = ::WaitForMultipleObjects(2, Waits, 0, DrainQuietMs);
					}
					if (Wait != WAIT_OBJECT_0)
					{
						//Stop requested, or the process is gone and the pipe stayed quiet
						::CancelIoEx(ReadPipe, &Ov);
						if (::GetOverlappedResult(ReadPipe, &Ov, &Got, 1) && Got > 0)
						{
							Buffer.SetNum((int32)Got, EAllowShrinking::No);
							Deliver(Buffer);
						}
						break;
					}
					Err = ::GetOverlappedResult(ReadPipe, &Ov, &Got, 0) ? ERROR_SUCCESS : ::GetLastError();
				}
			}

			if (Err != ERROR_SUCCESS)
			{
				//ERROR_BROKEN_PIPE: every writer closed (EOF)
				break;
			}
			if (Got > 0)
			{
				Buffer.SetNum((int32)Got, EAllowShrinking::No);
				Deliver(Buffer);
			}
		}
		::CloseHandle(Ov.hEvent);
	}
#elif PLATFORM_UNIX
	static void ReadBlocking(void* ReadPipe, FProcHandle& Process, void* /*StopEvent*/, const std::atomic<bool>& bStop,
		TArray<uint8>& Buffer, TFunctionRef<void(const TArray<uint8>&)> Deliver)
	{
		const int Fd = static_cast<FPipeHandle*>(ReadPipe)->GetHandle();
		::fcntl(Fd, F_SETFL, ::fcntl(Fd, F_GETFL) | O_NONBLOCK);
		bool bProcessExited = false;

		while (!bStop)
		{
			pollfd Poll = { Fd, POLLIN, 0 };
			const int Ready = ::poll(&Poll, 1, bProcessExited ? DrainQuietMs : PollIntervalMs);
			if (Ready > 0)
			{
				Buffer.SetNumUninitialized(ReadChunkBytes, EAllowShrinking::No);
				const ssize_t Got = ::read(Fd, Buffer.GetData(), ReadChunkBytes);
				if (Got > 0)
				{
					Buffer.SetNum((int32)Got, EAllowShrinking::No);
					Deliver(Buffer);
					continue;
				}
				if (Got == 0)
				{
					break; //EOF: every writer closed
				}
				if (errno == EAGAIN || errno == EINTR)
				{
					continue;
				}
				break;
			}
			if (Ready < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				break;
			}
			//Timeout: the poll interval only bounds how quickly a stop or exit is noticed
			if (bProcessExited)
			{
				break; //Process gone and the pipe stayed quiet
			}
			bProcessExited = !FPlatformProcess::IsProcRunning(Process);
		}
	}
#endif

	static constexpr bool bPlatformSupportsBlockingRead = PLATFORM_WINDOWS || PLATFORM_UNIX;
}

/** State shared by the handler and its threads in the opt-in threaded modes, so threads never outlive it. */
struct FSubProcessThreads
{
	std::atomic<bool> bStopRequested{ false };	//StopProcess, destructor, or the run finished
	std::atomic<bool> bHandlerAlive{ true };	//Cleared by the handler's destructor (game thread)
	std::atomic<bool> bLaunchFinished{ false };	//CreateProc has returned, successfully or not
	std::atomic<bool> bWriterStarted{ false };
	bool bAsyncWrites = false;

	std::atomic<void*> InputPipeWrite{ nullptr };	//Owned by the writer thread when bAsyncWrites
	TQueue<TArray<uint8>, EQueueMode::Mpsc> WriteQueue;

	FEvent* WriterWake = nullptr;
	FEvent* ReaderDone = nullptr;
	FEvent* WriterDone = nullptr;
	void* StopEvent = nullptr;	//Windows: lets the blocking reader wake on stop

	FSubProcessThreads()
	{
		WriterWake = FPlatformProcess::GetSynchEventFromPool(false);
		ReaderDone = FPlatformProcess::GetSynchEventFromPool(true);
		WriterDone = FPlatformProcess::GetSynchEventFromPool(true);
#if PLATFORM_WINDOWS
		StopEvent = ::CreateEventW(nullptr, 1, 0, nullptr);
#endif
	}

	~FSubProcessThreads()
	{
		FPlatformProcess::ReturnSynchEventToPool(WriterWake);
		FPlatformProcess::ReturnSynchEventToPool(ReaderDone);
		FPlatformProcess::ReturnSynchEventToPool(WriterDone);
#if PLATFORM_WINDOWS
		::CloseHandle(StopEvent);
#endif
	}

	void RequestStop()
	{
		bStopRequested = true;
#if PLATFORM_WINDOWS
		::SetEvent(StopEvent);
#endif
		WriterWake->Trigger();
	}
};

static void RunStdinWriter(TSharedPtr<FSubProcessThreads, ESPMode::ThreadSafe> T)
{
	bool bBroken = false;
	while (!bBroken)
	{
		T->WriterWake->Wait();
		if (T->bStopRequested)
		{
			break;
		}
		void* Pipe = T->InputPipeWrite.load();
		if (Pipe == nullptr)
		{
			if (T->bLaunchFinished)
			{
				break; //Launch failed
			}
			continue; //Not started yet: keep the queue until it is
		}
		TArray<uint8> Item;
		while (!T->bStopRequested && T->WriteQueue.Dequeue(Item))
		{
			if (!CLISubProcess::WriteAll(Pipe, Item))
			{
				bBroken = true;
				break;
			}
		}
	}
	if (void* Pipe = T->InputPipeWrite.exchange(nullptr))
	{
		FPlatformProcess::ClosePipe(nullptr, Pipe);
	}
	T->WriterDone->Trigger();
}


FSubProcessHandler::FSubProcessHandler()
{

}

FSubProcessHandler::~FSubProcessHandler()
{
	State.bIsBeingDestroyed = true;

	if (Threads.IsValid())
	{
		Threads->bHandlerAlive = false;
		StopThreads(true);
		return;
	}

	StopProcess();
}

void FSubProcessHandler::StartProcess(const FProcessParams& InParams)
{
	Params = InParams;

	if ((Params.bBlockingRead && Params.bProcessInBytes) || Params.bAsyncStdinWrites)
	{
		StartProcessThreaded();
		return;
	}
	StopThreads(true);

	Async(Params.ExecutionContext, [this]
	{
		uint32 OutId;

		void* InputPipeRead = nullptr;
		void* InputPipeWrite = nullptr;
		void* OutputPipeRead = nullptr;
		void* OutputPipeWrite = nullptr;

		FPlatformProcess::CreatePipe(OutputPipeRead, OutputPipeWrite, false);
		FPlatformProcess::CreatePipe(InputPipeRead, InputPipeWrite, true);

		//Pre-pipe
		if (!Params.InitialStdInput.IsEmpty())
		{
			FPlatformProcess::WritePipe(InputPipeWrite, Params.InitialStdInput);
		}

		State.ProcessHandle = FPlatformProcess::CreateProc(
			*Params.Url,
			*Params.Params,
			Params.bLaunchDetached,
			Params.bLaunchHidden,
			Params.bLaunchReallyHidden,
			&OutId,
			Params.PriorityModifier,
			CLISubProcess::WorkingDirectoryOrNull(Params.OptionalWorkingDirectory),
			OutputPipeWrite,
			InputPipeRead);

		State.InputPipeRead = InputPipeRead;
		State.InputPipeWrite = InputPipeWrite;
		State.OutputPipeRead = OutputPipeRead;
		State.OutputPipeWrite = OutputPipeWrite;
		State.ProcessId = OutId;
		const int32 LocalId = State.ProcessId;


		if (State.ProcessHandle.IsValid())
		{
			AsyncTask(ENamedThreads::GameThread, [&, LocalId]
			{
				OnProcessBegin(LocalId, true);
			});
		}
		else
		{
			ClosePipes();

			AsyncTask(ENamedThreads::GameThread, [&, LocalId]
			{
				OnProcessBegin(LocalId, false);
				OnProcessEnd(LocalId, -1);
			});
			return;
		}

		if (Params.bProcessInBytes)
		{
			//Buffer path
			TArray<uint8> Buffer;

			auto EmitBuffer = [&]
			{
				if (Params.bOutputToGameThread)
				{
					//Lock-out reading until emits are consumed.
					FThreadSafeBool bEmitComplete;

					AsyncTask(ENamedThreads::GameThread, [&, LocalId]
					{
						OnProcessOutputBytes(LocalId, Buffer);
						bEmitComplete = true;
					});

					while (!bEmitComplete)
					{
						FPlatformProcess::Sleep(0.0001f);
					}
				}
				else
				{
					OnProcessOutputBytes(LocalId, Buffer);
				}
			};

			do
			{
				bool bSuccess = FPlatformProcess::ReadPipeToArray(State.OutputPipeRead, Buffer);

				if (bSuccess)
				{
					EmitBuffer();
				}
				else if (Params.IdleReadSleepSeconds > 0.f)
				{
					FPlatformProcess::Sleep(Params.IdleReadSleepSeconds);
				}
			} while (FPlatformProcess::IsProcRunning(State.ProcessHandle));

			//Opt-in: read what the process wrote right before it exited
			if (Params.bDrainOutputOnExit)
			{
				int32 Reads = 0;
				while (!State.bIsBeingDestroyed && State.OutputPipeRead != nullptr &&
					FPlatformProcess::ReadPipeToArray(State.OutputPipeRead, Buffer) && ++Reads < CLISubProcess::MaxDrainReads)
				{
					EmitBuffer();
				}
			}
		}
		else
		{
			//String path
			FString LatestOutput;
			do
			{
				if (Params.bWaitForSend)
				{
					WaitLockActive = true;

					while (WaitLockActive)
					{
						FPlatformProcess::Sleep(0.1);
					}
				}

				LatestOutput = FPlatformProcess::ReadPipe(State.OutputPipeRead);
				State.OutputHistory += LatestOutput;

				if (!LatestOutput.IsEmpty())
				{
					if (Params.bOutputToGameThread)
					{
						const FString SafeLatestOutput = LatestOutput;
						AsyncTask(ENamedThreads::GameThread, [&, LocalId, SafeLatestOutput]
						{
							OnProcessOutput(LocalId, SafeLatestOutput);
						});
					}
					else
					{
						if (OnProcessOutput)
						{
							OnProcessOutput(LocalId, LatestOutput);
						}
					}
				}
				else if (Params.IdleReadSleepSeconds > 0.f)
				{
					FPlatformProcess::Sleep(Params.IdleReadSleepSeconds);
				}
			} while (FPlatformProcess::IsProcRunning(State.ProcessHandle) || !LatestOutput.IsEmpty());
		}

		int ExitCode = -1;
		bool ReturnCodeValid = FPlatformProcess::GetProcReturnCode(State.ProcessHandle, &ExitCode);

		if (!ReturnCodeValid)
		{
			ExitCode = -1;
		}
		//Only emit if we're not cleaning up
		if (!State.bIsBeingDestroyed)
		{
			AsyncTask(ENamedThreads::GameThread, [this, LocalId, ExitCode]
			{
				OnProcessEnd(LocalId, ExitCode);
			});
		}

		ClosePipes();
	});
}

void FSubProcessHandler::StopProcess()
{
	if (Threads.IsValid())
	{
		StopThreads(false);
		return;
	}

	if (State.ProcessHandle.IsValid())
	{
		ClosePipes();

		//Maybe queue input to bg thread?
		FPlatformProcess::TerminateProc(State.ProcessHandle, false);
	}
}

void FSubProcessHandler::SendInput(const TArray<uint8>& Bytes)
{
	TSharedPtr<FSubProcessThreads, ESPMode::ThreadSafe> T = Threads;
	if (T.IsValid() && T->bAsyncWrites)
	{
		if (!T->bStopRequested && Bytes.Num() > 0)
		{
			T->WriteQueue.Enqueue(Bytes);
			T->WriterWake->Trigger();
		}
		return;
	}

	if (State.ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(State.ProcessHandle))
	{
		FPlatformProcess::WritePipe(State.InputPipeWrite, Bytes.GetData(), Bytes.Num());
	}
}

void FSubProcessHandler::SendInput(const FString& Text)
{
	TSharedPtr<FSubProcessThreads, ESPMode::ThreadSafe> T = Threads;
	if (T.IsValid() && T->bAsyncWrites)
	{
		if (!T->bStopRequested && !Text.IsEmpty())
		{
			T->WriteQueue.Enqueue(CLISubProcess::LineToUtf8(Text));
			T->WriterWake->Trigger();
		}
		WaitLockActive = false;
		return;
	}

	if (State.ProcessHandle.IsValid() && FPlatformProcess::IsProcRunning(State.ProcessHandle))
	{
		FPlatformProcess::WritePipe(State.InputPipeWrite, Text);
	}
	WaitLockActive = false;
}

void FSubProcessHandler::Retire(TSharedPtr<FSubProcessHandler> Handler, float KeepAliveSeconds)
{
	if (!Handler.IsValid())
	{
		return;
	}
	//The ticker delegate holds the last reference until it fires once
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Handler](float)
	{
		return false;
	}), KeepAliveSeconds);
}

void FSubProcessHandler::ClosePipes()
{
	if (State.InputPipeRead != nullptr || State.InputPipeWrite != nullptr)
	{
		FPlatformProcess::ClosePipe(State.InputPipeRead, State.InputPipeWrite);
		State.InputPipeRead = nullptr;
		State.InputPipeWrite = nullptr;
	}
	if (State.OutputPipeRead != nullptr || State.OutputPipeWrite != nullptr)
	{
		FPlatformProcess::ClosePipe(State.OutputPipeRead, State.OutputPipeWrite);
		State.OutputPipeRead = nullptr;
		State.OutputPipeWrite = nullptr;
	}
}

// ---------------------------------------------------------------------------------------------------------
// Opt-in threaded modes (bBlockingRead / bAsyncStdinWrites). The threads own their pipe handles and close
// them on exit; the handler only signals them. Destroying the handler joins them.

void FSubProcessHandler::StartProcessThreaded()
{
	//One run at a time per handler: finish any previous one first
	StopThreads(true);

	TSharedPtr<FSubProcessThreads, ESPMode::ThreadSafe> T = MakeShared<FSubProcessThreads, ESPMode::ThreadSafe>();
	T->bAsyncWrites = Params.bAsyncStdinWrites;
	Threads = T;
	State.ProcessHandle = FProcHandle();

	if (T->bAsyncWrites)
	{
		T->bWriterStarted = true;
		Async(EAsyncExecution::Thread, [T]
		{
			RunStdinWriter(T);
		});
	}

	Async(Params.ExecutionContext, [this, T]
	{
		RunThreadedReader(T);
		T->RequestStop(); //Run is over: release the writer
		T->ReaderDone->Trigger();
	});
}

void FSubProcessHandler::StopThreads(bool bJoin)
{
	TSharedPtr<FSubProcessThreads, ESPMode::ThreadSafe> T = Threads;
	if (!T.IsValid())
	{
		return;
	}

	T->RequestStop();
	if (State.ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(State.ProcessHandle, false);
	}

	if (bJoin)
	{
		const bool bReaderJoined = T->ReaderDone->Wait(CLISubProcess::JoinTimeoutMs);
		const bool bWriterJoined = !T->bWriterStarted || T->WriterDone->Wait(CLISubProcess::JoinTimeoutMs);
		if (!bReaderJoined || !bWriterJoined)
		{
			UE_LOG(LogCLISystem, Error, TEXT("SubProcess threads for %s did not stop within %dms"), *Params.Url, CLISubProcess::JoinTimeoutMs);
		}
		Threads.Reset();
	}
}

void FSubProcessHandler::RunThreadedReader(TSharedPtr<FSubProcessThreads, ESPMode::ThreadSafe> T)
{
	const FProcessParams P = Params;

	void* InputPipeRead = nullptr;
	void* InputPipeWrite = nullptr;
	void* OutputPipeRead = nullptr;
	void* OutputPipeWrite = nullptr;

	const bool bWantBlocking = P.bBlockingRead && P.bProcessInBytes && CLISubProcess::bPlatformSupportsBlockingRead;
	bool bOutputPipeReady = false;
#if PLATFORM_WINDOWS
	bOutputPipeReady = bWantBlocking && CLISubProcess::CreateOverlappedOutputPipe(OutputPipeRead, OutputPipeWrite);
	const bool bBlocking = bOutputPipeReady;
#else
	const bool bBlocking = bWantBlocking;
#endif
	if (!bOutputPipeReady)
	{
		FPlatformProcess::CreatePipe(OutputPipeRead, OutputPipeWrite, false);
	}
	FPlatformProcess::CreatePipe(InputPipeRead, InputPipeWrite, true);

	if (!P.InitialStdInput.IsEmpty())
	{
		FPlatformProcess::WritePipe(InputPipeWrite, P.InitialStdInput);
	}

	uint32 OutId = 0;
	FProcHandle Process = FPlatformProcess::CreateProc(
		*P.Url,
		*P.Params,
		P.bLaunchDetached,
		P.bLaunchHidden,
		P.bLaunchReallyHidden,
		&OutId,
		P.PriorityModifier,
		CLISubProcess::WorkingDirectoryOrNull(P.OptionalWorkingDirectory),
		OutputPipeWrite,
		InputPipeRead);

	//The child has its own copies now. Closing ours lets the output pipe report EOF once the child (and
	//anything it handed the pipe to) is gone.
	FPlatformProcess::ClosePipe(InputPipeRead, OutputPipeWrite);

	State.ProcessHandle = Process;
	State.ProcessId = OutId;
	State.InputPipeRead = nullptr;
	State.OutputPipeWrite = nullptr;
	State.OutputPipeRead = OutputPipeRead;
	State.InputPipeWrite = T->bAsyncWrites ? nullptr : InputPipeWrite;
	const int32 LocalId = OutId;

	if (!Process.IsValid())
	{
		State.OutputPipeRead = nullptr;
		State.InputPipeWrite = nullptr;
		FPlatformProcess::ClosePipe(OutputPipeRead, InputPipeWrite);
		T->bLaunchFinished = true;
		T->RequestStop();
		AsyncTask(ENamedThreads::GameThread, [this, T, LocalId]
		{
			if (T->bHandlerAlive)
			{
				if (OnProcessBegin) { OnProcessBegin(LocalId, false); }
				if (OnProcessEnd) { OnProcessEnd(LocalId, -1); }
			}
		});
		return;
	}

	if (T->bAsyncWrites)
	{
		T->InputPipeWrite = InputPipeWrite;
	}
	T->bLaunchFinished = true;
	T->WriterWake->Trigger();

	AsyncTask(ENamedThreads::GameThread, [this, T, LocalId]
	{
		if (T->bHandlerAlive && OnProcessBegin)
		{
			OnProcessBegin(LocalId, true);
		}
	});

	if (T->bStopRequested)
	{
		//A stop raced the launch
		FPlatformProcess::TerminateProc(Process, false);
	}

	auto DeliverBytes = [this, &T, &P, LocalId](const TArray<uint8>& Bytes)
	{
		if (P.bOutputToGameThread)
		{
			TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bEmitComplete = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
			AsyncTask(ENamedThreads::GameThread, [this, T, LocalId, Bytes, bEmitComplete]
			{
				if (T->bHandlerAlive && OnProcessOutputBytes)
				{
					OnProcessOutputBytes(LocalId, Bytes);
				}
				*bEmitComplete = true;
			});
			while (!*bEmitComplete && !T->bStopRequested)
			{
				FPlatformProcess::Sleep(0.0001f);
			}
		}
		else if (OnProcessOutputBytes)
		{
			OnProcessOutputBytes(LocalId, Bytes);
		}
	};

	auto DeliverString = [this, &T, &P, LocalId](const FString& Text)
	{
		State.OutputHistory += Text;
		if (P.bOutputToGameThread)
		{
			AsyncTask(ENamedThreads::GameThread, [this, T, LocalId, Text]
			{
				if (T->bHandlerAlive && OnProcessOutput)
				{
					OnProcessOutput(LocalId, Text);
				}
			});
		}
		else if (OnProcessOutput)
		{
			OnProcessOutput(LocalId, Text);
		}
	};

	TArray<uint8> Buffer;
	if (bBlocking)
	{
#if PLATFORM_WINDOWS || PLATFORM_UNIX
		CLISubProcess::ReadBlocking(OutputPipeRead, Process, T->StopEvent, T->bStopRequested, Buffer, DeliverBytes);
#endif
	}
	else
	{
		auto ReadOnce = [&]() -> bool
		{
			if (P.bProcessInBytes)
			{
				if (FPlatformProcess::ReadPipeToArray(OutputPipeRead, Buffer))
				{
					DeliverBytes(Buffer);
					return true;
				}
				return false;
			}
			const FString Text = FPlatformProcess::ReadPipe(OutputPipeRead);
			if (Text.IsEmpty())
			{
				return false;
			}
			DeliverString(Text);
			return true;
		};

		while (!T->bStopRequested && FPlatformProcess::IsProcRunning(Process))
		{
			if (!ReadOnce() && P.IdleReadSleepSeconds > 0.f)
			{
				FPlatformProcess::Sleep(P.IdleReadSleepSeconds);
			}
		}
		//Read what the process wrote right before it exited
		int32 Reads = 0;
		while (!T->bStopRequested && ReadOnce() && ++Reads < CLISubProcess::MaxDrainReads)
		{
		}
	}

	//Output can end (EOF) before the process does
	while (!T->bStopRequested && FPlatformProcess::IsProcRunning(Process))
	{
		FPlatformProcess::Sleep(0.005f);
	}
	if (T->bStopRequested && FPlatformProcess::IsProcRunning(Process))
	{
		FPlatformProcess::TerminateProc(Process, false);
	}

	int32 ExitCode = -1;
	if (!FPlatformProcess::GetProcReturnCode(Process, &ExitCode))
	{
		ExitCode = -1;
	}
	if (!State.bIsBeingDestroyed)
	{
		AsyncTask(ENamedThreads::GameThread, [this, T, LocalId, ExitCode]
		{
			if (T->bHandlerAlive && OnProcessEnd)
			{
				OnProcessEnd(LocalId, ExitCode);
			}
		});
	}

	State.OutputPipeRead = nullptr;
	FPlatformProcess::ClosePipe(OutputPipeRead, nullptr);
	if (!T->bAsyncWrites)
	{
		State.InputPipeWrite = nullptr;
		FPlatformProcess::ClosePipe(nullptr, InputPipeWrite);
	}
}
