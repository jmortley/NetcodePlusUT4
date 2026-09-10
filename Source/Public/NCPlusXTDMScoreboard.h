#pragma once

#include "NetcodePlus.h"
#include "UTScoreboard.h"
#include "NCPlusXTDMScoreboard.generated.h"

class ANCAccuracyStatsReplicator;

UCLASS()
class NETCODEPLUS_API UNCPlusXTDMScoreboard : public UUTScoreboard
{
	GENERATED_UCLASS_BODY()
public:
	virtual void Draw_Implementation(float DeltaTime) override;
	virtual void SelectNext(int32 Offset, bool bDoNoWrap = false) override;
	virtual void SelectionLeft() override;
	virtual void SelectionRight() override;
	virtual void SelectionClick() override;
private:
	void SelectOtherColumn();
	struct FRowText
	{
		FString Name, Frags, Deaths, Accuracy, Ping;
		float RefreshAt = -1.f;
	};
	TMap<TWeakObjectPtr<AUTPlayerState>, FRowText> Rows;
	TWeakObjectPtr<ANCAccuracyStatsReplicator> AccuracyReplicator;
	float NextAccuracySearch = -1.f;
};
