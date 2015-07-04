#pragma once

#include "../../Flare.h"
#include "../Components/FlareListItem.h"
#include "../Components/FlareSpacecraftInfo.h"


class SFlareShipList : public SCompoundWidget
{
	/*----------------------------------------------------
		Slate arguments
	----------------------------------------------------*/

	SLATE_BEGIN_ARGS(SFlareShipList){}

	SLATE_ARGUMENT(TWeakObjectPtr<class AFlareMenuManager>, MenuManager)

	SLATE_ARGUMENT(FText, Title)
	
	SLATE_END_ARGS()


public:

	/*----------------------------------------------------
		Interaction
	----------------------------------------------------*/

	/** Create the widget */
	void Construct(const FArguments& InArgs);

	/** Add a new ship to the list */
	void AddShip(IFlareSpacecraftInterface* ShipCandidate);
	
	/** Update the list display from content */
	void RefreshList();

	/** Remove all entries from the list */
	void Reset();


protected:

	/*----------------------------------------------------
		Callbacks
	----------------------------------------------------*/

	/** Title visibility */
	EVisibility GetTitleVisibility() const;

	/** Target item generator */
	TSharedRef<ITableRow> GenerateTargetInfo(TSharedPtr<FInterfaceContainer> Item, const TSharedRef<STableViewBase>& OwnerTable);

	/** Target item selected */
	void OnTargetSelected(TSharedPtr<FInterfaceContainer> Item, ESelectInfo::Type SelectInfo);



protected:

	/*----------------------------------------------------
		Protected data
	----------------------------------------------------*/

	// HUD reference
	UPROPERTY()
	TWeakObjectPtr<class AFlareMenuManager>    MenuManager;

	// Menu components
	TSharedPtr<SFlareListItem>                                   PreviousSelection;
	TSharedPtr< SListView< TSharedPtr<FInterfaceContainer> > >   TargetList;
	TArray< TSharedPtr<FInterfaceContainer> >                    TargetListData;


};