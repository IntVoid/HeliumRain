
#include "../Flare.h"

#include "FlareSpacecraft.h"
#include "FlareAirframe.h"
#include "FlareOrbitalEngine.h"
#include "FlareRCS.h"
#include "FlareWeapon.h"
#include "FlareInternalComponent.h"

#include "Particles/ParticleSystemComponent.h"

#include "../Player/FlarePlayerController.h"
#include "../Game/FlareGame.h"


#define LOCTEXT_NAMESPACE "FlareShip"


/*----------------------------------------------------
	Constructor
----------------------------------------------------*/

AFlareSpacecraft::AFlareSpacecraft(const class FObjectInitializer& PCIP)
	: Super(PCIP)
	, AngularDeadAngle(0.5)
	, AngularInputDeadRatio(0.0025)
	, LinearDeadDistance(0.1)
	, LinearMaxDockingVelocity(10)
	, NegligibleSpeedRatio(0.0005)
	, Status(EFlareShipStatus::SS_Manual)
{
	// Create static mesh component
	Airframe = PCIP.CreateDefaultSubobject<UFlareAirframe>(this, TEXT("Airframe"));
	Airframe->SetSimulatePhysics(true);
	RootComponent = Airframe;

	// Camera settings
	CameraContainerYaw->AttachTo(Airframe);
	CameraMaxPitch = 80;
	CameraPanSpeed = 2;

	// Dock info
	ShipData.DockedTo = NAME_None;
	ShipData.DockedAt = -1;

	// Pilot
	IsPiloted = true;
}


/*----------------------------------------------------
	Gameplay events
----------------------------------------------------*/

void AFlareSpacecraft::BeginPlay()
{
	Super::BeginPlay();

	UpdateCOM();
}

void AFlareSpacecraft::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());

	// Update Camera
	if (!ExternalCamera && CombatMode)
	{
		if (CombatMode)
		{
			TArray<UFlareWeapon*> Weapons = GetWeaponList();
			if (Weapons.Num() > 0)
			{
				float AmmoVelocity = Weapons[0]->GetAmmoVelocity();
				FRotator ShipAttitude = GetActorRotation();
				FVector ShipVelocity = 100.f * GetLinearVelocity();

				// Bullet velocity
				FVector BulletVelocity = ShipAttitude.Vector();
				BulletVelocity.Normalize();
				BulletVelocity *= 100.f * AmmoVelocity; // TODO get from projectile


				FVector BulletDirection = (ShipVelocity + BulletVelocity).GetUnsafeNormal();

				FVector LocalBulletDirection = Airframe->GetComponentToWorld().GetRotation().Inverse().RotateVector(BulletDirection);

				float Pitch = FMath::RadiansToDegrees(FMath::Asin(LocalBulletDirection.Z));
				float Yaw = FMath::RadiansToDegrees(FMath::Asin(LocalBulletDirection.Y));

				SetCameraPitch(Pitch);
				SetCameraYaw(Yaw);
			}
		}
		else
		{
			SetCameraPitch(0);
			SetCameraYaw(0);
		}
	}

	// Attitude control
	if (Airframe && !IsPresentationMode())
	{
		UpdateCOM();

		if (IsAlive() && IsPiloted) // Also tick not piloted ship
		{
			Pilot->TickPilot(DeltaSeconds);
		}


		// Manual pilot
		if (IsManualPilot() && IsAlive())
		{
			if (IsPiloted)
			{
				LinearTargetVelocity = Pilot->GetLinearTargetVelocity();
				AngularTargetVelocity = Pilot->GetAngularTargetVelocity();
				ManualOrbitalBoost = Pilot->IsUseOrbitalBoost();
				if (Pilot->IsWantFire())
				{
					StartFire();
				}
				else
				{
					StopFire();
				}
			}
			else
			{
				UpdateLinearAttitudeManual(DeltaSeconds);
				UpdateAngularAttitudeManual(DeltaSeconds);
			}
		}

		// Autopilot
		else if (IsAutoPilot())
		{
			FFlareShipCommandData CurrentCommand;
			if (CommandData.Peek(CurrentCommand))
			{
				if (CurrentCommand.Type == EFlareCommandDataType::CDT_Location)
				{
					UpdateLinearAttitudeAuto(DeltaSeconds, (CurrentCommand.PreciseApproach ? LinearMaxDockingVelocity : LinearMaxVelocity));
				}
				else if (CurrentCommand.Type == EFlareCommandDataType::CDT_BrakeLocation)
				{
					UpdateLinearBraking(DeltaSeconds);
				}
				else if (CurrentCommand.Type == EFlareCommandDataType::CDT_Rotation)
				{
					UpdateAngularAttitudeAuto(DeltaSeconds);
				}
				else if (CurrentCommand.Type == EFlareCommandDataType::CDT_BrakeRotation)
				{
					UpdateAngularBraking(DeltaSeconds);
				}
				else if (CurrentCommand.Type == EFlareCommandDataType::CDT_Dock)
				{
					ConfirmDock(Cast<IFlareSpacecraftInterface>(CurrentCommand.ActionTarget), CurrentCommand.ActionTargetParam);
				}
			}
		}

		// Physics
		if (!IsDocked())
		{
			// TODO enable physic when docked but attach the ship to the station

			PhysicSubTick(DeltaSeconds);
		}
	}

	// Apply heat variation : add producted heat then substract radiated heat.

	// Get the to heat production and heat sink surface
	float HeatProduction = 0.f;
	float HeatSinkSurface = 0.f;
	for (int32 i = 0; i < Components.Num(); i++)
	{
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[i]);
		HeatProduction += Component->GetHeatProduction();
		HeatSinkSurface += Component->GetHeatSinkSurface();
	}

	// Add a part of sun radiation to ship heat production
	// Sun flow is 3.094KW/m^2 and keep only half.
	HeatProduction += HeatSinkSurface * 3.094 * 0.5;

	// Heat up
	ShipData.Heat += HeatProduction * DeltaSeconds;
	// Radiate: Stefan-Boltzmann constant=5.670373e-8
	float Temperature = ShipData.Heat / ShipDescription->HeatCapacity;
	float HeatRadiation = 0.f;
	if (Temperature > 0)
	{
		HeatRadiation = HeatSinkSurface * 5.670373e-8 * FMath::Pow(Temperature, 4) / 1000;
	}
	// Don't radiate too much energy : negative temperature is not possible
	ShipData.Heat -= FMath::Min(HeatRadiation * DeltaSeconds, ShipData.Heat);

	// Overheat after 800°K, compute heat damage from temperature beyond 800°K : 0.005%/(°K*s)
	float OverheatDamage = (Temperature - GetOverheatTemperature()) * DeltaSeconds * 0.00005;
	float BurningDamage = FMath::Max((Temperature - GetBurnTemperature()) * DeltaSeconds * 0.0001, 0.0);

	// Update component temperature and apply heat damage
	for (int32 i = 0; i < Components.Num(); i++)
	{
		// Apply temperature
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[i]);

		// Overheat apply damage is necessary
		if (OverheatDamage > 0)
		{
			Component->ApplyHeatDamage(Component->GetTotalHitPoints() * OverheatDamage, Component->GetTotalHitPoints() * BurningDamage);
		}
	}

	// If damage have been applied, power production may have change
	if (OverheatDamage > 0)
	{
		UpdatePower();
	}

	// Power outage
	if (ShipData.PowerOutageDelay > 0)
	{
		ShipData.PowerOutageDelay -=  DeltaSeconds;
		if (ShipData.PowerOutageDelay <=0)
		{
			ShipData.PowerOutageDelay = 0;
			UpdatePower(); // To update light
		}
	}

	// Update Alive status
	if (WasAlive && !IsAlive())
	{
		WasAlive = false;
		OnControlLost();
	}
}

void AFlareSpacecraft::NotifyHit(class UPrimitiveComponent* MyComp, class AActor* Other, class UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::ReceiveHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);

	// If receive hit from over actor, like a ship we must apply collision damages.
	// The applied damage energy is 0.2% of the kinetic energy of the other actor. The kinetic
	// energy is calculated from the relative speed between the 2 actors, and only with the relative
	// speed projected in the axis of the collision normal: if 2 very fast ship only slightly touch,
	// only few energy will be decipated by the impact.
	//
	// The damages are applied only to the current actor, the ReceiveHit method of the other actor
	// will also call an it will apply its collision damages itself.

	// If the other actor is a projectile, specific weapon damage code is done in the projectile hit
	// handler: in this case we ignore the collision
	AFlareShell* OtherProjectile = Cast<AFlareShell>(Other);
	if (OtherProjectile) {
		return;
	}

	UPrimitiveComponent* OtherRoot = Cast<UPrimitiveComponent>(Other->GetRootComponent());

	// Relative velocity
	FVector DeltaVelocity = ((OtherRoot->GetPhysicsLinearVelocity() - Airframe->GetPhysicsLinearVelocity()) / 100);

	// Compute the relative velocity in the impact axis then compute kinetic energy
	float ImpactSpeed = FVector::DotProduct(DeltaVelocity, HitNormal);
	float ImpactMass = OtherRoot->GetMass();
	float ImpactEnergy = 0.5 * ImpactMass * FMath::Square(ImpactSpeed) * 0.001; // In KJ

	// Convert only 0.2% of the energy as damages and make vary de damage radius with the damage:
	// minimum 20cm and about 1 meter for 20KJ of damage (about the damages provide by a bullet)
	float Energy = ImpactEnergy / 500;
	float  Radius = 0.2 + FMath::Sqrt(Energy) * 0.22;

	ApplyDamage(Energy, Radius, HitLocation);
}

void AFlareSpacecraft::Destroyed()
{
	if (Company)
	{
		Company->Unregister(this);
	}
}


/*----------------------------------------------------
	Player interface
----------------------------------------------------*/

void AFlareSpacecraft::SetExternalCamera(bool NewState)
{
	// Stop firing
	if (NewState)
	{
		StopFire();
		ManualOrbitalBoost = false;
	}

	// Reset rotations
	ExternalCamera = NewState;
	SetCameraPitch(0);
	SetCameraYaw(0);

	// Reset controls
	ManualLinearVelocity = FVector::ZeroVector;
	ManualAngularVelocity = FVector::ZeroVector;

	// Put the camera at the right spot
	if (ExternalCamera)
	{
		SetCameraLocalPosition(FVector::ZeroVector);
		SetCameraDistance(CameraMaxDistance * GetMeshScale());
	}
	else
	{
		FVector CameraOffset = WorldToLocal(Airframe->GetSocketLocation(FName("Camera")) - GetActorLocation());
		SetCameraDistance(0);
		SetCameraLocalPosition(CameraOffset);
	}
}

void AFlareSpacecraft::SetCombatMode(bool NewState)
{
	CombatMode = NewState;
	MouseOffset = FVector2D(0,0);
	MousePositionInput(MouseOffset);

	if (!NewState && FiringPressed)
	{
		StopFire();
	}
}


FVector AFlareSpacecraft::GetAimPosition(AFlareSpacecraft* TargettingShip, float BulletSpeed, float PredictionDelay) const
{
	//Relative Target Speed
	FVector TargetVelocity = Airframe->GetPhysicsLinearVelocity();
	FVector TargetLocation = GetActorLocation() + TargetVelocity * PredictionDelay;
	FVector BulletLocation = TargettingShip->GetActorLocation() + TargettingShip->GetLinearVelocity() * 100 * PredictionDelay;

	// Find the relative speed in the axis of target
	FVector TargetDirection = (TargetLocation - BulletLocation).GetUnsafeNormal();
	FVector BonusVelocity = TargettingShip->GetLinearVelocity() * 100;
	float BonusVelocityInTargetAxis = FVector::DotProduct(TargetDirection, BonusVelocity);
	float EffectiveBulletSpeed = BulletSpeed * 100.f + BonusVelocityInTargetAxis;

	float Divisor = FMath::Square(EffectiveBulletSpeed) - TargetVelocity.SizeSquared();

	float A = -1;
	float B = 2 * (TargetVelocity.X * (TargetLocation.X - BulletLocation.X) + TargetVelocity.Y * (TargetLocation.Y - BulletLocation.Y) + TargetVelocity.Z * (TargetLocation.Z - BulletLocation.Z)) / Divisor;
	float C = (TargetLocation - BulletLocation).SizeSquared() / Divisor;

	float Delta = FMath::Square(B) - 4 * A * C;

	float InterceptTime1 = (- B - FMath::Sqrt(Delta)) / (2 * A);
	float InterceptTime2 = (- B + FMath::Sqrt(Delta)) / (2 * A);

	float InterceptTime = FMath::Max(InterceptTime1, InterceptTime2);

	FVector InterceptLocation = TargetLocation + TargetVelocity * InterceptTime;

	return InterceptLocation;
}

void AFlareSpacecraft::SetStatus(EFlareShipStatus::Type NewStatus)
{
	FLOGV("AFlareSpacecraft::SetStatus %d", NewStatus - EFlareShipStatus::SS_Manual);
	Status = NewStatus;
}


/*----------------------------------------------------
	Ship interface
----------------------------------------------------*/

void AFlareSpacecraft::Load(const FFlareSpacecraftSave& Data)
{
	// Clear previous data
	WeaponList.Empty();
	WeaponDescriptionList.Empty();

	// Update local data
	ShipData = Data;
	ShipData.Name = FName(*GetName());

		// Load ship description
	UFlareSpacecraftComponentsCatalog* Catalog = GetGame()->GetShipPartsCatalog();
	FFlareSpacecraftDescription* Desc = GetGame()->GetSpacecraftCatalog()->Get(Data.Identifier);
	SetShipDescription(Desc);

	// Look for parent company
	SetOwnerCompany(GetGame()->FindCompany(Data.CompanyIdentifier));


	// Initialize components
	TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
	TArray<UFlareSpacecraftComponent*> PowerSources;
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
	{
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);
		FFlareSpacecraftComponentSave ComponentData;

		// Find component the corresponding component data comparing the slot id
		bool found = false;
		for (int32 i = 0; i < Data.Components.Num(); i++)
		{
			if (Component->SlotIdentifier == Data.Components[i].ShipSlotIdentifier)
			{
				ComponentData = Data.Components[i];
				found = true;
				break;
			}
		}

		// If no data, this is a cosmetic component and it don't need to be initialized
		if (!found)
		{
			continue;
		}

		// Reload the component
		ReloadPart(Component, &ComponentData);

		// Set RCS description
		FFlareSpacecraftComponentDescription* ComponentDescription = Catalog->Get(ComponentData.ComponentIdentifier);
		if (ComponentDescription->Type == EFlarePartType::RCS)
		{
			SetRCSDescription(ComponentDescription);
		}

		// Set orbital engine description
		else if (ComponentDescription->Type == EFlarePartType::OrbitalEngine)
		{
			SetOrbitalEngineDescription(ComponentDescription);
		}

		// If this is a weapon, add to weapon list.
		UFlareWeapon* Weapon = Cast<UFlareWeapon>(Component);
		if (Weapon)
		{
			WeaponList.Add(Weapon);
		}

		// Find the cockpit
		if(ComponentDescription->GeneralCharacteristics.LifeSupport)
		{
			ShipCockit = Component;
		}

		// Fill power sources
		if (Component->IsGenerator())
		{
			PowerSources.Add(Component);
		}
	}

	// Second pass, update component power sources and update power
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
	{
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);
		Component->UpdatePowerSources(&PowerSources);
	}
	UpdatePower();


	// Load weapon descriptions
	for (int32 i = 0; i < Data.Components.Num(); i++)
	{
		FFlareSpacecraftComponentDescription* ComponentDescription = Catalog->Get(Data.Components[i].ComponentIdentifier);
		if (ComponentDescription->Type == EFlarePartType::Weapon)
		{
			WeaponDescriptionList.Add(ComponentDescription);
		}
	}

	// Customization
	UpdateCustomization();

	// Re-dock if we were docked
	if (ShipData.DockedTo != NAME_None)
	{
		FLOGV("AFlareSpacecraft::Load : Looking for station '%s'", *ShipData.DockedTo.ToString());
		for (TActorIterator<AActor> ActorItr(GetWorld()); ActorItr; ++ActorItr)
		{
			AFlareSpacecraft* Station = Cast<AFlareSpacecraft>(*ActorItr);
			if (Station && *Station->GetName() == ShipData.DockedTo)
			{
				FLOGV("AFlareSpacecraft::Load : Found dock station '%s'", *Station->GetName());
				ConfirmDock(Station, ShipData.DockedAt);
				break;
			}
		}
	}

	// Initialize pilot
	Pilot = NewObject<UFlareShipPilot>(this, UFlareShipPilot::StaticClass());
	Pilot->Initialize(&ShipData.Pilot, GetCompany(), this);

	// Init alive status
	WasAlive = IsAlive();

}

FFlareSpacecraftSave* AFlareSpacecraft::Save()
{
	// Physical data
	ShipData.Location = GetActorLocation();
	ShipData.Rotation = GetActorRotation();
	ShipData.LinearVelocity = Airframe->GetPhysicsLinearVelocity();
	ShipData.AngularVelocity = Airframe->GetPhysicsAngularVelocity();

	// Save all components datas
	ShipData.Components.Empty();
	TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
	{
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);
		FFlareSpacecraftComponentSave* ComponentSave = Component->Save();

		if (ComponentSave) {
			ShipData.Components.Add(*ComponentSave);
		}
	}

	return &ShipData;
}

void AFlareSpacecraft::SetOwnerCompany(UFlareCompany* NewCompany)
{
	SetCompany(NewCompany);
	ShipData.CompanyIdentifier = NewCompany->GetIdentifier();
	Airframe->Initialize(NULL, Company, this);
	NewCompany->Register(this);
}

UFlareCompany* AFlareSpacecraft::GetCompany()
{
	return Company;
}

bool AFlareSpacecraft::IsMilitary()
{
	return (ShipDescription->GunSlots.Num() + ShipDescription->TurretSlots.Num()) > 0;
}

bool AFlareSpacecraft::IsStation()
{
	return ShipDescription->OrbitalEngineCount == 0;
}

float AFlareSpacecraft::GetSubsystemHealth(EFlareSubsystem::Type Type, bool WithArmor)
{
	float Health = 0.f;

	switch(Type)
	{
		case EFlareSubsystem::SYS_Propulsion:
		{
			TArray<UActorComponent*> Engines = GetComponentsByClass(UFlareEngine::StaticClass());
			float Total = 0.f;
			float EngineCount = 0;
			for (int32 ComponentIndex = 0; ComponentIndex < Engines.Num(); ComponentIndex++)
			{
				UFlareEngine* Engine = Cast<UFlareEngine>(Engines[ComponentIndex]);
				if (Engine->IsA(UFlareOrbitalEngine::StaticClass()))
				{
					EngineCount+=1.f;
					Total+=Engine->GetDamageRatio(WithArmor)*(Engine->IsPowered() ? 1 : 0);
				}
			}
			Health = Total/EngineCount;
		}
		break;
		case EFlareSubsystem::SYS_RCS:
		{
			TArray<UActorComponent*> Engines = GetComponentsByClass(UFlareEngine::StaticClass());
			float Total = 0.f;
			float EngineCount = 0;
			for (int32 ComponentIndex = 0; ComponentIndex < Engines.Num(); ComponentIndex++)
			{
				UFlareEngine* Engine = Cast<UFlareEngine>(Engines[ComponentIndex]);
				if (!Engine->IsA(UFlareOrbitalEngine::StaticClass()))
				{
					EngineCount+=1.f;
					Total+=Engine->GetDamageRatio(WithArmor)*(Engine->IsPowered() ? 1 : 0);
				}
			}
			Health = Total/EngineCount;
		}
		break;
		case EFlareSubsystem::SYS_LifeSupport:
		{
			if (ShipCockit)
			{
				Health = ShipCockit->GetDamageRatio(WithArmor) * (ShipCockit->IsPowered() ? 1 : 0);
			}
			else
			{
				// No cockpit mean no destructible
				Health = 1.0f;
			}
		}
		break;
		case EFlareSubsystem::SYS_Power:
		{
			TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
			float Total = 0.f;
			float GeneratorCount = 0;
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
			{
				UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);
				if (Component->IsGenerator())
				{
					GeneratorCount+=1.f;
					Total+=Component->GetDamageRatio(WithArmor);
				}
			}
			Health = Total/GeneratorCount;
		}
		break;
		case EFlareSubsystem::SYS_Weapon:
		{
			TArray<UActorComponent*> Weapons = GetComponentsByClass(UFlareWeapon::StaticClass());
			float Total = 0.f;
			for (int32 ComponentIndex = 0; ComponentIndex < Weapons.Num(); ComponentIndex++)
			{
				UFlareWeapon* Weapon = Cast<UFlareWeapon>(Weapons[ComponentIndex]);
				Total += Weapon->GetDamageRatio(WithArmor)*(Weapon->IsPowered() ? 1 : 0)*(Weapon->GetCurrentAmmo() > 0 ? 1 : 0);
			}
			Health = Total/Weapons.Num();
		}
		break;
		case EFlareSubsystem::SYS_Temperature:
		{
			TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
			float Total = 0.f;
			float HeatSinkCount = 0;
			for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
			{
				UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);
				if (Component->IsHeatSink())
				{
					HeatSinkCount+=1.f;
					Total+=Component->GetDamageRatio(WithArmor) * (ShipCockit->IsPowered() ? 1 : 0);
				}
			}
			Health = Total/HeatSinkCount;
		}
		break;
	}

	return Health;
}

float AFlareSpacecraft::GetTemperature()
{
	return ShipData.Heat / ShipDescription->HeatCapacity;
}

float AFlareSpacecraft::GetOverheatTemperature()
{
	return 1200;
}

float AFlareSpacecraft::GetBurnTemperature()
{
	return 1500;
}

bool AFlareSpacecraft::NavigateTo(FVector TargetLocation)
{
	// Pathfinding data
	TArray<FVector> Path;
	FVector Unused;
	FVector ShipExtent;
	FVector Temp = GetActorLocation();

	// Prepare data
	FLOG("AFlareSpacecraft::NavigateTo");
	GetActorBounds(true, Unused, ShipExtent);
	UpdateColliders();

	// Compute path
	if (ComputePath(Path, PathColliders, Temp, TargetLocation, ShipExtent.Size()))
	{
		FLOGV("AFlareSpacecraft::NavigateTo : generating path (%d stops)", Path.Num());

		// Generate commands for travel
		for (int32 i = 0; i < Path.Num(); i++)
		{
			PushCommandRotation((Path[i] - Temp), FVector(1,0,0)); // Front
			PushCommandLocation(Path[i]);
			Temp = Path[i];
		}

		// Move toward objective for pre-final approach
		PushCommandRotation((TargetLocation - Temp), FVector(1,0,0));
		PushCommandLocation(TargetLocation);
		return true;
	}

	// Failed
	FLOG("AFlareSpacecraft::NavigateTo failed : no path found");
	return false;
}

bool AFlareSpacecraft::IsManualPilot()
{
	return (Status == EFlareShipStatus::SS_Manual);
}

bool AFlareSpacecraft::IsAutoPilot()
{
	return (Status == EFlareShipStatus::SS_AutoPilot);
}

bool AFlareSpacecraft::IsDocked()
{
	return (Status == EFlareShipStatus::SS_Docked);
}

/*----------------------------------------------------
	Docking
----------------------------------------------------*/

bool AFlareSpacecraft::DockAt(IFlareSpacecraftInterface* TargetStation)
{
	FLOG("AFlareSpacecraft::DockAt");
	FFlareDockingInfo DockingInfo = TargetStation->RequestDock(this);

	// Try to dock
	if (DockingInfo.Granted)
	{
		FLOG("AFlareSpacecraft::DockAt : access granted");
		FVector ShipDockOffset = GetDockLocation();
		DockingInfo.EndPoint += DockingInfo.Rotation.RotateVector(ShipDockOffset * FVector(1, 0, 0)) - ShipDockOffset * FVector(0, 1, 1);
		DockingInfo.StartPoint = DockingInfo.EndPoint + 5000 * DockingInfo.Rotation.RotateVector(FVector(1, 0, 0));

		// Dock
		if (NavigateTo(DockingInfo.StartPoint))
		{
			// Align front to dock axis, ship top to station top, set speed
			PushCommandRotation((DockingInfo.EndPoint - DockingInfo.StartPoint), FVector(1, 0, 0));
			PushCommandRotation(FVector(0,0,1), FVector(0,0,1));

			// Move there
			PushCommandLocation(DockingInfo.EndPoint, true);
			PushCommandDock(DockingInfo);
			FLOG("AFlareSpacecraft::DockAt : navigation sent");
			return true;
		}
	}

	// Failed
	FLOG("AFlareSpacecraft::DockAt failed");
	return false;
}

void AFlareSpacecraft::ConfirmDock(IFlareSpacecraftInterface* DockStation, int32 DockId)
{
	FLOG("AFlareSpacecraft::ConfirmDock");
	ClearCurrentCommand();

	// Signal the PC
	AFlarePlayerController* PC = GetPC();
	if (PC && !ExternalCamera)
	{
		PC->SetExternalCamera(true);
	}

	// Set as docked
	DockStation->Dock(this, DockId);
	SetStatus(EFlareShipStatus::SS_Docked);
	ShipData.DockedTo = *DockStation->_getUObject()->GetName();
	ShipData.DockedAt = DockId;

	// Disable physics, reset speed
	LinearMaxVelocity = ShipDescription->LinearMaxVelocity;
	Airframe->SetSimulatePhysics(false);

	// Cut engines
	TArray<UActorComponent*> Engines = GetComponentsByClass(UFlareEngine::StaticClass());
	for (int32 EngineIndex = 0; EngineIndex < Engines.Num(); EngineIndex++)
	{
		UFlareEngine* Engine = Cast<UFlareEngine>(Engines[EngineIndex]);
		Engine->SetAlpha(0.0f);
	}

	// Reload and repair
	TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
	{
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);
		Component->Repair();

		UFlareWeapon* Weapon = Cast<UFlareWeapon>(Components[ComponentIndex]);
		if (Weapon)
		{
			Weapon->RefillAmmo();
		}
	}
	UpdatePower();
}

bool AFlareSpacecraft::Undock()
{
	FLOG("AFlareSpacecraft::Undock");
	FFlareShipCommandData Head;

	// Try undocking
	if (IsDocked())
	{
		// Enable physics
		Airframe->SetSimulatePhysics(true);

		// Evacuate
		GetDockStation()->ReleaseDock(this, ShipData.DockedAt);
		PushCommandLocation(RootComponent->GetComponentTransform().TransformPositionNoScale(5000 * FVector(-1, 0, 0)));

		// Update data
		SetStatus(EFlareShipStatus::SS_AutoPilot);
		ShipData.DockedTo = NAME_None;
		ShipData.DockedAt = -1;

		FLOG("AFlareSpacecraft::Undock successful");
		return true;
	}

	// Failed
	FLOG("AFlareSpacecraft::Undock failed");
	return false;
}

IFlareSpacecraftInterface* AFlareSpacecraft::GetDockStation()
{
	if (IsDocked())
	{
		for (TActorIterator<AActor> ActorItr(GetWorld()); ActorItr; ++ActorItr)
		{
			AFlareSpacecraft* Station = Cast<AFlareSpacecraft>(*ActorItr);
			if (Station && *Station->GetName() == ShipData.DockedTo)
			{
				return Station;
			}
		}
	}
	return NULL;
}

FFlareDockingInfo AFlareSpacecraft::RequestDock(IFlareSpacecraftInterface* Ship)
{
	FLOGV("AFlareSpacecraft::RequestDock ('%s')", *Ship->_getUObject()->GetName());

	// Looking for slot
	/*for (int32 i = 0; i < DockingSlots.Num(); i++)
	{
		if (!DockingSlots[i].Granted)
		{
			FLOGV("AFlareSpacecraft::RequestDock : found valid dock %d", i);
			DockingSlots[i].Granted = true;
			DockingSlots[i].Ship = Ship;
			return DockingSlots[i];
		}
	}*/
	// TODO Fix

	// Default values
	FFlareDockingInfo Info;
	Info.Granted = false;
	Info.Station = this;
	return Info;
}

void AFlareSpacecraft::ReleaseDock(IFlareSpacecraftInterface* Ship, int32 DockId)
{
	FLOGV("AFlareSpacecraft::ReleaseDock %d ('%s')", DockId, *Ship->_getUObject()->GetName());
	/*DockingSlots[DockId].Granted = false;
	DockingSlots[DockId].Occupied = false;
	DockingSlots[DockId].Ship = NULL;*/
	// TODO Fix
}

void AFlareSpacecraft::Dock(IFlareSpacecraftInterface* Ship, int32 DockId)
{
	FLOGV("AFlareSpacecraft::Dock %d ('%s')", DockId, *Ship->_getUObject()->GetName());
	/*DockingSlots[DockId].Granted = true;
	DockingSlots[DockId].Occupied = true;
	DockingSlots[DockId].Ship = Ship;*/
	// TODO Fix
}

TArray<IFlareSpacecraftInterface*> AFlareSpacecraft::GetDockedShips()
{
	TArray<IFlareSpacecraftInterface*> Result;

	/*for (int32 i = 0; i < DockingSlots.Num(); i++)
	{
		if (DockingSlots[i].Granted)
		{
			FLOGV("AFlareSpacecraft::GetDockedShips : found valid dock %d", i);
			Result.AddUnique(DockingSlots[i].Ship);
		}
	}*/
	//TODO Externalize

	return Result;
}

bool AFlareSpacecraft::HasAvailableDock(IFlareSpacecraftInterface* Ship) const
{
	// Looking for slot
	/*for (int32 i = 0; i < DockingSlots.Num(); i++)
	{
		if (!DockingSlots[i].Granted)
		{
			return true;
		}
	}*/

	//TODO Externalize

	return false;
}

/*----------------------------------------------------
	Navigation commands and helpers
----------------------------------------------------*/

void AFlareSpacecraft::PushCommandLinearBrake()
{
	FFlareShipCommandData Data;
	Data.Type = EFlareCommandDataType::CDT_Location;
	PushCommand(Data);
}

void AFlareSpacecraft::PushCommandAngularBrake()
{
	FFlareShipCommandData Data;
	Data.Type = EFlareCommandDataType::CDT_BrakeRotation;
	PushCommand(Data);
}

void AFlareSpacecraft::PushCommandLocation(const FVector& Location, bool Precise)
{
	FFlareShipCommandData Data;
	Data.Type = EFlareCommandDataType::CDT_Location;
	Data.LocationTarget = Location;
	Data.PreciseApproach = Precise;
	PushCommand(Data);
}

void AFlareSpacecraft::PushCommandRotation(const FVector& RotationTarget, const FVector& LocalShipAxis)
{
	FFlareShipCommandData Data;
	Data.Type = EFlareCommandDataType::CDT_Rotation;
	Data.RotationTarget = RotationTarget;
	Data.LocalShipAxis = LocalShipAxis;
	FLOGV("PushCommandRotation RotationTarget '%s'", *RotationTarget.ToString());
	FLOGV("PushCommandRotation LocalShipAxis '%s'", *LocalShipAxis.ToString());
	PushCommand(Data);
}

void AFlareSpacecraft::PushCommandDock(const FFlareDockingInfo& DockingInfo)
{
	FFlareShipCommandData Data;
	Data.Type = EFlareCommandDataType::CDT_Dock;
	Data.ActionTarget = Cast<AFlareSpacecraft>(DockingInfo.Station);
	Data.ActionTargetParam = DockingInfo.DockId;
	PushCommand(Data);
}

void AFlareSpacecraft::PushCommand(const FFlareShipCommandData& Command)
{
	SetStatus(EFlareShipStatus::SS_AutoPilot);
	CommandData.Enqueue(Command);

	FLOGV("Pushed command '%s'", *EFlareCommandDataType::ToString(Command.Type));
}

void AFlareSpacecraft::ClearCurrentCommand()
{
	FFlareShipCommandData Command;
	CommandData.Dequeue(Command);

	FLOGV("Cleared command '%s'", *EFlareCommandDataType::ToString(Command.Type));

	if (!CommandData.Peek(Command))
	{
		SetStatus(EFlareShipStatus::SS_Manual);
	}
}

void AFlareSpacecraft::AbortAllCommands()
{
	FFlareShipCommandData Command;

	while (CommandData.Dequeue(Command))
	{
		FLOGV("Abort command '%s'", *EFlareCommandDataType::ToString(Command.Type));
		if (Command.Type == EFlareCommandDataType::CDT_Dock)
		{
			// Release dock grant
			IFlareSpacecraftInterface* Station = Cast<IFlareSpacecraftInterface>(Command.ActionTarget);
			Station->ReleaseDock(this, Command.ActionTargetParam);
		}
	}
	SetStatus(EFlareShipStatus::SS_Manual);
}

FVector AFlareSpacecraft::GetDockLocation()
{
	FVector WorldLocation = RootComponent->GetSocketLocation(FName("Dock"));
	return RootComponent->GetComponentTransform().InverseTransformPosition(WorldLocation);
}

bool AFlareSpacecraft::ComputePath(TArray<FVector>& Path, TArray<AActor*>& PossibleColliders, FVector OriginLocation, FVector TargetLocation, float ShipSize)
{
	// Travel information
	float TravelLength;
	FVector TravelDirection;
	FVector Travel = TargetLocation - OriginLocation;
	Travel.ToDirectionAndLength(TravelDirection, TravelLength);

	for (int32 i = 0; i < PossibleColliders.Num(); i++)
	{
		// Get collider info
		FVector ColliderLocation;
		FVector ColliderExtent;
		PossibleColliders[i]->GetActorBounds(true, ColliderLocation, ColliderExtent);
		float ColliderSize = ShipSize + ColliderExtent.Size();

		// Colliding : split the travel
		if (FMath::LineSphereIntersection(OriginLocation, TravelDirection, TravelLength, ColliderLocation, ColliderSize))
		{
			//DrawDebugSphere(GetWorld(), ColliderLocation, ColliderSize, 12, FColor::Blue, true);

			// Get an orthogonal plane
			FPlane TravelOrthoPlane = FPlane(ColliderLocation, TargetLocation - ColliderLocation);
			FVector IntersectedLocation = FMath::LinePlaneIntersection(OriginLocation, TargetLocation, TravelOrthoPlane);

			// Relocate intersection inside the sphere
			FVector Intersector = IntersectedLocation - ColliderLocation;
			Intersector.Normalize();
			IntersectedLocation = ColliderLocation + Intersector * ColliderSize;

			// Collisions
			bool IsColliding = IsPointColliding(IntersectedLocation, PossibleColliders[i]);
			//DrawDebugPoint(GetWorld(), IntersectedLocation, 8, IsColliding ? FColor::Red : FColor::Green, true);

			// Dead end, go back
			if (IsColliding)
			{
				return false;
			}

			// Split travel
			else
			{
				Path.Add(IntersectedLocation);
				PossibleColliders.RemoveAt(i, 1);
				bool FirstPartOK = ComputePath(Path, PossibleColliders, OriginLocation, IntersectedLocation, ShipSize);
				bool SecondPartOK = ComputePath(Path, PossibleColliders, IntersectedLocation, TargetLocation, ShipSize);
				return FirstPartOK && SecondPartOK;
			}

		}
	}

	// No collision found
	return true;
}

void AFlareSpacecraft::UpdateColliders()
{
	PathColliders.Empty();
	for (TActorIterator<AActor> ActorItr(GetWorld()); ActorItr; ++ActorItr)
	{
		FVector Unused;
		FVector ColliderExtent;
		ActorItr->GetActorBounds(true, Unused, ColliderExtent);

		if (ColliderExtent.Size() < 100000 && ActorItr->IsRootComponentMovable())
		{
			PathColliders.Add(*ActorItr);
		}
	}
}

bool AFlareSpacecraft::IsPointColliding(FVector Candidate, AActor* Ignore)
{
	for (int32 i = 0; i < PathColliders.Num(); i++)
	{
		FVector ColliderLocation;
		FVector ColliderExtent;
		PathColliders[i]->GetActorBounds(true, ColliderLocation, ColliderExtent);

		if ((Candidate - ColliderLocation).Size() < ColliderExtent.Size() && PathColliders[i] != Ignore)
		{
			return true;
		}
	}

	return false;
}


/*----------------------------------------------------
	Damage status
----------------------------------------------------*/

void AFlareSpacecraft::ApplyDamage(float Energy, float Radius, FVector Location)
{
	// The damages are applied to all component touching the sphere defined by the radius and the
	// location in parameter.
	// The maximum damage are applied to a component only if its bounding sphere touch the center of
	// the damage sphere. There is a linear decrease of damage with a minumum of 0 if the 2 sphere
	// only touch.

	//FLOGV("Apply %f damages to %s with radius %f at %s", Energy, *GetHumanReadableName(), Radius, *Location.ToString());
	//DrawDebugSphere(GetWorld(), Location, Radius * 100, 12, FColor::Red, true);

	bool IsAliveBeforeDamage = IsAlive();

	TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
	{
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);

		float ComponentSize;
		FVector ComponentLocation;
		Component->GetBoundingSphere(ComponentLocation, ComponentSize);

		float Distance = (ComponentLocation - Location).Size() / 100.0f;
		float IntersectDistance =  Radius + ComponentSize/100 - Distance;

		// Hit this component
		if (IntersectDistance > 0)
		{
			//FLOGV("Component %s. ComponentSize=%f, Distance=%f, IntersectDistance=%f", *(Component->GetReadableName()), ComponentSize, Distance, IntersectDistance);
			float Efficiency = FMath::Clamp(IntersectDistance / Radius , 0.0f, 1.0f);
			Component->ApplyDamage(Energy * Efficiency);
		}
	}

	// Update power
	UpdatePower();

	// Heat the ship
	ShipData.Heat += Energy;
}

bool AFlareSpacecraft::IsAlive()
{
	return GetSubsystemHealth(EFlareSubsystem::SYS_LifeSupport) > 0;
}

bool AFlareSpacecraft::IsPowered()
{
	if (ShipCockit)
	{
		return ShipCockit->IsPowered();
	}
	else
	{
		return false;
	}
}

bool AFlareSpacecraft::HasPowerOutage()
{
	return GetPowerOutageDuration() > 0.f;
}

float AFlareSpacecraft::GetPowerOutageDuration()
{
	return ShipData.PowerOutageDelay;
}

void AFlareSpacecraft::OnElectricDamage(float DamageRatio)
{
	float MaxPower = 0.f;
	float AvailablePower = 0.f;

	TArray<UActorComponent*> Components = GetComponentsByClass(UFlareSpacecraftComponent::StaticClass());
	float Total = 0.f;
	float GeneratorCount = 0;
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ComponentIndex++)
	{
		UFlareSpacecraftComponent* Component = Cast<UFlareSpacecraftComponent>(Components[ComponentIndex]);
		MaxPower += Component->GetMaxGeneratedPower();
		AvailablePower += Component->GetGeneratedPower();
	}

	float PowerRatio = AvailablePower/MaxPower;


	//FLOGV("OnElectricDamage initial PowerOutageDelay=%f, DamageRatio=%f, PowerRatio=%f", ShipData.PowerOutageDelay, DamageRatio, PowerRatio);

	// The outage probability depend on global available power ratio
	if (FMath::FRand() > PowerRatio)
	{
		// The outage duration depend on the relative amount of damage the component just receive
		// This avoid very long outage if multiple small collision.
		// Between 10 and 20s of outage if component one shot
		ShipData.PowerOutageDelay += DamageRatio *  FMath::FRandRange(10, 20 * (1.f - PowerRatio));
		UpdatePower();
	}



}

/*----------------------------------------------------
		Pilot
----------------------------------------------------*/

void AFlareSpacecraft::EnablePilot(bool PilotEnabled)
{
	FLOGV("EnablePilot %d", PilotEnabled);
	IsPiloted = PilotEnabled;
	ManualOrbitalBoost = false;
}

/*----------------------------------------------------
	Attitude control : linear version
----------------------------------------------------*/

void AFlareSpacecraft::UpdateLinearAttitudeManual(float DeltaSeconds)
{
	// Manual orbital boost
	if (ManualOrbitalBoost)
	{
		ManualLinearVelocity = GetLinearMaxBoostingVelocity() * FVector(1, 0, 0);
	}

	// Add velocity command to current velocity
	LinearTargetVelocity = GetLinearVelocity() + Airframe->GetComponentToWorld().GetRotation().RotateVector(ManualLinearVelocity);
}

void AFlareSpacecraft::UpdateLinearAttitudeAuto(float DeltaSeconds, float MaxVelocity)
{
	// Location data
	FFlareShipCommandData Data;
	CommandData.Peek(Data);

	TArray<UActorComponent*> Engines = GetComponentsByClass(UFlareEngine::StaticClass());

	FVector DeltaPosition = (Data.LocationTarget - GetActorLocation()) / 100; // Distance in meters
	FVector DeltaPositionDirection = DeltaPosition;
	DeltaPositionDirection.Normalize();
	float Distance = FMath::Max(0.0f, DeltaPosition.Size() - LinearDeadDistance);

	FVector DeltaVelocity = -GetLinearVelocity();
	FVector DeltaVelocityAxis = DeltaVelocity;
	DeltaVelocityAxis.Normalize();

	float TimeToFinalVelocity;

	if (FMath::IsNearlyZero(DeltaVelocity.SizeSquared()))
	{
		TimeToFinalVelocity = 0;
	}
	else
	{

		FVector Acceleration = GetTotalMaxThrustInAxis(Engines, DeltaVelocityAxis, false) / Airframe->GetMass();
		float AccelerationInAngleAxis =  FMath::Abs(FVector::DotProduct(Acceleration, DeltaPositionDirection));

		TimeToFinalVelocity = (DeltaVelocity.Size() / AccelerationInAngleAxis);
	}

	float DistanceToStop = (DeltaVelocity.Size() / 2) * (TimeToFinalVelocity + DeltaSeconds);

	FVector RelativeResultSpeed;

	if (DistanceToStop > Distance)
	{
		RelativeResultSpeed = FVector::ZeroVector;
	}
	else
	{

		float MaxPreciseSpeed = FMath::Min((Distance - DistanceToStop) / DeltaSeconds, MaxVelocity);

		RelativeResultSpeed = DeltaPositionDirection;
		RelativeResultSpeed *= MaxPreciseSpeed;
	}

	// Under this distance we consider the variation negligible, and ensure null delta + null speed
	if (Distance < LinearDeadDistance && DeltaVelocity.Size() < NegligibleSpeedRatio * MaxVelocity)
	{
		Airframe->SetPhysicsLinearVelocity(FVector::ZeroVector, false); // TODO remove
		ClearCurrentCommand();
		RelativeResultSpeed = FVector::ZeroVector;

	}
	LinearTargetVelocity = RelativeResultSpeed;
}

void AFlareSpacecraft::UpdateLinearBraking(float DeltaSeconds)
{
	LinearTargetVelocity = FVector::ZeroVector;
	FVector LinearVelocity = WorldToLocal(Airframe->GetPhysicsLinearVelocity());

	// Null speed detection
	if (LinearVelocity.Size() < NegligibleSpeedRatio * LinearMaxVelocity)
	{
		Airframe->SetAllPhysicsLinearVelocity(FVector::ZeroVector);
		ClearCurrentCommand();
	}
}


/*----------------------------------------------------
	Attitude control : angular version
----------------------------------------------------*/

void AFlareSpacecraft::UpdateAngularAttitudeManual(float DeltaSeconds)
{
	AngularTargetVelocity = Airframe->GetComponentToWorld().GetRotation().RotateVector(ManualAngularVelocity);
}

void AFlareSpacecraft::UpdateAngularAttitudeAuto(float DeltaSeconds)
{
	TArray<UActorComponent*> Engines = GetComponentsByClass(UFlareEngine::StaticClass());

	// Rotation data
	FFlareShipCommandData Data;
	CommandData.Peek(Data);
	FVector TargetAxis = Data.RotationTarget;
	FVector LocalShipAxis = Data.LocalShipAxis;

	FVector AngularVelocity = Airframe->GetPhysicsAngularVelocity();
	FVector WorldShipAxis = Airframe->GetComponentToWorld().GetRotation().RotateVector(LocalShipAxis);

	WorldShipAxis.Normalize();
	TargetAxis.Normalize();

	FVector RotationDirection = FVector::CrossProduct(WorldShipAxis, TargetAxis);
	RotationDirection.Normalize();
	float Dot = FVector::DotProduct(WorldShipAxis, TargetAxis);
	float angle = FMath::RadiansToDegrees(FMath::Acos(Dot));

	FVector DeltaVelocity = -AngularVelocity;
	FVector DeltaVelocityAxis = DeltaVelocity;
	DeltaVelocityAxis.Normalize();

	float TimeToFinalVelocity;

	if (FMath::IsNearlyZero(DeltaVelocity.SizeSquared()))
	{
		TimeToFinalVelocity = 0;
	}
	else {
	    FVector SimpleAcceleration = DeltaVelocityAxis * AngularAccelerationRate;
	    // Scale with damages
	    float DamageRatio = GetTotalMaxTorqueInAxis(Engines, DeltaVelocityAxis, true) / GetTotalMaxTorqueInAxis(Engines, DeltaVelocityAxis, false);
	    FVector DamagedSimpleAcceleration = SimpleAcceleration * DamageRatio;

	    FVector Acceleration = DamagedSimpleAcceleration;
	    float AccelerationInAngleAxis =  FMath::Abs(FVector::DotProduct(DamagedSimpleAcceleration, RotationDirection));

	    TimeToFinalVelocity = (DeltaVelocity.Size() / AccelerationInAngleAxis);
	}

	float AngleToStop = (DeltaVelocity.Size() / 2) * (TimeToFinalVelocity + DeltaSeconds);

	FVector RelativeResultSpeed;

	if (AngleToStop > angle) {
		RelativeResultSpeed = FVector::ZeroVector;
	}
	else {

		float MaxPreciseSpeed = FMath::Min((angle - AngleToStop) / DeltaSeconds, AngularMaxVelocity);

		RelativeResultSpeed = RotationDirection;
		RelativeResultSpeed *= MaxPreciseSpeed;
	}

	// Under this angle we consider the variation negligible, and ensure null delta + null speed
	if (angle < AngularDeadAngle && DeltaVelocity.Size() < AngularDeadAngle)
	{
		Airframe->SetPhysicsAngularVelocity(FVector::ZeroVector, false); // TODO remove
		ClearCurrentCommand();
		RelativeResultSpeed = FVector::ZeroVector;
	}
	AngularTargetVelocity = RelativeResultSpeed;
}

void AFlareSpacecraft::UpdateAngularBraking(float DeltaSeconds)
{
	AngularTargetVelocity = FVector::ZeroVector;
	FVector AngularVelocity = Airframe->GetPhysicsAngularVelocity();
	// Null speed detection
	if (AngularVelocity.Size() < NegligibleSpeedRatio * AngularMaxVelocity)
	{
		AngularTargetVelocity = FVector::ZeroVector;
		Airframe->SetPhysicsAngularVelocity(FVector::ZeroVector, false); // TODO remove
		ClearCurrentCommand();
	}
}

/*----------------------------------------------------
	Customization
----------------------------------------------------*/

void AFlareSpacecraft::SetShipDescription(FFlareSpacecraftDescription* Description)
{
	ShipDescription = Description;

	// Load data from the ship info
	if (Description)
	{
		LinearMaxVelocity = Description->LinearMaxVelocity;
		AngularMaxVelocity = Description->AngularMaxVelocity;
	}
}

void AFlareSpacecraft::SetOrbitalEngineDescription(FFlareSpacecraftComponentDescription* Description)
{
	OrbitalEngineDescription = Description;
}

void AFlareSpacecraft::SetRCSDescription(FFlareSpacecraftComponentDescription* Description)
{
	RCSDescription = Description;

	// Find the RCS turn and power rating, since RCSs themselves don't do anything
	if (Description)
	{
		if (Airframe && Description->EngineCharacteristics.AngularAccelerationRate > 0)
		{
			float Mass = Airframe->GetMass() / 100000;
			AngularAccelerationRate = Description->EngineCharacteristics.AngularAccelerationRate / (60 * Mass);
		}
	}
}

void AFlareSpacecraft::UpdateCustomization()
{
	Super::UpdateCustomization();

	Airframe->UpdateCustomization();
}

void AFlareSpacecraft::StartPresentation()
{
	Super::StartPresentation();

	if (Airframe)
	{
		Airframe->SetSimulatePhysics(false);
	}
}


/*----------------------------------------------------
	Physics
----------------------------------------------------*/

void AFlareSpacecraft::PhysicSubTick(float DeltaSeconds)
{
	TArray<UActorComponent*> Engines = GetComponentsByClass(UFlareEngine::StaticClass());
	if (IsPowered())
	{
		// Clamp speed
		float MaxVelocity = LinearMaxVelocity;
		if (ManualOrbitalBoost)
		{
			FVector FrontDirection = Airframe->GetComponentToWorld().GetRotation().RotateVector(FVector(1,0,0));
			MaxVelocity = FVector::DotProduct(LinearTargetVelocity.GetUnsafeNormal(), FrontDirection) * GetLinearMaxBoostingVelocity();
		}
		LinearTargetVelocity = LinearTargetVelocity.GetClampedToMaxSize(MaxVelocity);

		// Linear physics
		FVector DeltaV = LinearTargetVelocity - GetLinearVelocity();
		FVector DeltaVAxis = DeltaV;
		DeltaVAxis.Normalize();

		if (!DeltaV.IsNearlyZero())
		{
			FVector Acceleration = DeltaVAxis * GetTotalMaxThrustInAxis(Engines, -DeltaVAxis, ManualOrbitalBoost).Size() / Airframe->GetMass();
			FVector ClampedAcceleration = Acceleration.GetClampedToMaxSize(DeltaV.Size() / DeltaSeconds);

			Airframe->SetPhysicsLinearVelocity(ClampedAcceleration * DeltaSeconds * 100, true); // Multiply by 100 because UE4 works in cm
		}

		// Angular physics
		FVector DeltaAngularV = AngularTargetVelocity - Airframe->GetPhysicsAngularVelocity();
		FVector DeltaAngularVAxis = DeltaAngularV;
		DeltaAngularVAxis.Normalize();

		if (!DeltaAngularV.IsNearlyZero())
		{
			FVector SimpleAcceleration = DeltaAngularVAxis * AngularAccelerationRate;

			// Scale with damages
			float DamageRatio = GetTotalMaxTorqueInAxis(Engines, DeltaAngularVAxis, true) / GetTotalMaxTorqueInAxis(Engines, DeltaAngularVAxis, false);
			FVector DamagedSimpleAcceleration = SimpleAcceleration * DamageRatio;
			FVector ClampedSimplifiedAcceleration = DamagedSimpleAcceleration.GetClampedToMaxSize(DeltaAngularV.Size() / DeltaSeconds);

			Airframe->SetPhysicsAngularVelocity(ClampedSimplifiedAcceleration  * DeltaSeconds, true);
		}

		// Update engine alpha
		for (int32 EngineIndex = 0; EngineIndex < Engines.Num(); EngineIndex++)
		{
			UFlareEngine* Engine = Cast<UFlareEngine>(Engines[EngineIndex]);
			FVector ThrustAxis = Engine->GetThrustAxis();
			float LinearAlpha = 0;
			float AngularAlpha = 0;

			if (IsPresentationMode()) {
				LinearAlpha = true;
			} else if (!DeltaV.IsNearlyZero()) {
				if (!(!ManualOrbitalBoost && Engine->IsA(UFlareOrbitalEngine::StaticClass()))) {
					LinearAlpha = -FVector::DotProduct(ThrustAxis, DeltaVAxis);
				}
			}

			FVector EngineOffset = (Engine->GetComponentLocation() - COM) / 100;
			FVector TorqueDirection = FVector::CrossProduct(EngineOffset, ThrustAxis);
			TorqueDirection.Normalize();

			if (!DeltaAngularV.IsNearlyZero() && !Engine->IsA(UFlareOrbitalEngine::StaticClass())) {
				AngularAlpha = -FVector::DotProduct(TorqueDirection, DeltaAngularVAxis);
			}

			Engine->SetAlpha(FMath::Clamp(LinearAlpha + AngularAlpha, 0.0f, 1.0f));
		}
	}
	else
	{
		// Shutdown engines
		for (int32 EngineIndex = 0; EngineIndex < Engines.Num(); EngineIndex++)
		{
			UFlareEngine* Engine = Cast<UFlareEngine>(Engines[EngineIndex]);
			Engine->SetAlpha(0);
		}
	}
}

void AFlareSpacecraft::UpdateCOM()
{
    COM = Airframe->GetBodyInstance()->GetCOMPosition();
}

/*----------------------------------------------------
		Damage system
----------------------------------------------------*/

void AFlareSpacecraft::OnControlLost()
{
	AFlarePlayerController* PC = GetPC();
	if (PC)
	{
		PC->Notify(LOCTEXT("ShipDestroyed", "Your ship has been destroyed !"), EFlareNotification::NT_Military, EFlareMenu::MENU_Company);
	}
}

void AFlareSpacecraft::OnEnemyKilled(IFlareSpacecraftInterface* Enemy)
{
	AFlarePlayerController* PC = GetPC();
	if (PC)
	{
		PC->Notify(LOCTEXT("ShipKilled", "Target destroyed"), EFlareNotification::NT_Military);
	}
}


/*----------------------------------------------------
	Input
----------------------------------------------------*/

void AFlareSpacecraft::SetupPlayerInputComponent(class UInputComponent* InputComponent)
{
	check(InputComponent);

	InputComponent->BindAxis("Thrust", this, &AFlareSpacecraft::ThrustInput);
	InputComponent->BindAxis("MoveVerticalInput", this, &AFlareSpacecraft::MoveVerticalInput);
	InputComponent->BindAxis("MoveHorizontalInput", this, &AFlareSpacecraft::MoveHorizontalInput);

	InputComponent->BindAxis("RollInput", this, &AFlareSpacecraft::RollInput);
	InputComponent->BindAxis("PitchInput", this, &AFlareSpacecraft::PitchInput);
	InputComponent->BindAxis("YawInput", this, &AFlareSpacecraft::YawInput);
	InputComponent->BindAxis("MouseInputY", this, &AFlareSpacecraft::PitchInput);
	InputComponent->BindAxis("MouseInputX", this, &AFlareSpacecraft::YawInput);

	InputComponent->BindAction("ZoomIn", EInputEvent::IE_Released, this, &AFlareSpacecraft::ZoomIn);
	InputComponent->BindAction("ZoomOut", EInputEvent::IE_Released, this, &AFlareSpacecraft::ZoomOut);

	InputComponent->BindAction("FaceForward", EInputEvent::IE_Released, this, &AFlareSpacecraft::FaceForward);
	InputComponent->BindAction("FaceBackward", EInputEvent::IE_Released, this, &AFlareSpacecraft::FaceBackward);
	InputComponent->BindAction("Brake", EInputEvent::IE_Released, this, &AFlareSpacecraft::Brake);
	InputComponent->BindAction("Boost", EInputEvent::IE_Pressed, this, &AFlareSpacecraft::BoostOn);
	InputComponent->BindAction("Boost", EInputEvent::IE_Released, this, &AFlareSpacecraft::BoostOff);
	InputComponent->BindAction("Manual", EInputEvent::IE_Released, this, &AFlareSpacecraft::ForceManual);

	InputComponent->BindAction("Fire", EInputEvent::IE_Pressed, this, &AFlareSpacecraft::FirePress);
	InputComponent->BindAction("Fire", EInputEvent::IE_Released, this, &AFlareSpacecraft::FireRelease);
}

void AFlareSpacecraft::FirePress()
{
	FiringPressed = true;

	if (CombatMode)
	{
		StartFire();
	}
}

void AFlareSpacecraft::FireRelease()
{
	FiringPressed = false;

	if (CombatMode)
	{
		StopFire();
	}
}

void AFlareSpacecraft::MousePositionInput(FVector2D Val)
{
	if (!ExternalCamera)
	{
		if (FiringPressed  || CombatMode)
		{
			float DistanceToCenter = FMath::Sqrt(FMath::Square(Val.X) + FMath::Square(Val.Y));

			// Compensation curve = 1 + (input-1)/(1-AngularInputDeadRatio)
			float CompensatedDistance = FMath::Clamp(1. + (DistanceToCenter - 1. ) / (1. - AngularInputDeadRatio) , 0., 1.);
			float Angle = FMath::Atan2(Val.Y, Val.X);

			ManualAngularVelocity.Z = CompensatedDistance * FMath::Cos(Angle) * AngularMaxVelocity;
			ManualAngularVelocity.Y = CompensatedDistance * FMath::Sin(Angle) * AngularMaxVelocity;
		}
		else
		{
			ManualAngularVelocity.Z = 0;
			ManualAngularVelocity.Y = 0;
		}
	}
}

void AFlareSpacecraft::ThrustInput(float Val)
{
	if (!ExternalCamera)
	{
		ManualLinearVelocity.X = Val * LinearMaxVelocity;
	}
}

void AFlareSpacecraft::MoveVerticalInput(float Val)
{
	if (!ExternalCamera)
	{
		ManualLinearVelocity.Z = LinearMaxVelocity * Val;
	}
}

void AFlareSpacecraft::MoveHorizontalInput(float Val)
{
	if (!ExternalCamera)
	{
		ManualLinearVelocity.Y = LinearMaxVelocity * Val;
	}
}

void AFlareSpacecraft::RollInput(float Val)
{
	if (!ExternalCamera)
	{
		ManualAngularVelocity.X = - Val * AngularMaxVelocity;
	}
}

void AFlareSpacecraft::PitchInput(float Val)
{
	if (ExternalCamera)
	{
		FRotator CurrentRot = WorldToLocal(CameraContainerPitch->GetComponentRotation().Quaternion()).Rotator();
		SetCameraPitch(CurrentRot.Pitch + Val * CameraPanSpeed);
	}
	else if (CombatMode)
	{
		MouseOffset.Y -= FMath::Sign(Val) * FMath::Pow(FMath::Abs(Val),1.3) * 0.05; // TODO Config sensibility
		if(MouseOffset.Size() > 1)
		{
			MouseOffset /= MouseOffset.Size();
		}
		MousePositionInput(MouseOffset);

	}
}

void AFlareSpacecraft::YawInput(float Val)
{
	if (ExternalCamera)
	{
		FRotator CurrentRot = WorldToLocal(CameraContainerPitch->GetComponentRotation().Quaternion()).Rotator();
		SetCameraYaw(CurrentRot.Yaw + Val * CameraPanSpeed);
	}
	else if (CombatMode)
	{
		MouseOffset.X += FMath::Sign(Val) * FMath::Pow(FMath::Abs(Val),1.3) * 0.05; // TODO Config sensibility
		if(MouseOffset.Size() > 1)
		{
			MouseOffset /= MouseOffset.Size();
		}
		MousePositionInput(MouseOffset);
	}
}

void AFlareSpacecraft::ZoomIn()
{
	if (ExternalCamera)
	{
		StepCameraDistance(true);
	}
}

void AFlareSpacecraft::ZoomOut()
{
	if (ExternalCamera)
	{
		StepCameraDistance(false);
	}
}

void AFlareSpacecraft::FaceForward()
{
	if (IsManualPilot())
	{
		PushCommandRotation(Airframe->GetPhysicsLinearVelocity(), FVector(1,0,0));
	}
}

void AFlareSpacecraft::FaceBackward()
{
	if (IsManualPilot())
	{
		PushCommandRotation((-Airframe->GetPhysicsLinearVelocity()), FVector(1, 0, 0));
	}
}

void AFlareSpacecraft::Brake()
{
	if (IsManualPilot())
	{
		// TODO
	}
}

void AFlareSpacecraft::BoostOn()
{
	if (IsManualPilot() && !ExternalCamera)
	{
		ManualOrbitalBoost = true;
	}
}

void AFlareSpacecraft::BoostOff()
{
	ManualOrbitalBoost = false;
}

void AFlareSpacecraft::ForceManual()
{
	if (Status != EFlareShipStatus::SS_Docked)
	{
		AbortAllCommands();
	}
}

void AFlareSpacecraft::StartFire()
{
	if (IsAlive() && (IsPiloted || !ExternalCamera))
	{
		for (int32 i = 0; i < WeaponList.Num(); i++)
		{
			WeaponList[i]->StartFire();
		}
	}
}

void AFlareSpacecraft::StopFire()
{
	if (IsAlive() && (IsPiloted || !ExternalCamera))
	{
		for (int32 i = 0; i < WeaponList.Num(); i++)
		{
			WeaponList[i]->StopFire();
		}
	}
}


/*----------------------------------------------------
		Getters (Attitude)
----------------------------------------------------*/

FVector AFlareSpacecraft::GetLinearVelocity() const
{
	return Airframe->GetPhysicsLinearVelocity() / 100;
}

FVector AFlareSpacecraft::GetTotalMaxThrustInAxis(TArray<UActorComponent*>& Engines, FVector Axis, bool WithOrbitalEngines) const
{
	Axis.Normalize();
	FVector TotalMaxThrust = FVector::ZeroVector;
	for (int32 i = 0; i < Engines.Num(); i++)
	{
		UFlareEngine* Engine = Cast<UFlareEngine>(Engines[i]);

		if (Engine->IsA(UFlareOrbitalEngine::StaticClass()) && !WithOrbitalEngines)
		{
			continue;
		}

		FVector WorldThrustAxis = Engine->GetThrustAxis();

		float Ratio = FVector::DotProduct(WorldThrustAxis, Axis);
		if (Ratio > 0)
		{
			TotalMaxThrust += WorldThrustAxis * Engine->GetMaxThrust() * Ratio;
		}
	}

	return TotalMaxThrust;
}

float AFlareSpacecraft::GetTotalMaxTorqueInAxis(TArray<UActorComponent*>& Engines, FVector TorqueAxis, bool WithDamages) const
{
	TorqueAxis.Normalize();
	float TotalMaxTorque = 0;

	for (int32 i = 0; i < Engines.Num(); i++) {
		UFlareEngine* Engine = Cast<UFlareEngine>(Engines[i]);

		// Ignore orbital engines for torque computation
		if (Engine->IsA(UFlareOrbitalEngine::StaticClass())) {
		  continue;
		}

		float MaxThrust = (WithDamages ? Engine->GetMaxThrust() : Engine->GetInitialMaxThrust());

		if (MaxThrust == 0)
		{
			// Not controlable engine
			continue;
		}

		FVector EngineOffset = (Engine->GetComponentLocation() - COM) / 100;

		FVector WorldThrustAxis = Engine->GetThrustAxis();
		WorldThrustAxis.Normalize();
		FVector TorqueDirection = FVector::CrossProduct(EngineOffset, WorldThrustAxis);
		TorqueDirection.Normalize();

		float ratio = FVector::DotProduct(TorqueAxis, TorqueDirection);

		if (ratio > 0) {
			TotalMaxTorque += FVector::CrossProduct(EngineOffset, WorldThrustAxis).Size() * MaxThrust * ratio;
		}

	}

	return TotalMaxTorque;
}


#undef LOCTEXT_NAMESPACE