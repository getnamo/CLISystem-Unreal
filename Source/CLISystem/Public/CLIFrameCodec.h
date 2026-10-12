#pragma once

#include "CoreMinimal.h"

/** 4-byte marker that starts every frame, e.g. FCLIFrameMagic('N', 'U', 'E', 1). */
struct FCLIFrameMagic
{
	uint8 Bytes[4];

	constexpr FCLIFrameMagic(uint8 A, uint8 B, uint8 C, uint8 D)
		: Bytes{ A, B, C, D }
	{
	}
};

/** One decoded frame. The header is usually UTF-8 JSON. */
struct CLISYSTEM_API FCLIFrame
{
	uint8 Type = 0;
	TArray<uint8> Header;
	TArray<uint8> Binary;

	FString HeaderAsString() const;
};

/**
 * Self-delimiting frames for talking to a child process in bytes mode (same layout NodeJs-Unreal uses):
 *
 *   MAGIC[4] | Type u8 | HeaderLen u32 LE | Header | BinaryLen u32 LE | Binary
 *
 * The decoder is incremental (feed it whatever the pipe delivers) and resyncs on the magic. Bytes that
 * aren't part of a frame, such as text a process prints before it starts framing or a crash dump, are
 * passed to OnStray instead of being dropped. A length over MaxFrameBytes counts as corruption: the
 * decoder skips that marker and keeps searching.
 *
 * Not thread safe: feed one decoder from one thread.
 */
class CLISYSTEM_API FCLIFrameCodec
{
public:
	static constexpr int64 DefaultMaxFrameBytes = 512ll * 1024 * 1024;
	static constexpr int32 FrameOverheadBytes = 13;

	explicit FCLIFrameCodec(const FCLIFrameMagic& InMagic, int64 InMaxFrameBytes = DefaultMaxFrameBytes);

	/** Encode one frame with a string header (sent as UTF-8). */
	static TArray<uint8> Encode(const FCLIFrameMagic& Magic, uint8 Type, const FString& Header, TConstArrayView<uint8> Binary = TConstArrayView<uint8>());

	/** Append one frame to Out. */
	static void EncodeTo(TArray<uint8>& Out, const FCLIFrameMagic& Magic, uint8 Type, TConstArrayView<uint8> HeaderBytes, TConstArrayView<uint8> Binary);

	/** Decode as many complete frames as Data finishes. Partial frames stay buffered for the next call. */
	void Feed(TConstArrayView<uint8> Data, TFunctionRef<void(FCLIFrame& Frame)> OnFrame, TFunctionRef<void(TConstArrayView<uint8> Stray)> OnStray);

	/** Pass anything still buffered (an incomplete frame or a partial marker) to OnStray and reset, e.g. after the process exits. */
	void Flush(TFunctionRef<void(TConstArrayView<uint8> Stray)> OnStray);

	void Reset();

	int32 NumBufferedBytes() const { return Buffer.Num(); }

	/** Several buffers in one binary field: Count u32 | Count x (Len u32 | Bytes). */
	static void BuildBinaryTable(const TArray<TArray<uint8>>& Buffers, TArray<uint8>& Out);
	static bool ParseBinaryTable(TConstArrayView<uint8> Data, TArray<TArray<uint8>>& OutBuffers);

private:
	int32 FindMagic(int32 From) const;
	int32 TrailingMagicPrefix(int32 From) const;

	FCLIFrameMagic Magic;
	int64 MaxFrameBytes;
	TArray<uint8> Buffer;
};
