# CLISystem - Unreal

Example wrapper - [PiperCLI](https://github.com/getnamo/PiperCLI-Unreal)

## How to use

See PiperCLI plugin's [PiperComponent](https://github.com/getnamo/PiperCLI-Unreal/blob/main/Source/PiperCLI/Private/PiperComponent.cpp) for a reference implementation.

1. Subclass `CLIProcessComponent` or the native `ProcessHandler`. 
2. Update your `FProcessParams`, typically Url and initial params passed in.
3. Listen to  `OnProcessOutput` or byte version if enabled and any other callbacks.
4. Use `SendInput` on the component to send data to the CLI process.

## Output polling

Pipe reads never block, so by default the output reader thread polls continuously: lowest latency, but it keeps a CPU core busy while the process runs. Set `FProcessParams.IdleReadSleepSeconds` (e.g. `0.001`) to sleep that long whenever a poll finds no new output. The default of `0` keeps the original behavior.

## Opt-in options (1.4.0)

All of these default to off, so existing users see no change.

| `FProcessParams` field | What it does |
|---|---|
| `bBlockingRead` | **Bytes mode, Win64 and Linux.** The reader blocks until output arrives instead of polling, so you get the latency of busy polling at almost no idle CPU (table below). It reads everything left after the process exits. It also notices the exit even when something the process started still holds the pipe open. Other platforms fall back to polling. |
| `bDrainOutputOnExit` | **Bytes mode.** Keep reading after the process exits until the pipe is empty, so the last output (final messages, crash dumps) isn't lost. String mode and `bBlockingRead` always do this. |
| `bAsyncStdinWrites` | `SendInput` queues the data and returns immediately. A writer thread writes it in order, so the calling thread never blocks on a full pipe. Input sent before the process has started is written once it starts. |

A handler with `bBlockingRead` or `bAsyncStdinWrites` joins its threads when it's destroyed. You can release it at any time; the 5 second keep-alive below isn't needed.

Measured round trip (UE → child → UE, small message, Win64) and the CPU the UE process used while the child was idle:

| Mode | Round trip p50 | Idle CPU |
|---|---|---|
| Default (busy polling) | 59 µs | 100% of one core |
| `IdleReadSleepSeconds = 0.001` | 1.5 ms | 1% |
| `bBlockingRead` | 67 µs | 1% |
| `bBlockingRead` + `bAsyncStdinWrites` | 88 µs | 0% |

## Releasing a handler

In the default polling mode the reader thread isn't joined, so freeing a handler while its process is still shutting down can crash. Hand it to `FSubProcessHandler::Retire(Handler)` instead, which keeps it alive for a few seconds (game thread).

## Working directory

An empty `OptionalWorkingDirectory` now means "inherit the editor's". Before 1.4.0 it made the launch fail on Windows with `ERROR_INVALID_NAME`.

## Frame codec

`FCLIFrameCodec` (`CLIFrameCodec.h`) is a small framing protocol for bytes mode, the same layout NodeJs-Unreal uses:

```
MAGIC[4] | Type u8 | HeaderLen u32 LE | Header (usually UTF-8 JSON) | BinaryLen u32 LE | Binary
```

```cpp
static const FCLIFrameMagic Magic('M', 'Y', 'P', 1);
FCLIFrameCodec Decoder(Magic);

// Reader thread (bOutputToGameThread = false)
Handler->OnProcessOutputBytes = [&](int32, const TArray<uint8>& Bytes)
{
	Decoder.Feed(Bytes,
		[](FCLIFrame& Frame) { /* Frame.Type, Frame.HeaderAsString(), Frame.Binary */ },
		[](TConstArrayView<uint8> Stray) { /* text outside frames: early errors, crash dumps */ });
};

Handler->SendInput(FCLIFrameCodec::Encode(Magic, 5, TEXT("{\"cmd\":\"hello\"}")));
```

The decoder is incremental and resyncs on the magic. Bytes outside frames are passed to `OnStray`, not dropped. A length over the decoder's maximum (512 MB by default) counts as corruption and is skipped. `BuildBinaryTable` / `ParseBinaryTable` pack several buffers into one binary field.

## Tests

```
UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests CLISystem; Quit" -unattended -nullrhi -nopause
```

The process tests use the platform shell (`cmd.exe` / `/bin/sh`), so they need no extra binaries.
