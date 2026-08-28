#pragma once

#include <cstdint>

namespace oxymp::client::game::natives {

/// Хеши нативных функций для сборки игры 1.0.3889.0.
///
/// Rockstar перетасовывает хеши нативов между сборками: из шести с лишним тысяч
/// канонических значений в этой сборке уцелело 66. Поэтому здесь лежат хеши
/// именно этой сборки, а не канонические, — и рядом с каждым указано
/// каноническое значение, по которому натив можно опознать в открытой базе.
///
/// Файл порождён и выверен: каждый хеш найден в таблице регистрации запущенной
/// игры. Значения, которых там нет, сюда не попадают — неверный хеш обернулся бы
/// вылетом в момент вызова, а не строчкой в журнале.
///
/// Соответствие взято из таблицы CrossMapping_Universal.h проекта CitizenFX
/// (FiveM, https://github.com/citizenfx/fivem), столбец 27 — для сборок 2944 и новее.
/// Имена и канонические хеши — оттуда же, из открытой базы нативов.
///
/// Игра обновилась — значения нужно пересобрать: см. README.

/// ATTACH_VEHICLE_TO_TOW_TRUCK, канонический хеш 0X29A16F8D621C4508.
inline constexpr std::uint64_t kAttachVehicleToTowTruck = 0XA86C8A254D6B6F40;

/// DETACH_VEHICLE_FROM_TOW_TRUCK, канонический хеш 0XC2DB6B6708350ED8.
inline constexpr std::uint64_t kDetachVehicleFromTowTruck = 0XD4BC322888F79B7E;

/// GET_ENTITY_ATTACHED_TO_TOW_TRUCK, канонический хеш 0XEFEA18DCF10F8F75.
inline constexpr std::uint64_t kGetEntityAttachedToTowTruck = 0X314C37CF34534BCB;

/// IS_VEHICLE_ATTACHED_TO_TOW_TRUCK, канонический хеш 0X146DF9EC4C4B9FD4.
inline constexpr std::uint64_t kIsVehicleAttachedToTowTruck = 0X4A64AADF9B40D2AF;

/// TASK_THROW_PROJECTILE, канонический хеш 0X7285951DBF6B5A51.
inline constexpr std::uint64_t kTaskThrowProjectile = 0X1BFCFCC8F6D798A6;

/// GET_WEAPONTYPE_GROUP, канонический хеш 0XC3287EE3050FB74C.
inline constexpr std::uint64_t kGetWeapontypeGroup = 0X6CC7A2E68E8A565A;

/// GET_VEHICLE_TRAILER_VEHICLE, канонический хеш 0X1CDD6BADC297830D.
inline constexpr std::uint64_t kGetVehicleTrailerVehicle = 0X20974C28142EB370;

/// ATTACH_VEHICLE_TO_TRAILER, канонический хеш 0X3C7D42D58F770B54.
inline constexpr std::uint64_t kAttachVehicleToTrailer = 0XF89624E52FCBE454;

/// DETACH_VEHICLE_FROM_TRAILER, канонический хеш 0X90532EDF0D2BDD86.
inline constexpr std::uint64_t kDetachVehicleFromTrailer = 0X157D966854AABDA6;

/// IS_VEHICLE_ATTACHED_TO_TRAILER, канонический хеш 0XE7CF3C4F9F489F0C.
inline constexpr std::uint64_t kIsVehicleAttachedToTrailer = 0XA6D8AF5A058A75F0;

/// EXPLODE_VEHICLE, канонический хеш 0XBA71116ADF5B514C.
inline constexpr std::uint64_t kExplodeVehicle = 0XAE89444B15234CCE;

/// SET_ENTITY_PROOFS, канонический хеш 0XFAEE099C6F890BB8.
inline constexpr std::uint64_t kSetEntityProofs = 0XD0F1DB0E50B367AD;

/// IS_ENTITY_DEAD, канонический хеш 0X5F9532F3B5CC2551.
inline constexpr std::uint64_t kIsEntityDead = 0X1C2F771CDC87A3A5;

/// SHOOT_SINGLE_BULLET_BETWEEN_COORDS, канонический хеш 0X867654CBC7606F2C.
inline constexpr std::uint64_t kShootSingleBulletBetweenCoords = 0XA934E5D7EEE03C7E;

/// GET_PED_BONE_COORDS, канонический хеш 0X17C07FC640E86B4E.
inline constexpr std::uint64_t kGetPedBoneCoords = 0X83FDC027F0BEA202;

/// TASK_JUMP, канонический хеш 0X0AE4086104E067B1.
inline constexpr std::uint64_t kTaskJump = 0XC3EBEA7530D64F53;

/// TASK_CLIMB, канонический хеш 0X89D9FCC2435112F1.
inline constexpr std::uint64_t kTaskClimb = 0X345C12E1D643634F;

/// TASK_RELOAD_WEAPON, канонический хеш 0X62D2916F56B9CD2D.
inline constexpr std::uint64_t kTaskReloadWeapon = 0X550B03C7904C63CD;

/// TASK_STAY_IN_COVER, канонический хеш 0XE5DA8615A6180789.
inline constexpr std::uint64_t kTaskStayInCover = 0X115CA3B4F950226D;

/// TASK_DRIVE_BY, канонический хеш 0X2F8AF0E82773A171.
inline constexpr std::uint64_t kTaskDriveBy = 0XD83588CA24829967;

/// TASK_VEHICLE_AIM_AT_COORD, канонический хеш 0X447C1E9EF844BC0F.
inline constexpr std::uint64_t kTaskVehicleAimAtCoord = 0XE61AF8D27706A774;

/// FORCE_PED_MOTION_STATE, канонический хеш 0XF28965D04F570DCA.
inline constexpr std::uint64_t kForcePedMotionState = 0X717804C8C8DA67BF;

/// IS_VEHICLE_SEAT_FREE, канонический хеш 0X22AC59A870E6A669.
inline constexpr std::uint64_t kIsVehicleSeatFree = 0XC39AE5D390581AD5;

/// IS_HORN_ACTIVE, канонический хеш 0X9D6BFC12B05C6121.
inline constexpr std::uint64_t kIsHornActive = 0X1A90E9DC88A85C9D;

/// START_VEHICLE_HORN, канонический хеш 0X9C8C6504B5B63D2C.
inline constexpr std::uint64_t kStartVehicleHorn = 0XAA8884A4BB5B0167;

/// GET_CONVERTIBLE_ROOF_STATE, канонический хеш 0XF8C397922FC03F41.
inline constexpr std::uint64_t kGetConvertibleRoofState = 0X54DA32C15F7A6ABA;

/// RAISE_CONVERTIBLE_ROOF, канонический хеш 0X8F5FB35D7E88FC70.
inline constexpr std::uint64_t kRaiseConvertibleRoof = 0XD6B15DF382A594C7;

/// LOWER_CONVERTIBLE_ROOF, канонический хеш 0XDED51F703D0FA83D.
inline constexpr std::uint64_t kLowerConvertibleRoof = 0X232B023FE4D977E2;

/// IS_VEHICLE_A_CONVERTIBLE, канонический хеш 0X52F357A30698BCCE.
inline constexpr std::uint64_t kIsVehicleAConvertible = 0X7943BD10E2A03FAC;

/// RESURRECT_PED, канонический хеш 0X71BC8E838B9C6035.
inline constexpr std::uint64_t kResurrectPed = 0X6ED737C2A74E181D;

/// GET_LANDING_GEAR_STATE, канонический хеш 0X9B0F3DCA3DB0F4CD.
inline constexpr std::uint64_t kGetLandingGearState = 0X68F7F7C5DF6717F8;

/// CONTROL_LANDING_GEAR, канонический хеш 0XCFC8BE9A5E1FE575.
inline constexpr std::uint64_t kControlLandingGear = 0XC2A036647DD761E4;

/// _VEHICLE_HAS_LANDING_GEAR, канонический хеш 0X4198AB0022B15F87.
inline constexpr std::uint64_t kVehicleHasLandingGear = 0X61F41693A4648B46;

/// ADD_EXPLOSION, канонический хеш 0XE3AD2BDBAEE269AC.
inline constexpr std::uint64_t kAddExplosion = 0XD2FD15A3D9DEE4CC;

/// WAIT, канонический хеш 0X4EDE34FBADD967A6.
/// REQUEST_IPL — просьба поставить расстановку карты.
///
/// Тем же нативом расстановку грузят в одиночной игре все, кто ставит карты
/// руками. Имя — короткое, без пути и расширения.
inline constexpr std::uint64_t kRequestIpl = 0xECFC57F5F11BCD83;

/// IS_IPL_ACTIVE — встала ли расстановка. Единственный способ отличить
/// поставленную карту от молча пропущенной.
inline constexpr std::uint64_t kIsIplActive = 0x5AEB336317DC4151;

inline constexpr std::uint64_t kWait = 0X4EDE34FBADD967A6;

/// GET_GAME_TIMER, канонический хеш 0X9CD27B0045628463.
inline constexpr std::uint64_t kGetGameTimer = 0X1DD05E817C89C737;

/// GET_FRAME_COUNT, канонический хеш 0XFC8202EFC642E6F2.
inline constexpr std::uint64_t kGetFrameCount = 0X8034325BF6D6E41F;

/// PLAYER_ID, канонический хеш 0X4F8644AF03D0E0D6.
inline constexpr std::uint64_t kPlayerId = 0X259BE71D8A81D4FA;

/// PLAYER_PED_ID, канонический хеш 0XD80958FC74E988A6.
inline constexpr std::uint64_t kPlayerPedId = 0X4A8C381C258A124D;

/// IS_PLAYER_PLAYING, канонический хеш 0X5E9564D8246B909A.
inline constexpr std::uint64_t kIsPlayerPlaying = 0X75EAB09F5E974116;

/// GET_ENTITY_COORDS, канонический хеш 0X3FEF770D40960D5A.
inline constexpr std::uint64_t kGetEntityCoords = 0XD1A6A821F5AC81DB;

/// SET_ENTITY_COORDS, канонический хеш 0X06843DA7060A026B.
inline constexpr std::uint64_t kSetEntityCoords = 0XB2BD5837A8D3CEDA;

/// GET_ENTITY_HEADING, канонический хеш 0XE83D4F9BA2A38914.
inline constexpr std::uint64_t kGetEntityHeading = 0XCFC0C995455A6204;

/// SET_ENTITY_HEADING, канонический хеш 0X8E2530AA8ADA980E.
inline constexpr std::uint64_t kSetEntityHeading = 0X5C96CEA06531AB03;

/// FREEZE_ENTITY_POSITION, канонический хеш 0X428CA6DBD1094446.
inline constexpr std::uint64_t kFreezeEntityPosition = 0X5D7CD709B34C90F0;

/// SET_ENTITY_VISIBLE, канонический хеш 0XEA1C610A04DB6BBB.
inline constexpr std::uint64_t kSetEntityVisible = 0X4285E11B28063EE0;

/// ATTACH_ENTITY_TO_ENTITY, канонический хеш 0X6B9BBD38AB0796DF.
inline constexpr std::uint64_t kAttachEntityToEntity = 0X4D306DD94DD6FDBA;

/// DETACH_ENTITY, канонический хеш 0X961AC54BF0613F5D.
inline constexpr std::uint64_t kDetachEntity = 0X837D67618BF89034;

/// GET_ENTITY_BONE_INDEX_BY_NAME, канонический хеш 0XFB71170B7E76ACBA.
inline constexpr std::uint64_t kGetEntityBoneIndexByName = 0X365DC1E8054AF31A;

/// SHUTDOWN_LOADING_SCREEN, канонический хеш 0X078EBE9809CCD637.
inline constexpr std::uint64_t kShutdownLoadingScreen = 0XCD17096A98584C2B;

/// DO_SCREEN_FADE_IN, канонический хеш 0XD4E8E24955024033.
inline constexpr std::uint64_t kDoScreenFadeIn = 0X10B228D2FDB7AF16;

/// DO_SCREEN_FADE_OUT, канонический хеш 0X891B5B39AC6302AF.
inline constexpr std::uint64_t kDoScreenFadeOut = 0X8F72AF14CE5AACE4;

/// TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME, канонический хеш 0X9DC711BC69C548DF.
inline constexpr std::uint64_t kTerminateAllScriptsWithThisName = 0XD13237BC328B938E;

/// SET_TEXT_FONT, канонический хеш 0X66E0276CC5F6B9DA.
inline constexpr std::uint64_t kSetTextFont = 0X8413CD3BCEEAD8DC;

/// SET_TEXT_SCALE, канонический хеш 0X07C837F9A01C34C9.
inline constexpr std::uint64_t kSetTextScale = 0XBFE94E91C83D8794;

/// SET_TEXT_COLOUR, канонический хеш 0XBE6B23FFA53FB442.
inline constexpr std::uint64_t kSetTextColour = 0X5A18938160AE52D0;

/// SET_TEXT_CENTRE, канонический хеш 0XC02F4DBFB51D988B.
inline constexpr std::uint64_t kSetTextCentre = 0XEAF65721ACB2FDFB;

/// SET_TEXT_OUTLINE, канонический хеш 0X2513DFB0FB8400FE.
inline constexpr std::uint64_t kSetTextOutline = 0XF18BC069A9C882EC;

/// BEGIN_TEXT_COMMAND_DISPLAY_TEXT, канонический хеш 0X25FBB336DF1804CB.
inline constexpr std::uint64_t kBeginTextCommandDisplayText = 0XEAEB6E7D3FAEBD5B;

/// ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME, канонический хеш 0X6C188BE134E074AA.
inline constexpr std::uint64_t kAddTextComponentSubstringPlayerName = 0X60D332F23943B34F;

/// END_TEXT_COMMAND_DISPLAY_TEXT, канонический хеш 0XCD015E5BB0D96A57.
inline constexpr std::uint64_t kEndTextCommandDisplayText = 0X25DD447A6EB3A86F;

/// IS_CUTSCENE_ACTIVE, канонический хеш 0X991251AFC3981F84.
inline constexpr std::uint64_t kIsCutsceneActive = 0X0CB7695268A7F50F;

/// IS_CUTSCENE_PLAYING, канонический хеш 0XD3C2E180A40F031E.
inline constexpr std::uint64_t kIsCutscenePlaying = 0XFD216000DC314A92;

/// STOP_CUTSCENE_IMMEDIATELY, канонический хеш 0XD220BDD222AC4A1E.
inline constexpr std::uint64_t kStopCutsceneImmediately = 0XA7E4AA8D29D3DAC1;

/// SET_PLAYER_CONTROL, канонический хеш 0X8D32347D6D4C40A2.
inline constexpr std::uint64_t kSetPlayerControl = 0X4686BC3BFDBB5348;

/// IS_PLAYER_CONTROL_ON, канонический хеш 0X49C32D60007AFA47.
inline constexpr std::uint64_t kIsPlayerControlOn = 0XE916D57851F785AB;

/// CLEAR_PLAYER_WANTED_LEVEL, канонический хеш 0XB302540597885499.
inline constexpr std::uint64_t kClearPlayerWantedLevel = 0X3C482AC51A8E85DC;

/// REQUEST_COLLISION_AT_COORD, канонический хеш 0X07503F7948F491A7.
inline constexpr std::uint64_t kRequestCollisionAtCoord = 0XEA2D52183C7EA9CF;

/// GET_GROUND_Z_FOR_3D_COORD, канонический хеш 0XC906A7DAB05C8D2B.
inline constexpr std::uint64_t kGetGroundZFor3dCoord = 0XB1EAADCB692D69CE;

/// SET_ENTITY_INVINCIBLE, канонический хеш 0X3882114BDE571AD4.
inline constexpr std::uint64_t kSetEntityInvincible = 0X935364B4448CD584;

/// SET_ENTITY_COLLISION, канонический хеш 0X1A9205C1B9EE827F.
inline constexpr std::uint64_t kSetEntityCollision = 0X44C48AC14D3C09ED;

/// GET_GAMEPLAY_CAM_ROT, канонический хеш 0X837765A25378F0BB.
inline constexpr std::uint64_t kGetGameplayCamRot = 0XD84A545408A3099A;

/// DRAW_RECT, канонический хеш 0X3A618A217E5154F0.
inline constexpr std::uint64_t kDrawRect = 0X81645EE95A114FAE;

/// REMOVE_CUTSCENE, канонический хеш 0X440AF51A3462B86F.
inline constexpr std::uint64_t kRemoveCutscene = 0XDD291722ADDCBD60;

/// SET_MISSION_FLAG, канонический хеш 0XC4301E5121A0ED73.
inline constexpr std::uint64_t kSetMissionFlag = 0X925970A93719CADE;

/// PAUSE_DEATH_ARREST_RESTART, канонический хеш 0X2C2B3493FBF51C71.
inline constexpr std::uint64_t kPauseDeathArrestRestart = 0XD43B9D1692F5C06E;

/// IGNORE_NEXT_RESTART, канонический хеш 0X21FFB63D8C615361.
inline constexpr std::uint64_t kIgnoreNextRestart = 0X72B1E2693AC30407;

/// SET_FADE_OUT_AFTER_DEATH, канонический хеш 0X4A18E01DF2C87B86.
inline constexpr std::uint64_t kSetFadeOutAfterDeath = 0X31E8D1058586E006;

/// SET_FADE_IN_AFTER_DEATH_ARREST, канонический хеш 0XDA66D2796BA33F12.
inline constexpr std::uint64_t kSetFadeInAfterDeathArrest = 0X40AC02FA167D4D0A;

/// SET_FADE_IN_AFTER_LOAD, канонический хеш 0XF3D78F59DFE18D79.
inline constexpr std::uint64_t kSetFadeInAfterLoad = 0XC83C5315C571C2FE;

/// IS_PLAYER_DEAD, канонический хеш 0X424D4687FA1E5652.
inline constexpr std::uint64_t kIsPlayerDead = 0XD5FF242D0AFC5855;

/// NETWORK_RESURRECT_LOCAL_PLAYER, канонический хеш 0XEA23C49EAA83ACFB.
inline constexpr std::uint64_t kNetworkResurrectLocalPlayer = 0XF24C94A1C99DA4AB;

/// GET_HASH_KEY, канонический хеш 0XD24D37CC275948CC.
inline constexpr std::uint64_t kGetHashKey = 0X70E57E9927B6BA58;

/// REQUEST_MODEL, канонический хеш 0X963D27A58DF860AC.
inline constexpr std::uint64_t kRequestModel = 0XEC9DAA34BBB4658C;

/// HAS_MODEL_LOADED, канонический хеш 0X98A4EB5D89A0C952.
inline constexpr std::uint64_t kHasModelLoaded = 0X6252BC0DD8A320DB;

/// SET_MODEL_AS_NO_LONGER_NEEDED, канонический хеш 0XE532F5D78798DAAB.
inline constexpr std::uint64_t kSetModelAsNoLongerNeeded = 0X55098D9E9AD58806;

/// SET_PLAYER_MODEL, канонический хеш 0X00A1CADD00108836.
inline constexpr std::uint64_t kSetPlayerModel = 0X52E0301351FCDEC5;

/// SET_PED_DEFAULT_COMPONENT_VARIATION, канонический хеш 0X45EEE61580806D63.
inline constexpr std::uint64_t kSetPedDefaultComponentVariation = 0X77EFA99E6A8FFC43;

/// SET_PED_DENSITY_MULTIPLIER_THIS_FRAME, канонический хеш 0X95E3D6257B166CF2.
inline constexpr std::uint64_t kSetPedDensityMultiplier = 0XF9A2335AB37CF17E;

/// SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME, канонический хеш 0X7A556143A1C03898.
inline constexpr std::uint64_t kSetScenarioPedDensityMultiplier = 0X0397A00D015A11D4;

/// SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, канонический хеш 0X245A6883D966D537.
inline constexpr std::uint64_t kSetVehicleDensityMultiplier = 0XA0265306DFF63938;

/// SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, канонический хеш 0XB3B3359379FE77D3.
inline constexpr std::uint64_t kSetRandomVehicleDensityMultiplier = 0X23D563236A543309;

/// SET_PARKED_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME, канонический хеш 0XEAE6DCC7EEE3DB1D.
inline constexpr std::uint64_t kSetParkedVehicleDensityMultiplier = 0X40C1C94D5A5157C5;

/// SET_VEHICLE_POPULATION_BUDGET, канонический хеш 0XCB9E1EB3BE2AF4E9.
inline constexpr std::uint64_t kSetVehiclePopulationBudget = 0X283C0970282AA5F3;

/// SET_PED_POPULATION_BUDGET, канонический хеш 0X8C95333CFC3340F3.
inline constexpr std::uint64_t kSetPedPopulationBudget = 0XAD9B1C8FED6B4D96;

/// SET_NUMBER_OF_PARKED_VEHICLES, канонический хеш 0XCAA15F13EBD417FF.
inline constexpr std::uint64_t kSetNumberOfParkedVehicles = 0XECDFDC2E8AC2D613;

/// SET_ALL_LOW_PRIORITY_VEHICLE_GENERATORS_ACTIVE, канонический хеш 0X608207E7A8FB787C.
inline constexpr std::uint64_t kSetAllLowPriorityVehicleGeneratorsActive = 0XEFAF1ADDE0F583C3;

/// REMOVE_VEHICLES_FROM_GENERATORS_IN_AREA, канонический хеш 0X46A1E1A299EC4BBA.
inline constexpr std::uint64_t kRemoveVehiclesFromGeneratorsInArea = 0XC4BCE90F7242F354;

/// GET_AMMO_IN_PED_WEAPON, канонический хеш 0X015A522136D7F951.
inline constexpr std::uint64_t kGetAmmoInPedWeapon = 0X1149D67DB429787A;

/// SET_PED_AMMO, канонический хеш 0X14E56BC5B5DB6A19.
inline constexpr std::uint64_t kSetPedAmmo = 0X45FC566246B3511B;

/// REMOVE_ALL_PED_WEAPONS, канонический хеш 0XF25DF915FA38C5F3.
inline constexpr std::uint64_t kRemoveAllPedWeapons = 0X1834D30866818A23;

/// CREATE_OBJECT_NO_OFFSET, канонический хеш 0X9A294B2138ABB884.
inline constexpr std::uint64_t kCreateObjectNoOffset = 0X43AFC452F25F3A2F;

/// DELETE_OBJECT, канонический хеш 0X539E0AE3E6634B9F.
inline constexpr std::uint64_t kDeleteObject = 0X51C8BEA2005931AB;

/// GET_CLOSEST_VEHICLE, канонический хеш 0XF73EB622C4F1689B.
inline constexpr std::uint64_t kGetClosestVehicle = 0XF0CA45A211FFDCD9;

/// IS_ENTITY_A_MISSION_ENTITY, канонический хеш 0X0A7B270912999B3C.
inline constexpr std::uint64_t kIsEntityAMissionEntity = 0X110821AE6C63DD4F;

/// SET_CREATE_RANDOM_COPS, канонический хеш 0X102E68B2024D536D.
inline constexpr std::uint64_t kSetCreateRandomCops = 0XD682DD0578BF5392;

/// SET_CREATE_RANDOM_COPS_NOT_ON_SCENARIOS, канонический хеш 0X8A4986851C4EF6E7.
inline constexpr std::uint64_t kSetCreateRandomCopsNotOnScenarios = 0XD5E2F27BCC913BE9;

/// SET_GARBAGE_TRUCKS, канонический хеш 0X2AFD795EEAC8D30D.
inline constexpr std::uint64_t kSetGarbageTrucks = 0X474491073FE815A8;

/// SET_RANDOM_BOATS, канонический хеш 0X84436EC293B1415F.
inline constexpr std::uint64_t kSetRandomBoats = 0XBB7BF0D30DB04384;

/// SET_RANDOM_TRAINS, канонический хеш 0X80D9F74197EA47D9.
inline constexpr std::uint64_t kSetRandomTrains = 0XAE03F542B985A69E;

/// DELETE_ALL_TRAINS, канонический хеш 0X736A718577F39C7D.
inline constexpr std::uint64_t kDeleteAllTrains = 0X47931C69C0D75B43;

/// CLEAR_AREA_OF_PEDS, канонический хеш 0XBE31FD6CE464AC59.
inline constexpr std::uint64_t kClearAreaOfPeds = 0X55F7AC4B2B875901;

/// CLEAR_AREA_OF_VEHICLES, канонический хеш 0X01C7B9B38428AEB6.
inline constexpr std::uint64_t kClearAreaOfVehicles = 0X60040CDD28AA1BC3;

/// CLEAR_AREA_OF_COPS, канонический хеш 0X04F8FC8FCF58F88D.
inline constexpr std::uint64_t kClearAreaOfCops = 0X8B0110C1F1D9D177;

/// SET_MAX_WANTED_LEVEL, канонический хеш 0XAA5F02DB48D704B9.
inline constexpr std::uint64_t kSetMaxWantedLevel = 0XDAE61414743C8D1D;

/// SET_POLICE_IGNORE_PLAYER, канонический хеш 0X32C62AA929C2DA6A.
inline constexpr std::uint64_t kSetPoliceIgnorePlayer = 0XDAA51A56DBEC0391;

/// ENABLE_DISPATCH_SERVICE, канонический хеш 0XDC0F817884CDD856.
inline constexpr std::uint64_t kEnableDispatchService = 0XCC1C92F7E1A3CE9D;

/// GET_IS_LOADING_SCREEN_ACTIVE, канонический хеш 0X10D0A8F259E93EC9.
inline constexpr std::uint64_t kIsLoadingScreenActive = 0XDCE42B3C644D1A4E;

/// IS_PAUSE_MENU_ACTIVE, канонический хеш 0XB0034A223497FFCB.
inline constexpr std::uint64_t kIsPauseMenuActive = 0X4D9174D8796EA622;

/// CLEAR_PRINTS, канонический хеш 0XCC33FA791322B9D9.
inline constexpr std::uint64_t kClearPrints = 0X406CBCEA35499884;

/// CLEAR_BRIEF, канонический хеш 0X9D292F73ADBD9313.
inline constexpr std::uint64_t kClearBrief = 0X3FE29AE9C01FA3C2;

/// CLEAR_ALL_HELP_MESSAGES, канонический хеш 0X6178F68A87A4D3A0.
inline constexpr std::uint64_t kClearAllHelpMessages = 0XAD01710361B8BCF5;

/// CLEAR_SMALL_PRINTS, канонический хеш 0X2CEA2839313C09AC.
inline constexpr std::uint64_t kClearSmallPrints = 0XFFD79EDD25B8EC72;

/// CLEAR_FLOATING_HELP, канонический хеш 0X50085246ABD3FEFA.
inline constexpr std::uint64_t kClearFloatingHelp = 0X665A7E873A6664BC;

/// THEFEED_FLUSH_QUEUE, канонический хеш 0XA8FDB297A8D25FBA.
inline constexpr std::uint64_t kFlushNotifications = 0XC138265FD0CDEA4E;

/// CLEAR_PED_TASKS_IMMEDIATELY, канонический хеш 0XAAA34F8A7CB32098.
inline constexpr std::uint64_t kClearPedTasksImmediately = 0X19626F992DC71FB9;

/// CLEAR_PED_TASKS, канонический хеш 0XE1EF3C1216AFF2CD.
///
/// Отдельно от «немедленно», и разница не в скорости, а в виде. Немедленный
/// обрывает движение на полукадре — тело дёргается; обычный доводит его до
/// конца перехода. Куклам нужен первый: их положение мы задаём сами, и ждать
/// перехода незачем. Движению, снятому по слову сервера, — второй.
inline constexpr std::uint64_t kClearPedTasks = 0X974022927CB47E68;

/// DISPLAY_HUD, канонический хеш 0XA6294919E56FF02A.
inline constexpr std::uint64_t kDisplayHud = 0X747786364137DC63;

/// DISPLAY_RADAR, канонический хеш 0XA0EBB943C300E693.
inline constexpr std::uint64_t kDisplayRadar = 0X37B894853929BF1A;

/// DISABLE_CONTROL_ACTION, канонический хеш 0XFE99B66D079CF6BC.
inline constexpr std::uint64_t kDisableControlAction = 0X66EFB3D6110055C4;

/// _SET_CONTROL_NORMAL, канонический хеш 0XE8A25867FBA3B05E.
inline constexpr std::uint64_t kSetControlNormal = 0X11E5CA6A9B6D7D2A;

/// SET_ENTITY_VELOCITY, канонический хеш 0X1C99BB7B6E96D16F.
inline constexpr std::uint64_t kSetEntityVelocity = 0X1AB7223AC0702871;

/// ON_ENTER_MP, канонический хеш 0X0888C3502DBBEEF5.
inline constexpr std::uint64_t kOnEnterMp = 0X81F7C34FD4E856D5;

// SET_INSTANCE_PRIORITY_MODE (0X2268617D0B5A5B35, канонический 0X9BAE5AD2508DF078)
// здесь намеренно нет, и возвращать его не нужно.
//
// Он вызывался при переключении мира на сетевую карту и понижал плотность
// пропов: земля оставалась без текстуры, а растительность рисовалась поверх
// неё. В CitizenFX этот натив запрещён во всех сборках с той же пометкой.
// Подробности — в online_map.cpp, там же и причина, по которой его звали.

/// NETWORK_SESSION_HOST_SINGLE_PLAYER, канонический хеш 0XC74C33FCA52856D5.
///
/// Поднимает сетевую сессию на одного. Тем и ценен: подбора игроков ему не
/// нужно, а значит и серверов Rockstar, — а признак сетевой игры игра при этом
/// выставляет свой, настоящий, со всеми объектами.
inline constexpr std::uint64_t kNetworkSessionHostSinglePlayer = 0X827C8918F31EF9CD;

/// NETWORK_SESSION_LEAVE_SINGLE_PLAYER, канонический хеш 0X3442775428FD2DAA.
///
/// Обратное к предыдущему. Заводится вместе с ним: сессию, которую некому
/// закрыть, придётся закрывать перезапуском игры.
inline constexpr std::uint64_t kNetworkSessionLeaveSinglePlayer = 0X566589BD8D826713;

/// NETWORK_IS_GAME_IN_PROGRESS, канонический хеш 0X10FAB35428CCC9D7.
inline constexpr std::uint64_t kNetworkIsGameInProgress = 0X76CD105BCAC6EB9F;

/// CREATE_PED, канонический хеш 0XD49F9B0955C367DE.
inline constexpr std::uint64_t kCreatePed = 0XB1DBFEB95C0EFB88;

/// DELETE_PED, канонический хеш 0X9614299DCB53E54B.
inline constexpr std::uint64_t kDeletePed = 0X734A9F4537A31459;

/// DOES_ENTITY_EXIST, канонический хеш 0X7239B21A38F536BA.
inline constexpr std::uint64_t kDoesEntityExist = 0XFC8BFE4B41177C22;

/// SET_ENTITY_COORDS_NO_OFFSET, канонический хеш 0X239A3351AC1DA385.
inline constexpr std::uint64_t kSetEntityCoordsNoOffset = 0X62C438C53BB57AFD;

/// SET_BLOCKING_OF_NON_TEMPORARY_EVENTS, канонический хеш 0X9F8AA94D6D97DBF4.
inline constexpr std::uint64_t kSetBlockingOfNonTemporaryEvents = 0XAAA71DD7E9059338;

/// SET_ENTITY_AS_MISSION_ENTITY, канонический хеш 0XAD738C3085FE7E11.
inline constexpr std::uint64_t kSetEntityAsMissionEntity = 0XEE0BCDB1B5E36BCB;

/// SET_PED_CAN_BE_TARGETTED, канонический хеш 0X63F58F7C80513AAD.
inline constexpr std::uint64_t kSetPedCanBeTargetted = 0X3F58BFCF656F0DF1;

/// SET_PED_DIES_WHEN_INJURED, канонический хеш 0X5BA7919BED300023.
inline constexpr std::uint64_t kSetPedDiesWhenInjured = 0X9E6CC93E007219AC;

/// SET_ENTITY_LOD_DIST, канонический хеш 0X5927F96A78577363.
inline constexpr std::uint64_t kSetEntityLodDist = 0XF88FC425EC7D675D;

/// SET_AMBIENT_VOICE_NAME, канонический хеш 0X6C8065A3B780185B.
inline constexpr std::uint64_t kSetAmbientVoiceName = 0X397CF4F4C8B17365;

/// GET_ENTITY_HEALTH, канонический хеш 0XEEF059FAD016D209.
inline constexpr std::uint64_t kGetEntityHealth = 0X8D91ADE44AC79BC9;

/// SET_ENTITY_HEALTH, канонический хеш 0X6B76DC1F3AE6E6A3.
inline constexpr std::uint64_t kSetEntityHealth = 0XD25E9BDC14A0B649;

/// GET_ENTITY_VELOCITY, канонический хеш 0X4805D2B1D8CF94A9.
inline constexpr std::uint64_t kGetEntityVelocity = 0XE5741C6B6539231F;

/// SET_GAME_PAUSED, канонический хеш 0X577D1284D6873711.
inline constexpr std::uint64_t kSetGamePaused = 0X98E393364463951A;

/// _SET_MINIMAP_REVEALED, канонический хеш 0XF8DEE0A5600CBB93.
inline constexpr std::uint64_t kSetMinimapRevealed = 0X84DE06FB962FF36D;

/// TASK_GO_STRAIGHT_TO_COORD, канонический хеш 0XD76B57B44F1E6F8B.
inline constexpr std::uint64_t kTaskGoStraightToCoord = 0X63C8DCBEC1CF8225;

/// SET_PED_DESIRED_MOVE_BLEND_RATIO, канонический хеш 0X1E982AC8716912C5.
inline constexpr std::uint64_t kSetPedDesiredMoveBlendRatio = 0XA6897CC743103C98;

/// SET_PED_DESIRED_HEADING, канонический хеш 0XAA5A7ECE2AA8FE70.
inline constexpr std::uint64_t kSetPedDesiredHeading = 0XFBF90D96AEB26BCF;

/// SET_TIME_SCALE, канонический хеш 0X1D408577D440E81E.
///
/// Ход времени в игре. Единица — обычный. Игра замедляет его сама: на колесе
/// выбора персонажа, на смерти, на переключении оружия, — и в мультиплеере это
/// не спецэффект, а расхождение: у одного время идёт вдвое медленнее, чем у
/// остальных.
inline constexpr std::uint64_t kSetTimeScale = 0XE6AC149D1121535D;

/// DISABLE_ALL_CONTROL_ACTIONS, канонический хеш 0X5F4B6931816E599B.
inline constexpr std::uint64_t kDisableAllControlActions = 0XD4510218399ED105;

/// RESET_PLAYER_ARREST_STATE, канонический хеш 0X2D03E13C460760D6.
inline constexpr std::uint64_t kResetPlayerArrestState = 0X3C2C878E6683CE75;

/// HIDE_HUD_COMPONENT_THIS_FRAME, канонический хеш 0X6806C51AD12B83B8.
inline constexpr std::uint64_t kHideHudComponentThisFrame = 0X4EB223432F8FA0A0;

/// DISPLAY_HUD_WHEN_DEAD_THIS_FRAME, канонический хеш 0X71B74D2AE19338D0.
inline constexpr std::uint64_t kDisplayHudWhenDeadThisFrame = 0XFE2EB239B608CDF9;

/// IS_SCREEN_FADED_OUT, канонический хеш 0XB16FCE9DDC7BA182.
inline constexpr std::uint64_t kIsScreenFadedOut = 0X15CCE8886267624F;

/// SET_DRAW_ORIGIN, канонический хеш 0XAA0008F3BBB8F416.
///
/// Переносит начало координат рисования в точку мира: всё, что нарисовано
/// после, ложится на экран там, где эта точка видна, и уезжает вместе с ней.
/// Ради надписей над головами это и заведено.
inline constexpr std::uint64_t kSetDrawOrigin = 0XB56F2B356187E2E0;

/// CLEAR_DRAW_ORIGIN, канонический хеш 0XFF0B610F6BE0D7AF.
inline constexpr std::uint64_t kClearDrawOrigin = 0XCE3DA51E28972A56;

/// SET_TEXT_PROPORTIONAL, канонический хеш 0X038C1F517D7FDCF8.
inline constexpr std::uint64_t kSetTextProportional = 0XEA62FB8CA7210CF3;

/// GET_ENTITY_MODEL, канонический хеш 0X9F47B058362C84B5.
inline constexpr std::uint64_t kGetEntityModel = 0X4B423FAA24E8ABF0;

/// GET_ENTITY_ROTATION, канонический хеш 0XAFBD61CC738D9EB9.
inline constexpr std::uint64_t kGetEntityRotation = 0X88124E0D60FB8D11;

/// SET_ENTITY_ROTATION, канонический хеш 0X8524A8B0171D5E07.
inline constexpr std::uint64_t kSetEntityRotation = 0XCF39804E8C88080E;

/// IS_PED_IN_ANY_VEHICLE, канонический хеш 0X997ABD671D25CA0B.
inline constexpr std::uint64_t kIsPedInAnyVehicle = 0X7F420695E3F776FB;

/// GET_VEHICLE_PED_IS_IN, канонический хеш 0X9A9112A0FE9A4713.
inline constexpr std::uint64_t kGetVehiclePedIsIn = 0X6EF03BE64E058E2F;

/// GET_PED_IN_VEHICLE_SEAT, канонический хеш 0XBB40DD2270B65366.
inline constexpr std::uint64_t kGetPedInVehicleSeat = 0XFD5C5BBD1FE92BB7;

/// CREATE_VEHICLE, канонический хеш 0XAF35D0D2583051B0.
inline constexpr std::uint64_t kCreateVehicle = 0X5779387E956077A6;

/// DELETE_VEHICLE, канонический хеш 0XEA386986E786A54F.
inline constexpr std::uint64_t kDeleteVehicle = 0X8C1F7D7A31B2A38E;

/// SET_PED_INTO_VEHICLE, канонический хеш 0XF75B0D629E1C063D.
inline constexpr std::uint64_t kSetPedIntoVehicle = 0X73CAFD2038E812B3;

/// TASK_LEAVE_VEHICLE, канонический хеш 0XD3DBCE61A490BE02.
inline constexpr std::uint64_t kTaskLeaveVehicle = 0X23EB5FC236231892;

/// SET_VEHICLE_ENGINE_ON, канонический хеш 0X2497C4717C8B881E.
inline constexpr std::uint64_t kSetVehicleEngineOn = 0XC229299217554C78;

/// SET_VEHICLE_ON_GROUND_PROPERLY, канонический хеш 0X49733E92263139D1.
inline constexpr std::uint64_t kSetVehicleOnGroundProperly = 0X1DE99C193C7EC64B;

/// IS_PED_SHOOTING, канонический хеш 0X34616828CD07F1A1.
inline constexpr std::uint64_t kIsPedShooting = 0X65F146FF416F109F;

/// IS_PLAYER_FREE_AIMING, канонический хеш 0X2E397FD2ECD37C87.
inline constexpr std::uint64_t kIsPlayerFreeAiming = 0X1C751EF63BF4D501;

/// IS_PLAYER_TARGETTING_ANYTHING, канонический хеш 0X78CFE51896B6B8A4.
inline constexpr std::uint64_t kIsPlayerTargettingAnything = 0X4F035D45FC2856F8;

/// GET_SELECTED_PED_WEAPON, канонический хеш 0X0A6DB4965674D243.
inline constexpr std::uint64_t kGetSelectedPedWeapon = 0XB0D77D90171EC35F;

/// GIVE_WEAPON_TO_PED, канонический хеш 0XBF0FD6E56C964FCB.
inline constexpr std::uint64_t kGiveWeaponToPed = 0XB41DEC3AAC1AA107;

/// SET_CURRENT_PED_WEAPON, канонический хеш 0XADF692B254977C0C.
inline constexpr std::uint64_t kSetCurrentPedWeapon = 0X3C0F448853B71C92;

/// IS_PED_RAGDOLL, канонический хеш 0X47E4E977581C5B55.
inline constexpr std::uint64_t kIsPedRagdoll = 0X8BF5256C439DF778;

/// SET_PED_TO_RAGDOLL, канонический хеш 0XAE99FB955581844A.
inline constexpr std::uint64_t kSetPedToRagdoll = 0XB1C2DC5C115FA50D;

/// SET_PED_CAN_RAGDOLL, канонический хеш 0XB128377056A54E2A.
inline constexpr std::uint64_t kSetPedCanRagdoll = 0X9FF00EA9A61211D2;

/// IS_PED_JUMPING, канонический хеш 0XCEDABC5900A0BF97.
inline constexpr std::uint64_t kIsPedJumping = 0X2C807E70DCB4BB36;

/// HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY, канонический хеш 0XC86D67D52A707CF8.
inline constexpr std::uint64_t kHasEntityBeenDamagedByEntity = 0X9B3D4335E0EDB0BE;

/// CLEAR_ENTITY_LAST_DAMAGE_ENTITY, канонический хеш 0XA72CD9CA74A5ECBA.
inline constexpr std::uint64_t kClearEntityLastDamageEntity = 0XE4DC7B3DD712372B;

/// APPLY_DAMAGE_TO_PED, канонический хеш 0X697157CED63F18D4.
inline constexpr std::uint64_t kApplyDamageToPed = 0X39AB1812D20C2C99;

/// TASK_AIM_GUN_AT_COORD, канонический хеш 0X6671F3EEC681BDA1.
inline constexpr std::uint64_t kTaskAimGunAtCoord = 0XC86A930D894F8CE2;

/// TASK_SHOOT_AT_COORD, канонический хеш 0X46A6CC01E0826106.
inline constexpr std::uint64_t kTaskShootAtCoord = 0X6C4E9ADFB1521AAC;

/// GET_ENTITY_FORWARD_VECTOR, канонический хеш 0X0A794A5A57F8DF91.
inline constexpr std::uint64_t kGetEntityForwardVector = 0X90D0E0397D3F7690;

/// _SET_BIGMAP_ACTIVE, канонический хеш 0X231C8F89D0539D8F.
///
/// Разворачивает радар в большую карту — ту самую, что открывается в GTA Online
/// по долгому нажатию на карту. Мир при этом продолжает жить, в отличие от карты
/// в меню паузы.
inline constexpr std::uint64_t kSetBigmapActive = 0XC2F71CC2AB70CFB1;

/// SET_PAUSE_MENU_ACTIVE, канонический хеш 0XDF47FC56C71569CF.
inline constexpr std::uint64_t kSetPauseMenuActive = 0X915FA95E87D33FF5;

/// SET_ENTITY_LOAD_COLLISION_FLAG, канонический хеш 0X0DC7CABAB1E9B67E.
///
/// Просит игру держать столкновения подгруженными вокруг сущности. Без этого
/// мир вокруг игрока бывает нарисован, но не осязаем — и сквозь него проваливаются.
inline constexpr std::uint64_t kSetEntityLoadCollisionFlag = 0X788F35D395511DFE;

/// NEW_LOAD_SCENE_START_SPHERE, канонический хеш 0XACCFB4ACF53551B0.
///
/// Заставляет игру подгрузить всё вокруг точки, не дожидаясь, пока туда доедет
/// игрок. Ради переносов и заведено: перенесённый в неподготовленное место
/// оказывается над пустотой.
inline constexpr std::uint64_t kNewLoadSceneStartSphere = 0X4A3280817398D754;

/// IS_NEW_LOAD_SCENE_LOADED, канонический хеш 0X01B8247A7A8B9AD1.
inline constexpr std::uint64_t kIsNewLoadSceneLoaded = 0X9E2D35FA908F57B4;

/// NEW_LOAD_SCENE_STOP, канонический хеш 0XC197616D221FF4A4.
inline constexpr std::uint64_t kNewLoadSceneStop = 0X6981C3213B841071;

/// SET_PED_ARMOUR, канонический хеш 0XCEA04D83135264CC.
inline constexpr std::uint64_t kSetPedArmour = 0X10A676E622A468AA;

/// SET_PED_MAX_HEALTH, канонический хеш 0XF5F6378C4F3419D3.
inline constexpr std::uint64_t kSetPedMaxHealth = 0X36A20106D0B42723;

/// CLEAR_PED_BLOOD_DAMAGE, канонический хеш 0X8FE22675A5A45817.
inline constexpr std::uint64_t kClearPedBloodDamage = 0X8EA9C5E0178372E1;

/// GET_FIRST_BLIP_INFO_ID, канонический хеш 0X1BEDE233E6CD2A1F.
///
/// Первая отметка заданного вида на карте. Нужна одна — метка игрока: по ней
/// работает телепорт «туда, куда я поставил точку».
inline constexpr std::uint64_t kGetFirstBlipInfoId = 0XD56419CB9E15983F;

/// GET_BLIP_INFO_ID_COORD, канонический хеш 0XFA7C7F0AADF25D09.
inline constexpr std::uint64_t kGetBlipInfoIdCoord = 0X7DFE6973AE84B6ED;

/// DOES_BLIP_EXIST, канонический хеш 0XA6DB27D19ECBB7DA.
inline constexpr std::uint64_t kDoesBlipExist = 0XC450B06E5AAA0985;

// --- Метки на карте ----------------------------------------------------------
//
// Ставит их сервер, а рисует игра, и другого пути к её карте нет: метка — не
// сущность мира, а строчка в её собственном списке.

/// ADD_BLIP_FOR_COORD, канонический хеш 0X5A039BB0BCA604B6.
inline constexpr std::uint64_t kAddBlipForCoord = 0X34864AB7DA700AA6;

/// REMOVE_BLIP, канонический хеш 0X86A652570E5F25DD.
inline constexpr std::uint64_t kRemoveBlip = 0XFE54B8568B2ABD12;

/// SET_BLIP_SPRITE, канонический хеш 0XDF735600A4696DAF.
inline constexpr std::uint64_t kSetBlipSprite = 0X4C905FB262965D5D;

/// SET_BLIP_COLOUR, канонический хеш 0X03D7FB09E75D6B7E.
inline constexpr std::uint64_t kSetBlipColour = 0X61183D6239A9D7B8;

/// SET_BLIP_ALPHA, канонический хеш 0X45FF974EEE1C8734.
inline constexpr std::uint64_t kSetBlipAlpha = 0XF42EBD7CD0682A8B;

/// SET_BLIP_SCALE, канонический хеш 0XD38744167B2FA257.
inline constexpr std::uint64_t kSetBlipScale = 0X5D3946F818C6B331;

/// SET_BLIP_DISPLAY, канонический хеш 0X9029B2F3DA924928.
inline constexpr std::uint64_t kSetBlipDisplay = 0XF55F62DA99DB0C2F;

/// SET_BLIP_AS_SHORT_RANGE, канонический хеш 0XBE8BE4FE60E27B72.
inline constexpr std::uint64_t kSetBlipAsShortRange = 0X360B279488A775FC;

/// BEGIN_TEXT_COMMAND_SET_BLIP_NAME, канонический хеш 0XF9113A30DE5C6670.
inline constexpr std::uint64_t kBeginTextCommandSetBlipName = 0XF3D182B81172EAB6;

/// END_TEXT_COMMAND_SET_BLIP_NAME, канонический хеш 0XBC38B49BCB83BC9B.
inline constexpr std::uint64_t kEndTextCommandSetBlipName = 0XFB605529038475D2;

/// SET_VEHICLE_FIXED, канонический хеш 0X115722B1B9C14C1C.
inline constexpr std::uint64_t kSetVehicleFixed = 0XF698038C13845696;

/// SET_VEHICLE_DEFORMATION_FIXED, канонический хеш 0X953DA1E1B12C0491.
///
/// Отдельно от SET_VEHICLE_FIXED, потому что тот вмятин не снимает: прочности
/// и стёкла возвращает, а смятое крыло оставляет как было. Починка без второго
/// вызова выглядит наполовину сделанной.
inline constexpr std::uint64_t kSetVehicleDeformationFixed = 0X1D1124C855316790;

/// SET_VEHICLE_DIRT_LEVEL, канонический хеш 0X79D3B596FE44EE8B.
inline constexpr std::uint64_t kSetVehicleDirtLevel = 0X9452FE4900245259;

/// IS_MODEL_IN_CDIMAGE, канонический хеш 0X35B9E0803292B641.
///
/// Есть ли такая модель у игры вообще. Проверяется перед заказом: заказ
/// несуществующей модели ждёт её вечно.
inline constexpr std::uint64_t kIsModelInCdimage = 0XE7D342E0F16AAA8F;

/// IS_MODEL_A_VEHICLE, канонический хеш 0X19AAC8F07BFEC53E.
inline constexpr std::uint64_t kIsModelAVehicle = 0XAD1840C2E6AF7D5E;

/// SET_WEATHER_TYPE_NOW_PERSIST, канонический хеш 0XED712CA327900C8A.
inline constexpr std::uint64_t kSetWeatherTypeNow = 0XE38A58649E049502;

/// NETWORK_OVERRIDE_CLOCK_TIME, канонический хеш 0XE679E3E06E363892.
inline constexpr std::uint64_t kOverrideClockTime = 0XAFD3BC0F6EBB5474;

/// GET_GAMEPLAY_CAM_COORD, канонический хеш 0X14D6F5678D8F1B37.
///
/// Где стоит камера. Вместе с положением персонажа этого хватает, чтобы узнать,
/// куда она смотрит, не разбираясь в том, какой угол в каком порядке отдаёт игра.
inline constexpr std::uint64_t kGetGameplayCamCoord = 0XCF141FCD0940B0A3;

// --- Ввод водителя ------------------------------------------------------------
//
// Угол руля, газ и тормоз игра наружу не отдаёт: нативов для них у неё нет, и
// FiveM читает их прямо из памяти машины. Мы берём их с другой стороны — из
// ввода того, кто за рулём. Это ровно те числа, из которых игра эти величины и
// получает, и достаются они нативом, а не смещением в структуре, которое живёт
// до ближайшего обновления игры.

/// GET_CONTROL_NORMAL, канонический хеш 0XEC3C9B8D5327B563.
inline constexpr std::uint64_t kGetControlNormal = 0XB504E1B50AA21FC5;

/// IS_CONTROL_JUST_PRESSED, канонический хеш 0X580417101DDB492F.
inline constexpr std::uint64_t kIsControlJustPressed = 0X875A214D5EBCA509;

/// IS_CONTROL_PRESSED, канонический хеш 0XF3A21BCD95725A4A.
inline constexpr std::uint64_t kIsControlPressed = 0X6D05C5731A838CB3;

// --- Движение тел -------------------------------------------------------------

/// GET_ENTITY_ROTATION_VELOCITY, канонический хеш 0X213B91045D09B983.
inline constexpr std::uint64_t kGetEntityRotationVelocity = 0X47507DD57C93B472;

// --- Машина: то, что меняется на ходу ------------------------------------------

/// GET_VEHICLE_ENGINE_HEALTH, канонический хеш 0XC45D23BAF168AAB8.
inline constexpr std::uint64_t kGetVehicleEngineHealth = 0X4C7724D572378B05;

/// SET_VEHICLE_ENGINE_HEALTH, канонический хеш 0X45F6D8EEF34ABEF1.
inline constexpr std::uint64_t kSetVehicleEngineHealth = 0X2AEBE39F6BF7D6BC;

/// GET_VEHICLE_BODY_HEALTH, канонический хеш 0XF271147EB7B40F12.
inline constexpr std::uint64_t kGetVehicleBodyHealth = 0X3B5692CB240DBC2F;

/// SET_VEHICLE_BODY_HEALTH, канонический хеш 0XB77D05AC8C78AADB.
inline constexpr std::uint64_t kSetVehicleBodyHealth = 0X3E7E7AD923FD91A7;

/// GET_VEHICLE_PETROL_TANK_HEALTH, канонический хеш 0X7D5DABE888D2D074.
inline constexpr std::uint64_t kGetVehiclePetrolTankHealth = 0X31B58D7972181BFA;

/// SET_VEHICLE_PETROL_TANK_HEALTH, канонический хеш 0X70DB57649FA8D0D8.
inline constexpr std::uint64_t kSetVehiclePetrolTankHealth = 0XDF9DC0584881B7AF;

/// GET_IS_VEHICLE_ENGINE_RUNNING, канонический хеш 0XAE31E7DF9B5B132E.
inline constexpr std::uint64_t kGetIsVehicleEngineRunning = 0X182BD9AD1675B5DE;

/// GET_VEHICLE_LIGHTS_STATE, канонический хеш 0XB91B4C20085BD12F.
inline constexpr std::uint64_t kGetVehicleLightsState = 0X9FFEA38DBCE391EC;

/// SET_VEHICLE_LIGHTS, канонический хеш 0X34E710FF01247C5A.
inline constexpr std::uint64_t kSetVehicleLights = 0XBA3C1A9AA7FD9616;

/// SET_VEHICLE_FULLBEAM, канонический хеш 0X8B7FD87F0DDB421E.
inline constexpr std::uint64_t kSetVehicleFullbeam = 0X2F12C305B28C6C59;

/// IS_VEHICLE_SIREN_ON, канонический хеш 0X4C9BF537BE2634B2.
inline constexpr std::uint64_t kIsVehicleSirenOn = 0XE101D58DA98B6070;

/// SET_VEHICLE_SIREN, канонический хеш 0XF4924635A19EB37D.
inline constexpr std::uint64_t kSetVehicleSiren = 0X4539850624F18A9E;

/// SET_VEHICLE_STEER_BIAS, канонический хеш 0X42A8EC77D5150CBE.
inline constexpr std::uint64_t kSetVehicleSteerBias = 0XDEABDA7736297FEB;

/// SET_VEHICLE_HANDBRAKE, канонический хеш 0X684785568EF26A22.
inline constexpr std::uint64_t kSetVehicleHandbrake = 0XB2FD24D644A68449;

/// SET_VEHICLE_BRAKE_LIGHTS, канонический хеш 0X92B35082E0B42F66.
inline constexpr std::uint64_t kSetVehicleBrakeLights = 0XE456FB21FF21AE99;

// --- Машина: повреждения -------------------------------------------------------

/// GET_VEHICLE_DOOR_ANGLE_RATIO, канонический хеш 0XFE3F9C29F7B32BD5.
inline constexpr std::uint64_t kGetVehicleDoorAngleRatio = 0X7BFB76C576628F3D;

/// SET_VEHICLE_DOOR_OPEN, канонический хеш 0X7C65DAC73C35C862.
inline constexpr std::uint64_t kSetVehicleDoorOpen = 0XBFE60A5CC0C835D8;

/// SET_VEHICLE_DOOR_SHUT, канонический хеш 0X93D9BD300D7789E5.
inline constexpr std::uint64_t kSetVehicleDoorShut = 0X6515021478088FBC;

/// IS_VEHICLE_DOOR_DAMAGED, канонический хеш 0XB8E181E559464527.
inline constexpr std::uint64_t kIsVehicleDoorDamaged = 0XAD830DCD82C63F31;

/// SET_VEHICLE_DOOR_BROKEN, канонический хеш 0XD4D4F6A4AB575A33.
inline constexpr std::uint64_t kSetVehicleDoorBroken = 0X89E9F387C190061F;

/// IS_VEHICLE_WINDOW_INTACT, канонический хеш 0X46E571A0E20D01F1.
inline constexpr std::uint64_t kIsVehicleWindowIntact = 0X01D37530E5C420F5;

/// SMASH_VEHICLE_WINDOW, канонический хеш 0X9E5B5E4D2CCD2259.
inline constexpr std::uint64_t kSmashVehicleWindow = 0X62DFD44586348C12;

/// IS_VEHICLE_TYRE_BURST, канонический хеш 0XBA291848A0815CA9.
inline constexpr std::uint64_t kIsVehicleTyreBurst = 0X548F6F43A7CB6F45;

/// SET_VEHICLE_TYRE_BURST, канонический хеш 0XEC6A202EE4960385.
inline constexpr std::uint64_t kSetVehicleTyreBurst = 0XE488FDAA43A181AE;

/// SET_VEHICLE_TYRE_FIXED, канонический хеш 0X6E13FC662B882D1D.
inline constexpr std::uint64_t kSetVehicleTyreFixed = 0XF516E954BCB89C18;

// --- Машина: внешность, меняется редко -----------------------------------------

/// GET_VEHICLE_COLOURS, канонический хеш 0XA19435F193E081AC.
inline constexpr std::uint64_t kGetVehicleColours = 0XFF4B16F297D9CB3E;

/// SET_VEHICLE_COLOURS, канонический хеш 0X4F1D4BE3A7F24601.
inline constexpr std::uint64_t kSetVehicleColours = 0XD133EF7430EDCD09;

/// GET_VEHICLE_EXTRA_COLOURS, канонический хеш 0X3BC4245933A166F7.
inline constexpr std::uint64_t kGetVehicleExtraColours = 0X741D9B0685E67684;

/// SET_VEHICLE_EXTRA_COLOURS, канонический хеш 0X2036F561ADD12E33.
inline constexpr std::uint64_t kSetVehicleExtraColours = 0XBB361D7264AC4FD8;

/// GET_VEHICLE_NUMBER_PLATE_TEXT, канонический хеш 0X7CE1CCB9B293020E.
inline constexpr std::uint64_t kGetVehicleNumberPlateText = 0XCA7159F2C5FF745A;

/// SET_VEHICLE_NUMBER_PLATE_TEXT, канонический хеш 0X95A88F0B409CDA47.
inline constexpr std::uint64_t kSetVehicleNumberPlateText = 0X3FEAE59CDE6D3946;

/// GET_VEHICLE_NUMBER_PLATE_TEXT_INDEX, канонический хеш 0XF11BC2DD9A3E7195.
inline constexpr std::uint64_t kGetVehicleNumberPlateTextIndex = 0X4F06416A18248EA0;

/// SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX, канонический хеш 0X9088EB5A43FFB0A1.
inline constexpr std::uint64_t kSetVehicleNumberPlateTextIndex = 0X05D3F682DDA06C20;

/// GET_VEHICLE_LIVERY, канонический хеш 0X2BB9230590DA5E8A.
inline constexpr std::uint64_t kGetVehicleLivery = 0XA089B04A208DBD0B;

/// SET_VEHICLE_LIVERY, канонический хеш 0X60BF608F1B8CD1B6.
inline constexpr std::uint64_t kSetVehicleLivery = 0XA1C03303EC67320B;

/// GET_VEHICLE_DIRT_LEVEL, канонический хеш 0X8F17BC8BA08DA62B.
inline constexpr std::uint64_t kGetVehicleDirtLevel = 0XF04E476AE02C4646;

/// GET_VEHICLE_MOD, канонический хеш 0X772960298DA26FDB.
inline constexpr std::uint64_t kGetVehicleMod = 0X94C9CD3D66808551;

/// SET_VEHICLE_MOD, канонический хеш 0X6AF0636DDEDCB6DD.
inline constexpr std::uint64_t kSetVehicleMod = 0X8450270DC5896D39;

/// SET_VEHICLE_MOD_KIT, канонический хеш 0X1F2AA07F00B3217A.
inline constexpr std::uint64_t kSetVehicleModKit = 0XB5AD06DDA85E2E8F;

/// TOGGLE_VEHICLE_MOD, канонический хеш 0X2A1F4F37F95BAD08.
inline constexpr std::uint64_t kToggleVehicleMod = 0XF5501FF9869DAC7C;

/// IS_TOGGLE_MOD_ON, канонический хеш 0X84B233A8C8FC8AE7.
inline constexpr std::uint64_t kIsToggleModOn = 0X1D5A665629D417A7;

/// GET_VEHICLE_WHEEL_TYPE, канонический хеш 0XB3ED1BFB4BE636DC.
inline constexpr std::uint64_t kGetVehicleWheelType = 0X6A375D21624F9187;

/// SET_VEHICLE_WHEEL_TYPE, канонический хеш 0X487EB21CC7295BA1.
inline constexpr std::uint64_t kSetVehicleWheelType = 0XE33678A9AE50A01B;

/// GET_VEHICLE_WINDOW_TINT, канонический хеш 0X0EE21293DAD47C95.
inline constexpr std::uint64_t kGetVehicleWindowTint = 0XDA63CE76F9AAB439;

/// SET_VEHICLE_WINDOW_TINT, канонический хеш 0X57C51E6BAD752696.
inline constexpr std::uint64_t kSetVehicleWindowTint = 0XFE620ED8E0A3C209;

/// GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS, канонический хеш 0XA7C4F2C6E744A550.
inline constexpr std::uint64_t kGetVehicleMaxNumberOfPassengers = 0X2EEC0612337D20CE;

// --- Чем занят персонаж --------------------------------------------------------

/// IS_PED_IN_MELEE_COMBAT, канонический хеш 0X4E209B2C1EAD5159.
inline constexpr std::uint64_t kIsPedInMeleeCombat = 0XFFAC548682B3D56E;

/// GET_PED_STEALTH_MOVEMENT, канонический хеш 0X7C2AC9CA66575FBF.
inline constexpr std::uint64_t kGetPedStealthMovement = 0XC2BF1F6F84E31EB2;

/// SET_PED_STEALTH_MOVEMENT, канонический хеш 0X88CBB5CEB96B7BD2.
inline constexpr std::uint64_t kSetPedStealthMovement = 0XF9358C41CC69C616;

/// IS_PED_CLIMBING, канонический хеш 0X53E8CB4F48BFE623.
inline constexpr std::uint64_t kIsPedClimbing = 0X7CB06BFD42FB0E24;

/// IS_PED_VAULTING, канонический хеш 0X117C70D1F5730B5E.
inline constexpr std::uint64_t kIsPedVaulting = 0X75B105C651D87D0E;

/// IS_PED_SWIMMING, канонический хеш 0X9DE327631295B4C2.
inline constexpr std::uint64_t kIsPedSwimming = 0X2CFBD7757B4D922F;

/// IS_PED_SWIMMING_UNDER_WATER, канонический хеш 0XC024869A53992F34.
inline constexpr std::uint64_t kIsPedSwimmingUnderWater = 0X9AC89B274C35B3FC;

/// IS_PED_DIVING, канонический хеш 0X5527B8246FEF9B11.
inline constexpr std::uint64_t kIsPedDiving = 0XCD80FA7E842E5CA9;

/// IS_PED_FALLING, канонический хеш 0XFB92A102F1C4DFA3.
inline constexpr std::uint64_t kIsPedFalling = 0X9857C978BD3CBEDA;

/// GET_PED_PARACHUTE_STATE, канонический хеш 0X79CFD9827CC979B6.
inline constexpr std::uint64_t kGetPedParachuteState = 0X57E7FD3BD6BB28C0;

/// IS_PED_RELOADING, канонический хеш 0X24B100C68C645951.
inline constexpr std::uint64_t kIsPedReloading = 0XC722DDBD6C3E86D7;

/// IS_PED_IN_COVER, канонический хеш 0X60DFD0691A170B88.
inline constexpr std::uint64_t kIsPedInCover = 0XD6179D448A06A77F;

/// IS_PED_GETTING_UP, канонический хеш 0X2A74E1D5F2F00EEC.
inline constexpr std::uint64_t kIsPedGettingUp = 0X0B3FC0E7676E79BF;

/// IS_PED_DOING_DRIVEBY, канонический хеш 0XB2C086CC1BF8F2BF.
inline constexpr std::uint64_t kIsPedDoingDriveby = 0XB29E06D8C7B733E6;

/// GET_PED_ARMOUR, канонический хеш 0X9483AF821605B1D8.
inline constexpr std::uint64_t kGetPedArmour = 0XE5E6F6EFCE07789A;

// --- Посадка в машину ----------------------------------------------------------

/// IS_PED_GETTING_INTO_A_VEHICLE, канонический хеш 0XBB062B2B5722478E.
inline constexpr std::uint64_t kIsPedGettingIntoAVehicle = 0X9D6DF8F3584AAC2B;

/// GET_VEHICLE_PED_IS_TRYING_TO_ENTER, канонический хеш 0X814FA8BE5449445D.
inline constexpr std::uint64_t kGetVehiclePedIsTryingToEnter = 0XF9F8E3060F7CAEDB;

/// GET_SEAT_PED_IS_TRYING_TO_ENTER, канонический хеш 0X6F4C85ACD641BCD2.
inline constexpr std::uint64_t kGetSeatPedIsTryingToEnter = 0X03603B0046EE6ACD;

/// IS_PED_IN_VEHICLE, канонический хеш 0XA3EE4A07279BB9DB.
inline constexpr std::uint64_t kIsPedInVehicle = 0XCECDBB848D53DEB2;

/// TASK_ENTER_VEHICLE, канонический хеш 0XC20E50AA46D09CA8.
inline constexpr std::uint64_t kTaskEnterVehicle = 0XEBA229B2E0BB05E0;

// --- Проигрывание движений -----------------------------------------------------

/// REQUEST_ANIM_DICT, канонический хеш 0XD3BD40951412FEF6.
inline constexpr std::uint64_t kRequestAnimDict = 0X80813AC549A1E8AE;

/// HAS_ANIM_DICT_LOADED, канонический хеш 0XD031A9162D01088C.
inline constexpr std::uint64_t kHasAnimDictLoaded = 0XE100DD4F82A51BDE;

/// TASK_PLAY_ANIM, канонический хеш 0XEA47FE3719165B94.
inline constexpr std::uint64_t kTaskPlayAnim = 0X10425721983AE158;

/// IS_ENTITY_PLAYING_ANIM, канонический хеш 0X1F0B79228E461EC9.
inline constexpr std::uint64_t kIsEntityPlayingAnim = 0X13CCB1AD131C1082;

/// STOP_ANIM_TASK, канонический хеш 0X97FF36A1D40EA00A.
inline constexpr std::uint64_t kStopAnimTask = 0X08D8528BA8E43641;

/// CLEAR_PED_SECONDARY_TASK, канонический хеш 0X176CECF6F920D707.
inline constexpr std::uint64_t kClearPedSecondaryTask = 0XBEB96F1A510EE9AA;

/// TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD, канонический хеш 0X11315AB3385B8AC0.
inline constexpr std::uint64_t kTaskGoToCoordWhileAimingAtCoord = 0XB8551FB832F73124;

/// SET_PED_CONFIG_FLAG, канонический хеш 0X1913FE4CBF41C463.
inline constexpr std::uint64_t kSetPedConfigFlag = 0X0428AFDCAA63B06E;

/// SET_PED_CAN_PLAY_AMBIENT_ANIMS, канонический хеш 0X6373D1349925A70E.
inline constexpr std::uint64_t kSetPedCanPlayAmbientAnims = 0XC1BC1B8A5AA67C6B;

// ---------------------------------------------------------------------------
// Внешность персонажа: одежда, аксессуары, лицо
//
// Читаются они у своего игрока и применяются к чужим куклам. Обратного чтения
// для цвета волос, глаз и слоёв лица у игры нет вовсе — только запись; ровно так
// же обстоит дело и у alt:V, и оттого эти три величины ведёт тот, кто их задал,
// а не тот, кто на них смотрит.
// ---------------------------------------------------------------------------

/// GET_PED_DRAWABLE_VARIATION, канонический хеш 0X67F3780DD425D4FC.
inline constexpr std::uint64_t kGetPedDrawableVariation = 0XC0120BBCC298EA2F;

/// GET_PED_TEXTURE_VARIATION, канонический хеш 0X04A355E041E004E6.
inline constexpr std::uint64_t kGetPedTextureVariation = 0XD6AED6BFCC58AF7F;

/// GET_PED_PALETTE_VARIATION, канонический хеш 0XE3DD5F2A84B42281.
inline constexpr std::uint64_t kGetPedPaletteVariation = 0XDAF263B0E792EAEC;

/// SET_PED_COMPONENT_VARIATION, канонический хеш 0X262B14F48D29DE80.
inline constexpr std::uint64_t kSetPedComponentVariation = 0XD1C578C204015E1F;

/// GET_PED_PROP_INDEX, канонический хеш 0X898CC20EA75BACD8.
inline constexpr std::uint64_t kGetPedPropIndex = 0XB204F40D393426B6;

/// GET_PED_PROP_TEXTURE_INDEX, канонический хеш 0XE131A28626F81AB2.
inline constexpr std::uint64_t kGetPedPropTextureIndex = 0X0DC23FA727759F9F;

/// SET_PED_PROP_INDEX, канонический хеш 0X93376B65A266EB5F.
inline constexpr std::uint64_t kSetPedPropIndex = 0X7F08C4791E6D6969;

/// CLEAR_PED_PROP, канонический хеш 0X0943E5B8E078E76E.
inline constexpr std::uint64_t kClearPedProp = 0X09397806857F5DFB;

/// _GET_PED_HEAD_BLEND_DATA, канонический хеш 0X2746BD9D88C5C5D0.
inline constexpr std::uint64_t kGetPedHeadBlendData = 0X5CB7287FD7611BC7;

/// SET_PED_HEAD_BLEND_DATA, канонический хеш 0X9414E18B9434C2FE.
inline constexpr std::uint64_t kSetPedHeadBlendData = 0X0A5987DCA39E8BE5;

/// _GET_PED_HEAD_OVERLAY_VALUE, канонический хеш 0XA60EF3B6461A4D43.
inline constexpr std::uint64_t kGetPedHeadOverlayValue = 0X8E73BECF421D257E;

/// SET_PED_HEAD_OVERLAY, канонический хеш 0X48F44967FA05CC1E.
inline constexpr std::uint64_t kSetPedHeadOverlay = 0XE5B6C9B29510B84E;

/// _SET_PED_HEAD_OVERLAY_COLOR, канонический хеш 0X497BF74A7B9CB952.
inline constexpr std::uint64_t kSetPedHeadOverlayColor = 0X94965BB62753D4D6;

/// _SET_PED_HAIR_COLOR, канонический хеш 0X4CFFC65454C93A49.
inline constexpr std::uint64_t kSetPedHairColor = 0X894EE2587C8D8D1E;

/// _SET_PED_EYE_COLOR, канонический хеш 0X50B56988B170AFDF.
inline constexpr std::uint64_t kSetPedEyeColor = 0X348FF3E632DCB635;

// ---------------------------------------------------------------------------
// Внешность машины: неон, дым из-под колёс, дополнения кузова
// ---------------------------------------------------------------------------

/// _GET_VEHICLE_NEON_LIGHTS_COLOUR, канонический хеш 0X7619EEE8C886757F.
inline constexpr std::uint64_t kGetVehicleNeonLightsColour = 0X64FEACF0AD019F1F;

/// _SET_VEHICLE_NEON_LIGHTS_COLOUR, канонический хеш 0X8E0A582209A62695.
inline constexpr std::uint64_t kSetVehicleNeonLightsColour = 0XEAB8A43F6621850F;

/// _IS_VEHICLE_NEON_LIGHT_ENABLED, канонический хеш 0X8C4B92553E4766A5.
inline constexpr std::uint64_t kIsVehicleNeonLightEnabled = 0XF1B79038130E3C08;

/// _SET_VEHICLE_NEON_LIGHT_ENABLED, канонический хеш 0X2AA720E4287BF269.
inline constexpr std::uint64_t kSetVehicleNeonLightEnabled = 0XE62930EC6FAABCA5;

/// GET_VEHICLE_TYRE_SMOKE_COLOR, канонический хеш 0XB635392A4938B3C3.
inline constexpr std::uint64_t kGetVehicleTyreSmokeColor = 0X9D35AABAEE206518;

/// SET_VEHICLE_TYRE_SMOKE_COLOR, канонический хеш 0XB5BA80F839791C0F.
inline constexpr std::uint64_t kSetVehicleTyreSmokeColor = 0X5DA0536AEAD1FF31;

/// IS_VEHICLE_EXTRA_TURNED_ON, канонический хеш 0XD2E6822DBFD6C8BD.
inline constexpr std::uint64_t kIsVehicleExtraTurnedOn = 0X5318DF85BEB6B95F;

/// SET_VEHICLE_EXTRA, канонический хеш 0X7EE3A3C5E4A40CC9.
inline constexpr std::uint64_t kSetVehicleExtra = 0XD772F6AA66750D2B;

/// DOES_EXTRA_EXIST, канонический хеш 0X1262D55792428154.
inline constexpr std::uint64_t kDoesExtraExist = 0X579FA5568DE0C2A0;

/// GET_VEHICLE_MOD_VARIATION, канонический хеш 0XB3924ECD70E095DC.
inline constexpr std::uint64_t kGetVehicleModVariation = 0XEFDD8C5443F6C9E4;

// --- Нарисованное в мире ------------------------------------------------------
//
// Маркер и контрольная точка ставятся сервером, а рисует их игра. Рисует
// по-разному, и это видно прямо здесь: у маркера один натив, зовущийся каждый
// кадр, а у точки — заведение, правка и снятие, потому что её игра помнит
// сама.

/// DRAW_MARKER, канонический хеш 0X28477EC23D892089.
inline constexpr std::uint64_t kDrawMarker = 0X7E763410A91A972B;

/// CREATE_CHECKPOINT, канонический хеш 0X0134F0835AB6BFCB.
inline constexpr std::uint64_t kCreateCheckpoint = 0XDCC9FF4954D6DCB1;

/// DELETE_CHECKPOINT, канонический хеш 0XF5ED37F54CD4D52E.
inline constexpr std::uint64_t kDeleteCheckpoint = 0XC2A5E7DCD1900AA1;

/// SET_CHECKPOINT_CYLINDER_HEIGHT, канонический хеш 0X2707AAE9D9297D89.
inline constexpr std::uint64_t kSetCheckpointCylinderHeight = 0XA7DD2E2BCBD9C8D5;

/// SET_CHECKPOINT_RGBA, канонический хеш 0X7167371E8AD747F7.
inline constexpr std::uint64_t kSetCheckpointRgba = 0XEDC9B904B870CE80;

/// _SET_CHECKPOINT_ICON_RGBA, канонический хеш 0XB9EA40907C680580.
inline constexpr std::uint64_t kSetCheckpointIconRgba = 0XF7408C8F340BA88B;

// --- Насадки на оружие --------------------------------------------------------
//
// Ставит их сервер, а надевает игра. Отдельного «снять все» у неё нет: снятая
// насадка снимается по одной, а проще всего оружие выдать заново.

/// GIVE_WEAPON_COMPONENT_TO_PED, канонический хеш 0XD966D51AA5B28BB9.
inline constexpr std::uint64_t kGiveWeaponComponentToPed = 0X6D5FA72F8C43D132;

/// REMOVE_WEAPON_COMPONENT_FROM_PED, канонический хеш 0X1E8BE90C74FB4C09.
inline constexpr std::uint64_t kRemoveWeaponComponentFromPed = 0X80E6FC2ACEAF8AA3;

/// SET_PED_WEAPON_TINT_INDEX, канонический хеш 0X50969B9B89ED5738.
inline constexpr std::uint64_t kSetPedWeaponTintIndex = 0XC37D2709B04BD397;

// --- Свои цвета машины --------------------------------------------------------
//
// Не из палитры, а красками. Игра держит их отдельно от номера цвета: краска
// перекрывает номер, и снимается она своим вызовом — оттого здесь и «снять», а
// не «покрасить в никакой».

/// SET_VEHICLE_CUSTOM_PRIMARY_COLOUR, канонический хеш 0X7141766F91D15BEA.
inline constexpr std::uint64_t kSetVehicleCustomPrimaryColour = 0X84F5FD9CD27457EE;

/// SET_VEHICLE_CUSTOM_SECONDARY_COLOUR, канонический хеш 0X36CED73BFED89754.
inline constexpr std::uint64_t kSetVehicleCustomSecondaryColour = 0X593A3115B8AE759B;

/// CLEAR_VEHICLE_CUSTOM_PRIMARY_COLOUR, канонический хеш 0X55E1D2758F34E437.
inline constexpr std::uint64_t kClearVehicleCustomPrimaryColour = 0X963D9A7202C06F65;

/// CLEAR_VEHICLE_CUSTOM_SECONDARY_COLOUR, канонический хеш 0X5FFBDEEC3E8E2009.
inline constexpr std::uint64_t kClearVehicleCustomSecondaryColour = 0X588D8FDC61F7CFAD;

/// _SET_PED_DECORATION, канонический хеш 0X5F5D1665E352A839.
///
/// Ставит татуировку: набор и рисунок в нём, оба хешем. У alt:V этот же натив
/// зовётся addPedDecorationFromHashes.
inline constexpr std::uint64_t kSetPedDecoration = 0X49A5A17556C31561;

/// CLEAR_PED_DECORATIONS, канонический хеш 0X0E5173C163976E38.
///
/// Снимает все разом: своего «сними одну» у игры нет.
inline constexpr std::uint64_t kClearPedDecorations = 0XA781062E77B54775;

/// _SET_PED_FACE_FEATURE, канонический хеш 0X71A5C1DBA060049E.
///
/// Двадцать подвижных черт лица: нос, скулы, подбородок и прочие. Довод дробный,
/// от минус единицы до единицы.
inline constexpr std::uint64_t kSetPedFaceFeature = 0XAAF9B08B469F707F;

/// SET_VEHICLE_DOORS_LOCKED, канонический хеш 0XB664292EAECF7FA6.
inline constexpr std::uint64_t kSetVehicleDoorsLocked = 0X0B74F181ADFC39BF;

// --- Взгляд -------------------------------------------------------------------
//
// Голова у персонажа поворачивается отдельно от тела, и задача взгляда —
// вторичная: она уживается с ходьбой, а не отменяет её.

/// TASK_LOOK_AT_COORD, канонический хеш 0X6FA46612594F7973.
inline constexpr std::uint64_t kTaskLookAtCoord = 0XE237FA90A8AFEE59;

} // namespace oxymp::client::game::natives
