#pragma once
#include "GenericPlatform/GenericPlatformProcess.h"
#include "SubProcess.generated.h"


USTRUCT(BlueprintType)
struct CLISYSTEM_API FProcessParams
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category="FProcessParams")
	FString Url;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	FString Params;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	FString InitialStdInput;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bLaunchDetached = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bLaunchHidden = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bLaunchReallyHidden = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	int32 PriorityModifier = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	FString OptionalWorkingDirectory;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bOutputToGameThread = true;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bProcessInBytes = false;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bWaitForSend = false;

	//Seconds the output reader sleeps when a poll finds no new output. Pipe reads don't
	//block, so 0 (default) polls continuously for the lowest latency but keeps a core busy
	//while the process runs; e.g. 0.001 trades up to ~1ms latency for an idle thread.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	float IdleReadSleepSeconds = 0.f;

	//Opt-in, bytes mode only (Win64 and Linux; other platforms fall back to polling). The output reader
	//blocks until output arrives instead of polling: no busy core and no idle-sleep latency. It reads all
	//remaining output after the process exits, and a handler using it joins its threads when destroyed.
	//IdleReadSleepSeconds is ignored when this is on.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bBlockingRead = false;

	//Opt-in, bytes mode. Keep reading after the process exits until the pipe is empty, so output written
	//right before exit (last messages, crash dumps) isn't lost. String mode and bBlockingRead always drain.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bDrainOutputOnExit = false;

	//Opt-in. SendInput queues the data and returns immediately; a writer thread writes it to the pipe in
	//order, so the calling thread never blocks on a full pipe. Input sent before the process has started
	//is kept and written once it starts.
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessParams")
	bool bAsyncStdinWrites = false;

	EAsyncExecution ExecutionContext = EAsyncExecution::Thread;
};

USTRUCT(BlueprintType)
struct CLISYSTEM_API FProcessState
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessState")
	int32 ProcessId = -1;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "FProcessState")
	FString OutputHistory;

	//These are not blueprint accessible
	FProcHandle ProcessHandle;
	void* OutputPipeWrite;
	void* OutputPipeRead;
	void* InputPipeWrite;
	void* InputPipeRead;
	bool bIsBeingDestroyed = false;
};


class CLISYSTEM_API FSubProcessHandler
{
public:

	FSubProcessHandler();
	~FSubProcessHandler();

	TFunction<void(const int32 ProcessId, bool StartSucceded)> OnProcessBegin = nullptr;
	TFunction<void(const int32 ProcessId, const FString& OutputString)> OnProcessOutput = nullptr;
	TFunction<void(const int32 ProcessId, const TArray<uint8>& OutputBytes)> OnProcessOutputBytes = nullptr;
	TFunction<void(const int32 ProcessId, int32 ReturnCode)> OnProcessEnd = nullptr;

	void StartProcess(const FProcessParams& InParams);
	void StopProcess();

	void SendInput(const TArray<uint8>& Bytes);
	void SendInput(const FString& Text);

	FProcessParams Params;

	/** Keeps a handler alive for KeepAliveSeconds before releasing it (game thread). In the default polling
	mode the reader thread isn't joined, so a handler released while its process is still shutting down can
	be used after it's freed; retire it instead. Handlers using bBlockingRead or bAsyncStdinWrites join their
	threads on destruction and don't need this. */
	static void Retire(TSharedPtr<FSubProcessHandler> Handler, float KeepAliveSeconds = 5.f);

protected:

	void ClosePipes();
	FProcessState State;

	FThreadSafeBool WaitLockActive;

private:

	/** Opt-in modes (bBlockingRead / bAsyncStdinWrites): runs one process with threads that own their pipes. */
	void StartProcessThreaded();
	void RunThreadedReader(TSharedPtr<struct FSubProcessThreads, ESPMode::ThreadSafe> Threads);
	void StopThreads(bool bJoin);

	TSharedPtr<struct FSubProcessThreads, ESPMode::ThreadSafe> Threads;
};