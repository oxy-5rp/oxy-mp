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

/// WAIT, канонический хеш 0X4EDE34FBADD967A6.
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

/// SET_VEHICLE_FIXED, канонический хеш 0X115722B1B9C14C1C.
inline constexpr std::uint64_t kSetVehicleFixed = 0XF698038C13845696;

/// SET_VEHICLE_DIRT_LEVEL, канонический хеш 0X79D3B596FE44EE8B.
inline constexpr std::uint64_t kSetVehicleDirtLevel = 0X9452FE4900245259;

/// IS_MODEL_IN_CDIMAGE, канонический хеш 0X35B9E0803292B641.
///
/// Есть ли такая модель у игры вообще. Проверяется перед заказом: заказ
/// несуществующей модели ждёт её вечно.
inline constexpr std::uint64_t kIsModelInCdimage = 0XE7D342E0F16AAA8F;

/// IS_MODEL_A_VEHICLE, канонический хеш 0X19AAC8F07BFEC53E.
inline constexpr std::uint64_t kIsModelAVehicle = 0XAD1840C2E6AF7D5E;

/// SET_WEATHER_TYPE_NOW, канонический хеш 0XED712CA327900C8A.
inline constexpr std::uint64_t kSetWeatherTypeNow = 0XE38A58649E049502;

/// NETWORK_OVERRIDE_CLOCK_TIME, канонический хеш 0XE679E3E06E363892.
inline constexpr std::uint64_t kOverrideClockTime = 0XAFD3BC0F6EBB5474;

/// GET_GAMEPLAY_CAM_COORD, канонический хеш 0X14D6F5678D8F1B37.
///
/// Где стоит камера. Вместе с положением персонажа этого хватает, чтобы узнать,
/// куда она смотрит, не разбираясь в том, какой угол в каком порядке отдаёт игра.
inline constexpr std::uint64_t kGetGameplayCamCoord = 0XCF141FCD0940B0A3;
} // namespace oxymp::client::game::natives
