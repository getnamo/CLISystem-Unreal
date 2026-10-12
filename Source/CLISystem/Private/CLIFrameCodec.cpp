#include "CLIFrameCodec.h"

namespace CLIFrameCodecPrivate
{
	static void AppendU32(TArray<uint8>& Out, uint32 Value)
	{
		const uint8 Bytes[4] = { (uint8)(Value & 0xFF), (uint8)((Value >> 8) & 0xFF), (uint8)((Value >> 16) & 0xFF), (uint8)((Value >> 24) & 0xFF) };
		Out.Append(Bytes, 4);
	}

	static uint32 ReadU32(const uint8* Data)
	{
		return (uint32)Data[0] | ((uint32)Data[1] << 8) | ((uint32)Data[2] << 16) | ((uint32)Data[3] << 24);
	}
}

FString FCLIFrame::HeaderAsString() const
{
	FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Header.GetData()), Header.Num());
	return FString(Converted.Length(), Converted.Get());
}

FCLIFrameCodec::FCLIFrameCodec(const FCLIFrameMagic& InMagic, int64 InMaxFrameBytes)
	: Magic(InMagic)
	, MaxFrameBytes(InMaxFrameBytes)
{
}

TArray<uint8> FCLIFrameCodec::Encode(const FCLIFrameMagic& InMagic, uint8 Type, const FString& Header, TConstArrayView<uint8> Binary)
{
	FTCHARToUTF8 Utf8(*Header);
	TArray<uint8> Out;
	EncodeTo(Out, InMagic, Type, TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Binary);
	return Out;
}

void FCLIFrameCodec::EncodeTo(TArray<uint8>& Out, const FCLIFrameMagic& InMagic, uint8 Type, TConstArrayView<uint8> HeaderBytes, TConstArrayView<uint8> Binary)
{
	Out.Reserve(Out.Num() + FrameOverheadBytes + HeaderBytes.Num() + Binary.Num());
	Out.Append(InMagic.Bytes, 4);
	Out.Add(Type);
	CLIFrameCodecPrivate::AppendU32(Out, (uint32)HeaderBytes.Num());
	Out.Append(HeaderBytes.GetData(), HeaderBytes.Num());
	CLIFrameCodecPrivate::AppendU32(Out, (uint32)Binary.Num());
	Out.Append(Binary.GetData(), Binary.Num());
}

int32 FCLIFrameCodec::FindMagic(int32 From) const
{
	const uint8* Data = Buffer.GetData();
	for (int32 i = From, Last = Buffer.Num() - 4; i <= Last; ++i)
	{
		if (Data[i] == Magic.Bytes[0] && Data[i + 1] == Magic.Bytes[1] && Data[i + 2] == Magic.Bytes[2] && Data[i + 3] == Magic.Bytes[3])
		{
			return i;
		}
	}
	return INDEX_NONE;
}

int32 FCLIFrameCodec::TrailingMagicPrefix(int32 From) const
{
	//How many trailing bytes (up to 3) could be the start of a marker that the next chunk completes
	const int32 Available = Buffer.Num() - From;
	for (int32 Len = FMath::Min(3, Available); Len > 0; --Len)
	{
		if (FMemory::Memcmp(Buffer.GetData() + Buffer.Num() - Len, Magic.Bytes, Len) == 0)
		{
			return Len;
		}
	}
	return 0;
}

void FCLIFrameCodec::Feed(TConstArrayView<uint8> Data, TFunctionRef<void(FCLIFrame& Frame)> OnFrame, TFunctionRef<void(TConstArrayView<uint8> Stray)> OnStray)
{
	Buffer.Append(Data.GetData(), Data.Num());

	int32 Pos = 0;
	const int32 Num = Buffer.Num();
	while (Pos < Num)
	{
		const int32 MagicAt = FindMagic(Pos);
		if (MagicAt == INDEX_NONE)
		{
			const int32 Keep = TrailingMagicPrefix(Pos);
			if (Num - Keep > Pos)
			{
				OnStray(TConstArrayView<uint8>(Buffer.GetData() + Pos, Num - Keep - Pos));
			}
			Pos = Num - Keep;
			break;
		}
		if (MagicAt > Pos)
		{
			OnStray(TConstArrayView<uint8>(Buffer.GetData() + Pos, MagicAt - Pos));
			Pos = MagicAt;
		}
		if (Num - Pos < 9)
		{
			break;
		}

		const uint8* Frame = Buffer.GetData() + Pos;
		const int64 HeaderLen = CLIFrameCodecPrivate::ReadU32(Frame + 5);
		if (FrameOverheadBytes + HeaderLen > MaxFrameBytes)
		{
			//Not a real frame: pass the first byte through and resync after it
			OnStray(TConstArrayView<uint8>(Frame, 1));
			++Pos;
			continue;
		}
		if ((int64)Num - Pos < 9 + HeaderLen + 4)
		{
			break;
		}
		const int64 BinaryLen = CLIFrameCodecPrivate::ReadU32(Frame + 9 + HeaderLen);
		const int64 Total = FrameOverheadBytes + HeaderLen + BinaryLen;
		if (Total > MaxFrameBytes)
		{
			OnStray(TConstArrayView<uint8>(Frame, 1));
			++Pos;
			continue;
		}
		if ((int64)Num - Pos < Total)
		{
			break;
		}

		FCLIFrame Decoded;
		Decoded.Type = Frame[4];
		Decoded.Header.Append(Frame + 9, (int32)HeaderLen);
		Decoded.Binary.Append(Frame + 13 + HeaderLen, (int32)BinaryLen);
		Pos += (int32)Total;
		OnFrame(Decoded);
	}

	if (Pos > 0)
	{
		Buffer.RemoveAt(0, Pos, EAllowShrinking::No);
	}
}

void FCLIFrameCodec::Flush(TFunctionRef<void(TConstArrayView<uint8> Stray)> OnStray)
{
	if (Buffer.Num() > 0)
	{
		OnStray(TConstArrayView<uint8>(Buffer.GetData(), Buffer.Num()));
	}
	Reset();
}

void FCLIFrameCodec::Reset()
{
	Buffer.Reset();
}

void FCLIFrameCodec::BuildBinaryTable(const TArray<TArray<uint8>>& Buffers, TArray<uint8>& Out)
{
	int64 Size = 4;
	for (const TArray<uint8>& Item : Buffers)
	{
		Size += 4 + Item.Num();
	}
	Out.Reserve(Out.Num() + Size);
	CLIFrameCodecPrivate::AppendU32(Out, (uint32)Buffers.Num());
	for (const TArray<uint8>& Item : Buffers)
	{
		CLIFrameCodecPrivate::AppendU32(Out, (uint32)Item.Num());
		Out.Append(Item);
	}
}

bool FCLIFrameCodec::ParseBinaryTable(TConstArrayView<uint8> Data, TArray<TArray<uint8>>& OutBuffers)
{
	OutBuffers.Reset();
	if (Data.Num() < 4)
	{
		return Data.Num() == 0;
	}
	const uint8* Ptr = Data.GetData();
	const int64 End = Data.Num();
	int64 Pos = 4;
	const uint32 Count = CLIFrameCodecPrivate::ReadU32(Ptr);
	if ((int64)Count * 4 > End - 4)
	{
		return false;
	}
	OutBuffers.Reserve(Count);
	for (uint32 i = 0; i < Count; ++i)
	{
		if (End - Pos < 4)
		{
			return false;
		}
		const int64 Len = CLIFrameCodecPrivate::ReadU32(Ptr + Pos);
		Pos += 4;
		if (End - Pos < Len)
		{
			return false;
		}
		OutBuffers.Emplace(Ptr + Pos, (int32)Len);
		Pos += Len;
	}
	return Pos == End;
}
