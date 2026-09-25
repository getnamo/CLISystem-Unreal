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
