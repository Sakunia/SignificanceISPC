#include "Math/Vector.isph"

struct FStaticAcceleratedManagedSignificanceInfo
{
    // Basic info
    FVector3f Location;
    float SignificanceDistanceSquare;
    int8 NumTickStages;
    float TickRateResponseExp;

    // Updated data.
	int8 CurrentTickStage;	
	float CurrentSignificance;
    
    int8 bIsSignificant;
    int8 bWasSignificant;
    int8 bDidChangeTickState;

    bool bIsDirty;

    // non ispc data
    bool bSupportsTickRateDilation;
};

inline float square(const float& a)
{
    return a * a;
}

export void ComputeSignififance( const uniform FVector3f ViewPoints[],
                                 const uniform int32 NumViewports,
                                 uniform FStaticAcceleratedManagedSignificanceInfo uniform Data[],
                                 const uniform int32 Begin,
                                 const uniform int32 End,
                                 uniform bool& bAnyDirty)
{
    const varying FVector3f v1 = ViewPoints[0];
    int uniform NumDirty = 0;

    foreach(i = Begin ... End)
    {    
        FStaticAcceleratedManagedSignificanceInfo* Entry = &Data[i];
        const FVector3f v2 = Entry->Location;
                
        float A = square(v2.V[0] - v1.V[0]);
        float B = square(v2.V[1] - v1.V[1]);
        float C = square(v2.V[2] - v1.V[2]);
            
        const float DistSquared = A+B+C;


        // > 0 is relevant
        const float NewSignifiance = min(1.f - max((DistSquared / Entry->SignificanceDistanceSquare), 0.0f),1.0);
        const float OldSignifiance = Entry->CurrentSignificance;

        // Set tick interval.
        const int8 NewCurrentTickStage = Entry->NumTickStages * NewSignifiance;

        const bool bIsSignificant = NewSignifiance >= 0.01f;
        const bool bWasSignificant = OldSignifiance == -1.f ? -1 : OldSignifiance >= 0.01f;
        const bool bDidChangeTickState = Entry->CurrentTickStage != NewCurrentTickStage;

        const bool bIsDirty = bIsSignificant != bWasSignificant || bDidChangeTickState;

        // Update values
        Entry->CurrentSignificance = NewSignifiance; 
            
        Entry->bIsSignificant = bIsSignificant;
        Entry->bWasSignificant = bWasSignificant;

        Entry->bDidChangeTickState = bDidChangeTickState;
        Entry->CurrentTickStage = NewCurrentTickStage;
        Entry->bIsDirty = bIsDirty;

        // Is it dirty?
        if(bIsDirty)
        {
            NumDirty += 1;
        }
    }

    bAnyDirty = NumDirty > 0;
};