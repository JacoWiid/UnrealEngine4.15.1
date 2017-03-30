// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "SplitEdge.h"
#include "IMeshEditorModeEditingContract.h"
#include "IMeshEditorModeUIContract.h"
#include "UICommandInfo.h"
#include "EditableMesh.h"
#include "MeshElement.h"
#include "MultiBoxBuilder.h"
#include "UICommandList.h"
#include "ViewportInteractor.h"

#define LOCTEXT_NAMESPACE "MeshEditorMode"


void USplitEdgeCommand::RegisterUICommand( FBindingContext* BindingContext )
{
	UI_COMMAND_EXT( BindingContext, /* Out */ UICommandInfo, "SplitEdge", "Split Edge Mode", "Set the primary action to split edges.", EUserInterfaceActionType::RadioButton, FInputChord() );
}


bool USplitEdgeCommand::TryStartingToDrag( IMeshEditorModeEditingContract& MeshEditorMode, UViewportInteractor* ViewportInteractor )
{
	bool bHaveEdge = false;

	// Figure out what to split
	MeshEditorMode.GetSelectedMeshesAndEdges( /* Out */ SplitEdgeMeshesAndEdgesToSplit );

	if( SplitEdgeMeshesAndEdgesToSplit.Num() > 0 )
	{
		for( auto& MeshAndEdges : SplitEdgeMeshesAndEdgesToSplit )
		{
			UEditableMesh* EditableMesh = MeshAndEdges.Key;
			const TArray<FMeshElement>& EdgeElements = MeshAndEdges.Value;

			// Figure out where to split
			MeshEditorMode.FindEdgeSplitUnderInteractor( ViewportInteractor, EditableMesh, EdgeElements, /* Out */ SplitEdgeSplitList );

			// Split the edges
			if( SplitEdgeSplitList.Num() > 0 )
			{
				bHaveEdge = true;
			}
		}
	}

	return bHaveEdge;
}


void USplitEdgeCommand::ApplyDuringDrag( IMeshEditorModeEditingContract& MeshEditorMode, UViewportInteractor* ViewportInteractor )
{
	if( SplitEdgeMeshesAndEdgesToSplit.Num() > 0 && SplitEdgeSplitList.Num() > 0 )
	{
		MeshEditorMode.DeselectAllMeshElements();

		static TArray<FMeshElement> MeshElementsToSelect;
		MeshElementsToSelect.Reset();

		for( auto& MeshAndEdges : SplitEdgeMeshesAndEdgesToSplit )
		{
			UEditableMesh* EditableMesh = MeshAndEdges.Key;
			verify( !EditableMesh->AnyChangesToUndo() );

			const TArray<FMeshElement>& EdgeElements = MeshAndEdges.Value;

			for( const FMeshElement& EdgeElement : EdgeElements )
			{
				const FEdgeID EdgeID( EdgeElement.ElementAddress.ElementID );

				static TArray<FVertexID> NewVertexIDs;
				NewVertexIDs.Reset();

				EditableMesh->SplitEdge( EdgeID, SplitEdgeSplitList, /* Out */ NewVertexIDs );

				// Select all of the new vertices that were created by splitting the edge
				{
					for( const FVertexID NewVertexID : NewVertexIDs )
					{
						FMeshElement MeshElementToSelect;
						{
							MeshElementToSelect.Component = EdgeElement.Component;
							MeshElementToSelect.ElementAddress.SubMeshAddress = EdgeElement.ElementAddress.SubMeshAddress;
							MeshElementToSelect.ElementAddress.ElementType = EEditableMeshElementType::Vertex;
							MeshElementToSelect.ElementAddress.ElementID = NewVertexID;
						}

						// Queue selection of this new element.  We don't want it to be part of the current action.
						MeshElementsToSelect.Add( MeshElementToSelect );
					}
				}
			}

			MeshEditorMode.TrackUndo( EditableMesh, EditableMesh->MakeUndo() );
		}

		MeshEditorMode.SelectMeshElements( MeshElementsToSelect );
	}
}



#undef LOCTEXT_NAMESPACE

