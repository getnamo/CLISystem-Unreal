#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CLISystemTestReceiver.generated.h"

/** Collects UCLIProcessComponent events for the automation tests (game thread). */
UCLASS()
class UCLISystemTestReceiver : public UObject
{
	GENERATED_BODY()
public:
	int32 Begins = 0;
	int32 Ends = 0;
	TArray<uint8> Bytes;

	UFUNCTION()
	void OnBegin(const FString& State) { ++Begins; }

	UFUNCTION()
	void OnEnd(const FString& State) { ++Ends; }

	UFUNCTION()
	void OnBytes(const TArray<uint8>& Buffer) { Bytes.Append(Buffer); }

	FString Text() const
	{
		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}
};
