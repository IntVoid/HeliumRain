
#include "../Flare.h"
#include "FlareHUD.h"
#include "../Player/FlarePlayerController.h"
#include "../Ships/FlareShipInterface.h"
#include "../Stations/FlareStationInterface.h"
#include "../FlareLoadingScreen/FlareLoadingScreen.h"


#define LOCTEXT_NAMESPACE "FlareHUD"


/*----------------------------------------------------
	Constructor
----------------------------------------------------*/

AFlareHUD::AFlareHUD(const class FObjectInitializer& PCIP)
	: Super(PCIP)
	, MenuIsOpen(false)
	, FadeDuration(0.15)
{
	// Load content (general icons)
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDReticleIconObj      (TEXT("/Game/Gameplay/HUD/TX_Reticle.TX_Reticle"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDAimIconObj          (TEXT("/Game/Gameplay/HUD/TX_Aim.TX_Aim"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDAimHelperIconObj    (TEXT("/Game/Gameplay/HUD/TX_AimHelper.TX_AimHelper"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDNoseIconObj         (TEXT("/Game/Gameplay/HUD/TX_Nose.TX_Nose"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDDesignatorCornerObj (TEXT("/Game/Gameplay/HUD/TX_DesignatorCorner.TX_DesignatorCorner"));

	// Load content (status icons)
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDTemperatureIconObj  (TEXT("/Game/Slate/Icons/TX_Icon_Temperature.TX_Icon_Temperature"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDPowerIconObj        (TEXT("/Game/Slate/Icons/TX_Icon_Power.TX_Icon_Power"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDPropulsionIconObj   (TEXT("/Game/Slate/Icons/TX_Icon_Propulsion.TX_Icon_Propulsion"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDRCSIconObj          (TEXT("/Game/Slate/Icons/TX_Icon_RCS.TX_Icon_RCS"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> HUDWeaponIconObj       (TEXT("/Game/Slate/Icons/TX_Icon_Shell.TX_Icon_Shell"));

	// Set content (general icons)
	HUDReticleIcon = HUDReticleIconObj.Object;
	HUDAimIcon = HUDAimIconObj.Object;
	HUDAimHelperIcon = HUDAimHelperIconObj.Object;
	HUDNoseIcon = HUDNoseIconObj.Object;
	HUDDesignatorCornerTexture = HUDDesignatorCornerObj.Object;

	// Set content (status icons)
	HUDTemperatureIcon = HUDTemperatureIconObj.Object;
	HUDPowerIcon = HUDPowerIconObj.Object;
	HUDPropulsionIcon = HUDPropulsionIconObj.Object;
	HUDRCSIcon = HUDRCSIconObj.Object;
	HUDWeaponIcon = HUDWeaponIconObj.Object;

	// Dynamic data
	FadeTimer = FadeDuration;
	HudColor = FLinearColor::White;
	HudColor.A = 0.7;
}


/*----------------------------------------------------
	Gameplay events
----------------------------------------------------*/

void AFlareHUD::BeginPlay()
{
	Super::BeginPlay();
}

void AFlareHUD::Tick(float DeltaSeconds)
{
	AFlarePlayerController* PC = Cast<AFlarePlayerController>(GetOwner());
	Super::Tick(DeltaSeconds);

	// Mouse control
	if (PC)
	{
		FVector2D MousePos = PC->GetMousePosition();
		FVector2D ViewportSize = FVector2D(GEngine->GameViewport->Viewport->GetSizeXY());
		FVector2D ViewportCenter = FVector2D(ViewportSize.X / 2, ViewportSize.Y / 2);
		MousePos = 2 * ((MousePos - ViewportCenter) / ViewportSize);
		PC->MousePositionInput(MousePos);
	}

	// Fade system
	if (Fader.IsValid() && FadeTimer >= 0)
	{
		FadeTimer += DeltaSeconds;
		FLinearColor Color = FLinearColor::Black;
		float Alpha = FMath::Clamp(FadeTimer / FadeDuration, 0.0f, 1.0f);

		// Fade process
		if (Alpha < 1)
		{
			Color.A = FadeFromBlack ? 1 - Alpha : Alpha;
			Fader->SetVisibility(EVisibility::Visible);
			Fader->SetBorderBackgroundColor(Color);
		}

		// Callback
		else if (FadeTarget != EFlareMenu::MENU_None)
		{
			ProcessFadeTarget();
		}

		// Done
		else
		{
			Fader->SetVisibility(EVisibility::Hidden);
		}
	}
}

void AFlareHUD::DrawHUD()
{
	Super::DrawHUD();
	AFlarePlayerController* PC = Cast<AFlarePlayerController>(GetOwner());
	AFlareShip* Ship = PC->GetShipPawn();

	// Draw designators and context menu
	if (!MenuIsOpen)
	{
		// Draw designators
		FoundTargetUnderMouse = false;
		for (TActorIterator<AActor> ActorItr(GetWorld()); ActorItr; ++ActorItr)
		{
			AFlareShipBase* ShipBase = Cast<AFlareShipBase>(*ActorItr);
			if (PC && ShipBase && ShipBase != PC->GetShipPawn() && ShipBase != PC->GetMenuPawn())
			{
				if (PC->LineOfSightTo(ShipBase))
				{
					DrawHUDDesignator(ShipBase);
				}
			}
		}

		// Hide the context menu
		if (!FoundTargetUnderMouse || !IsInteractive)
		{
			ContextMenu->Hide();
		}
	}

	// Update HUD materials
	if (PC && Ship && !MenuIsOpen)
	{
		// Get HUD data
		float FocusDistance = 1000000;
		FVector2D ScreenPosition;
		FVector2D ViewportSize = FVector2D(GEngine->GameViewport->Viewport->GetSizeXY());
		int32 HelperScale = ViewportSize.Y;
		FRotator ShipAttitude = Ship->GetActorRotation();
		FVector ShipVelocity = 100 * Ship->GetLinearVelocity();
		
		// Bullet velocity
		FVector BulletVelocity = ShipAttitude.Vector();
		BulletVelocity.Normalize();
		BulletVelocity *= 50000; // TODO get from projectile
		
		// Update nose
		if (!Ship->IsExternalCamera())
		{
			DrawHUDIcon(ViewportSize / 2, 32, Ship->IsCombatMode() ? HUDAimIcon : HUDNoseIcon, HudColor, true);
		}

		// Update inertial vector
		FVector EndPoint = Ship->GetActorLocation() + FocusDistance * ShipVelocity;
		if (PC->ProjectWorldLocationToScreen(EndPoint, ScreenPosition))
		{
			DrawHUDIcon(ScreenPosition, 32, HUDReticleIcon, HudColor, true);
		}
	}
}

void AFlareHUD::DrawHUDDesignator(AFlareShipBase* ShipBase)
{
	// Calculation data
	FVector2D ScreenPosition;
	FVector2D ViewportSize = FVector2D(GEngine->GameViewport->Viewport->GetSizeXY());
	AFlarePlayerController* PC = Cast<AFlarePlayerController>(GetOwner());
	FVector PlayerLocation = PC->GetShipPawn()->GetActorLocation();
	FVector TargetLocation = ShipBase->GetActorLocation();

	if (PC->ProjectWorldLocationToScreen(TargetLocation, ScreenPosition))
	{
		// Compute apparent size in screenspace
		float ShipSize = 2 * ShipBase->GetMeshScale();
		float Distance = (TargetLocation - PlayerLocation).Size();
		float ApparentAngle = FMath::RadiansToDegrees(FMath::Atan(ShipSize / Distance));
		float Size = (ApparentAngle / PC->PlayerCameraManager->GetFOVAngle()) * ViewportSize.X;
		FVector2D ObjectSize = FMath::Min(0.8f * Size, 500.0f) * FVector2D(1, 1);

		// Check if the mouse is there
		FVector2D MousePos = PC->GetMousePosition();
		FVector2D ShipBoxMin = ScreenPosition - ObjectSize / 2;
		FVector2D ShipBoxMax = ScreenPosition + ObjectSize / 2;
		bool Hovering = (MousePos.X >= ShipBoxMin.X && MousePos.Y >= ShipBoxMin.Y && MousePos.X <= ShipBoxMax.X && MousePos.Y <= ShipBoxMax.Y);

		// Draw the context menu
		AFlareShip* Ship = Cast<AFlareShip>(ShipBase);
		if (Hovering && !FoundTargetUnderMouse && IsInteractive)
		{
			// Update state
			FoundTargetUnderMouse = true;
			ContextMenuPosition = ScreenPosition;

			// If station, set data
			AFlareStation* Station = Cast<AFlareStation>(ShipBase);
			if (Station)
			{
				ContextMenu->SetStation(Station);
				ContextMenu->Show();
			}

			// If ship, set data
			if (Ship)
			{
				ContextMenu->SetShip(Ship);
				if (Ship->IsAlive())
				{
					ContextMenu->Show();
				}
			}
		}

		// Draw the HUD designator
		else if ((Ship && Ship->IsAlive()) || !Ship)
		{
			float CornerSize = 16;
			float IconSize = 32;

			// Draw designator corners
			DrawHUDDesignatorCorner(ScreenPosition, ObjectSize, CornerSize, FVector2D(-1, -1), 0);
			DrawHUDDesignatorCorner(ScreenPosition, ObjectSize, CornerSize, FVector2D(-1, +1), -90);
			DrawHUDDesignatorCorner(ScreenPosition, ObjectSize, CornerSize, FVector2D(+1, +1), -180);
			DrawHUDDesignatorCorner(ScreenPosition, ObjectSize, CornerSize, FVector2D(+1, -1), -270);

			// Draw the status
			if (Ship && ObjectSize.X > 2 * IconSize)
			{
				int32 NumberOfIcons = Ship->IsMilitary() ? 5 : 4;
				FVector2D StatusPos = ScreenPosition - ObjectSize / 2;
				StatusPos.X += 0.5 * (ObjectSize.X - NumberOfIcons * IconSize);
				StatusPos.Y -= (IconSize + 0.5 * CornerSize);
				DrawHUDDesignatorStatus(StatusPos, IconSize, Ship);
			}

			// Combat helper
			AFlareShip* PlayerShip = PC->GetShipPawn();
			if (Ship && PlayerShip && PlayerShip->IsCombatMode())
			{
				if (PC->ProjectWorldLocationToScreen(Ship->GetAimPosition(PlayerShip, 50000), ScreenPosition)) // TODO get from projectile
				{
					DrawHUDIcon(ScreenPosition, 32, HUDAimHelperIcon, HudColor, true);
				}
			}
		}
	}
}

void AFlareHUD::DrawHUDDesignatorCorner(FVector2D Position, FVector2D ObjectSize, float IconSize, FVector2D MainOffset, float Rotation)
{
	DrawTexture(HUDDesignatorCornerTexture,
		Position.X + (ObjectSize.X + IconSize) * MainOffset.X / 2,
		Position.Y + (ObjectSize.Y + IconSize) * MainOffset.Y / 2,
		IconSize, IconSize, 0, 0, 1, 1,
		HudColor,
		BLEND_Translucent, 1.0f, false,
		Rotation);
}

void AFlareHUD::DrawHUDDesignatorStatus(FVector2D Position, float IconSize, AFlareShip* Ship)
{
	Position = DrawHUDDesignatorStatusIcon(Position, IconSize, Ship->GetSubsystemHealth(EFlareSubsystem::SYS_Temperature), HUDTemperatureIcon);
	Position = DrawHUDDesignatorStatusIcon(Position, IconSize, Ship->GetSubsystemHealth(EFlareSubsystem::SYS_Power), HUDPowerIcon);
	Position = DrawHUDDesignatorStatusIcon(Position, IconSize, Ship->GetSubsystemHealth(EFlareSubsystem::SYS_Propulsion), HUDPropulsionIcon);
	Position = DrawHUDDesignatorStatusIcon(Position, IconSize, Ship->GetSubsystemHealth(EFlareSubsystem::SYS_RCS), HUDRCSIcon);

	if (Ship->IsMilitary())
	{
		DrawHUDDesignatorStatusIcon(Position, IconSize, Ship->GetSubsystemHealth(EFlareSubsystem::SYS_Weapon), HUDWeaponIcon);
	}
}

FVector2D AFlareHUD::DrawHUDDesignatorStatusIcon(FVector2D Position, float IconSize, float Health, UTexture2D* Texture)
{
	FLinearColor Color = FLinearColor(FColor::MakeRedToGreenColorFromScalar(Health)).Desaturate(0.05);
	Color.A = 0.7;
	DrawHUDIcon(Position, IconSize, Texture, Color);
	return Position + IconSize * FVector2D(1, 0);
}

void AFlareHUD::DrawHUDIcon(FVector2D Position, float IconSize, UTexture2D* Texture, FLinearColor Color, bool Center)
{
	if (Center)
	{
		Position -= (IconSize / 2) * FVector2D::UnitVector;
	}
	DrawTexture(Texture, Position.X, Position.Y, IconSize, IconSize, 0, 0, 1, 1, Color);
}


/*----------------------------------------------------
	Menu interaction
----------------------------------------------------*/

void AFlareHUD::SetupMenu(FFlarePlayerSave& PlayerData)
{
	if (GEngine->IsValidLowLevel())
	{
		// HUD widget
		SAssignNew(OverlayContainer, SOverlay)

			// Context menu
			+ SOverlay::Slot()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				SAssignNew(ContextMenu, SFlareContextMenu).OwnerHUD(this)
			];

		// Create menus
		SAssignNew(HUDMenu, SFlareHUDMenu).OwnerHUD(this);
		SAssignNew(Dashboard, SFlareDashboard).OwnerHUD(this);
		SAssignNew(CompanyMenu, SFlareCompanyMenu).OwnerHUD(this);
		SAssignNew(ShipMenu, SFlareShipMenu).OwnerHUD(this);
		SAssignNew(StationMenu, SFlareStationMenu).OwnerHUD(this);
		SAssignNew(SectorMenu, SFlareSectorMenu).OwnerHUD(this);
		SAssignNew(Notifier, SFlareNotifier).OwnerHUD(this);

		// Fade-to-black system
		SAssignNew(Fader, SBorder)
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			.BorderImage(FFlareStyleSet::Get().GetBrush("/Brushes/SB_Black"));

		// Register menus at their Z-Index
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(HUDMenu.ToSharedRef()),          0);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(OverlayContainer.ToSharedRef()), 10);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(Dashboard.ToSharedRef()),        50);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(CompanyMenu.ToSharedRef()),      50);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(ShipMenu.ToSharedRef()),         50);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(StationMenu.ToSharedRef()),      50);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(SectorMenu.ToSharedRef()),       50);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(Notifier.ToSharedRef()),         90);
		GEngine->GameViewport->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(Fader.ToSharedRef()),            100);

		// Setup menus
		Dashboard->Setup();
		CompanyMenu->Setup(PlayerData);
		ShipMenu->Setup();
		StationMenu->Setup();
		SectorMenu->Setup();

		// Setup extra menus
		ContextMenu->Hide();
		Fader->SetVisibility(EVisibility::Hidden);
		AFlarePlayerController* PC = Cast<AFlarePlayerController>(GetOwner());
		if (PC)
		{
			HUDMenu->SetTargetShip(PC->GetShipPawn());
		}
	}
}

void AFlareHUD::Notify(FText Text, EFlareNotification::Type Type, EFlareMenu::Type TargetMenu, void* TargetInfo)
{
	Notifier->Notify(Text, Type, TargetMenu, TargetInfo);
}

void AFlareHUD::OpenMenu(EFlareMenu::Type Target, void* Data)
{
	MenuIsOpen = true;
	FadeOut();
	FadeTarget = Target;
	FadeTargetData = Data;
}

void AFlareHUD::CloseMenu(bool HardClose)
{
	if (MenuIsOpen)
	{
		if (HardClose)
		{
			ExitMenu();
		}
		else
		{
			OpenMenu(EFlareMenu::MENU_Exit);
		}
	}
	MenuIsOpen = false;
}

void AFlareHUD::SetInteractive(bool Status)
{
	IsInteractive = Status;
}


/*----------------------------------------------------
	Menu commands
----------------------------------------------------*/

void AFlareHUD::ProcessFadeTarget()
{
	AFlarePlayerController* PC = Cast<AFlarePlayerController>(GetOwner());

	switch (FadeTarget)
	{
		case EFlareMenu::MENU_Dashboard:
			OpenDashboard();
			break;

		case EFlareMenu::MENU_Company:
			InspectCompany(static_cast<UFlareCompany*>(FadeTargetData));
			break;

		case EFlareMenu::MENU_Ship:
			InspectShip(static_cast<IFlareShipInterface*>(FadeTargetData));
			break;

		case EFlareMenu::MENU_ShipConfig:
			InspectShip(static_cast<IFlareShipInterface*>(FadeTargetData), true);
			break;

		case EFlareMenu::MENU_Sector:
			OpenSector();
			break;

		case EFlareMenu::MENU_Station:
			InspectStation(static_cast<IFlareStationInterface*>(FadeTargetData));
			break;

		case EFlareMenu::MENU_Quit:
			PC->ConsoleCommand("quit");
			break;

		case EFlareMenu::MENU_Exit:
			ExitMenu();
			break;

		case EFlareMenu::MENU_None:
		default:
			break;
	}

	FadeTarget = EFlareMenu::MENU_None;
	FadeTargetData = NULL;
}

void AFlareHUD::OpenDashboard()
{
	ResetMenu();
	SetMenuPawn(true);
	OverlayContainer->SetVisibility(EVisibility::Hidden);

	Dashboard->Enter();
}

void AFlareHUD::InspectCompany(UFlareCompany* Target)
{
	ResetMenu();
	SetMenuPawn(true);
	OverlayContainer->SetVisibility(EVisibility::Hidden);

	if (Target == NULL)
	{
		Target = Cast<AFlarePlayerController>(GetOwner())->GetCompany();
	}
	CompanyMenu->Enter(Target);
}

void AFlareHUD::InspectShip(IFlareShipInterface* Target, bool IsEditable)
{
	ResetMenu();
	SetMenuPawn(true);
	OverlayContainer->SetVisibility(EVisibility::Hidden);

	if (Target == NULL)
	{
		Target = Cast<AFlarePlayerController>(GetOwner())->GetShipPawn();
	}
	ShipMenu->Enter(Target, IsEditable);
}

void AFlareHUD::InspectStation(IFlareStationInterface* Target, bool IsEditable)
{
	ResetMenu();
	SetMenuPawn(true);
	OverlayContainer->SetVisibility(EVisibility::Hidden);

	AFlareShip* PlayerShip = Cast<AFlarePlayerController>(GetOwner())->GetShipPawn();

	if (Target == NULL && PlayerShip && PlayerShip->IsDocked())
	{
		Target = PlayerShip->GetDockStation();
	}
	StationMenu->Enter(Target);
}

void AFlareHUD::OpenSector()
{
	ResetMenu();
	SetMenuPawn(true);
	OverlayContainer->SetVisibility(EVisibility::Hidden);

	SectorMenu->Enter();
}

void AFlareHUD::ExitMenu()
{
	ResetMenu();
	SetMenuPawn(false);
	HUDMenu->SetVisibility(EVisibility::Visible);
	OverlayContainer->SetVisibility(EVisibility::Visible);
}


/*----------------------------------------------------
	Menu management
----------------------------------------------------*/

void AFlareHUD::ResetMenu()
{
	AFlarePlayerController* PC = Cast<AFlarePlayerController>(GetOwner());

	Dashboard->Exit();
	CompanyMenu->Exit();
	ShipMenu->Exit();
	StationMenu->Exit();
	SectorMenu->Exit();
	HUDMenu->SetVisibility(EVisibility::Hidden);

	if (PC)
	{
		PC->GetMenuPawn()->ResetContent();
	}

	FadeIn();
	HUDMenu->SetTargetShip(PC->GetShipPawn());
}

void AFlareHUD::FadeIn()
{
	FadeFromBlack = true;
	FadeTimer = 0;
}

void AFlareHUD::FadeOut()
{
	FadeFromBlack = false;
	FadeTimer = 0;
}

void AFlareHUD::SetMenuPawn(bool Status)
{
	AFlarePlayerController* PC = Cast<AFlarePlayerController>(GetOwner());
	if (PC)
	{
		if (Status)
		{
			PC->OnEnterMenu();
		}
		else
		{
			PC->OnExitMenu();
		}
	}
}


/*----------------------------------------------------
	Slate
----------------------------------------------------*/

const FSlateBrush* AFlareHUD::GetMenuIcon(EFlareMenu::Type MenuType)
{
	switch (MenuType)
	{
	case EFlareMenu::MENU_Dashboard:      return FFlareStyleSet::GetIcon("Dashboard");
	case EFlareMenu::MENU_Company:        return FFlareStyleSet::GetIcon("Company");
	case EFlareMenu::MENU_Ship:           return FFlareStyleSet::GetIcon("Ship");
	case EFlareMenu::MENU_ShipConfig:     return FFlareStyleSet::GetIcon("ShipUpgrade");
	case EFlareMenu::MENU_Station:        return FFlareStyleSet::GetIcon("Station");
	case EFlareMenu::MENU_Undock:         return FFlareStyleSet::GetIcon("Undock");
	case EFlareMenu::MENU_Sector:         return FFlareStyleSet::GetIcon("Sector");
	case EFlareMenu::MENU_Orbit:          return FFlareStyleSet::GetIcon("Universe");
	case EFlareMenu::MENU_Encyclopedia:   return FFlareStyleSet::GetIcon("Encyclopedia");
	case EFlareMenu::MENU_Help:           return FFlareStyleSet::GetIcon("Help");
	case EFlareMenu::MENU_Settings:       return FFlareStyleSet::GetIcon("Settings");
	case EFlareMenu::MENU_Quit:           return FFlareStyleSet::GetIcon("Quit");
	case EFlareMenu::MENU_Exit:           return FFlareStyleSet::GetIcon("Close");

	case EFlareMenu::MENU_None:
	default:
		return NULL;
	}
}

void AFlareHUD::ShowLoadingScreen()
{
	IFlareLoadingScreenModule* LoadingScreenModule = FModuleManager::LoadModulePtr<IFlareLoadingScreenModule>("FlareLoadingScreen");
	if (LoadingScreenModule)
	{
		LoadingScreenModule->StartInGameLoadingScreen();
	}
}

#undef LOCTEXT_NAMESPACE

