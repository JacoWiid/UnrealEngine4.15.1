// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "EnginePrivate.h"
#include "Net/UnrealNetwork.h"
#include "Engine/ActorChannel.h"
#include "GameFramework/GameNetworkManager.h"
#include "NetworkingDistanceConstants.h"

/*-----------------------------------------------------------------------------
	AActor networking implementation.
-----------------------------------------------------------------------------*/

//
// Static variables for networking.
//
static bool		SavedbHidden;
static AActor*	SavedOwner;

#define DEPRECATED_NET_PRIORITY -17.0f // Pick an arbitrary invalid priority, check for that

float AActor::GetNetPriority(const FVector& ViewPos, const FVector& ViewDir, APlayerController* Viewer, UActorChannel* InChannel, float Time, bool bLowBandwidth)
{
	return DEPRECATED_NET_PRIORITY;
}

float AActor::GetNetPriority(const FVector& ViewPos, const FVector& ViewDir, APlayerController* Viewer, AActor* ViewTarget, UActorChannel* InChannel, float Time, bool bLowBandwidth)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	// Call the deprecated version so subclasses will still work for a version
	float DeprecatedValue = AActor::GetNetPriority(ViewPos, ViewDir, Viewer, InChannel, Time, bLowBandwidth);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	if (DeprecatedValue != DEPRECATED_NET_PRIORITY)
	{
		return DeprecatedValue;
	}

	if (bNetUseOwnerRelevancy && Owner)
	{
		// If we should use our owner's priority, pass it through
		return Owner->GetNetPriority(ViewPos, ViewDir, Viewer, ViewTarget, InChannel, Time, bLowBandwidth);
	}

	if (ViewTarget && (this == ViewTarget || Instigator == ViewTarget))
	{
		// If we're the view target or owned by the view target, use a high priority
		Time *= 4.f;
	}
	else if (!bHidden)
	{
		FVector Dir = GetActorLocation() - ViewPos;
		float DistSq = Dir.SizeSquared();

		// Adjust priority based on distance and whether actor is in front of viewer
		if ((ViewDir | Dir) < 0.f)
		{
			if (DistSq > NEARSIGHTTHRESHOLDSQUARED)
			{
				Time *= 0.2f;
			}
			else if (DistSq > CLOSEPROXIMITYSQUARED)
			{
				Time *= 0.4f;
			}
		}
		else if ((DistSq < FARSIGHTTHRESHOLDSQUARED) && (FMath::Square(ViewDir | Dir) > 0.5f * DistSq))
		{
			// Compute the amount of distance along the ViewDir vector. Dir is not normalized
			// Increase priority if we're being looked directly at
			Time *= 2.f;
		}
		else if (DistSq > MEDSIGHTTHRESHOLDSQUARED)
		{
			Time *= 0.4f;
		}
	}

	return NetPriority * Time;
}

bool AActor::GetNetDormancy(const FVector& ViewPos, const FVector& ViewDir, class APlayerController* Viewer, AActor* ViewTarget, UActorChannel* InChannel, float Time, bool bLowBandwidth)
{
	// For now, per peer dormancy is not supported
	return false;
}

void AActor::PreNetReceive()
{
	SavedbHidden = bHidden;
	SavedOwner = Owner;
}

void AActor::PostNetReceive()
{
	ExchangeB( bHidden, SavedbHidden );
	Exchange ( Owner, SavedOwner );

	if (bHidden != SavedbHidden)
	{
		SetActorHiddenInGame(SavedbHidden);
	}
	if (Owner != SavedOwner)
	{
		SetOwner(SavedOwner);
	}
}

void AActor::OnRep_ReplicatedMovement()
{
	if( RootComponent && RootComponent->IsSimulatingPhysics() )
	{
		PostNetReceivePhysicState();
	}
	else
	{
		if (Role == ROLE_SimulatedProxy)
		{
			PostNetReceiveVelocity(ReplicatedMovement.LinearVelocity);
			PostNetReceiveLocationAndRotation();
		}
	}
}

void AActor::PostNetReceiveLocationAndRotation()
{
	if( RootComponent && RootComponent->IsRegistered() && (ReplicatedMovement.Location != GetActorLocation() || ReplicatedMovement.Rotation != GetActorRotation()) )
	{
		SetActorLocationAndRotation(ReplicatedMovement.Location, ReplicatedMovement.Rotation, /*bSweep=*/ false);
	}
}

void AActor::PostNetReceiveVelocity(const FVector& NewVelocity)
{
}

void AActor::PostNetReceivePhysicState()
{
	UPrimitiveComponent* RootPrimComp = Cast<UPrimitiveComponent>(RootComponent);
	if (RootPrimComp)
	{
		FRigidBodyState NewState;
		ReplicatedMovement.CopyTo(NewState);

		FVector DeltaPos(FVector::ZeroVector);
		RootPrimComp->ConditionalApplyRigidBodyState(NewState, GEngine->PhysicErrorCorrection, DeltaPos);
	}
}

bool AActor::IsNetRelevantFor(const APlayerController* RealViewer, const AActor* ViewTarget, const FVector& SrcLocation) const
{
	if (bAlwaysRelevant || IsOwnedBy(ViewTarget) || IsOwnedBy(RealViewer) || this == ViewTarget || ViewTarget == Instigator)
	{
		return true;
	}
	else if ( bNetUseOwnerRelevancy && Owner)
	{
		return Owner->IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
	}
	else if ( bOnlyRelevantToOwner )
	{
		return false;
	}
	else if ( RootComponent && RootComponent->AttachParent && RootComponent->AttachParent->GetOwner() && (Cast<USkeletalMeshComponent>(RootComponent->AttachParent) || (RootComponent->AttachParent->GetOwner() == Owner)) )
	{
		return RootComponent->AttachParent->GetOwner()->IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation);
	}
	else if( bHidden && (!RootComponent || !RootComponent->IsCollisionEnabled()) )
	{
		return false;
	}

	if (!RootComponent)
	{
		UE_LOG(LogNet, Warning, TEXT("Actor %s / %s has no root component in AActor::IsNetRelevantFor. (Make bAlwaysRelevant=true?)"), *GetClass()->GetName(), *GetName() );
		return false;
	}
	if (GetDefault<AGameNetworkManager>()->bUseDistanceBasedRelevancy)
	{
		return ((SrcLocation - GetActorLocation()).SizeSquared() < NetCullDistanceSquared);
	}

	return true;
}

void AActor::GatherCurrentMovement()
{
	UPrimitiveComponent* RootPrimComp = Cast<UPrimitiveComponent>(GetRootComponent());
	if (RootPrimComp && RootPrimComp->IsSimulatingPhysics())
	{
		FRigidBodyState RBState;
		RootPrimComp->GetRigidBodyState(RBState);

		ReplicatedMovement.FillFrom(RBState);
	}
	else if(RootComponent != NULL)
	{
		// If we are attached, don't replicate absolute position
		if( RootComponent->AttachParent != NULL )
		{
			// Networking for attachments assumes the RootComponent of the AttachParent actor. 
			// If that's not the case, we can't update this, as the client wouldn't be able to resolve the Component and would detach as a result.
			if( AttachmentReplication.AttachParent != NULL )
			{
				AttachmentReplication.LocationOffset = RootComponent->RelativeLocation;
				AttachmentReplication.RotationOffset = RootComponent->RelativeRotation;
				AttachmentReplication.RelativeScale3D = RootComponent->RelativeScale3D;
			}
		}
		else
		{
			ReplicatedMovement.Location = RootComponent->GetComponentLocation();
			ReplicatedMovement.Rotation = RootComponent->GetComponentRotation();
			ReplicatedMovement.LinearVelocity = GetVelocity();
			ReplicatedMovement.bRepPhysics = false;
		}
	}
}

static void GetLifetimeBlueprintReplicationList( const AActor* ThisActor, const UBlueprintGeneratedClass* MyClass, TArray< FLifetimeProperty > & OutLifetimeProps )
{
	if ( MyClass == NULL )
	{
		return;
	}

	uint32 PropertiesLeft = MyClass->NumReplicatedProperties;

	for ( TFieldIterator<UProperty> It( MyClass, EFieldIteratorFlags::ExcludeSuper ); It && PropertiesLeft > 0; ++It )
	{
		UProperty * Prop = *It;
		if ( Prop != NULL && Prop->GetPropertyFlags() & CPF_Net )
		{
			PropertiesLeft--;
			OutLifetimeProps.Add( FLifetimeProperty( Prop->RepIndex ) );
		}
	}

	return GetLifetimeBlueprintReplicationList( ThisActor, Cast< UBlueprintGeneratedClass >( MyClass->GetSuperStruct() ), OutLifetimeProps );
}

void AActor::GetLifetimeReplicatedProps( TArray< FLifetimeProperty > & OutLifetimeProps ) const
{
	GetLifetimeBlueprintReplicationList( this, Cast< UBlueprintGeneratedClass >( GetClass() ), OutLifetimeProps );

	DOREPLIFETIME( AActor, Role );
	DOREPLIFETIME( AActor, RemoteRole );
	DOREPLIFETIME( AActor, Owner );
	DOREPLIFETIME( AActor, bHidden );

	DOREPLIFETIME( AActor, bTearOff );
	DOREPLIFETIME( AActor, bCanBeDamaged );
	DOREPLIFETIME( AActor, AttachmentReplication );

	DOREPLIFETIME( AActor, Instigator );

	DOREPLIFETIME_CONDITION( AActor, ReplicatedMovement, COND_SimulatedOrPhysics );
}

bool AActor::ReplicateSubobjects(UActorChannel *Channel, FOutBunch *Bunch, FReplicationFlags *RepFlags)
{
	check(Channel);
	check(Bunch);
	check(RepFlags);

	bool WroteSomething = false;

	for (UActorComponent* ActorComp : ReplicatedComponents)
	{
		if (ActorComp && ActorComp->GetIsReplicated())
		{
			WroteSomething |= ActorComp->ReplicateSubobjects(Channel, Bunch, RepFlags);		// Lets the component add subobjects before replicating its own properties.
			WroteSomething |= Channel->ReplicateSubobject(ActorComp, *Bunch, *RepFlags);	// (this makes those subobjects 'supported', and from here on those objects may have reference replicated)		
		}
	}
	return WroteSomething;
}

void AActor::GetSubobjectsWithStableNamesForNetworking(TArray<UObject*> &ObjList)
{	
	// For experimenting with replicating ALL stably named components initially
	for (UActorComponent* Component : OwnedComponents)
	{
		if (Component && !Component->IsPendingKill() && Component->IsNameStableForNetworking())
		{
			ObjList.Add(Component);
			Component->GetSubobjectsWithStableNamesForNetworking(ObjList);
		}
	}

	// Sort the list so that we generate the same list on client/server
	struct FCompareComponentNames
	{
		FORCEINLINE bool operator()( UObject& A, UObject& B ) const
		{
			return A.GetName() < B.GetName();
		}
	};

	Sort( ObjList.GetData(), ObjList.Num(), FCompareComponentNames() );
}

void AActor::OnSubobjectCreatedFromReplication(UObject *NewSubobject)
{
	check(NewSubobject);
	if ( UActorComponent * Component = Cast<UActorComponent>(NewSubobject) )
	{
		Component->RegisterComponent();
		Component->SetIsReplicated(true);
	}
}

/** Called on the actor when a new subobject is dynamically created via replication */
void AActor::OnSubobjectDestroyFromReplication(UObject *NewSubobject)
{
	check(NewSubobject);
	if ( UActorComponent * Component = Cast<UActorComponent>(NewSubobject) )
	{
		Component->DestroyComponent();
	}
}

bool AActor::IsNameStableForNetworking() const
{
	return IsNetStartupActor() || HasAnyFlags( RF_ClassDefaultObject | RF_ArchetypeObject );
}

bool AActor::IsSupportedForNetworking() const
{
	return true;		// All actors are supported for networking
}