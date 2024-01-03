// Fill out your copyright notice in the Description page of Project Settings.

#include "SignificanceManagerAccelerated.h"
#include "GameFramework/PlayerController.h"

#if INTEL_ISPC
#include "SignificanceISPC.ispc.generated.h"
#endif

DECLARE_CYCLE_STAT( TEXT("Update"), STAT_Update, STATGROUP_SignificanceManagerAccelerated );
DECLARE_CYCLE_STAT( TEXT("Update Transform queue"), STAT_Update_Transforms, STATGROUP_SignificanceManagerAccelerated );
DECLARE_CYCLE_STAT( TEXT("Super Update"), STAT_SuperUpdate, STATGROUP_SignificanceManagerAccelerated );
DECLARE_CYCLE_STAT( TEXT("ISPC Struct (Threaded)"), STAT_ISPC_Stuct, STATGROUP_SignificanceManagerAccelerated );
DECLARE_CYCLE_STAT( TEXT("On update Signifiance state (Threaded)"), STAT_UpdateSignifianceState, STATGROUP_SignificanceManagerAccelerated );

DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Num Managed Objects"), STAT_AccSignificanceManager_NumObjects, STATGROUP_SignificanceManagerAccelerated);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Num Dynamic Objects"), STAT_AccSignificanceManager_NumDynamicObjects, STATGROUP_SignificanceManagerAccelerated);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("ProcessedPerFrame"), STAT_AccSignificanceManager_NumProcessed, STATGROUP_SignificanceManagerAccelerated);

const FString DYNAMIC_TAG = "_SMA_Dynamic";

void USignificanceManagerAccelerated::Update(TArrayView<const FTransform> InViewpoints)
{
	const bool IsHostOrDedicatedServer = GetWorld()->GetNetMode() != ENetMode::NM_Client;

	// Call super for the default entries.
	{
		SCOPE_CYCLE_COUNTER(STAT_SuperUpdate)
		Super::Update(InViewpoints);
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_Update_Transforms)
		ProcessTransformUpdateQueue();
	}
	
	// ISPC accelerated types
	{
		SCOPE_CYCLE_COUNTER(STAT_Update)

		// Handle static
		const int32 NumObjectsToHandle = StaticEntries.Num();
		const int32 NumToHandleThisFrame = FMath::Max(NumObjectsToHandle / NumFramesForFullCycle,1);

		SET_DWORD_STAT(STAT_AccSignificanceManager_NumProcessed,NumToHandleThisFrame)
		
		const int32 NumThreads = FMath::Clamp(NumToHandleThisFrame / 100,1,FWindowsPlatformMisc::NumberOfWorkerThreadsToSpawn() - 1	);
		const int32 NumToHandlePerTask = FMath::DivideAndRoundUp(NumToHandleThisFrame, NumThreads);

		// TODO do we want to support multi viewport?
		TArray<FVector3f> Viewpoints3f;
		for	(const auto& Viewpoint : InViewpoints)
		{
			Viewpoints3f.Add(FVector3f(Viewpoint.GetLocation()));
		}
		
		CachedViewPoints = Viewpoints3f;
		
		// Handle dynamic.
		struct FUpdateData
		{
			bool bUpdateTick;
			bool bUpdateSignificanceState;
			UObject* Object;
			int32 TickLevel;
			int32 NumTickLevels;
		};
		
		TQueue<FUpdateData,EQueueMode::Mpsc> GainQueue;
		TQueue<UObject*,EQueueMode::Mpsc> LossQueue;

		ParallelFor(NumThreads,[&](int32 TaskID)
		{
			const int32 Begin = FMath::Min(LastHandledItem + (TaskID * NumToHandlePerTask),NumObjectsToHandle);
			const int32 End = FMath::Min(Begin + NumToHandlePerTask,NumObjectsToHandle);
			bool bAnyDirty = false;
			{
				SCOPE_CYCLE_COUNTER(STAT_ISPC_Stuct)
				
				ispc::ComputeSignififance(
					(ispc::FVector3f*)Viewpoints3f.GetData(),
					Viewpoints3f.Num(),
					(ispc::FStaticAcceleratedManagedSignificanceInfo*)StaticEntries.GetData(),
					Begin,
					End,
					bAnyDirty);
			}

			if(bAnyDirty)
			{
				for (int32 Item = Begin; Item < End; Item++)
				{
					const FStaticAcceleratedManagedSignificanceInfo& DataEntry = StaticEntries[Item];

					if (DataEntry.bIsDirty)
					{
						const FStaticAcceleratedManagedObjectInfo* Entry = StaticEntriesObjects[Item];
						
						if ( !Entry->PostSignificanceFunction )
						{	// Default implementation.
							// On Gain.
							const bool bDidChangeSignificance = ((DataEntry.bIsSignificant != SignificanceState::Insignificant) && (DataEntry.bWasSignificant != SignificanceState::Significant));
							const bool bDidChangeTickRate = DataEntry.bSupportsTickRateDilation && (DataEntry.bIsSignificant != SignificanceState::Insignificant) && (DataEntry.bDidChangeTickState != SignificanceState::Insignificant);
							
							if ((bDidChangeSignificance) || ( bDidChangeTickRate) )
							{
								UObject* Target = Entry->ManagedObject.Get();
								if (Entry->Type == USignificanceManager::EPostSignificanceType::Sequential)
								{
									GainQueue.Enqueue(FUpdateData{bDidChangeTickRate,bDidChangeSignificance,Target,DataEntry.CurrentTickStage,DataEntry.NumTickStages});
								}
								else
								{
									if (bDidChangeSignificance)
									{
										OnSignificanceGain(Target, Entry->Type);
									}
									if (bDidChangeTickRate)
									{
										OnSignificanceTickRateUpdate(Target, DataEntry.CurrentTickStage,DataEntry.NumTickStages);
									}
								}
							}
							// On Loss
							else if ((DataEntry.bIsSignificant != SignificanceState::Significant) && (DataEntry.bWasSignificant != SignificanceState::Insignificant))
							{
								if (Entry->Type == USignificanceManager::EPostSignificanceType::Sequential)
								{
									LossQueue.Enqueue(Entry->ManagedObject.Get());
								}
								else
								{
									OnSignificanceLoss(Entry->ManagedObject.Get(),Entry->Type);	
								}
							}
						}
						else
						{
							// TODO.
							// default behaviour.
						}
					}
				}
			}
		},false);

		// Handle non thread safe calls.
		{
			SCOPE_CYCLE_COUNTER(STAT_UpdateSignifianceState)
			{
				FUpdateData Entry;
				while (GainQueue.Dequeue(Entry))
				{
					if (Entry.bUpdateSignificanceState)
					{
						OnSignificanceGain(Entry.Object,USignificanceManager::EPostSignificanceType::Sequential);
					}
					
					if (Entry.bUpdateTick)
					{
						OnSignificanceTickRateUpdate(Entry.Object,Entry.TickLevel,Entry.NumTickLevels);
					}
				}
			}
			{
				UObject* Entry = nullptr;
				while (LossQueue.Dequeue(Entry))
				{
					OnSignificanceLoss(Entry,USignificanceManager::EPostSignificanceType::Sequential);
					OnSignificanceTickRateUpdate(Entry,-1,0);
				}
			}
		}
		
		// Cycle
		if (NumObjectsToHandle > 0)
		{
			LastHandledItem = (LastHandledItem + NumToHandleThisFrame) % NumObjectsToHandle;
		}
	}

	if ( IsHostOrDedicatedServer )
	{
		
	}
}

bool USignificanceManagerAccelerated::IsEntrySignificant( FVector Location, float Range ) const
{
	bool Result = false;
	
	for(int32 i = 0; i < CachedViewPoints.Num(); i++)
	{
		FVector ViewLocation = FVector(CachedViewPoints[i]);
		
		if( FVector::Distance( Location, ViewLocation ) < Range )
		{
			Result = true;
		}
	}

	return Result;
}

void USignificanceManagerAccelerated::BeginDestroy()
{
	Super::BeginDestroy();
	
	// zero out.
	SET_DWORD_STAT(STAT_AccSignificanceManager_NumObjects,0)
	SET_DWORD_STAT(STAT_AccSignificanceManager_NumDynamicObjects,0)
}

void USignificanceManagerAccelerated::RegisterObject(UObject* Object, FName Tag, FManagedObjectSignificanceFunction SignificanceFunction, USignificanceManager::EPostSignificanceType InPostSignificanceType, USignificanceManager::FManagedObjectPostSignificanceFunction InPostSignificanceFunction)
{
	if (IsRunningDedicatedServer())
	{
		// Server side significance only.s
		
		return;
	}
	
	// If we handle with a custom function, handle like default significance implementation.
	if (SignificanceFunction)
	{
		Super::RegisterObject(Object, Tag, SignificanceFunction, InPostSignificanceType, InPostSignificanceFunction);
		return;
	}

	INC_DWORD_STAT(STAT_AccSignificanceManager_NumObjects);
	
	// TODO make one big getter instead of several interface calls.
	// check if static through interface.
	const float Range = GetSignificanceRange(Object);
	const FVector Location = GetObjectLocation(Object);
	const bool bHandlesTick = true; //GetIsTickManaged(Object);
	const int32 NumTickLevels = GetNumTickLevels(Object);
	const float TickExponent = GetTickExponent(Object);
	
	const FStaticAcceleratedManagedSignificanceInfo ObjectInfo = FStaticAcceleratedManagedSignificanceInfo(Location, Range, bHandlesTick, NumTickLevels, TickExponent);
	FStaticAcceleratedManagedObjectInfo* SignificanceObjectInfo = new FStaticAcceleratedManagedObjectInfo(Object,Tag,InPostSignificanceType,InPostSignificanceFunction); 
	
	const int32 EntryId = StaticEntriesObjects.Add(SignificanceObjectInfo);
	StaticEntries.Add(ObjectInfo);

	const bool bIsDynamic = Tag.ToString().Contains(DYNAMIC_TAG);
	if (bIsDynamic)
	{
		INC_DWORD_STAT(STAT_AccSignificanceManager_NumDynamicObjects)
		
		if (AActor* Actor = Cast<AActor>(Object))
		{
			Actor->GetRootComponent()->TransformUpdated.AddUObject(this,&USignificanceManagerAccelerated::OnTransformUpdated);
			
			// when assigned to an actor, we use the root component to identify the actor
			UniqueIDToEntryMap.FindOrAdd(Actor->GetRootComponent()->GetUniqueID(),FLoopUpData(Actor->GetRootComponent()->GetUniqueID(),EntryId));
		}
		else if (USceneComponent* SceneComponent = Cast<USceneComponent>(Object))
		{
			SceneComponent->TransformUpdated.AddUObject(this,&USignificanceManagerAccelerated::OnTransformUpdated);

			// TODO there should be a better way to deal with this.
			// when assigning to an component, we take the attached root component instead since that is the only data we get from the delegate. 
			UniqueIDToEntryMap.FindOrAdd(SceneComponent->GetAttachmentRoot()->GetUniqueID(),FLoopUpData(SceneComponent->GetUniqueID(),EntryId));
		}
	}

	// Calculate initial significance.
	SetIsSignificance(Object,IsEntrySignificant(Location,Range));
}

void USignificanceManagerAccelerated::RegisterStaticObject(UObject* Object, FName Tag, EPostSignificanceType InPostSignificanceType, FManagedObjectPostSignificanceFunction InPostSignificanceFunction)
{
	RegisterObject(Object,Tag,nullptr,InPostSignificanceType,InPostSignificanceFunction);
}

void USignificanceManagerAccelerated::RegisterDynamicObject(UObject* Object, FName Tag,	EPostSignificanceType InPostSignificanceType, FManagedObjectPostSignificanceFunction InPostSignificanceFunction)
{
	// Append dynamic.
	Tag = FName(Tag.ToString() + DYNAMIC_TAG);
	RegisterObject(Object,Tag,nullptr,InPostSignificanceType,InPostSignificanceFunction);
}

void USignificanceManagerAccelerated::RemoveStaticObject(UObject* Object)
{
	if (IsRunningDedicatedServer())
	{
		// Server side significance only
		return;
	}
	
	TWeakObjectPtr<UObject> Target = Object;
	const int32 Index = StaticEntriesObjects.IndexOfByPredicate([Target]( const FStaticAcceleratedManagedObjectInfo* Entry)
	{
		return Entry->ManagedObject == Target;
	});

	DEC_DWORD_STAT(STAT_AccSignificanceManager_NumObjects);
	
	FStaticAcceleratedManagedObjectInfo* Entry = StaticEntriesObjects[Index];
	delete Entry;
	StaticEntriesObjects[Index] = nullptr;
	StaticEntriesObjects.RemoveAt(Index);

	StaticEntries.RemoveAt(Index);

	StaticEntries.Shrink();
	StaticEntriesObjects.Shrink();	
}

void USignificanceManagerAccelerated::RegisterStaticNetworkObject(UObject* Object, FName Tag, EPostSignificanceType InPostSignificanceType, FManagedObjectPostSignificanceFunction InPostSignificanceFunction)
{
}

void USignificanceManagerAccelerated::RemoveStaticNetworkObject(UObject* Object)
{
}

void USignificanceManagerAccelerated::DumpSignificanceDebugData()
{
	for (int32 i = 0; i < StaticEntries.Num(); i++)
	{
		FString Name = {"missing"};
		const bool bIsSignificant = StaticEntries[i].bIsSignificant == SignificanceState::Significant;
		const float Significance = StaticEntries[i].CurrentSignificance;
		bool bIsActorTickEnabled = false;
		
		if ( auto Object = StaticEntriesObjects[i]->ManagedObject.Get() )
		{
			Name = Object->GetName();

			if (AActor* Actor = Cast<AActor>(Object))
			{
				bIsActorTickEnabled = Actor->IsActorTickEnabled();
			}
		}

		UE_LOG(LogTemp,Warning,TEXT("[SMA] %s, %s, %f, Tick Enabled: %s "),*Name,bIsSignificant ? TEXT("Enabled") : TEXT("Disabled"), Significance, bIsActorTickEnabled ? TEXT("Enabled") : TEXT("Disabled"));
	}
}

void USignificanceManagerAccelerated::OnTransformUpdated(USceneComponent* InRootComponent,EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport)
{
	LocationUpdateQueue.Add(InRootComponent->GetUniqueID());
}

void USignificanceManagerAccelerated::ProcessTransformUpdateQueue()
{
	if (LocationUpdateQueue.Num() > 0)
	{
		TArray<uint32> Array = LocationUpdateQueue.Array();

		const int32 NumObjectsToHandle = Array.Num();
		const int32 NumThreads = FMath::Clamp(NumObjectsToHandle / 100,1,FWindowsPlatformMisc::NumberOfWorkerThreadsToSpawn() - 1	);
		const int32 NumToHandlePerTask = FMath::DivideAndRoundUp(NumObjectsToHandle, NumThreads);

		ParallelFor(NumThreads,[&](int32 TaskID)
		{
			const int32 Begin = FMath::Min((TaskID * NumToHandlePerTask),NumObjectsToHandle);
			const int32 End = FMath::Min(Begin + NumToHandlePerTask,NumObjectsToHandle);
			
			for (int32 i = Begin; i < End; i++)
			{
				const uint32 UID = Array[i];
				const FLoopUpData& Entry = UniqueIDToEntryMap[UID];
			
				// Actor case.
				if (Entry.UniqueId == UID)
				{
					const FVector NewLocation = GetObjectLocation(StaticEntriesObjects[Entry.Id]->ManagedObject.Get());
					StaticEntries[Entry.Id].CachedLocation = FVector3f(NewLocation);
				}
				// Resolve component.
				else
				{
					
				}
			}
		},false);
		
		// Reset for next cycle.
		LocationUpdateQueue.Reset(); 
	}
	
}

void USignificanceManagerAccelerated::OnSignificanceLoss(UObject* Object, EPostSignificanceType InPostSignificanceType )
{
	// Implement this in sub class
}

void USignificanceManagerAccelerated::OnSignificanceGain(UObject* Object, EPostSignificanceType InPostSignificanceType )
{
	// Implement this in sub class
}

void USignificanceManagerAccelerated::OnSignificanceTickRateUpdate(UObject* Object, int32 TickLevel, int32 NumTickLevels)
{
	// Implement this in sub class
}

void USignificanceManagerAccelerated::AddTestEntry(UObject* Object, bool bDynamic)
{
	USignificanceManager* Manager = USignificanceManager::Get(Object->GetWorld());
	
	if (auto MyTestMang =Cast<USignificanceManagerAccelerated>(Manager))
	{
		if (bDynamic)
		{
			MyTestMang->RegisterDynamicObject(Object,FName("TEXT"),EPostSignificanceType::Concurrent,nullptr);
		}
		else
		{
			MyTestMang->RegisterStaticObject(Object,FName("TEXT"),EPostSignificanceType::Concurrent,nullptr);
		}
	}
}