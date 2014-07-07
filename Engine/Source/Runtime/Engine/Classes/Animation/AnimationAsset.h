// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

/**
 * Abstract base class of animation assets that can be played back and evaluated to produce a pose.
 *
 */

#pragma once

#include "SkeletalMeshTypes.h"
#include "AnimInterpFilter.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "AnimationAsset.generated.h"

/** Transform definition */
USTRUCT(BlueprintType)
struct FBlendSampleData
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY()
	int32 SampleDataIndex;

	UPROPERTY()
	float TotalWeight;

	UPROPERTY()
	float Time;

	// transient perbone interpolation data
	TArray<float> PerBoneBlendData;

	FBlendSampleData()
		:	SampleDataIndex(0)
		,	TotalWeight(0.f)
		,	Time(0.f)
	{}
	FBlendSampleData(int32 Index)
		:	SampleDataIndex(Index)
		,	TotalWeight(0.f)
		,	Time(0.f)
	{}
	bool operator==( const FBlendSampleData& Other ) const 
	{
		// if same position, it's same point
		return (Other.SampleDataIndex== SampleDataIndex);
	}
	void AddWeight(float Weight)
	{
		TotalWeight += Weight;
	}
	float GetWeight() const
	{
		return FMath::Clamp<float>(TotalWeight, 0.f, 1.f);
	}
};

USTRUCT()
struct FBlendFilter
{
	GENERATED_USTRUCT_BODY()

	FFIRFilterTimeBased FilterPerAxis[3];

	FBlendFilter()
	{
	}

	FVector GetFilterLastOutput()
	{
		return FVector (FilterPerAxis[0].LastOutput, FilterPerAxis[1].LastOutput, FilterPerAxis[2].LastOutput);
	}
};

// Root Bone Lock options when extracting Root Motion
UENUM()
namespace ERootMotionRootLock
{
	enum Type
	{
		// Use reference pose root bone position
		RefPose,

		// Use root bone position on first frame of animation.
		AnimFirstFrame,

		// FTransform::Identity
		Zero
	};
}


/** Animation Extraction Context */
USTRUCT()
struct FAnimExtractContext
{
	GENERATED_USTRUCT_BODY()

	/** Is Root Motion Translation being extracted? */
	UPROPERTY()
	bool bExtractRootMotionTranslation;

	/** Is Root Motion Rotation being extracted? */
	UPROPERTY()
	bool bExtractRootMotionRotation;

	/** Position in animation to extract pose from */
	UPROPERTY()
	float CurrentTime;

	/** Root Motion Root Bone Lock option. **/
	UPROPERTY()
	TEnumAsByte<ERootMotionRootLock::Type> RootMotionRootLock;

	FAnimExtractContext()
		: bExtractRootMotionTranslation(false)
		, bExtractRootMotionRotation(false)
		, CurrentTime(0.f)
		, RootMotionRootLock(ERootMotionRootLock::RefPose)
	{
	}

	FAnimExtractContext(float InCurrentTime)
		: bExtractRootMotionTranslation(false)
		, bExtractRootMotionRotation(false)
		, CurrentTime(InCurrentTime)
		, RootMotionRootLock(ERootMotionRootLock::RefPose)
	{
	}

	FAnimExtractContext(float InCurrentTime, bool InbExtractRootMotionTranslation, bool InbExtractRootMotionRotation, ERootMotionRootLock::Type InRootMotionRootLock)
		: bExtractRootMotionTranslation(InbExtractRootMotionTranslation)
		, bExtractRootMotionRotation(InbExtractRootMotionRotation)
		, CurrentTime(InCurrentTime)
		, RootMotionRootLock(InRootMotionRootLock)
	{
	}
};


/**
 * Information about an animation asset that needs to be ticked
 */
USTRUCT()
struct FAnimTickRecord
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY()
	class UAnimationAsset* SourceAsset;

	float* TimeAccumulator;
	FVector BlendSpacePosition;	
	FBlendFilter* BlendFilter;
	TArray<FBlendSampleData>* BlendSampleDataCache;
	float PlayRateMultiplier;
	float EffectiveBlendWeight;
	bool bLooping;

public:
	FAnimTickRecord()
	{
	}
};

UENUM()
namespace EAnimGroupRole
{
	enum Type
	{
		// This node can be the leader, as long as it has a higher blend weight than the previous best leader
		CanBeLeader,
		
		// This node will always be a follower (unless there are only followers, in which case the first one ticked wins)
		AlwaysFollower,

		// This node will always be a leader (if more than one node is AlwaysLeader, the last one ticked wins)
		AlwaysLeader
	};
}

USTRUCT()
struct FAnimGroupInstance
{
	GENERATED_USTRUCT_BODY()

public:
	// The list of animation players in this group which are going to be evaluated this frame
	TArray<FAnimTickRecord> ActivePlayers;

	// The current group leader
	int32 GroupLeaderIndex;
public:
	FAnimGroupInstance()
		: GroupLeaderIndex(INDEX_NONE)
	{
	}

	void Reset()
	{
		GroupLeaderIndex = INDEX_NONE;
		ActivePlayers.Empty(ActivePlayers.Num());
	}

	// Checks the last tick record in the ActivePlayers array to see if it's a better leader than the current candidate.
	// This should be called once for each record added to ActivePlayers, after the record is setup.
	void TestTickRecordForLeadership(EAnimGroupRole::Type MembershipType)
	{
		int32 TestIndex = ActivePlayers.Num() - 1;
		const FAnimTickRecord& Candidate = ActivePlayers[TestIndex];
		
		switch (MembershipType)
		{
		case EAnimGroupRole::CanBeLeader:
			// Set it if we're better than the current leader (or if there is no leader yet)
			if ((GroupLeaderIndex == INDEX_NONE) || (ActivePlayers[GroupLeaderIndex].EffectiveBlendWeight < Candidate.EffectiveBlendWeight))
			{
				// This is a better leader
				GroupLeaderIndex = TestIndex;
			}
			break;
		case EAnimGroupRole::AlwaysLeader:
			// Always set the leader index
			GroupLeaderIndex = TestIndex;
			break;
		default:
		case EAnimGroupRole::AlwaysFollower:
			// Never set the leader index; the actual tick code will handle the case of no leader by using the first element in the array
			break;
		}
	}
};

/** Utility struct to accumulate root motion. */
USTRUCT()
struct FRootMotionMovementParams
{
	GENERATED_USTRUCT_BODY()

		UPROPERTY()
		bool bHasRootMotion;

	UPROPERTY()
		FTransform RootMotionTransform;

	FRootMotionMovementParams()
		: bHasRootMotion(false)
		, RootMotionTransform(FTransform::Identity)
	{
	}

	void Set(const FTransform & InTransform)
	{
		bHasRootMotion = true;
		RootMotionTransform = InTransform;
	}

	void Accumulate(const FTransform & InTransform)
	{
		if (!bHasRootMotion)
		{
			Set(InTransform);
		}
		else
		{
			RootMotionTransform = InTransform * RootMotionTransform;
		}
	}

	void Accumulate(const FRootMotionMovementParams & MovementParams)
	{
		if (MovementParams.bHasRootMotion)
		{
			Accumulate(MovementParams.RootMotionTransform);
		}
	}

	void Clear()
	{
		bHasRootMotion = false;
	}
};

// This structure is used to either advance or synchronize animation players
struct FAnimAssetTickContext
{
public:
	FAnimAssetTickContext(float InDeltaTime)
		: DeltaTime(InDeltaTime)
		, SyncPoint(0.0f)
		, bIsLeader(true)
	{
	}
	
	// Are we the leader of our sync group (or ungrouped)?
	bool IsLeader() const
	{
		return bIsLeader;
	}

	bool IsFollower() const
	{
		return !bIsLeader;
	}

	// Return the delta time of the tick
	float GetDeltaTime() const
	{
		return DeltaTime;
	}

	void SetSyncPoint(float NormalizedTime)
	{
		SyncPoint = NormalizedTime;
	}

	// Returns the synchronization point (normalized time; only legal to call if ticking a follower)
	float GetSyncPoint() const
	{
		checkSlow(!bIsLeader);
		return SyncPoint;
	}

	void ConvertToFollower()
	{
		bIsLeader = false;
	}

	bool ShouldGenerateNotifies() const
	{
		return IsLeader();
	}

private:
	float DeltaTime;

	// The structure used to pass synchronization state between members of a sync group
	float SyncPoint;

	bool bIsLeader;
};

USTRUCT()
struct FAnimationGroupReference
{
	GENERATED_USTRUCT_BODY()
	
	// The name of the group
	UPROPERTY(EditAnywhere, Category=Settings)
	FName GroupName;

	// The type of membership in the group (potential leader, always follower, etc...)
	UPROPERTY(EditAnywhere, Category=Settings)
	TEnumAsByte<EAnimGroupRole::Type> GroupRole;

	FAnimationGroupReference()
		: GroupRole(EAnimGroupRole::CanBeLeader)
	{
	}
};

UCLASS(abstract, MinimalAPI)
class UAnimationAsset : public UObject
{
	GENERATED_UCLASS_BODY()

private:
	/** Pointer to the Skeleton this asset can be played on .	*/
	UPROPERTY(AssetRegistrySearchable, Category=Animation, VisibleAnywhere)
	class USkeleton* Skeleton;

	/** Skeleton guid. If changes, you need to remap info*/
	FGuid SkeletonGuid;

public:
	/** Advances the asset player instance 
	 * 
	 * @param Instance		AnimationTickRecord Instance - saves data to evaluate
	 * @param InstanceOwner	AnimInstance playing this asset
	 * @param Context		The tick context (leader/follower, delta time, sync point, etc...)
	 */
	virtual void TickAssetPlayerInstance(const FAnimTickRecord& Instance, class UAnimInstance* InstanceOwner, FAnimAssetTickContext& Context) const {}
	// this is used in editor only when used for transition getter
	// this doesn't mean max time. In Sequence, this is SequenceLength,
	// but for BlendSpace CurrentTime is normalized [0,1], so this is 1
	virtual float GetMaxCurrentTime() { return 0.f; }

	ENGINE_API void SetSkeleton(USkeleton* NewSkeleton);
	void ResetSkeleton(USkeleton* NewSkeleton);
	virtual void PostLoad() override;

	/** Validate our stored data against our skeleton and update accordingly */
	void ValidateSkeleton();

	virtual void Serialize(FArchive& Ar) override;

#if WITH_EDITOR
	/** Replace Skeleton 
	 * 
	 * @param NewSkeleton	NewSkeleton to change to 
	 */
	ENGINE_API bool ReplaceSkeleton(USkeleton* NewSkeleton);

	/** Retrieve all animations that are used by this asset 
	 * 
	 * @param (out)		AnimationSequences 
	 **/
	ENGINE_API virtual bool GetAllAnimationSequencesReferred(TArray<class UAnimSequence*>& AnimationSequences);

	/** Replace this assets references to other animations based on ReplacementMap 
	 * 
	 * @param ReplacementMap	Mapping of original asset to new asset
	 **/
	ENGINE_API virtual void ReplaceReferredAnimations(const TMap<UAnimSequence*, UAnimSequence*>& ReplacementMap);	

#endif

public:
	class USkeleton* GetSkeleton() const { return Skeleton; }
};

