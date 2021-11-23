/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ParkFile.h"

#include "Cheats.h"
#include "Context.h"
#include "Editor.h"
#include "GameState.h"
#include "OpenRCT2.h"
#include "ParkImporter.h"
#include "Version.h"
#include "core/Console.hpp"
#include "core/Crypt.h"
#include "core/DataSerialiser.h"
#include "core/File.h"
#include "core/OrcaStream.hpp"
#include "core/Path.hpp"
#include "drawing/Drawing.h"
#include "interface/Viewport.h"
#include "interface/Window.h"
#include "localisation/Date.h"
#include "localisation/Localisation.h"
#include "management/Award.h"
#include "management/Finance.h"
#include "management/NewsItem.h"
#include "object/Object.h"
#include "object/ObjectManager.h"
#include "object/ObjectRepository.h"
#include "peep/RideUseSystem.h"
#include "peep/Staff.h"
#include "ride/ShopItem.h"
#include "ride/Vehicle.h"
#include "scenario/Scenario.h"
#include "scenario/ScenarioRepository.h"
#include "world/Balloon.h"
#include "world/Climate.h"
#include "world/Duck.h"
#include "world/EntityList.h"
#include "world/Entrance.h"
#include "world/Fountain.h"
#include "world/Litter.h"
#include "world/Map.h"
#include "world/MoneyEffect.h"
#include "world/Park.h"
#include "world/Particle.h"
#include "world/Sprite.h"

#include <cstdint>
#include <ctime>
#include <numeric>
#include <optional>
#include <string_view>
#include <vector>

using namespace OpenRCT2;

static std::string_view MapToNewObjectIdentifier(std::string_view s);
static std::optional<std::string_view> GetDATPathName(std::string_view newPathName);
static const FootpathMapping* GetFootpathMapping(const ObjectEntryDescriptor& desc);
static void UpdateFootpathsFromMapping(
    ObjectEntryIndex* pathToSurfaceMap, ObjectEntryIndex* pathToQueueSurfaceMap, ObjectEntryIndex* pathToRailingsMap,
    ObjectList& requiredObjects, ObjectEntryIndex& surfaceCount, ObjectEntryIndex& railingCount, ObjectEntryIndex entryIndex,
    const FootpathMapping* footpathMapping);

namespace OpenRCT2
{
    // Current version that is saved.
    constexpr uint32_t PARK_FILE_CURRENT_VERSION = 0x6;

    // The minimum version that is forwards compatible with the current version.
    constexpr uint32_t PARK_FILE_MIN_VERSION = 0x6;

    namespace ParkFileChunkType
    {
        // clang-format off
//      constexpr uint32_t RESERVED_0           = 0x00;
        constexpr uint32_t AUTHORING            = 0x01;
        constexpr uint32_t OBJECTS              = 0x02;
        constexpr uint32_t SCENARIO             = 0x03;
        constexpr uint32_t GENERAL              = 0x04;
        constexpr uint32_t CLIMATE              = 0x05;
        constexpr uint32_t PARK                 = 0x06;
//      constexpr uint32_t HISTORY              = 0x07;
        constexpr uint32_t RESEARCH             = 0x08;
        constexpr uint32_t NOTIFICATIONS        = 0x09;
        constexpr uint32_t INTERFACE            = 0x20;
        constexpr uint32_t TILES                = 0x30;
        constexpr uint32_t ENTITIES             = 0x31;
        constexpr uint32_t RIDES                = 0x32;
        constexpr uint32_t BANNERS              = 0x33;
//      constexpr uint32_t STAFF                = 0x35;
        constexpr uint32_t CHEATS               = 0x36;
        constexpr uint32_t RESTRICTED_OBJECTS   = 0x37;
        constexpr uint32_t PACKED_OBJECTS       = 0x80;
        // clang-format on
    }; // namespace ParkFileChunkType

    class ParkFile
    {
    public:
        ObjectList RequiredObjects;
        std::vector<const ObjectRepositoryItem*> ExportObjectsList;
        bool OmitTracklessRides{};

    private:
        std::unique_ptr<OrcaStream> _os;
        ObjectEntryIndex _pathToSurfaceMap[MAX_PATH_OBJECTS];
        ObjectEntryIndex _pathToQueueSurfaceMap[MAX_PATH_OBJECTS];
        ObjectEntryIndex _pathToRailingsMap[MAX_PATH_OBJECTS];

    public:
        void Load(const std::string_view& path)
        {
            FileStream fs(path, FILE_MODE_OPEN);
            Load(fs);
        }

        void Load(IStream& stream)
        {
            _os = std::make_unique<OrcaStream>(stream, OrcaStream::Mode::READING);
            RequiredObjects = {};
            ReadWriteObjectsChunk(*_os);
            ReadWritePackedObjectsChunk(*_os);
        }

        void Import()
        {
            auto& os = *_os;
            ReadWriteTilesChunk(os);
            ReadWriteBannersChunk(os);
            ReadWriteRidesChunk(os);
            ReadWriteEntitiesChunk(os);
            ReadWriteScenarioChunk(os);
            ReadWriteGeneralChunk(os);
            ReadWriteParkChunk(os);
            ReadWriteClimateChunk(os);
            ReadWriteResearchChunk(os);
            ReadWriteNotificationsChunk(os);
            ReadWriteInterfaceChunk(os);
            ReadWriteCheatsChunk(os);
            ReadWriteRestrictedObjectsChunk(os);
            if (os.GetHeader().TargetVersion < 0x4)
            {
                UpdateTrackElementsRideType();
            }

            // Initial cash will eventually be removed
            gInitialCash = gCash;
        }

        void Save(IStream& stream)
        {
            OrcaStream os(stream, OrcaStream::Mode::WRITING);

            auto& header = os.GetHeader();
            header.Magic = PARK_FILE_MAGIC;
            header.TargetVersion = PARK_FILE_CURRENT_VERSION;
            header.MinVersion = PARK_FILE_MIN_VERSION;

            ReadWriteAuthoringChunk(os);
            ReadWriteObjectsChunk(os);
            ReadWriteTilesChunk(os);
            ReadWriteBannersChunk(os);
            ReadWriteRidesChunk(os);
            ReadWriteEntitiesChunk(os);
            ReadWriteScenarioChunk(os);
            ReadWriteGeneralChunk(os);
            ReadWriteParkChunk(os);
            ReadWriteClimateChunk(os);
            ReadWriteResearchChunk(os);
            ReadWriteNotificationsChunk(os);
            ReadWriteInterfaceChunk(os);
            ReadWriteCheatsChunk(os);
            ReadWriteRestrictedObjectsChunk(os);
            ReadWritePackedObjectsChunk(os);
        }

        void Save(const std::string_view& path)
        {
            FileStream fs(path, FILE_MODE_WRITE);
            Save(fs);
        }

        scenario_index_entry ReadScenarioChunk()
        {
            scenario_index_entry entry{};
            auto& os = *_os;
            os.ReadWriteChunk(ParkFileChunkType::SCENARIO, [&entry](OrcaStream::ChunkStream& cs) {
                entry.category = cs.Read<uint8_t>();

                std::string name;
                ReadWriteStringTable(cs, name, "en-GB");
                String::Set(entry.name, sizeof(entry.name), name.c_str());
                String::Set(entry.internal_name, sizeof(entry.internal_name), name.c_str());

                std::string parkName;
                ReadWriteStringTable(cs, parkName, "en-GB");

                std::string scenarioDetails;
                ReadWriteStringTable(cs, scenarioDetails, "en-GB");
                String::Set(entry.details, sizeof(entry.details), scenarioDetails.c_str());

                entry.objective_type = cs.Read<uint8_t>();
                entry.objective_arg_1 = cs.Read<uint8_t>();
                entry.objective_arg_3 = cs.Read<int16_t>();
                entry.objective_arg_2 = cs.Read<int32_t>();

                entry.source_game = ScenarioSource::Other;
            });
            return entry;
        }

    private:
        static uint8_t GetMinCarsPerTrain(uint8_t value)
        {
            return value >> 4;
        }

        static uint8_t GetMaxCarsPerTrain(uint8_t value)
        {
            return value & 0xF;
        }

        void ReadWriteAuthoringChunk(OrcaStream& os)
        {
            // Write-only for now
            if (os.GetMode() == OrcaStream::Mode::WRITING)
            {
                os.ReadWriteChunk(ParkFileChunkType::AUTHORING, [](OrcaStream::ChunkStream& cs) {
                    cs.Write(std::string_view(gVersionInfoFull));
                    std::vector<std::string> authors;
                    cs.ReadWriteVector(authors, [](std::string& s) {});
                    cs.Write(std::string_view());                  // custom notes that can be attached to the save
                    cs.Write(static_cast<uint64_t>(std::time(0))); // date started
                    cs.Write(static_cast<uint64_t>(std::time(0))); // date modified
                });
            }
        }

        void ReadWriteObjectsChunk(OrcaStream& os)
        {
            static constexpr uint8_t DESCRIPTOR_NONE = 0;
            static constexpr uint8_t DESCRIPTOR_DAT = 1;
            static constexpr uint8_t DESCRIPTOR_JSON = 2;

            if (os.GetMode() == OrcaStream::Mode::READING)
            {
                std::fill(std::begin(_pathToSurfaceMap), std::end(_pathToSurfaceMap), OBJECT_ENTRY_INDEX_NULL);
                std::fill(std::begin(_pathToQueueSurfaceMap), std::end(_pathToQueueSurfaceMap), OBJECT_ENTRY_INDEX_NULL);
                std::fill(std::begin(_pathToRailingsMap), std::end(_pathToRailingsMap), OBJECT_ENTRY_INDEX_NULL);
                auto* pathToSurfaceMap = _pathToSurfaceMap;
                auto* pathToQueueSurfaceMap = _pathToQueueSurfaceMap;
                auto* pathToRailingsMap = _pathToRailingsMap;
                const auto version = os.GetHeader().TargetVersion;

                ObjectList requiredObjects;
                os.ReadWriteChunk(
                    ParkFileChunkType::OBJECTS,
                    [&requiredObjects, pathToSurfaceMap, pathToQueueSurfaceMap, pathToRailingsMap,
                     version](OrcaStream::ChunkStream& cs) {
                        ObjectEntryIndex surfaceCount = 0;
                        ObjectEntryIndex railingsCount = 0;
                        auto numSubLists = cs.Read<uint16_t>();
                        for (size_t i = 0; i < numSubLists; i++)
                        {
                            auto objectType = static_cast<ObjectType>(cs.Read<uint16_t>());
                            auto subListSize = static_cast<ObjectEntryIndex>(cs.Read<uint32_t>());
                            for (ObjectEntryIndex j = 0; j < subListSize; j++)
                            {
                                auto kind = cs.Read<uint8_t>();

                                switch (kind)
                                {
                                    case DESCRIPTOR_NONE:
                                        break;
                                    case DESCRIPTOR_DAT:
                                    {
                                        rct_object_entry datEntry;
                                        cs.Read(&datEntry, sizeof(datEntry));
                                        ObjectEntryDescriptor desc(datEntry);
                                        if (version <= 2 && datEntry.GetType() == ObjectType::Paths)
                                        {
                                            auto footpathMapping = GetFootpathMapping(desc);
                                            if (footpathMapping != nullptr)
                                            {
                                                UpdateFootpathsFromMapping(
                                                    pathToSurfaceMap, pathToQueueSurfaceMap, pathToRailingsMap, requiredObjects,
                                                    surfaceCount, railingsCount, j, footpathMapping);

                                                continue;
                                            }
                                        }

                                        requiredObjects.SetObject(j, desc);
                                        break;
                                    }
                                    case DESCRIPTOR_JSON:
                                    {
                                        ObjectEntryDescriptor desc;
                                        desc.Type = objectType;
                                        desc.Identifier = MapToNewObjectIdentifier(cs.Read<std::string>());
                                        desc.Version = cs.Read<std::string>();

                                        if (version <= 2)
                                        {
                                            auto footpathMapping = GetFootpathMapping(desc);
                                            if (footpathMapping != nullptr)
                                            {
                                                // We have surface objects for this footpath
                                                UpdateFootpathsFromMapping(
                                                    pathToSurfaceMap, pathToQueueSurfaceMap, pathToRailingsMap, requiredObjects,
                                                    surfaceCount, railingsCount, j, footpathMapping);

                                                continue;
                                            }
                                        }

                                        requiredObjects.SetObject(j, desc);
                                        break;
                                    }
                                    default:
                                        throw std::runtime_error("Unknown object descriptor kind.");
                                }
                            }
                        }
                    });
                RequiredObjects = std::move(requiredObjects);
            }
            else
            {
                os.ReadWriteChunk(ParkFileChunkType::OBJECTS, [](OrcaStream::ChunkStream& cs) {
                    auto& objManager = GetContext()->GetObjectManager();
                    auto objectList = objManager.GetLoadedObjects();

                    // Write number of object sub lists
                    cs.Write(static_cast<uint16_t>(ObjectType::Count));
                    for (auto objectType = ObjectType::Ride; objectType < ObjectType::Count; objectType++)
                    {
                        // Write sub list
                        const auto& list = objectList.GetList(objectType);
                        cs.Write(static_cast<uint16_t>(objectType));
                        cs.Write(static_cast<uint32_t>(list.size()));
                        for (const auto& entry : list)
                        {
                            if (entry.HasValue())
                            {
                                if (entry.Generation == ObjectGeneration::JSON)
                                {
                                    cs.Write(DESCRIPTOR_JSON);
                                    cs.Write(entry.Identifier);
                                    cs.Write(""); // reserved for version
                                }
                                else
                                {
                                    cs.Write(DESCRIPTOR_DAT);
                                    cs.Write(&entry.Entry, sizeof(rct_object_entry));
                                }
                            }
                            else
                            {
                                cs.Write(DESCRIPTOR_NONE);
                            }
                        }
                    }
                });
            }
        }

        void ReadWriteScenarioChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::SCENARIO, [&os](OrcaStream::ChunkStream& cs) {
                cs.ReadWrite(gScenarioCategory);
                ReadWriteStringTable(cs, gScenarioName, "en-GB");

                auto& park = GetContext()->GetGameState()->GetPark();
                ReadWriteStringTable(cs, park.Name, "en-GB");

                ReadWriteStringTable(cs, gScenarioDetails, "en-GB");

                cs.ReadWrite(gScenarioObjective.Type);
                cs.ReadWrite(gScenarioObjective.Year);
                cs.ReadWrite(gScenarioObjective.NumGuests);
                cs.ReadWrite(gScenarioObjective.Currency);

                cs.ReadWrite(gScenarioParkRatingWarningDays);

                cs.ReadWrite(gScenarioCompletedCompanyValue);
                if (gScenarioCompletedCompanyValue == MONEY64_UNDEFINED
                    || gScenarioCompletedCompanyValue == COMPANY_VALUE_ON_FAILED_OBJECTIVE)
                {
                    cs.Write("");
                }
                else
                {
                    cs.ReadWrite(gScenarioCompletedBy);
                }

                if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    auto earlyCompletion = cs.Read<bool>();
                    if (network_get_mode() == NETWORK_MODE_CLIENT)
                    {
                        gAllowEarlyCompletionInNetworkPlay = earlyCompletion;
                    }
                }
                else
                {
                    cs.Write(AllowEarlyCompletion());
                }

                if (os.GetHeader().TargetVersion >= 1)
                {
                    cs.ReadWrite(gScenarioFileName);
                }
            });
        }

        void ReadWriteGeneralChunk(OrcaStream& os)
        {
            auto found = os.ReadWriteChunk(ParkFileChunkType::GENERAL, [this](OrcaStream::ChunkStream& cs) {
                cs.ReadWrite(gGamePaused);
                cs.ReadWrite(gCurrentTicks);
                cs.ReadWrite(gDateMonthTicks);
                cs.ReadWrite(gDateMonthsElapsed);

                if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    uint32_t s0{}, s1{};
                    cs.ReadWrite(s0);
                    cs.ReadWrite(s1);
                    Random::Rct2::Seed s{ s0, s1 };
                    gScenarioRand.seed(s);
                }
                else
                {
                    auto randState = gScenarioRand.state();
                    cs.Write(randState.s0);
                    cs.Write(randState.s1);
                }

                cs.ReadWrite(gGuestInitialHappiness);
                cs.ReadWrite(gGuestInitialCash);
                cs.ReadWrite(gGuestInitialHunger);
                cs.ReadWrite(gGuestInitialThirst);

                cs.ReadWrite(gNextGuestNumber);
                cs.ReadWriteVector(gPeepSpawns, [&cs](PeepSpawn& spawn) {
                    cs.ReadWrite(spawn.x);
                    cs.ReadWrite(spawn.y);
                    cs.ReadWrite(spawn.z);
                    cs.ReadWrite(spawn.direction);
                });

                cs.ReadWrite(gLandPrice);
                cs.ReadWrite(gConstructionRightsPrice);
                cs.ReadWrite(gGrassSceneryTileLoopPosition);
                cs.ReadWrite(gWidePathTileLoopPosition);

                ReadWriteRideRatingCalculationData(cs, gRideRatingUpdateState);
            });
            if (!found)
            {
                throw std::runtime_error("No general chunk found.");
            }
        }

        void ReadWriteRideRatingCalculationData(OrcaStream::ChunkStream& cs, RideRatingUpdateState& calcData)
        {
            cs.ReadWrite(calcData.AmountOfBrakes);
            cs.ReadWrite(calcData.Proximity);
            cs.ReadWrite(calcData.ProximityStart);
            cs.ReadWrite(calcData.CurrentRide);
            cs.ReadWrite(calcData.State);
            cs.ReadWrite(calcData.ProximityTrackType);
            cs.ReadWrite(calcData.ProximityBaseHeight);
            cs.ReadWrite(calcData.ProximityTotal);
            cs.ReadWriteArray(calcData.ProximityScores, [&cs](uint16_t& value) {
                cs.ReadWrite(value);
                return true;
            });
            cs.ReadWrite(calcData.AmountOfBrakes);
            cs.ReadWrite(calcData.AmountOfReversers);
            cs.ReadWrite(calcData.StationFlags);
        }

        void ReadWriteInterfaceChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::INTERFACE, [](OrcaStream::ChunkStream& cs) {
                cs.ReadWrite(gSavedView.x);
                cs.ReadWrite(gSavedView.y);
                if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    auto savedZoomlevel = static_cast<ZoomLevel>(cs.Read<int8_t>());
                    gSavedViewZoom = std::clamp(savedZoomlevel, ZoomLevel::min(), ZoomLevel::max());
                }
                else
                {
                    cs.Write(static_cast<int8_t>(gSavedViewZoom));
                }
                cs.ReadWrite(gSavedViewRotation);
                cs.ReadWrite(gLastEntranceStyle);
                cs.ReadWrite(gEditorStep);
            });
        }

        void ReadWriteCheatsChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::CHEATS, [](OrcaStream::ChunkStream& cs) {
                DataSerialiser ds(cs.GetMode() == OrcaStream::Mode::WRITING, cs.GetStream());
                CheatsSerialise(ds);
            });
        }

        void ReadWriteRestrictedObjectsChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::RESTRICTED_OBJECTS, [](OrcaStream::ChunkStream& cs) {
                auto& restrictedScenery = GetRestrictedScenery();

                // We are want to support all object types in the future, so convert scenery type
                // to object type when we write the list
                cs.ReadWriteVector(restrictedScenery, [&cs](ScenerySelection& item) {
                    if (cs.GetMode() == OrcaStream::Mode::READING)
                    {
                        item.SceneryType = GetSceneryTypeFromObjectType(static_cast<ObjectType>(cs.Read<uint16_t>()));
                        item.EntryIndex = cs.Read<ObjectEntryIndex>();
                    }
                    else
                    {
                        cs.Write(static_cast<uint16_t>(GetObjectTypeFromSceneryType(item.SceneryType)));
                        cs.Write(item.EntryIndex);
                    }
                });
            });
        }

        void ReadWritePackedObjectsChunk(OrcaStream& os)
        {
            static constexpr uint8_t DESCRIPTOR_DAT = 0;
            static constexpr uint8_t DESCRIPTOR_PARKOBJ = 1;

            if (os.GetMode() == OrcaStream::Mode::WRITING && ExportObjectsList.size() == 0)
            {
                // Do not emit chunk if there are no packed objects
                return;
            }

            os.ReadWriteChunk(ParkFileChunkType::PACKED_OBJECTS, [this](OrcaStream::ChunkStream& cs) {
                if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    auto& objRepository = GetContext()->GetObjectRepository();
                    auto numObjects = cs.Read<uint32_t>();
                    for (uint32_t i = 0; i < numObjects; i++)
                    {
                        auto type = cs.Read<uint8_t>();
                        if (type == DESCRIPTOR_DAT)
                        {
                            rct_object_entry entry;
                            cs.Read(&entry, sizeof(entry));
                            auto size = cs.Read<uint32_t>();
                            std::vector<uint8_t> data;
                            data.resize(size);
                            cs.Read(data.data(), data.size());

                            auto legacyIdentifier = entry.GetName();
                            if (objRepository.FindObjectLegacy(legacyIdentifier) == nullptr)
                            {
                                objRepository.AddObjectFromFile(
                                    ObjectGeneration::DAT, legacyIdentifier, data.data(), data.size());
                            }
                        }
                        else if (type == DESCRIPTOR_PARKOBJ)
                        {
                            auto identifier = cs.Read<std::string>();
                            auto size = cs.Read<uint32_t>();
                            std::vector<uint8_t> data;
                            data.resize(size);
                            cs.Read(data.data(), data.size());
                            if (objRepository.FindObject(identifier) == nullptr)
                            {
                                objRepository.AddObjectFromFile(ObjectGeneration::JSON, identifier, data.data(), data.size());
                            }
                        }
                        else
                        {
                            throw std::runtime_error("Unsupported packed object");
                        }
                    }
                }
                else
                {
                    auto& stream = cs.GetStream();
                    auto countPosition = stream.GetPosition();

                    // Write placeholder count, update later
                    uint32_t count = 0;
                    cs.Write(count);

                    // Write objects
                    for (const auto* ori : ExportObjectsList)
                    {
                        auto extension = Path::GetExtension(ori->Path);
                        if (String::Equals(extension, ".dat", true))
                        {
                            cs.Write(DESCRIPTOR_DAT);
                            cs.Write(&ori->ObjectEntry, sizeof(rct_object_entry));
                        }
                        else if (String::Equals(extension, ".parkobj", true))
                        {
                            cs.Write(DESCRIPTOR_PARKOBJ);
                            cs.Write(ori->Identifier);
                        }
                        else
                        {
                            Console::WriteLine("%s not packed: unsupported extension.", ori->Identifier.c_str());
                            continue;
                        }

                        auto data = File::ReadAllBytes(ori->Path);
                        cs.Write<uint32_t>(static_cast<uint32_t>(data.size()));
                        cs.Write(data.data(), data.size());
                        count++;
                    }

                    auto backupPosition = stream.GetPosition();
                    stream.SetPosition(countPosition);
                    cs.Write(count);
                    stream.SetPosition(backupPosition);
                }
            });
        }

        void ReadWriteClimateChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::CLIMATE, [](OrcaStream::ChunkStream& cs) {
                cs.ReadWrite(gClimate);
                cs.ReadWrite(gClimateUpdateTimer);

                for (auto* cl : { &gClimateCurrent, &gClimateNext })
                {
                    cs.ReadWrite(cl->Weather);
                    cs.ReadWrite(cl->Temperature);
                    cs.ReadWrite(cl->WeatherEffect);
                    cs.ReadWrite(cl->WeatherGloom);
                    cs.ReadWrite(cl->Level);
                }
            });
        }

        void ReadWriteParkChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::PARK, [](OrcaStream::ChunkStream& cs) {
                auto& park = GetContext()->GetGameState()->GetPark();
                cs.ReadWrite(park.Name);
                cs.ReadWrite(gCash);
                cs.ReadWrite(gBankLoan);
                cs.ReadWrite(gMaxBankLoan);
                cs.ReadWrite(gBankLoanInterestRate);
                cs.ReadWrite(gParkFlags);
                cs.ReadWrite(gParkEntranceFee);
                cs.ReadWrite(gStaffHandymanColour);
                cs.ReadWrite(gStaffMechanicColour);
                cs.ReadWrite(gStaffSecurityColour);
                cs.ReadWrite(gSamePriceThroughoutPark);

                // Finances
                if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    auto numMonths = std::min<uint32_t>(EXPENDITURE_TABLE_MONTH_COUNT, cs.Read<uint32_t>());
                    auto numTypes = std::min<uint32_t>(static_cast<uint32_t>(ExpenditureType::Count), cs.Read<uint32_t>());
                    for (uint32_t i = 0; i < numMonths; i++)
                    {
                        for (uint32_t j = 0; j < numTypes; j++)
                        {
                            gExpenditureTable[i][j] = cs.Read<money64>();
                        }
                    }
                }
                else
                {
                    auto numMonths = static_cast<uint32_t>(EXPENDITURE_TABLE_MONTH_COUNT);
                    auto numTypes = static_cast<uint32_t>(ExpenditureType::Count);

                    cs.Write(numMonths);
                    cs.Write(numTypes);
                    for (uint32_t i = 0; i < numMonths; i++)
                    {
                        for (uint32_t j = 0; j < numTypes; j++)
                        {
                            cs.Write(gExpenditureTable[i][j]);
                        }
                    }
                }
                cs.ReadWrite(gHistoricalProfit);

                // Marketing
                cs.ReadWriteVector(gMarketingCampaigns, [&cs](MarketingCampaign& campaign) {
                    cs.ReadWrite(campaign.Type);
                    cs.ReadWrite(campaign.WeeksLeft);
                    cs.ReadWrite(campaign.Flags);
                    cs.ReadWrite(campaign.RideId);
                });

                // Awards
                cs.ReadWriteArray(gCurrentAwards, [&cs](Award& award) {
                    if (award.Time != 0)
                    {
                        cs.ReadWrite(award.Time);
                        cs.ReadWrite(award.Type);
                        return true;
                    }

                    return false;
                });

                cs.ReadWrite(gParkValue);
                cs.ReadWrite(gCompanyValue);
                cs.ReadWrite(gParkSize);
                cs.ReadWrite(gNumGuestsInPark);
                cs.ReadWrite(gNumGuestsHeadingForPark);
                cs.ReadWrite(gParkRating);
                cs.ReadWrite(gParkRatingCasualtyPenalty);
                cs.ReadWrite(gCurrentExpenditure);
                cs.ReadWrite(gCurrentProfit);
                cs.ReadWrite(gWeeklyProfitAverageDividend);
                cs.ReadWrite(gWeeklyProfitAverageDivisor);
                cs.ReadWrite(gTotalAdmissions);
                cs.ReadWrite(gTotalIncomeFromAdmissions);
                cs.ReadWrite(gTotalRideValueForMoney);
                cs.ReadWrite(gNumGuestsInParkLastWeek);
                cs.ReadWrite(gGuestChangeModifier);
                cs.ReadWrite(_guestGenerationProbability);
                cs.ReadWrite(_suggestedGuestMaximum);

                cs.ReadWriteArray(gPeepWarningThrottle, [&cs](uint8_t& value) {
                    cs.ReadWrite(value);
                    return true;
                });

                cs.ReadWriteArray(gParkRatingHistory, [&cs](uint8_t& value) {
                    cs.ReadWrite(value);
                    return true;
                });

                cs.ReadWriteArray(gGuestsInParkHistory, [&cs](uint32_t& value) {
                    cs.ReadWrite(value);
                    return true;
                });

                cs.ReadWriteArray(gCashHistory, [&cs](money64& value) {
                    cs.ReadWrite(value);
                    return true;
                });
                cs.ReadWriteArray(gWeeklyProfitHistory, [&cs](money64& value) {
                    cs.ReadWrite(value);
                    return true;
                });
                cs.ReadWriteArray(gParkValueHistory, [&cs](money64& value) {
                    cs.ReadWrite(value);
                    return true;
                });
            });
        }

        void ReadWriteResearchChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::RESEARCH, [](OrcaStream::ChunkStream& cs) {
                // Research status
                cs.ReadWrite(gResearchFundingLevel);
                cs.ReadWrite(gResearchPriorities);
                cs.ReadWrite(gResearchProgressStage);
                cs.ReadWrite(gResearchProgress);
                cs.ReadWrite(gResearchExpectedMonth);
                cs.ReadWrite(gResearchExpectedDay);
                ReadWriteResearchItem(cs, gResearchLastItem);
                ReadWriteResearchItem(cs, gResearchNextItem);

                // Invention list
                cs.ReadWriteVector(gResearchItemsUninvented, [&cs](ResearchItem& item) { ReadWriteResearchItem(cs, item); });
                cs.ReadWriteVector(gResearchItemsInvented, [&cs](ResearchItem& item) { ReadWriteResearchItem(cs, item); });
            });
        }

        static void ReadWriteResearchItem(OrcaStream::ChunkStream& cs, std::optional<ResearchItem>& item)
        {
            if (cs.GetMode() == OrcaStream::Mode::READING)
            {
                auto hasValue = cs.Read<bool>();
                if (hasValue)
                {
                    ResearchItem placeholder;
                    ReadWriteResearchItem(cs, placeholder);
                    item = placeholder;
                }
            }
            else
            {
                if (item)
                {
                    cs.Write<bool>(true);
                    ReadWriteResearchItem(cs, *item);
                }
                else
                {
                    cs.Write<bool>(false);
                }
            }
        }

        static void ReadWriteResearchItem(OrcaStream::ChunkStream& cs, ResearchItem& item)
        {
            cs.ReadWrite(item.type);
            cs.ReadWrite(item.baseRideType);
            cs.ReadWrite(item.entryIndex);
            cs.ReadWrite(item.flags);
            cs.ReadWrite(item.category);
        }

        void ReadWriteNotificationsChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::NOTIFICATIONS, [](OrcaStream::ChunkStream& cs) {
                if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    gNewsItems.Clear();

                    std::vector<News::Item> recent;
                    cs.ReadWriteVector(recent, [&cs](News::Item& item) { ReadWriteNewsItem(cs, item); });
                    for (size_t i = 0; i < std::min<size_t>(recent.size(), News::ItemHistoryStart); i++)
                    {
                        gNewsItems[i] = recent[i];
                    }

                    std::vector<News::Item> archived;
                    cs.ReadWriteVector(archived, [&cs](News::Item& item) { ReadWriteNewsItem(cs, item); });
                    size_t offset = News::ItemHistoryStart;
                    for (size_t i = 0; i < std::min<size_t>(archived.size(), News::MaxItemsArchive); i++)
                    {
                        gNewsItems[offset + i] = archived[i];
                    }
                }
                else
                {
                    std::vector<News::Item> recent(std::begin(gNewsItems.GetRecent()), std::end(gNewsItems.GetRecent()));
                    cs.ReadWriteVector(recent, [&cs](News::Item& item) { ReadWriteNewsItem(cs, item); });

                    std::vector<News::Item> archived(std::begin(gNewsItems.GetArchived()), std::end(gNewsItems.GetArchived()));
                    cs.ReadWriteVector(archived, [&cs](News::Item& item) { ReadWriteNewsItem(cs, item); });
                }
            });
        }

        static void ReadWriteNewsItem(OrcaStream::ChunkStream& cs, News::Item& item)
        {
            cs.ReadWrite(item.Type);
            cs.ReadWrite(item.Flags);
            cs.ReadWrite(item.Assoc);
            cs.ReadWrite(item.Ticks);
            cs.ReadWrite(item.MonthYear);
            cs.ReadWrite(item.Day);
            if (cs.GetMode() == OrcaStream::Mode::READING)
            {
                auto s = cs.Read<std::string>();
                item.Text = s;
            }
            else
            {
                cs.Write(std::string_view(item.Text));
            }
        }

        void ReadWriteTilesChunk(OrcaStream& os)
        {
            auto* pathToSurfaceMap = _pathToSurfaceMap;
            auto* pathToQueueSurfaceMap = _pathToQueueSurfaceMap;
            auto* pathToRailingsMap = _pathToRailingsMap;

            auto found = os.ReadWriteChunk(
                ParkFileChunkType::TILES,
                [pathToSurfaceMap, pathToQueueSurfaceMap, pathToRailingsMap](OrcaStream::ChunkStream& cs) {
                    cs.ReadWrite(gMapSize); // x
                    cs.Write(gMapSize);     // y

                    if (cs.GetMode() == OrcaStream::Mode::READING)
                    {
                        OpenRCT2::GetContext()->GetGameState()->InitAll(gMapSize);

                        auto numElements = cs.Read<uint32_t>();

                        std::vector<TileElement> tileElements;
                        tileElements.resize(numElements);
                        cs.Read(tileElements.data(), tileElements.size() * sizeof(TileElement));
                        SetTileElements(std::move(tileElements));
                        {
                            tile_element_iterator it;
                            tile_element_iterator_begin(&it);
                            while (tile_element_iterator_next(&it))
                            {
                                if (it.element->GetType() == TILE_ELEMENT_TYPE_PATH)
                                {
                                    auto* pathElement = it.element->AsPath();
                                    if (pathElement->HasLegacyPathEntry())
                                    {
                                        auto pathEntryIndex = pathElement->GetLegacyPathEntryIndex();
                                        if (pathToRailingsMap[pathEntryIndex] != OBJECT_ENTRY_INDEX_NULL)
                                        {
                                            if (pathElement->IsQueue())
                                                pathElement->SetSurfaceEntryIndex(pathToQueueSurfaceMap[pathEntryIndex]);
                                            else
                                                pathElement->SetSurfaceEntryIndex(pathToSurfaceMap[pathEntryIndex]);

                                            pathElement->SetRailingsEntryIndex(pathToRailingsMap[pathEntryIndex]);
                                        }
                                    }
                                }
                            }
                        }
                        UpdateParkEntranceLocations();
                    }
                    else
                    {
                        auto tileElements = GetReorganisedTileElementsWithoutGhosts();
                        cs.Write(static_cast<uint32_t>(tileElements.size()));
                        cs.Write(tileElements.data(), tileElements.size() * sizeof(TileElement));
                    }
                });
            if (!found)
            {
                throw std::runtime_error("No tiles chunk found.");
            }
        }

        void UpdateTrackElementsRideType()
        {
            for (int32_t x = 0; x < MAXIMUM_MAP_SIZE_TECHNICAL; x++)
            {
                for (int32_t y = 0; y < MAXIMUM_MAP_SIZE_TECHNICAL; y++)
                {
                    TileElement* tileElement = map_get_first_element_at(TileCoordsXY{ x, y });
                    if (tileElement == nullptr)
                        continue;
                    do
                    {
                        if (tileElement->GetType() != TILE_ELEMENT_TYPE_TRACK)
                            continue;

                        auto* trackElement = tileElement->AsTrack();
                        const auto* ride = get_ride(trackElement->GetRideIndex());
                        if (ride != nullptr)
                        {
                            trackElement->SetRideType(ride->type);
                        }

                    } while (!(tileElement++)->IsLastForTile());
                }
            }
        }

        void ReadWriteBannersChunk(OrcaStream& os)
        {
            os.ReadWriteChunk(ParkFileChunkType::BANNERS, [&os](OrcaStream::ChunkStream& cs) {
                auto version = os.GetHeader().TargetVersion;
                if (cs.GetMode() == OrcaStream::Mode::WRITING)
                {
                    auto numBanners = GetNumBanners();
                    cs.Write(static_cast<uint32_t>(numBanners));

                    [[maybe_unused]] size_t numWritten = 0;
                    for (BannerIndex i = 0; i < MAX_BANNERS; i++)
                    {
                        auto banner = GetBanner(i);
                        if (banner != nullptr)
                        {
                            ReadWriteBanner(version, cs, *banner);
                            numWritten++;
                        }
                    }

                    assert(numWritten == numBanners);
                }
                else if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    if (version == 0)
                    {
                        std::vector<Banner> banners;
                        cs.ReadWriteVector(banners, [version, &cs](Banner& banner) { ReadWriteBanner(version, cs, banner); });
                        for (size_t i = 0; i < banners.size(); i++)
                        {
                            auto bannerIndex = static_cast<BannerIndex>(i);
                            auto banner = GetOrCreateBanner(bannerIndex);
                            if (banner != nullptr)
                            {
                                *banner = std::move(banners[i]);
                                banner->id = bannerIndex;
                            }
                        }
                    }
                    else
                    {
                        auto numBanners = cs.Read<uint32_t>();
                        for (size_t i = 0; i < numBanners; i++)
                        {
                            Banner readBanner;
                            ReadWriteBanner(version, cs, readBanner);

                            auto banner = GetOrCreateBanner(readBanner.id);
                            if (banner == nullptr)
                            {
                                throw std::runtime_error("Invalid banner index");
                            }
                            else
                            {
                                *banner = std::move(readBanner);
                            }
                        }
                    }
                }
            });
        }

        static void ReadWriteBanner(uint32_t version, OrcaStream::ChunkStream& cs, Banner& banner)
        {
            if (version >= 1)
            {
                cs.ReadWrite(banner.id);
            }
            cs.ReadWrite(banner.type);
            cs.ReadWrite(banner.flags);
            cs.ReadWrite(banner.text);
            cs.ReadWrite(banner.colour);
            cs.ReadWrite(banner.ride_index);
            cs.ReadWrite(banner.text_colour);
            cs.ReadWrite(banner.position.x);
            cs.ReadWrite(banner.position.y);
        }

        void ReadWriteRidesChunk(OrcaStream& os)
        {
            const auto version = os.GetHeader().TargetVersion;
            os.ReadWriteChunk(ParkFileChunkType::RIDES, [this, &version](OrcaStream::ChunkStream& cs) {
                std::vector<ride_id_t> rideIds;
                if (cs.GetMode() == OrcaStream::Mode::READING)
                {
                    ride_init_all();
                }
                else
                {
                    if (OmitTracklessRides)
                    {
                        auto tracklessRides = GetTracklessRides();
                        for (const auto& ride : GetRideManager())
                        {
                            auto it = std::find(tracklessRides.begin(), tracklessRides.end(), ride.id);
                            if (it == tracklessRides.end())
                            {
                                rideIds.push_back(ride.id);
                            }
                        }
                    }
                    else
                    {
                        for (const auto& ride : GetRideManager())
                        {
                            rideIds.push_back(ride.id);
                        }
                    }
                }
                cs.ReadWriteVector(rideIds, [&cs, &version](ride_id_t& rideId) {
                    // Ride ID
                    cs.ReadWrite(rideId);

                    auto& ride = *GetOrAllocateRide(rideId);

                    // Status
                    cs.ReadWrite(ride.type);
                    cs.ReadWrite(ride.subtype);
                    cs.ReadWrite(ride.mode);
                    cs.ReadWrite(ride.status);
                    cs.ReadWrite(ride.depart_flags);
                    cs.ReadWrite(ride.lifecycle_flags);

                    // Meta
                    cs.ReadWrite(ride.custom_name);
                    cs.ReadWrite(ride.default_name_number);

                    cs.ReadWriteArray(ride.price, [&cs](money16& price) {
                        cs.ReadWrite(price);
                        return true;
                    });

                    // Colours
                    cs.ReadWrite(ride.entrance_style);
                    cs.ReadWrite(ride.colour_scheme_type);
                    cs.ReadWriteArray(ride.track_colour, [&cs](TrackColour& tc) {
                        cs.ReadWrite(tc.main);
                        cs.ReadWrite(tc.additional);
                        cs.ReadWrite(tc.supports);
                        return true;
                    });

                    cs.ReadWriteArray(ride.vehicle_colours, [&cs](VehicleColour& vc) {
                        cs.ReadWrite(vc.Body);
                        cs.ReadWrite(vc.Trim);
                        cs.ReadWrite(vc.Ternary);
                        return true;
                    });

                    // Stations
                    cs.ReadWrite(ride.num_stations);
                    cs.ReadWriteArray(ride.stations, [&cs](RideStation& station) {
                        cs.ReadWrite(station.Start);
                        cs.ReadWrite(station.Height);
                        cs.ReadWrite(station.Length);
                        cs.ReadWrite(station.Depart);
                        cs.ReadWrite(station.TrainAtStation);
                        cs.ReadWrite(station.Entrance);
                        cs.ReadWrite(station.Exit);
                        cs.ReadWrite(station.SegmentLength);
                        cs.ReadWrite(station.SegmentTime);
                        cs.ReadWrite(station.QueueTime);
                        cs.ReadWrite(station.QueueLength);
                        cs.ReadWrite(station.LastPeepInQueue);
                        return true;
                    });

                    cs.ReadWrite(ride.overall_view.x);
                    cs.ReadWrite(ride.overall_view.y);

                    // Vehicles
                    cs.ReadWrite(ride.num_vehicles);
                    cs.ReadWrite(ride.num_cars_per_train);
                    cs.ReadWrite(ride.proposed_num_vehicles);
                    cs.ReadWrite(ride.proposed_num_cars_per_train);
                    cs.ReadWrite(ride.max_trains);
                    if (version < 0x5)
                    {
                        uint8_t value;
                        cs.ReadWrite(value);
                        ride.MinCarsPerTrain = GetMinCarsPerTrain(value);
                        ride.MaxCarsPerTrain = GetMaxCarsPerTrain(value);
                    }
                    else
                    {
                        cs.ReadWrite(ride.MinCarsPerTrain);
                        cs.ReadWrite(ride.MaxCarsPerTrain);
                    }

                    cs.ReadWrite(ride.min_waiting_time);
                    cs.ReadWrite(ride.max_waiting_time);
                    cs.ReadWriteArray(ride.vehicles, [&cs](uint16_t& v) {
                        cs.ReadWrite(v);
                        return true;
                    });

                    // Operation
                    cs.ReadWrite(ride.operation_option);
                    cs.ReadWrite(ride.lift_hill_speed);
                    cs.ReadWrite(ride.num_circuits);

                    // Special
                    cs.ReadWrite(ride.boat_hire_return_direction);
                    cs.ReadWrite(ride.boat_hire_return_position);
                    cs.ReadWrite(ride.ChairliftBullwheelLocation[0]);
                    cs.ReadWrite(ride.ChairliftBullwheelLocation[1]);
                    cs.ReadWrite(ride.chairlift_bullwheel_rotation);
                    cs.ReadWrite(ride.slide_in_use);
                    cs.ReadWrite(ride.slide_peep);
                    cs.ReadWrite(ride.slide_peep_t_shirt_colour);
                    cs.ReadWrite(ride.spiral_slide_progress);
                    cs.ReadWrite(ride.race_winner);
                    cs.ReadWrite(ride.cable_lift);
                    cs.ReadWrite(ride.CableLiftLoc);

                    // Stats
                    if (cs.GetMode() == OrcaStream::Mode::READING)
                    {
                        auto hasMeasurement = cs.Read<uint8_t>();
                        if (hasMeasurement != 0)
                        {
                            ride.measurement = std::make_unique<RideMeasurement>();
                            ReadWriteRideMeasurement(cs, *ride.measurement);
                        }
                    }
                    else
                    {
                        if (ride.measurement == nullptr)
                        {
                            cs.Write<uint8_t>(0);
                        }
                        else
                        {
                            cs.Write<uint8_t>(1);
                            ReadWriteRideMeasurement(cs, *ride.measurement);
                        }
                    }

                    cs.ReadWrite(ride.special_track_elements);
                    cs.ReadWrite(ride.max_speed);
                    cs.ReadWrite(ride.average_speed);
                    cs.ReadWrite(ride.current_test_segment);
                    cs.ReadWrite(ride.average_speed_test_timeout);

                    cs.ReadWrite(ride.max_positive_vertical_g);
                    cs.ReadWrite(ride.max_negative_vertical_g);
                    cs.ReadWrite(ride.max_lateral_g);
                    cs.ReadWrite(ride.previous_vertical_g);
                    cs.ReadWrite(ride.previous_lateral_g);

                    cs.ReadWrite(ride.testing_flags);
                    cs.ReadWrite(ride.CurTestTrackLocation);

                    cs.ReadWrite(ride.turn_count_default);
                    cs.ReadWrite(ride.turn_count_banked);
                    cs.ReadWrite(ride.turn_count_sloped);

                    cs.ReadWrite(ride.inversions);
                    cs.ReadWrite(ride.drops);
                    cs.ReadWrite(ride.start_drop_height);
                    cs.ReadWrite(ride.highest_drop_height);
                    cs.ReadWrite(ride.sheltered_length);
                    cs.ReadWrite(ride.var_11C);
                    cs.ReadWrite(ride.num_sheltered_sections);
                    if (version > 5)
                    {
                        cs.ReadWrite(ride.sheltered_eighths);
                        cs.ReadWrite(ride.holes);
                    }
                    cs.ReadWrite(ride.current_test_station);
                    cs.ReadWrite(ride.num_block_brakes);
                    cs.ReadWrite(ride.total_air_time);

                    cs.ReadWrite(ride.excitement);
                    cs.ReadWrite(ride.intensity);
                    cs.ReadWrite(ride.nausea);

                    cs.ReadWrite(ride.value);

                    cs.ReadWrite(ride.num_riders);
                    cs.ReadWrite(ride.build_date);
                    cs.ReadWrite(ride.upkeep_cost);

                    cs.ReadWrite(ride.cur_num_customers);
                    cs.ReadWrite(ride.num_customers_timeout);

                    cs.ReadWriteArray(ride.num_customers, [&cs](uint16_t& v) {
                        cs.ReadWrite(v);
                        return true;
                    });

                    cs.ReadWrite(ride.total_customers);
                    cs.ReadWrite(ride.total_profit);
                    cs.ReadWrite(ride.popularity);
                    cs.ReadWrite(ride.popularity_time_out);
                    cs.ReadWrite(ride.popularity_next);
                    cs.ReadWrite(ride.guests_favourite);
                    cs.ReadWrite(ride.no_primary_items_sold);
                    cs.ReadWrite(ride.no_secondary_items_sold);
                    cs.ReadWrite(ride.income_per_hour);
                    cs.ReadWrite(ride.profit);
                    cs.ReadWrite(ride.satisfaction);
                    cs.ReadWrite(ride.satisfaction_time_out);
                    cs.ReadWrite(ride.satisfaction_next);

                    // Breakdown
                    cs.ReadWrite(ride.breakdown_reason_pending);
                    cs.ReadWrite(ride.mechanic_status);
                    cs.ReadWrite(ride.mechanic);
                    cs.ReadWrite(ride.inspection_station);
                    cs.ReadWrite(ride.broken_vehicle);
                    cs.ReadWrite(ride.broken_car);
                    cs.ReadWrite(ride.breakdown_reason);
                    cs.ReadWrite(ride.reliability_subvalue);
                    cs.ReadWrite(ride.reliability_percentage);
                    cs.ReadWrite(ride.unreliability_factor);
                    cs.ReadWrite(ride.downtime);
                    cs.ReadWrite(ride.inspection_interval);
                    cs.ReadWrite(ride.last_inspection);

                    cs.ReadWriteArray(ride.downtime_history, [&cs](uint8_t& v) {
                        cs.ReadWrite(v);
                        return true;
                    });

                    cs.ReadWrite(ride.breakdown_sound_modifier);
                    cs.ReadWrite(ride.not_fixed_timeout);
                    cs.ReadWrite(ride.last_crash_type);
                    cs.ReadWrite(ride.connected_message_throttle);

                    cs.ReadWrite(ride.vehicle_change_timeout);

                    cs.ReadWrite(ride.current_issues);
                    cs.ReadWrite(ride.last_issue_time);

                    // Music
                    cs.ReadWrite(ride.music);
                    cs.ReadWrite(ride.music_tune_id);
                    cs.ReadWrite(ride.music_position);
                    return true;
                });
            });
        }

        static void ReadWriteRideMeasurement(OrcaStream::ChunkStream& cs, RideMeasurement& measurement)
        {
            cs.ReadWrite(measurement.flags);
            cs.ReadWrite(measurement.last_use_tick);
            cs.ReadWrite(measurement.num_items);
            cs.ReadWrite(measurement.current_item);
            cs.ReadWrite(measurement.vehicle_index);
            cs.ReadWrite(measurement.current_station);
            for (size_t i = 0; i < measurement.num_items; i++)
            {
                cs.ReadWrite(measurement.vertical[i]);
                cs.ReadWrite(measurement.lateral[i]);
                cs.ReadWrite(measurement.velocity[i]);
                cs.ReadWrite(measurement.altitude[i]);
            }
        }

        template<typename T> static void ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, T& entity);

        static void ReadWriteEntityCommon(OrcaStream::ChunkStream& cs, EntityBase& entity)
        {
            cs.ReadWrite(entity.sprite_index);
            cs.ReadWrite(entity.sprite_height_negative);
            cs.ReadWrite(entity.x);
            cs.ReadWrite(entity.y);
            cs.ReadWrite(entity.z);
            cs.ReadWrite(entity.sprite_width);
            cs.ReadWrite(entity.sprite_height_positive);
            cs.ReadWrite(entity.sprite_direction);
        }

        static std::vector<ObjectEntryIndex> LegacyGetRideTypesBeenOn(const std::array<uint8_t, 16>& srcArray)
        {
            std::vector<ObjectEntryIndex> ridesTypesBeenOn;
            for (ObjectEntryIndex i = 0; i < RCT12_MAX_RIDE_OBJECTS; i++)
            {
                if (srcArray[i / 8] & (1 << (i % 8)))
                {
                    ridesTypesBeenOn.push_back(i);
                }
            }
            return ridesTypesBeenOn;
        }
        static std::vector<ride_id_t> LegacyGetRidesBeenOn(const std::array<uint8_t, 32>& srcArray)
        {
            std::vector<ride_id_t> ridesBeenOn;
            for (uint16_t i = 0; i < RCT12_MAX_RIDES_IN_PARK; i++)
            {
                if (srcArray[i / 8] & (1 << (i % 8)))
                {
                    ridesBeenOn.push_back(static_cast<ride_id_t>(i));
                }
            }
            return ridesBeenOn;
        }

        static void ReadWritePeep(OrcaStream& os, OrcaStream::ChunkStream& cs, Peep& entity)
        {
            auto version = os.GetHeader().TargetVersion;

            ReadWriteEntityCommon(cs, entity);

            auto guest = entity.As<Guest>();
            auto staff = entity.As<Staff>();

            if (cs.GetMode() == OrcaStream::Mode::READING)
            {
                auto name = cs.Read<std::string>();
                entity.SetName(name);
            }
            else
            {
                cs.Write(static_cast<const char*>(entity.Name));
            }

            cs.ReadWrite(entity.NextLoc);
            cs.ReadWrite(entity.NextFlags);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->OutsideOfPark);
                }
                else
                {
                    cs.Ignore<bool>();
                }
            }

            cs.ReadWrite(entity.State);
            cs.ReadWrite(entity.SubState);
            cs.ReadWrite(entity.SpriteType);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->GuestNumRides);
                }
                else
                {
                    cs.ReadWrite(staff->AssignedStaffType);
                }
            }

            cs.ReadWrite(entity.TshirtColour);
            cs.ReadWrite(entity.TrousersColour);
            cs.ReadWrite(entity.DestinationX);
            cs.ReadWrite(entity.DestinationY);
            cs.ReadWrite(entity.DestinationTolerance);
            cs.ReadWrite(entity.Var37);
            cs.ReadWrite(entity.Energy);
            cs.ReadWrite(entity.EnergyTarget);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->Happiness);
                    cs.ReadWrite(guest->HappinessTarget);
                    cs.ReadWrite(guest->Nausea);
                    cs.ReadWrite(guest->NauseaTarget);
                    cs.ReadWrite(guest->Hunger);
                    cs.ReadWrite(guest->Thirst);
                    cs.ReadWrite(guest->Toilet);
                }
                else
                {
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                }
            }

            cs.ReadWrite(entity.Mass);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->TimeToConsume);
                }
                else
                {
                    uint8_t temp{};
                    cs.ReadWrite(temp);
                }
            }

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    if (cs.GetMode() == OrcaStream::Mode::READING)
                    {
                        guest->Intensity = IntensityRange(cs.Read<uint8_t>());
                    }
                    else
                    {
                        cs.Write(static_cast<uint8_t>(guest->Intensity));
                    }
                    cs.ReadWrite(guest->NauseaTolerance);
                }
                else
                {
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                }
            }

            cs.ReadWrite(entity.WindowInvalidateFlags);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->PaidOnDrink);
                    std::array<uint8_t, 16> rideTypeBeenOn;
                    cs.ReadWriteArray(rideTypeBeenOn, [&cs](uint8_t& rideType) {
                        cs.ReadWrite(rideType);
                        return true;
                    });
                    OpenRCT2::RideUse::GetTypeHistory().Set(guest->sprite_index, LegacyGetRideTypesBeenOn(rideTypeBeenOn));
                    cs.ReadWrite(guest->ItemFlags);
                    cs.ReadWrite(guest->Photo2RideRef);
                    cs.ReadWrite(guest->Photo3RideRef);
                    cs.ReadWrite(guest->Photo4RideRef);
                }
                else
                {
                    cs.Ignore<money16>();

                    std::vector<uint8_t> temp;
                    cs.ReadWriteVector(temp, [&cs](uint8_t& rideType) {
                        cs.ReadWrite(rideType);
                        return true;
                    });
                    cs.Ignore<uint64_t>();
                    cs.Ignore<ride_id_t>();
                    cs.Ignore<ride_id_t>();
                    cs.Ignore<ride_id_t>();
                }
            }

            cs.ReadWrite(entity.CurrentRide);
            cs.ReadWrite(entity.CurrentRideStation);
            cs.ReadWrite(entity.CurrentTrain);
            cs.ReadWrite(entity.TimeToSitdown);
            cs.ReadWrite(entity.SpecialSprite);
            cs.ReadWrite(entity.ActionSpriteType);
            cs.ReadWrite(entity.NextActionSpriteType);
            cs.ReadWrite(entity.ActionSpriteImageOffset);
            cs.ReadWrite(entity.Action);
            cs.ReadWrite(entity.ActionFrame);
            cs.ReadWrite(entity.StepProgress);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->GuestNextInQueue);
                }
                else
                {
                    cs.ReadWrite(staff->MechanicTimeSinceCall);
                }
            }

            cs.ReadWrite(entity.PeepDirection);
            cs.ReadWrite(entity.InteractionRideIndex);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->TimeInQueue);
                    std::array<uint8_t, 32> ridesBeenOn;
                    cs.ReadWriteArray(ridesBeenOn, [&cs](uint8_t& rideType) {
                        cs.ReadWrite(rideType);
                        return true;
                    });
                    OpenRCT2::RideUse::GetHistory().Set(guest->sprite_index, LegacyGetRidesBeenOn(ridesBeenOn));
                }
                else
                {
                    cs.Ignore<uint16_t>();

                    std::vector<uint8_t> ridesBeenOn;
                    cs.ReadWriteVector(ridesBeenOn, [&cs](uint8_t& rideId) {
                        cs.ReadWrite(rideId);
                        return true;
                    });
                }
            }

            cs.ReadWrite(entity.Id);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->CashInPocket);
                    cs.ReadWrite(guest->CashSpent);
                    cs.ReadWrite(guest->ParkEntryTime);
                    cs.ReadWrite(guest->RejoinQueueTimeout);
                    cs.ReadWrite(guest->PreviousRide);
                    cs.ReadWrite(guest->PreviousRideTimeOut);
                    cs.ReadWriteArray(guest->Thoughts, [&cs](PeepThought& thought) {
                        cs.ReadWrite(thought.type);

                        uint8_t item{};
                        cs.ReadWrite(item);
                        if (item == 255)
                        {
                            thought.item = PeepThoughtItemNone;
                        }
                        else
                        {
                            thought.item = item;
                        }

                        cs.ReadWrite(thought.freshness);
                        cs.ReadWrite(thought.fresh_timeout);
                        return true;
                    });
                }
                else
                {
                    cs.Ignore<money32>();
                    cs.Ignore<money32>();
                    cs.ReadWrite(staff->HireDate);
                    cs.Ignore<int8_t>();
                    cs.Ignore<ride_id_t>();
                    cs.Ignore<uint16_t>();

                    std::vector<PeepThought> temp;
                    cs.ReadWriteVector(temp, [&cs](PeepThought& thought) {
                        cs.ReadWrite(thought.type);
                        cs.ReadWrite(thought.item);
                        cs.ReadWrite(thought.freshness);
                        cs.ReadWrite(thought.fresh_timeout);
                        return true;
                    });
                }
            }

            cs.ReadWrite(entity.PathCheckOptimisation);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->GuestHeadingToRideId);
                    cs.ReadWrite(guest->GuestIsLostCountdown);
                    cs.ReadWrite(guest->Photo1RideRef);
                }
                else
                {
                    cs.Ignore<ride_id_t>();
                    cs.ReadWrite(staff->StaffOrders);
                    cs.Ignore<ride_id_t>();
                }
            }

            cs.ReadWrite(entity.PeepFlags);
            cs.ReadWrite(entity.PathfindGoal.x);
            cs.ReadWrite(entity.PathfindGoal.y);
            cs.ReadWrite(entity.PathfindGoal.z);
            cs.ReadWrite(entity.PathfindGoal.direction);
            for (size_t i = 0; i < std::size(entity.PathfindHistory); i++)
            {
                cs.ReadWrite(entity.PathfindHistory[i].x);
                cs.ReadWrite(entity.PathfindHistory[i].y);
                cs.ReadWrite(entity.PathfindHistory[i].z);
                cs.ReadWrite(entity.PathfindHistory[i].direction);
            }
            cs.ReadWrite(entity.WalkingFrameNum);

            if (version <= 1)
            {
                if (guest != nullptr)
                {
                    cs.ReadWrite(guest->LitterCount);
                    cs.ReadWrite(guest->GuestTimeOnRide);
                    cs.ReadWrite(guest->DisgustingCount);
                    cs.ReadWrite(guest->PaidToEnter);
                    cs.ReadWrite(guest->PaidOnRides);
                    cs.ReadWrite(guest->PaidOnFood);
                    cs.ReadWrite(guest->PaidOnSouvenirs);
                    cs.ReadWrite(guest->AmountOfFood);
                    cs.ReadWrite(guest->AmountOfDrinks);
                    cs.ReadWrite(guest->AmountOfSouvenirs);
                    cs.ReadWrite(guest->VandalismSeen);
                    cs.ReadWrite(guest->VoucherType);
                    cs.ReadWrite(guest->VoucherRideId);
                    cs.ReadWrite(guest->SurroundingsThoughtTimeout);
                    cs.ReadWrite(guest->Angriness);
                    cs.ReadWrite(guest->TimeLost);
                    cs.ReadWrite(guest->DaysInQueue);
                    cs.ReadWrite(guest->BalloonColour);
                    cs.ReadWrite(guest->UmbrellaColour);
                    cs.ReadWrite(guest->HatColour);
                    cs.ReadWrite(guest->FavouriteRide);
                    cs.ReadWrite(guest->FavouriteRideRating);
                }
                else
                {
                    cs.Ignore<uint8_t>();
                    cs.ReadWrite(staff->StaffMowingTimeout);
                    cs.Ignore<uint8_t>();
                    cs.ReadWrite(staff->StaffLawnsMown);
                    cs.ReadWrite(staff->StaffGardensWatered);
                    cs.ReadWrite(staff->StaffLitterSwept);
                    cs.ReadWrite(staff->StaffBinsEmptied);
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<ride_id_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<uint8_t>();
                    cs.Ignore<ride_id_t>();
                    cs.Ignore<uint8_t>();
                }
            }
        }

        template<typename T> void WriteEntitiesOfType(OrcaStream& os, OrcaStream::ChunkStream& cs);
        template<typename... T> void WriteEntitiesOfTypes(OrcaStream& os, OrcaStream::ChunkStream& cs);

        template<typename T> void ReadEntitiesOfType(OrcaStream& os, OrcaStream::ChunkStream& cs);

        template<typename... T> void ReadEntitiesOfTypes(OrcaStream& os, OrcaStream::ChunkStream& cs);

        void ReadWriteEntitiesChunk(OrcaStream& os);

        static void ReadWriteStringTable(OrcaStream::ChunkStream& cs, std::string& value, const std::string_view& lcode)
        {
            std::vector<std::tuple<std::string, std::string>> table;
            if (cs.GetMode() != OrcaStream::Mode::READING)
            {
                table.push_back(std::make_tuple(std::string(lcode), value));
            }
            cs.ReadWriteVector(table, [&cs](std::tuple<std::string, std::string>& v) {
                cs.ReadWrite(std::get<0>(v));
                cs.ReadWrite(std::get<1>(v));
            });
            if (cs.GetMode() == OrcaStream::Mode::READING)
            {
                auto fr = std::find_if(table.begin(), table.end(), [&lcode](const std::tuple<std::string, std::string>& v) {
                    return std::get<0>(v) == lcode;
                });
                if (fr != table.end())
                {
                    value = std::get<1>(*fr);
                }
                else if (table.size() > 0)
                {
                    value = std::get<1>(table[0]);
                }
                else
                {
                    value = "";
                }
            }
        }
    };

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, Vehicle& entity)
    {
        ReadWriteEntityCommon(cs, entity);
        cs.ReadWrite(entity.SubType);
        cs.ReadWrite(entity.Pitch);
        cs.ReadWrite(entity.bank_rotation);
        cs.ReadWrite(entity.remaining_distance);
        cs.ReadWrite(entity.velocity);
        cs.ReadWrite(entity.acceleration);
        cs.ReadWrite(entity.ride);
        cs.ReadWrite(entity.vehicle_type);
        cs.ReadWrite(entity.colours.body_colour);
        cs.ReadWrite(entity.colours.trim_colour);
        cs.ReadWrite(entity.track_progress);
        cs.ReadWrite(entity.BoatLocation);
        cs.ReadWrite(entity.TrackTypeAndDirection);
        cs.ReadWrite(entity.TrackLocation.x);
        cs.ReadWrite(entity.TrackLocation.y);
        cs.ReadWrite(entity.TrackLocation.z);
        cs.ReadWrite(entity.next_vehicle_on_train);
        cs.ReadWrite(entity.prev_vehicle_on_ride);
        cs.ReadWrite(entity.next_vehicle_on_ride);
        cs.ReadWrite(entity.var_44);
        cs.ReadWrite(entity.mass);
        cs.ReadWrite(entity.update_flags);
        cs.ReadWrite(entity.SwingSprite);
        cs.ReadWrite(entity.current_station);
        cs.ReadWrite(entity.current_time);
        cs.ReadWrite(entity.crash_z);
        cs.ReadWrite(entity.status);
        cs.ReadWrite(entity.sub_state);
        for (size_t i = 0; i < std::size(entity.peep); i++)
        {
            cs.ReadWrite(entity.peep[i]);
            cs.ReadWrite(entity.peep_tshirt_colours[i]);
        }
        cs.ReadWrite(entity.num_seats);
        cs.ReadWrite(entity.num_peeps);
        cs.ReadWrite(entity.next_free_seat);
        cs.ReadWrite(entity.restraints_position);
        cs.ReadWrite(entity.crash_x);
        cs.ReadWrite(entity.sound2_flags);
        cs.ReadWrite(entity.spin_sprite);
        cs.ReadWrite(entity.sound1_id);
        cs.ReadWrite(entity.sound1_volume);
        cs.ReadWrite(entity.sound2_id);
        cs.ReadWrite(entity.sound2_volume);
        cs.ReadWrite(entity.sound_vector_factor);
        cs.ReadWrite(entity.time_waiting);
        cs.ReadWrite(entity.speed);
        cs.ReadWrite(entity.powered_acceleration);
        cs.ReadWrite(entity.dodgems_collision_direction);
        cs.ReadWrite(entity.animation_frame);
        if (cs.GetMode() == OrcaStream::Mode::READING && os.GetHeader().TargetVersion <= 2)
        {
            uint16_t lower = 0, upper = 0;
            cs.ReadWrite(lower);
            cs.ReadWrite(upper);
            entity.animationState = lower | (upper << 16);
        }
        else
        {
            cs.ReadWrite(entity.animationState);
        }
        cs.ReadWrite(entity.scream_sound_id);
        cs.ReadWrite(entity.TrackSubposition);
        cs.ReadWrite(entity.var_CE);
        cs.ReadWrite(entity.var_CF);
        cs.ReadWrite(entity.lost_time_out);
        cs.ReadWrite(entity.vertical_drop_countdown);
        cs.ReadWrite(entity.var_D3);
        cs.ReadWrite(entity.mini_golf_current_animation);
        cs.ReadWrite(entity.mini_golf_flags);
        cs.ReadWrite(entity.ride_subtype);
        cs.ReadWrite(entity.colours_extended);
        cs.ReadWrite(entity.seat_rotation);
        cs.ReadWrite(entity.target_seat_rotation);
        cs.ReadWrite(entity.IsCrashedVehicle);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, Guest& guest)
    {
        ReadWritePeep(os, cs, guest);

        if (os.GetHeader().TargetVersion <= 1)
        {
            return;
        }

        cs.ReadWrite(guest.GuestNumRides);
        cs.ReadWrite(guest.GuestNextInQueue);
        cs.ReadWrite(guest.ParkEntryTime);
        cs.ReadWrite(guest.GuestHeadingToRideId);
        cs.ReadWrite(guest.GuestIsLostCountdown);
        cs.ReadWrite(guest.GuestTimeOnRide);
        cs.ReadWrite(guest.PaidToEnter);
        cs.ReadWrite(guest.PaidOnRides);
        cs.ReadWrite(guest.PaidOnFood);
        cs.ReadWrite(guest.PaidOnDrink);
        cs.ReadWrite(guest.PaidOnSouvenirs);
        cs.ReadWrite(guest.OutsideOfPark);
        cs.ReadWrite(guest.Happiness);
        cs.ReadWrite(guest.HappinessTarget);
        cs.ReadWrite(guest.Nausea);
        cs.ReadWrite(guest.NauseaTarget);
        cs.ReadWrite(guest.Hunger);
        cs.ReadWrite(guest.Thirst);
        cs.ReadWrite(guest.Toilet);
        cs.ReadWrite(guest.TimeToConsume);
        if (cs.GetMode() == OrcaStream::Mode::READING)
        {
            guest.Intensity = IntensityRange(cs.Read<uint8_t>());
        }
        else
        {
            cs.Write(static_cast<uint8_t>(guest.Intensity));
        }
        cs.ReadWrite(guest.NauseaTolerance);

        if (os.GetHeader().TargetVersion < 3)
        {
            std::array<uint8_t, 16> rideTypeBeenOn;
            cs.ReadWriteArray(rideTypeBeenOn, [&cs](uint8_t& rideType) {
                cs.ReadWrite(rideType);
                return true;
            });
            OpenRCT2::RideUse::GetTypeHistory().Set(guest.sprite_index, LegacyGetRideTypesBeenOn(rideTypeBeenOn));
        }

        cs.ReadWrite(guest.TimeInQueue);
        if (os.GetHeader().TargetVersion < 3)
        {
            std::array<uint8_t, 32> ridesBeenOn;
            cs.ReadWriteArray(ridesBeenOn, [&cs](uint8_t& rideType) {
                cs.ReadWrite(rideType);
                return true;
            });
            OpenRCT2::RideUse::GetHistory().Set(guest.sprite_index, LegacyGetRidesBeenOn(ridesBeenOn));
        }
        else
        {
            if (cs.GetMode() == OrcaStream::Mode::READING)
            {
                std::vector<ride_id_t> rideUse;
                cs.ReadWriteVector(rideUse, [&cs](ride_id_t& rideId) { cs.ReadWrite(rideId); });
                OpenRCT2::RideUse::GetHistory().Set(guest.sprite_index, std::move(rideUse));
                std::vector<ObjectEntryIndex> rideTypeUse;
                cs.ReadWriteVector(rideTypeUse, [&cs](ObjectEntryIndex& rideType) { cs.ReadWrite(rideType); });
                OpenRCT2::RideUse::GetTypeHistory().Set(guest.sprite_index, std::move(rideTypeUse));
            }
            else
            {
                auto* rideUse = OpenRCT2::RideUse::GetHistory().GetAll(guest.sprite_index);
                if (rideUse == nullptr)
                {
                    std::vector<ride_id_t> empty;
                    cs.ReadWriteVector(empty, [&cs](ride_id_t& rideId) { cs.ReadWrite(rideId); });
                }
                else
                {
                    cs.ReadWriteVector(*rideUse, [&cs](ride_id_t& rideId) { cs.ReadWrite(rideId); });
                }
                auto* rideTypeUse = OpenRCT2::RideUse::GetTypeHistory().GetAll(guest.sprite_index);
                if (rideTypeUse == nullptr)
                {
                    std::vector<ObjectEntryIndex> empty;
                    cs.ReadWriteVector(empty, [&cs](ObjectEntryIndex& rideId) { cs.ReadWrite(rideId); });
                }
                else
                {
                    cs.ReadWriteVector(*rideTypeUse, [&cs](ObjectEntryIndex& rideId) { cs.ReadWrite(rideId); });
                }
            }
        }
        cs.ReadWrite(guest.CashInPocket);
        cs.ReadWrite(guest.CashSpent);
        cs.ReadWrite(guest.Photo1RideRef);
        cs.ReadWrite(guest.Photo2RideRef);
        cs.ReadWrite(guest.Photo3RideRef);
        cs.ReadWrite(guest.Photo4RideRef);
        cs.ReadWrite(guest.RejoinQueueTimeout);
        cs.ReadWrite(guest.PreviousRide);
        cs.ReadWrite(guest.PreviousRideTimeOut);
        cs.ReadWriteArray(guest.Thoughts, [version = os.GetHeader().TargetVersion, &cs](PeepThought& thought) {
            cs.ReadWrite(thought.type);
            if (version <= 2)
            {
                int16_t item{};
                cs.ReadWrite(item);
                thought.item = item;
            }
            else
            {
                cs.ReadWrite(thought.item);
            }
            cs.ReadWrite(thought.freshness);
            cs.ReadWrite(thought.fresh_timeout);
            return true;
        });
        cs.ReadWrite(guest.LitterCount);
        cs.ReadWrite(guest.DisgustingCount);
        cs.ReadWrite(guest.AmountOfFood);
        cs.ReadWrite(guest.AmountOfDrinks);
        cs.ReadWrite(guest.AmountOfSouvenirs);
        cs.ReadWrite(guest.VandalismSeen);
        cs.ReadWrite(guest.VoucherType);
        cs.ReadWrite(guest.VoucherRideId);
        cs.ReadWrite(guest.SurroundingsThoughtTimeout);
        cs.ReadWrite(guest.Angriness);
        cs.ReadWrite(guest.TimeLost);
        cs.ReadWrite(guest.DaysInQueue);
        cs.ReadWrite(guest.BalloonColour);
        cs.ReadWrite(guest.UmbrellaColour);
        cs.ReadWrite(guest.HatColour);
        cs.ReadWrite(guest.FavouriteRide);
        cs.ReadWrite(guest.FavouriteRideRating);
        cs.ReadWrite(guest.ItemFlags);
    }

    static std::vector<TileCoordsXY> GetPatrolArea(Staff& staff)
    {
        std::vector<TileCoordsXY> area;
        if (staff.PatrolInfo != nullptr)
        {
            for (size_t i = 0; i < STAFF_PATROL_AREA_SIZE; i++)
            {
                // 32 blocks per array item (32 bits)
                auto arrayItem = staff.PatrolInfo->Data[i];
                for (size_t j = 0; j < 32; j++)
                {
                    auto blockIndex = (i * 32) + j;
                    if (arrayItem & (1 << j))
                    {
                        auto sx = (blockIndex % STAFF_PATROL_AREA_BLOCKS_PER_LINE) * 4;
                        auto sy = (blockIndex / STAFF_PATROL_AREA_BLOCKS_PER_LINE) * 4;
                        for (size_t y = 0; y < 4; y++)
                        {
                            for (size_t x = 0; x < 4; x++)
                            {
                                area.push_back({ static_cast<int32_t>(sx + x), static_cast<int32_t>(sy + y) });
                            }
                        }
                    }
                }
            }
        }
        return area;
    }

    static void SetPatrolArea(Staff& staff, const std::vector<TileCoordsXY>& area)
    {
        if (area.empty())
        {
            staff.ClearPatrolArea();
        }
        else
        {
            for (const auto& coord : area)
            {
                staff.SetPatrolArea(coord.ToCoordsXY(), true);
            }
        }
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, Staff& entity)
    {
        ReadWritePeep(os, cs, entity);

        std::vector<TileCoordsXY> patrolArea;
        if (cs.GetMode() == OrcaStream::Mode::WRITING)
        {
            patrolArea = GetPatrolArea(entity);
        }
        cs.ReadWriteVector(patrolArea, [&cs](TileCoordsXY& value) { cs.ReadWrite(value); });
        if (cs.GetMode() == OrcaStream::Mode::READING)
        {
            SetPatrolArea(entity, patrolArea);
        }

        if (os.GetHeader().TargetVersion <= 1)
        {
            return;
        }

        cs.ReadWrite(entity.AssignedStaffType);
        cs.ReadWrite(entity.MechanicTimeSinceCall);
        cs.ReadWrite(entity.HireDate);
        if (os.GetHeader().TargetVersion <= 2)
        {
            cs.Ignore<uint8_t>();
        }
        cs.ReadWrite(entity.StaffOrders);
        cs.ReadWrite(entity.StaffMowingTimeout);
        cs.ReadWrite(entity.StaffLawnsMown);
        cs.ReadWrite(entity.StaffGardensWatered);
        cs.ReadWrite(entity.StaffLitterSwept);
        cs.ReadWrite(entity.StaffBinsEmptied);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, SteamParticle& steamParticle)
    {
        ReadWriteEntityCommon(cs, steamParticle);
        cs.ReadWrite(steamParticle.time_to_move);
        cs.ReadWrite(steamParticle.frame);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, MoneyEffect& moneyEffect)
    {
        ReadWriteEntityCommon(cs, moneyEffect);
        cs.ReadWrite(moneyEffect.MoveDelay);
        cs.ReadWrite(moneyEffect.NumMovements);
        cs.ReadWrite(moneyEffect.Vertical);
        cs.ReadWrite(moneyEffect.Value);
        cs.ReadWrite(moneyEffect.OffsetX);
        cs.ReadWrite(moneyEffect.Wiggle);
    }

    template<>
    void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, VehicleCrashParticle& vehicleCrashParticle)
    {
        ReadWriteEntityCommon(cs, vehicleCrashParticle);
        cs.ReadWrite(vehicleCrashParticle.frame);
        cs.ReadWrite(vehicleCrashParticle.time_to_live);
        cs.ReadWrite(vehicleCrashParticle.frame);
        cs.ReadWrite(vehicleCrashParticle.colour[0]);
        cs.ReadWrite(vehicleCrashParticle.colour[1]);
        cs.ReadWrite(vehicleCrashParticle.crashed_sprite_base);
        cs.ReadWrite(vehicleCrashParticle.velocity_x);
        cs.ReadWrite(vehicleCrashParticle.velocity_y);
        cs.ReadWrite(vehicleCrashParticle.velocity_z);
        cs.ReadWrite(vehicleCrashParticle.acceleration_x);
        cs.ReadWrite(vehicleCrashParticle.acceleration_y);
        cs.ReadWrite(vehicleCrashParticle.acceleration_z);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, ExplosionCloud& entity)
    {
        ReadWriteEntityCommon(cs, entity);
        cs.ReadWrite(entity.frame);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, CrashSplashParticle& entity)
    {
        ReadWriteEntityCommon(cs, entity);
        cs.ReadWrite(entity.frame);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, ExplosionFlare& entity)
    {
        ReadWriteEntityCommon(cs, entity);
        cs.ReadWrite(entity.frame);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, JumpingFountain& fountain)
    {
        ReadWriteEntityCommon(cs, fountain);
        cs.ReadWrite(fountain.NumTicksAlive);
        cs.ReadWrite(fountain.frame);
        cs.ReadWrite(fountain.FountainFlags);
        cs.ReadWrite(fountain.TargetX);
        cs.ReadWrite(fountain.TargetY);
        cs.ReadWrite(fountain.TargetY);
        cs.ReadWrite(fountain.Iteration);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, Balloon& balloon)
    {
        ReadWriteEntityCommon(cs, balloon);
        cs.ReadWrite(balloon.popped);
        cs.ReadWrite(balloon.time_to_move);
        cs.ReadWrite(balloon.frame);
        cs.ReadWrite(balloon.colour);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, Duck& duck)
    {
        ReadWriteEntityCommon(cs, duck);
        cs.ReadWrite(duck.frame);
        cs.ReadWrite(duck.target_x);
        cs.ReadWrite(duck.target_y);
        cs.ReadWrite(duck.state);
    }

    template<> void ParkFile::ReadWriteEntity(OrcaStream& os, OrcaStream::ChunkStream& cs, Litter& entity)
    {
        ReadWriteEntityCommon(cs, entity);
        cs.ReadWrite(entity.SubType);
        cs.ReadWrite(entity.creationTick);
    }

    template<typename T> void ParkFile::WriteEntitiesOfType(OrcaStream& os, OrcaStream::ChunkStream& cs)
    {
        uint16_t count = GetEntityListCount(T::cEntityType);
        cs.Write(T::cEntityType);
        cs.Write(count);
        for (auto* ent : EntityList<T>())
        {
            cs.Write(ent->sprite_index);
            ReadWriteEntity(os, cs, *ent);
        }
    }

    template<typename... T> void ParkFile::WriteEntitiesOfTypes(OrcaStream& os, OrcaStream::ChunkStream& cs)
    {
        (WriteEntitiesOfType<T>(os, cs), ...);
    }

    template<typename T> void ParkFile::ReadEntitiesOfType(OrcaStream& os, OrcaStream::ChunkStream& cs)
    {
        [[maybe_unused]] auto t = cs.Read<EntityType>();
        assert(t == T::cEntityType);
        auto count = cs.Read<uint16_t>();
        for (auto i = 0; i < count; ++i)
        {
            T placeholder{};

            auto index = cs.Read<uint16_t>();
            auto* ent = CreateEntityAt<T>(index);
            if (ent == nullptr)
            {
                // Unable to allocate entity
                ent = &placeholder;
            }
            ReadWriteEntity(os, cs, *ent);
        }
    }

    template<typename... T> void ParkFile::ReadEntitiesOfTypes(OrcaStream& os, OrcaStream::ChunkStream& cs)
    {
        (ReadEntitiesOfType<T>(os, cs), ...);
    }

    void ParkFile::ReadWriteEntitiesChunk(OrcaStream& os)
    {
        os.ReadWriteChunk(ParkFileChunkType::ENTITIES, [this, &os](OrcaStream::ChunkStream& cs) {
            if (cs.GetMode() == OrcaStream::Mode::READING)
            {
                reset_sprite_list();
            }

            std::vector<uint16_t> entityIndices;
            if (cs.GetMode() == OrcaStream::Mode::READING)
            {
                ReadEntitiesOfTypes<
                    Vehicle, Guest, Staff, Litter, SteamParticle, MoneyEffect, VehicleCrashParticle, ExplosionCloud,
                    CrashSplashParticle, ExplosionFlare, JumpingFountain, Balloon, Duck>(os, cs);
            }
            else
            {
                WriteEntitiesOfTypes<
                    Vehicle, Guest, Staff, Litter, SteamParticle, MoneyEffect, VehicleCrashParticle, ExplosionCloud,
                    CrashSplashParticle, ExplosionFlare, JumpingFountain, Balloon, Duck>(os, cs);
            }
        });
    }
} // namespace OpenRCT2

void ParkFileExporter::Export(std::string_view path)
{
    auto parkFile = std::make_unique<OpenRCT2::ParkFile>();
    parkFile->Save(path);
}

void ParkFileExporter::Export(IStream& stream)
{
    auto parkFile = std::make_unique<OpenRCT2::ParkFile>();
    parkFile->ExportObjectsList = ExportObjectsList;
    parkFile->Save(stream);
}

enum : uint32_t
{
    S6_SAVE_FLAG_EXPORT = 1 << 0,
    S6_SAVE_FLAG_SCENARIO = 1 << 1,
    S6_SAVE_FLAG_AUTOMATIC = 1u << 31,
};

int32_t scenario_save(const utf8* path, int32_t flags)
{
    if (flags & S6_SAVE_FLAG_SCENARIO)
    {
        log_verbose("saving scenario");
    }
    else
    {
        log_verbose("saving game");
    }

    if (!(flags & S6_SAVE_FLAG_AUTOMATIC))
    {
        window_close_construction_windows();
    }

    viewport_set_saved_view();

    bool result = false;
    auto parkFile = std::make_unique<OpenRCT2::ParkFile>();
    try
    {
        if (flags & S6_SAVE_FLAG_EXPORT)
        {
            auto& objManager = OpenRCT2::GetContext()->GetObjectManager();
            parkFile->ExportObjectsList = objManager.GetPackableObjects();
        }
        parkFile->OmitTracklessRides = true;
        if (flags & S6_SAVE_FLAG_SCENARIO)
        {
            // s6exporter->SaveScenario(path);
        }
        else
        {
            // s6exporter->SaveGame(path);
        }
        parkFile->Save(path);
        result = true;
    }
    catch (const std::exception&)
    {
    }

    gfx_invalidate_screen();

    if (result && !(flags & S6_SAVE_FLAG_AUTOMATIC))
    {
        gScreenAge = 0;
    }
    return result;
}

class ParkFileImporter final : public IParkImporter
{
private:
#ifdef __clang__
    [[maybe_unused]]
#endif
    const IObjectRepository& _objectRepository;
    std::unique_ptr<OpenRCT2::ParkFile> _parkFile;

public:
    ParkFileImporter(IObjectRepository& objectRepository)
        : _objectRepository(objectRepository)
    {
    }

    ParkLoadResult Load(const utf8* path) override
    {
        _parkFile = std::make_unique<OpenRCT2::ParkFile>();
        _parkFile->Load(path);
        return ParkLoadResult(std::move(_parkFile->RequiredObjects));
    }

    ParkLoadResult LoadSavedGame(const utf8* path, bool skipObjectCheck = false) override
    {
        return Load(path);
    }

    ParkLoadResult LoadScenario(const utf8* path, bool skipObjectCheck = false) override
    {
        return Load(path);
    }

    ParkLoadResult LoadFromStream(
        OpenRCT2::IStream* stream, bool isScenario, bool skipObjectCheck = false, const utf8* path = String::Empty) override
    {
        _parkFile = std::make_unique<OpenRCT2::ParkFile>();
        _parkFile->Load(*stream);
        return ParkLoadResult(std::move(_parkFile->RequiredObjects));
    }

    void Import() override
    {
        _parkFile->Import();
        game_fix_save_vars();
    }

    bool GetDetails(scenario_index_entry* dst) override
    {
        *dst = _parkFile->ReadScenarioChunk();
        return true;
    }
};

std::unique_ptr<IParkImporter> ParkImporter::CreateParkFile(IObjectRepository& objectRepository)
{
    return std::make_unique<ParkFileImporter>(objectRepository);
}

static std::map<std::string_view, std::string_view> oldObjectIds = {
    { "official.scgpanda", "rct2dlc.scenery_group.scgpanda" },
    { "official.wtrpink", "rct2dlc.water.wtrpink" },
    { "official.ttrftl07", "toontowner.scenery_small.ttrftl07" },
    { "official.pandagr", "rct2dlc.scenery_small.pandagr" },
    { "official.ttrftl04", "toontowner.scenery_small.ttrftl04" },
    { "official.bigpanda", "rct2dlc.scenery_small.bigpanda" },
    { "official.ttrftl02", "toontowner.scenery_small.ttrftl02" },
    { "official.ttrftl03", "toontowner.scenery_small.ttrftl03" },
    { "official.ttrftl08", "toontowner.scenery_small.ttrftl08" },
    { "official.xxbbbr01", "toontowner.scenery_small.xxbbbr01" },
    { "official.mg-prar", "mamabear.scenery_wall.mg-prar" },
    { "official.litterpa", "rct2dlc.footpath_item.litterpa" },
    { "openrct2.railings.invisible", "openrct2.footpath_railings.invisible" },
    { "official.zpanda", "rct2dlc.ride.zpanda" },
    { "openrct2.surface.void", "openrct2.terrain_surface.void" },
    { "openrct2.station.noentrance", "openrct2.station.noentrance" },
    { "openrct2.station.noplatformnoentrance", "openrct2.station.noplatformnoentrance" },
    { "rct2.sct", "rct2.scenery_large.sct" },
    { "rct2.soh3", "rct2.scenery_large.soh3" },
    { "rct2.scln", "rct2.scenery_large.scln" },
    { "rct2.smh2", "rct2.scenery_large.smh2" },
    { "rct2.sdn3", "rct2.scenery_large.sdn3" },
    { "rct2.stb1", "rct2.scenery_large.stb1" },
    { "rct2.nitroent", "rct2.scenery_large.nitroent" },
    { "rct2.smh1", "rct2.scenery_large.smh1" },
    { "rct2.badrack", "rct2.scenery_large.badrack" },
    { "rct2.glthent", "rct2.scenery_large.glthent" },
    { "rct2.sth", "rct2.scenery_large.sth" },
    { "rct2.svlc", "rct2.scenery_large.svlc" },
    { "rct2.ssr", "rct2.scenery_large.ssr" },
    { "rct2.spyr", "rct2.scenery_large.spyr" },
    { "rct2.prship", "rct2.scenery_large.prship" },
    { "rct2.saloon", "rct2.scenery_large.saloon" },
    { "rct2.smb", "rct2.scenery_large.smb" },
    { "rct2.soh1", "rct2.scenery_large.soh1" },
    { "rct2.stg2", "rct2.scenery_large.stg2" },
    { "rct2.sspx", "rct2.scenery_large.sspx" },
    { "rct2.sip", "rct2.scenery_large.sip" },
    { "rct2.ssig4", "rct2.scenery_large.ssig4" },
    { "rct2.ssig3", "rct2.scenery_large.ssig3" },
    { "rct2.ssig2", "rct2.scenery_large.ssig2" },
    { "rct2.smn1", "rct2.scenery_large.smn1" },
    { "rct2.scol", "rct2.scenery_large.scol" },
    { "rct2.ssh", "rct2.scenery_large.ssh" },
    { "rct2.spg", "rct2.scenery_large.spg" },
    { "rct2.sah3", "rct2.scenery_large.sah3" },
    { "rct2.stg1", "rct2.scenery_large.stg1" },
    { "rct2.ssk1", "rct2.scenery_large.ssk1" },
    { "rct2.sah", "rct2.scenery_large.sah" },
    { "rct2.sgp", "rct2.scenery_large.sgp" },
    { "rct2.mdsaent", "rct2.scenery_large.mdsaent" },
    { "rct2.sdn2", "rct2.scenery_large.sdn2" },
    { "rct2.sah2", "rct2.scenery_large.sah2" },
    { "rct2.shs1", "rct2.scenery_large.shs1" },
    { "rct2.sps", "rct2.scenery_large.sps" },
    { "rct2.sst", "rct2.scenery_large.sst" },
    { "rct2.stb2", "rct2.scenery_large.stb2" },
    { "rct2.ssig1", "rct2.scenery_large.ssig1" },
    { "rct2.sdn1", "rct2.scenery_large.sdn1" },
    { "rct2.tavern", "rct2.scenery_large.tavern" },
    { "rct2.shs2", "rct2.scenery_large.shs2" },
    { "rct2.wwbank", "rct2.scenery_large.wwbank" },
    { "rct2.sob", "rct2.scenery_large.sob" },
    { "rct2.soh2", "rct2.scenery_large.soh2" },
    { "rct2.genstore", "rct2.scenery_large.genstore" },
    { "rct2.scgpirat", "rct2.scenery_group.scgpirat" },
    { "rct2.scgsport", "rct2.scenery_group.scgsport" },
    { "rct2.scgspook", "rct2.scenery_group.scgspook" },
    { "rct2.scgclass", "rct2.scenery_group.scgclass" },
    { "rct2.scghallo", "rct2.scenery_group.scghallo" },
    { "rct2.scgegypt", "rct2.scenery_group.scgegypt" },
    { "rct2.scgwater", "rct2.scenery_group.scgwater" },
    { "rct2.scgurban", "rct2.scenery_group.scgurban" },
    { "rct2.scgwond", "rct2.scenery_group.scgwond" },
    { "rct2.scgmine", "rct2.scenery_group.scgmine" },
    { "rct2.scgorien", "rct2.scenery_group.scgorien" },
    { "rct2.scgshrub", "rct2.scenery_group.scgshrub" },
    { "rct2.scgfence", "rct2.scenery_group.scgfence" },
    { "rct2.scggiant", "rct2.scenery_group.scggiant" },
    { "rct2.scgspace", "rct2.scenery_group.scgspace" },
    { "rct2.scgsixfl", "rct2.scenery_group.scgsixfl" },
    { "rct2.scgjuras", "rct2.scenery_group.scgjuras" },
    { "rct2.scgcandy", "rct2.scenery_group.scgcandy" },
    { "rct2.scgmart", "rct2.scenery_group.scgmart" },
    { "rct2.scgpathx", "rct2.scenery_group.scgpathx" },
    { "rct2.scgtrees", "rct2.scenery_group.scgtrees" },
    { "rct2.scgabstr", "rct2.scenery_group.scgabstr" },
    { "rct2.scgsnow", "rct2.scenery_group.scgsnow" },
    { "rct2.scgwwest", "rct2.scenery_group.scgwwest" },
    { "rct2.scggardn", "rct2.scenery_group.scggardn" },
    { "rct2.scgwalls", "rct2.scenery_group.scgwalls" },
    { "rct2.scgindus", "rct2.scenery_group.scgindus" },
    { "rct2.scgjungl", "rct2.scenery_group.scgjungl" },
    { "rct2.scgmedie", "rct2.scenery_group.scgmedie" },
    { "rct2.music.rock1", "rct2.music.rock1" },
    { "rct2.music.toyland", "rct2.music.toyland" },
    { "rct2.music.fantasy", "rct2.music.fantasy" },
    { "rct2.music.custom2", "rct2.music.custom2" },
    { "rct2.music.candy", "rct2.music.candy" },
    { "rct2.music.egyptian", "rct2.music.egyptian" },
    { "rct2.music.pirate", "rct2.music.pirate" },
    { "rct2.music.ice", "rct2.music.ice" },
    { "rct2.music.techno", "rct2.music.techno" },
    { "rct2.music.jurassic", "rct2.music.jurassic" },
    { "rct2.music.medieval", "rct2.music.medieval" },
    { "rct2.music.rock2", "rct2.music.rock2" },
    { "rct2.music.modern", "rct2.music.modern" },
    { "rct2.music.ragtime", "rct2.music.ragtime" },
    { "rct2.music.fairground", "rct2.music.fairground" },
    { "rct2.music.jungle", "rct2.music.jungle" },
    { "rct2.music.dodgems", "rct2.music.dodgems" },
    { "rct2.music.gentle", "rct2.music.gentle" },
    { "rct2.music.summer", "rct2.music.summer" },
    { "rct2.music.oriental", "rct2.music.oriental" },
    { "rct2.music.wildwest", "rct2.music.wildwest" },
    { "rct2.music.space", "rct2.music.space" },
    { "rct2.music.horror", "rct2.music.horror" },
    { "rct2.music.custom1", "rct2.music.custom1" },
    { "rct2.music.snow", "rct2.music.snow" },
    { "rct2.music.rock3", "rct2.music.rock3" },
    { "rct2.music.organ", "rct2.music.organ" },
    { "rct2.music.mechanical", "rct2.music.mechanical" },
    { "rct2.music", "rct2.music.water" },
    { "rct2.music.urban", "rct2.music.urban" },
    { "rct2.music.martian", "rct2.music.martian" },
    { "rct2.music.roman", "rct2.music.roman" },
    { "rct2.bn1", "rct2.footpath_banner.bn1" },
    { "rct2.bn3", "rct2.footpath_banner.bn3" },
    { "rct2.bn7", "rct2.footpath_banner.bn7" },
    { "rct2.bn6", "rct2.footpath_banner.bn6" },
    { "rct2.bn5", "rct2.footpath_banner.bn5" },
    { "rct2.bn8", "rct2.footpath_banner.bn8" },
    { "rct2.bn9", "rct2.footpath_banner.bn9" },
    { "rct2.bn2", "rct2.footpath_banner.bn2" },
    { "rct2.bn4", "rct2.footpath_banner.bn4" },
    { "rct2.wtrorng", "rct2.water.wtrorng" },
    { "rct2.wtrgreen", "rct2.water.wtrgreen" },
    { "rct2.wtrgrn", "rct2.water.wtrgrn" },
    { "rct2.wtrcyan", "rct2.water.wtrcyan" },
    { "rct2.trf3", "rct2.scenery_small.trf3" },
    { "rct2.tl3", "rct2.scenery_small.tl3" },
    { "rct2.tl0", "rct2.scenery_small.tl0" },
    { "rct2.stldw", "rct2.scenery_small.stldw" },
    { "rct2.trms", "rct2.scenery_small.trms" },
    { "rct2.tsm", "rct2.scenery_small.tsm" },
    { "rct2.tef", "rct2.scenery_small.tef" },
    { "rct2.tes1", "rct2.scenery_small.tes1" },
    { "rct2.hang1", "rct2.scenery_small.hang1" },
    { "rct2.tsk", "rct2.scenery_small.tsk" },
    { "rct2.tmg", "rct2.scenery_small.tmg" },
    { "rct2.jeldrop1", "rct2.scenery_small.jeldrop1" },
    { "rct2.chest1", "rct2.scenery_small.chest1" },
    { "rct2.trf", "rct2.scenery_small.trf" },
    { "rct2.ttf", "rct2.scenery_small.ttf" },
    { "rct2.sktdw2", "rct2.scenery_small.sktdw2" },
    { "rct2.roof5", "rct2.scenery_small.roof5" },
    { "rct2.tww", "rct2.scenery_small.tww" },
    { "rct2.tbn", "rct2.scenery_small.tbn" },
    { "rct2.totem1", "rct2.scenery_small.totem1" },
    { "rct2.brbase2", "rct2.scenery_small.brbase2" },
    { "rct2.chest2", "rct2.scenery_small.chest2" },
    { "rct2.tct", "rct2.scenery_small.tct" },
    { "rct2.tge4", "rct2.scenery_small.tge4" },
    { "rct2.tsp2", "rct2.scenery_small.tsp2" },
    { "rct2.stlbaset", "rct2.scenery_small.stlbaset" },
    { "rct2.tgs3", "rct2.scenery_small.tgs3" },
    { "rct2.tep", "rct2.scenery_small.tep" },
    { "rct2.tq2", "rct2.scenery_small.tq2" },
    { "rct2.tf2", "rct2.scenery_small.tf2" },
    { "rct2.tgs1", "rct2.scenery_small.tgs1" },
    { "rct2.tic", "rct2.scenery_small.tic" },
    { "rct2.tgc1", "rct2.scenery_small.tgc1" },
    { "rct2.tsf1", "rct2.scenery_small.tsf1" },
    { "rct2.tjt4", "rct2.scenery_small.tjt4" },
    { "rct2.cog2r", "rct2.scenery_small.cog2r" },
    { "rct2.cog2", "rct2.scenery_small.cog2" },
    { "rct2.tsph", "rct2.scenery_small.tsph" },
    { "rct2.tm0", "rct2.scenery_small.tm0" },
    { "rct2.tg10", "rct2.scenery_small.tg10" },
    { "rct2.tlp", "rct2.scenery_small.tlp" },
    { "rct2.beanst1", "rct2.scenery_small.beanst1" },
    { "rct2.georoof1", "rct2.scenery_small.georoof1" },
    { "rct2.allsort2", "rct2.scenery_small.allsort2" },
    { "rct2.tge1", "rct2.scenery_small.tge1" },
    { "rct2.tsh", "rct2.scenery_small.tsh" },
    { "rct2.tgs2", "rct2.scenery_small.tgs2" },
    { "rct2.cnballs", "rct2.scenery_small.cnballs" },
    { "rct2.tos", "rct2.scenery_small.tos" },
    { "rct2.colagum", "rct2.scenery_small.colagum" },
    { "rct2.brbase", "rct2.scenery_small.brbase" },
    { "rct2.tsnb", "rct2.scenery_small.tsnb" },
    { "rct2.mment2", "rct2.scenery_small.mment2" },
    { "rct2.tg12", "rct2.scenery_small.tg12" },
    { "rct2.tsh4", "rct2.scenery_small.tsh4" },
    { "rct2.tjp2", "rct2.scenery_small.tjp2" },
    { "rct2.tg17", "rct2.scenery_small.tg17" },
    { "rct2.tmj", "rct2.scenery_small.tmj" },
    { "rct2.tgg", "rct2.scenery_small.tgg" },
    { "rct2.tg7", "rct2.scenery_small.tg7" },
    { "rct2.tmo3", "rct2.scenery_small.tmo3" },
    { "rct2.tmw", "rct2.scenery_small.tmw" },
    { "rct2.thl", "rct2.scenery_small.thl" },
    { "rct2.roof9", "rct2.scenery_small.roof9" },
    { "rct2.chocroof", "rct2.scenery_small.chocroof" },
    { "rct2.ten", "rct2.scenery_small.ten" },
    { "rct2.roof14", "rct2.scenery_small.roof14" },
    { "rct2.tg15", "rct2.scenery_small.tg15" },
    { "rct2.tdf", "rct2.scenery_small.tdf" },
    { "rct2.tot2", "rct2.scenery_small.tot2" },
    { "rct2.tbp", "rct2.scenery_small.tbp" },
    { "rct2.tg19", "rct2.scenery_small.tg19" },
    { "rct2.helmet1", "rct2.scenery_small.helmet1" },
    { "rct2.tg14", "rct2.scenery_small.tg14" },
    { "rct2.tsh1", "rct2.scenery_small.tsh1" },
    { "rct2.tb1", "rct2.scenery_small.tb1" },
    { "rct2.twf", "rct2.scenery_small.twf" },
    { "rct2.tr1", "rct2.scenery_small.tr1" },
    { "rct2.tropt1", "rct2.scenery_small.tropt1" },
    { "rct2.suppw3", "rct2.scenery_small.suppw3" },
    { "rct2.tg4", "rct2.scenery_small.tg4" },
    { "rct2.pirroof2", "rct2.scenery_small.pirroof2" },
    { "rct2.tg5", "rct2.scenery_small.tg5" },
    { "rct2.twp", "rct2.scenery_small.twp" },
    { "rct2.allsort1", "rct2.scenery_small.allsort1" },
    { "rct2.tjt1", "rct2.scenery_small.tjt1" },
    { "rct2.tml", "rct2.scenery_small.tml" },
    { "rct2.ball2", "rct2.scenery_small.ball2" },
    { "rct2.cwfcrv32", "rct2.scenery_small.cwfcrv32" },
    { "rct2.tbr2", "rct2.scenery_small.tbr2" },
    { "rct2.tot1", "rct2.scenery_small.tot1" },
    { "rct2.roof8", "rct2.scenery_small.roof8" },
    { "rct2.tghc", "rct2.scenery_small.tghc" },
    { "rct2.lolly1", "rct2.scenery_small.lolly1" },
    { "rct2.roof10", "rct2.scenery_small.roof10" },
    { "rct2.tas3", "rct2.scenery_small.tas3" },
    { "rct2.tmo1", "rct2.scenery_small.tmo1" },
    { "rct2.sktdw", "rct2.scenery_small.sktdw" },
    { "rct2.corroof2", "rct2.scenery_small.corroof2" },
    { "rct2.prcan", "rct2.scenery_small.prcan" },
    { "rct2.tjf", "rct2.scenery_small.tjf" },
    { "rct2.tscp", "rct2.scenery_small.tscp" },
    { "rct2.tdt2", "rct2.scenery_small.tdt2" },
    { "rct2.cwbcrv32", "rct2.scenery_small.cwbcrv32" },
    { "rct2.beanst2", "rct2.scenery_small.beanst2" },
    { "rct2.twh2", "rct2.scenery_small.twh2" },
    { "rct2.tst5", "rct2.scenery_small.tst5" },
    { "rct2.tcrp", "rct2.scenery_small.tcrp" },
    { "rct2.tpm", "rct2.scenery_small.tpm" },
    { "rct2.tst3", "rct2.scenery_small.tst3" },
    { "rct2.romroof2", "rct2.scenery_small.romroof2" },
    { "rct2.ggrs1", "rct2.scenery_small.ggrs1" },
    { "rct2.cwbcrv33", "rct2.scenery_small.cwbcrv33" },
    { "rct2.stbase", "rct2.scenery_small.stbase" },
    { "rct2.carrot", "rct2.scenery_small.carrot" },
    { "rct2.buttfly", "rct2.scenery_small.buttfly" },
    { "rct2.tal", "rct2.scenery_small.tal" },
    { "rct2.trws", "rct2.scenery_small.trws" },
    { "rct2.tbr3", "rct2.scenery_small.tbr3" },
    { "rct2.tsmp", "rct2.scenery_small.tsmp" },
    { "rct2.tel", "rct2.scenery_small.tel" },
    { "rct2.thrs", "rct2.scenery_small.thrs" },
    { "rct2.tl1", "rct2.scenery_small.tl1" },
    { "rct2.tstd", "rct2.scenery_small.tstd" },
    { "rct2.twn", "rct2.scenery_small.twn" },
    { "rct2.ts3", "rct2.scenery_small.ts3" },
    { "rct2.ts1", "rct2.scenery_small.ts1" },
    { "rct2.tmo5", "rct2.scenery_small.tmo5" },
    { "rct2.twh1", "rct2.scenery_small.twh1" },
    { "rct2.tdm", "rct2.scenery_small.tdm" },
    { "rct2.tg9", "rct2.scenery_small.tg9" },
    { "rct2.torn1", "rct2.scenery_small.torn1" },
    { "rct2.tqf", "rct2.scenery_small.tqf" },
    { "rct2.snail", "rct2.scenery_small.snail" },
    { "rct2.tcd", "rct2.scenery_small.tcd" },
    { "rct2.tas", "rct2.scenery_small.tas" },
    { "rct2.spider1", "rct2.scenery_small.spider1" },
    { "rct2.tst1", "rct2.scenery_small.tst1" },
    { "rct2.roof13", "rct2.scenery_small.roof13" },
    { "rct2.tjt5", "rct2.scenery_small.tjt5" },
    { "rct2.tct2", "rct2.scenery_small.tct2" },
    { "rct2.ts4", "rct2.scenery_small.ts4" },
    { "rct2.wspout", "rct2.scenery_small.wspout" },
    { "rct2.tst4", "rct2.scenery_small.tst4" },
    { "rct2.wdbase", "rct2.scenery_small.wdbase" },
    { "rct2.toh2", "rct2.scenery_small.toh2" },
    { "rct2.igroof", "rct2.scenery_small.igroof" },
    { "rct2.ball3", "rct2.scenery_small.ball3" },
    { "rct2.tsf3", "rct2.scenery_small.tsf3" },
    { "rct2.fern1", "rct2.scenery_small.fern1" },
    { "rct2.mment1", "rct2.scenery_small.mment1" },
    { "rct2.brcrrf1", "rct2.scenery_small.brcrrf1" },
    { "rct2.sktbase", "rct2.scenery_small.sktbase" },
    { "rct2.cog2ur", "rct2.scenery_small.cog2ur" },
    { "rct2.wag1", "rct2.scenery_small.wag1" },
    { "rct2.tk4", "rct2.scenery_small.tk4" },
    { "rct2.tdn4", "rct2.scenery_small.tdn4" },
    { "rct2.ball4", "rct2.scenery_small.ball4" },
    { "rct2.mallow1", "rct2.scenery_small.mallow1" },
    { "rct2.tjb2", "rct2.scenery_small.tjb2" },
    { "rct2.tmbj", "rct2.scenery_small.tmbj" },
    { "rct2.smskull", "rct2.scenery_small.smskull" },
    { "rct2.tg21", "rct2.scenery_small.tg21" },
    { "rct2.tgs4", "rct2.scenery_small.tgs4" },
    { "rct2.tct1", "rct2.scenery_small.tct1" },
    { "rct2.tk3", "rct2.scenery_small.tk3" },
    { "rct2.tbr1", "rct2.scenery_small.tbr1" },
    { "rct2.tsnc", "rct2.scenery_small.tsnc" },
    { "rct2.tbc", "rct2.scenery_small.tbc" },
    { "rct2.tg16", "rct2.scenery_small.tg16" },
    { "rct2.pirflag", "rct2.scenery_small.pirflag" },
    { "rct2.toh1", "rct2.scenery_small.toh1" },
    { "rct2.tcc", "rct2.scenery_small.tcc" },
    { "rct2.roof1", "rct2.scenery_small.roof1" },
    { "rct2.cog1r", "rct2.scenery_small.cog1r" },
    { "rct2.th1", "rct2.scenery_small.th1" },
    { "rct2.tg3", "rct2.scenery_small.tg3" },
    { "rct2.tsh0", "rct2.scenery_small.tsh0" },
    { "rct2.tcb", "rct2.scenery_small.tcb" },
    { "rct2.roof4", "rct2.scenery_small.roof4" },
    { "rct2.tht", "rct2.scenery_small.tht" },
    { "rct2.minroof1", "rct2.scenery_small.minroof1" },
    { "rct2.tp1", "rct2.scenery_small.tp1" },
    { "rct2.tsh5", "rct2.scenery_small.tsh5" },
    { "rct2.tgc2", "rct2.scenery_small.tgc2" },
    { "rct2.ttg", "rct2.scenery_small.ttg" },
    { "rct2.tjb3", "rct2.scenery_small.tjb3" },
    { "rct2.romroof1", "rct2.scenery_small.romroof1" },
    { "rct2.tco", "rct2.scenery_small.tco" },
    { "rct2.pipe32", "rct2.scenery_small.pipe32" },
    { "rct2.sktbaset", "rct2.scenery_small.sktbaset" },
    { "rct2.tcf", "rct2.scenery_small.tcf" },
    { "rct2.tnss", "rct2.scenery_small.tnss" },
    { "rct2.tot4", "rct2.scenery_small.tot4" },
    { "rct2.tus", "rct2.scenery_small.tus" },
    { "rct2.tsg", "rct2.scenery_small.tsg" },
    { "rct2.roof2", "rct2.scenery_small.roof2" },
    { "rct2.suppleg2", "rct2.scenery_small.suppleg2" },
    { "rct2.georoof2", "rct2.scenery_small.georoof2" },
    { "rct2.tac", "rct2.scenery_small.tac" },
    { "rct2.tce", "rct2.scenery_small.tce" },
    { "rct2.tbw", "rct2.scenery_small.tbw" },
    { "rct2.stldw2", "rct2.scenery_small.stldw2" },
    { "rct2.cog2u", "rct2.scenery_small.cog2u" },
    { "rct2.tdt1", "rct2.scenery_small.tdt1" },
    { "rct2.th2", "rct2.scenery_small.th2" },
    { "rct2.ts5", "rct2.scenery_small.ts5" },
    { "rct2.tgh1", "rct2.scenery_small.tgh1" },
    { "rct2.cog1u", "rct2.scenery_small.cog1u" },
    { "rct2.tsp1", "rct2.scenery_small.tsp1" },
    { "rct2.tcfs", "rct2.scenery_small.tcfs" },
    { "rct2.tjb1", "rct2.scenery_small.tjb1" },
    { "rct2.suppw1", "rct2.scenery_small.suppw1" },
    { "rct2.tk1", "rct2.scenery_small.tk1" },
    { "rct2.tvl", "rct2.scenery_small.tvl" },
    { "rct2.tghc2", "rct2.scenery_small.tghc2" },
    { "rct2.trfs", "rct2.scenery_small.trfs" },
    { "rct2.tly", "rct2.scenery_small.tly" },
    { "rct2.wag2", "rct2.scenery_small.wag2" },
    { "rct2.tg13", "rct2.scenery_small.tg13" },
    { "rct2.tgh2", "rct2.scenery_small.tgh2" },
    { "rct2.tas1", "rct2.scenery_small.tas1" },
    { "rct2.tge2", "rct2.scenery_small.tge2" },
    { "rct2.tmc", "rct2.scenery_small.tmc" },
    { "rct2.tap", "rct2.scenery_small.tap" },
    { "rct2.tbr", "rct2.scenery_small.tbr" },
    { "rct2.tg6", "rct2.scenery_small.tg6" },
    { "rct2.tsc", "rct2.scenery_small.tsc" },
    { "rct2.jelbab1", "rct2.scenery_small.jelbab1" },
    { "rct2.tmo4", "rct2.scenery_small.tmo4" },
    { "rct2.tns", "rct2.scenery_small.tns" },
    { "rct2.tcl", "rct2.scenery_small.tcl" },
    { "rct2.tas2", "rct2.scenery_small.tas2" },
    { "rct2.tl2", "rct2.scenery_small.tl2" },
    { "rct2.roof12", "rct2.scenery_small.roof12" },
    { "rct2.trf2", "rct2.scenery_small.trf2" },
    { "rct2.tmo2", "rct2.scenery_small.tmo2" },
    { "rct2.tg20", "rct2.scenery_small.tg20" },
    { "rct2.tjt3", "rct2.scenery_small.tjt3" },
    { "rct2.tm3", "rct2.scenery_small.tm3" },
    { "rct2.tb2", "rct2.scenery_small.tb2" },
    { "rct2.teepee1", "rct2.scenery_small.teepee1" },
    { "rct2.tot3", "rct2.scenery_small.tot3" },
    { "rct2.pirroof1", "rct2.scenery_small.pirroof1" },
    { "rct2.jeldrop2", "rct2.scenery_small.jeldrop2" },
    { "rct2.tntroof1", "rct2.scenery_small.tntroof1" },
    { "rct2.toh3", "rct2.scenery_small.toh3" },
    { "rct2.pipe8", "rct2.scenery_small.pipe8" },
    { "rct2.tsh2", "rct2.scenery_small.tsh2" },
    { "rct2.tbn1", "rct2.scenery_small.tbn1" },
    { "rct2.tcy", "rct2.scenery_small.tcy" },
    { "rct2.wasp", "rct2.scenery_small.wasp" },
    { "rct2.tmm3", "rct2.scenery_small.tmm3" },
    { "rct2.ters", "rct2.scenery_small.ters" },
    { "rct2.tsp", "rct2.scenery_small.tsp" },
    { "rct2.ts6", "rct2.scenery_small.ts6" },
    { "rct2.tp2", "rct2.scenery_small.tp2" },
    { "rct2.torn2", "rct2.scenery_small.torn2" },
    { "rct2.tst2", "rct2.scenery_small.tst2" },
    { "rct2.tm2", "rct2.scenery_small.tm2" },
    { "rct2.tf1", "rct2.scenery_small.tf1" },
    { "rct2.tsb", "rct2.scenery_small.tsb" },
    { "rct2.mment3", "rct2.scenery_small.mment3" },
    { "rct2.tg11", "rct2.scenery_small.tg11" },
    { "rct2.ts2", "rct2.scenery_small.ts2" },
    { "rct2.tms1", "rct2.scenery_small.tms1" },
    { "rct2.spcroof1", "rct2.scenery_small.spcroof1" },
    { "rct2.tk2", "rct2.scenery_small.tk2" },
    { "rct2.whoriz", "rct2.scenery_small.whoriz" },
    { "rct2.tjt6", "rct2.scenery_small.tjt6" },
    { "rct2.brcrrf2", "rct2.scenery_small.brcrrf2" },
    { "rct2.suppleg1", "rct2.scenery_small.suppleg1" },
    { "rct2.roof11", "rct2.scenery_small.roof11" },
    { "rct2.tas4", "rct2.scenery_small.tas4" },
    { "rct2.tjt2", "rct2.scenery_small.tjt2" },
    { "rct2.tsd", "rct2.scenery_small.tsd" },
    { "rct2.mint1", "rct2.scenery_small.mint1" },
    { "rct2.corroof", "rct2.scenery_small.corroof" },
    { "rct2.brbase3", "rct2.scenery_small.brbase3" },
    { "rct2.pipe32j", "rct2.scenery_small.pipe32j" },
    { "rct2.tsf2", "rct2.scenery_small.tsf2" },
    { "rct2.tg18", "rct2.scenery_small.tg18" },
    { "rct2.tdt3", "rct2.scenery_small.tdt3" },
    { "rct2.tq1", "rct2.scenery_small.tq1" },
    { "rct2.tge5", "rct2.scenery_small.tge5" },
    { "rct2.cndyrk1", "rct2.scenery_small.cndyrk1" },
    { "rct2.tsc2", "rct2.scenery_small.tsc2" },
    { "rct2.tg2", "rct2.scenery_small.tg2" },
    { "rct2.roof3", "rct2.scenery_small.roof3" },
    { "rct2.cog1ur", "rct2.scenery_small.cog1ur" },
    { "rct2.jbean1", "rct2.scenery_small.jbean1" },
    { "rct2.icecube", "rct2.scenery_small.icecube" },
    { "rct2.suppw2", "rct2.scenery_small.suppw2" },
    { "rct2.tsq", "rct2.scenery_small.tsq" },
    { "rct2.sumrf", "rct2.scenery_small.sumrf" },
    { "rct2.roof7", "rct2.scenery_small.roof7" },
    { "rct2.tcn", "rct2.scenery_small.tcn" },
    { "rct2.ts0", "rct2.scenery_small.ts0" },
    { "rct2.tsh3", "rct2.scenery_small.tsh3" },
    { "rct2.tcj", "rct2.scenery_small.tcj" },
    { "rct2.badshut2", "rct2.scenery_small.badshut2" },
    { "rct2.tt1", "rct2.scenery_small.tt1" },
    { "rct2.wdiag", "rct2.scenery_small.wdiag" },
    { "rct2.tg8", "rct2.scenery_small.tg8" },
    { "rct2.jngroof1", "rct2.scenery_small.jngroof1" },
    { "rct2.tg1", "rct2.scenery_small.tg1" },
    { "rct2.tck", "rct2.scenery_small.tck" },
    { "rct2.badshut", "rct2.scenery_small.badshut" },
    { "rct2.cog1", "rct2.scenery_small.cog1" },
    { "rct2.ball1", "rct2.scenery_small.ball1" },
    { "rct2.pagroof1", "rct2.scenery_small.pagroof1" },
    { "rct2.chbbase", "rct2.scenery_small.chbbase" },
    { "rct2.tjb4", "rct2.scenery_small.tjb4" },
    { "rct2.tjp1", "rct2.scenery_small.tjp1" },
    { "rct2.cwfcrv33", "rct2.scenery_small.cwfcrv33" },
    { "rct2.mallow2", "rct2.scenery_small.mallow2" },
    { "rct2.roof6", "rct2.scenery_small.roof6" },
    { "rct2.tmzp", "rct2.scenery_small.tmzp" },
    { "rct2.tlc", "rct2.scenery_small.tlc" },
    { "rct2.tmm1", "rct2.scenery_small.tmm1" },
    { "rct2.tig", "rct2.scenery_small.tig" },
    { "rct2.tge3", "rct2.scenery_small.tge3" },
    { "rct2.tgs", "rct2.scenery_small.tgs" },
    { "rct2.titc", "rct2.scenery_small.titc" },
    { "rct2.terb", "rct2.scenery_small.terb" },
    { "rct2.tmm2", "rct2.scenery_small.tmm2" },
    { "rct2.tbr4", "rct2.scenery_small.tbr4" },
    { "rct2.tdn5", "rct2.scenery_small.tdn5" },
    { "rct2.tmp", "rct2.scenery_small.tmp" },
    { "rct2.trc", "rct2.scenery_small.trc" },
    { "rct2.tm1", "rct2.scenery_small.tm1" },
    { "rct2.tr2", "rct2.scenery_small.tr2" },
    { "rct2.fire1", "rct2.scenery_small.fire1" },
    { "rct2.wc7", "rct2.scenery_wall.wc7" },
    { "rct2.wc4", "rct2.scenery_wall.wc4" },
    { "rct2.wmf", "rct2.scenery_wall.wmf" },
    { "rct2.wc2", "rct2.scenery_wall.wc2" },
    { "rct2.wfwg", "rct2.scenery_wall.wfwg" },
    { "rct2.wallcfar", "rct2.scenery_wall.wallcfar" },
    { "rct2.wallcfwn", "rct2.scenery_wall.wallcfwn" },
    { "rct2.wallco16", "rct2.scenery_wall.wallco16" },
    { "rct2.wallpr34", "rct2.scenery_wall.wallpr34" },
    { "rct2.wallbb16", "rct2.scenery_wall.wallbb16" },
    { "rct2.wallmm17", "rct2.scenery_wall.wallmm17" },
    { "rct2.wallrs32", "rct2.scenery_wall.wallrs32" },
    { "rct2.wpw3", "rct2.scenery_wall.wpw3" },
    { "rct2.wew", "rct2.scenery_wall.wew" },
    { "rct2.wallsp32", "rct2.scenery_wall.wallsp32" },
    { "rct2.wallbb34", "rct2.scenery_wall.wallbb34" },
    { "rct2.wallsign", "rct2.scenery_wall.wallsign" },
    { "rct2.wallwd16", "rct2.scenery_wall.wallwd16" },
    { "rct2.wwtw", "rct2.scenery_wall.wwtw" },
    { "rct2.wallgl32", "rct2.scenery_wall.wallgl32" },
    { "rct2.wallcf16", "rct2.scenery_wall.wallcf16" },
    { "rct2.wwtwa", "rct2.scenery_wall.wwtwa" },
    { "rct2.walljn32", "rct2.scenery_wall.walljn32" },
    { "rct2.whg", "rct2.scenery_wall.whg" },
    { "rct2.wallmn32", "rct2.scenery_wall.wallmn32" },
    { "rct2.wpw2", "rct2.scenery_wall.wpw2" },
    { "rct2.wbr2a", "rct2.scenery_wall.wbr2a" },
    { "rct2.wsw", "rct2.scenery_wall.wsw" },
    { "rct2.wmw", "rct2.scenery_wall.wmw" },
    { "rct2.wallnt32", "rct2.scenery_wall.wallnt32" },
    { "rct2.wallpr33", "rct2.scenery_wall.wallpr33" },
    { "rct2.wallcf32", "rct2.scenery_wall.wallcf32" },
    { "rct2.wc8", "rct2.scenery_wall.wc8" },
    { "rct2.wc5", "rct2.scenery_wall.wc5" },
    { "rct2.wallu132", "rct2.scenery_wall.wallu132" },
    { "rct2.wallbadm", "rct2.scenery_wall.wallbadm" },
    { "rct2.wsw2", "rct2.scenery_wall.wsw2" },
    { "rct2.wc1", "rct2.scenery_wall.wc1" },
    { "rct2.wallsk16", "rct2.scenery_wall.wallsk16" },
    { "rct2.wsw1", "rct2.scenery_wall.wsw1" },
    { "rct2.wallcfpc", "rct2.scenery_wall.wallcfpc" },
    { "rct2.wc12", "rct2.scenery_wall.wc12" },
    { "rct2.wallpost", "rct2.scenery_wall.wallpost" },
    { "rct2.whgg", "rct2.scenery_wall.whgg" },
    { "rct2.wc10", "rct2.scenery_wall.wc10" },
    { "rct2.wc14", "rct2.scenery_wall.wc14" },
    { "rct2.wrw", "rct2.scenery_wall.wrw" },
    { "rct2.wallcb8", "rct2.scenery_wall.wallcb8" },
    { "rct2.wcw2", "rct2.scenery_wall.wcw2" },
    { "rct2.wchg", "rct2.scenery_wall.wchg" },
    { "rct2.wbr1a", "rct2.scenery_wall.wbr1a" },
    { "rct2.wallpg32", "rct2.scenery_wall.wallpg32" },
    { "rct2.wpf", "rct2.scenery_wall.wpf" },
    { "rct2.wallnt33", "rct2.scenery_wall.wallnt33" },
    { "rct2.wallcx32", "rct2.scenery_wall.wallcx32" },
    { "rct2.wallst16", "rct2.scenery_wall.wallst16" },
    { "rct2.wc9", "rct2.scenery_wall.wc9" },
    { "rct2.wpfg", "rct2.scenery_wall.wpfg" },
    { "rct2.wc13", "rct2.scenery_wall.wc13" },
    { "rct2.wjf", "rct2.scenery_wall.wjf" },
    { "rct2.wallbr16", "rct2.scenery_wall.wallbr16" },
    { "rct2.wbrg", "rct2.scenery_wall.wbrg" },
    { "rct2.wc11", "rct2.scenery_wall.wc11" },
    { "rct2.wallbb33", "rct2.scenery_wall.wallbb33" },
    { "rct2.wallwf32", "rct2.scenery_wall.wallwf32" },
    { "rct2.wfw1", "rct2.scenery_wall.wfw1" },
    { "rct2.wallgl8", "rct2.scenery_wall.wallgl8" },
    { "rct2.wallrs8", "rct2.scenery_wall.wallrs8" },
    { "rct2.wallsc16", "rct2.scenery_wall.wallsc16" },
    { "rct2.wallcy32", "rct2.scenery_wall.wallcy32" },
    { "rct2.wallpr32", "rct2.scenery_wall.wallpr32" },
    { "rct2.wallu232", "rct2.scenery_wall.wallu232" },
    { "rct2.wallbb32", "rct2.scenery_wall.wallbb32" },
    { "rct2.wallgl16", "rct2.scenery_wall.wallgl16" },
    { "rct2.wallst32", "rct2.scenery_wall.wallst32" },
    { "rct2.wallrh32", "rct2.scenery_wall.wallrh32" },
    { "rct2.wallst8", "rct2.scenery_wall.wallst8" },
    { "rct2.wswg", "rct2.scenery_wall.wswg" },
    { "rct2.wallrs16", "rct2.scenery_wall.wallrs16" },
    { "rct2.wallcf8", "rct2.scenery_wall.wallcf8" },
    { "rct2.wallwd33", "rct2.scenery_wall.wallwd33" },
    { "rct2.wbr3", "rct2.scenery_wall.wbr3" },
    { "rct2.wallcbdr", "rct2.scenery_wall.wallcbdr" },
    { "rct2.wallbr8", "rct2.scenery_wall.wallbr8" },
    { "rct2.wallcbpc", "rct2.scenery_wall.wallcbpc" },
    { "rct2.wallbb8", "rct2.scenery_wall.wallbb8" },
    { "rct2.wallmm16", "rct2.scenery_wall.wallmm16" },
    { "rct2.wallcb16", "rct2.scenery_wall.wallcb16" },
    { "rct2.wallcz32", "rct2.scenery_wall.wallcz32" },
    { "rct2.walltn32", "rct2.scenery_wall.walltn32" },
    { "rct2.wallstfn", "rct2.scenery_wall.wallstfn" },
    { "rct2.wallwd8", "rct2.scenery_wall.wallwd8" },
    { "rct2.wallig24", "rct2.scenery_wall.wallig24" },
    { "rct2.wallbr32", "rct2.scenery_wall.wallbr32" },
    { "rct2.wmfg", "rct2.scenery_wall.wmfg" },
    { "rct2.wallstwn", "rct2.scenery_wall.wallstwn" },
    { "rct2.walljb16", "rct2.scenery_wall.walljb16" },
    { "rct2.wallcbwn", "rct2.scenery_wall.wallcbwn" },
    { "rct2.wc18", "rct2.scenery_wall.wc18" },
    { "rct2.wbr2", "rct2.scenery_wall.wbr2" },
    { "rct2.wallrk32", "rct2.scenery_wall.wallrk32" },
    { "rct2.wcw1", "rct2.scenery_wall.wcw1" },
    { "rct2.walllt32", "rct2.scenery_wall.walllt32" },
    { "rct2.wpw1", "rct2.scenery_wall.wpw1" },
    { "rct2.wgw2", "rct2.scenery_wall.wgw2" },
    { "rct2.walltxgt", "rct2.scenery_wall.walltxgt" },
    { "rct2.wc6", "rct2.scenery_wall.wc6" },
    { "rct2.wallbrdr", "rct2.scenery_wall.wallbrdr" },
    { "rct2.wallcw32", "rct2.scenery_wall.wallcw32" },
    { "rct2.wc17", "rct2.scenery_wall.wc17" },
    { "rct2.wallcfdr", "rct2.scenery_wall.wallcfdr" },
    { "rct2.wallbrwn", "rct2.scenery_wall.wallbrwn" },
    { "rct2.wc3", "rct2.scenery_wall.wc3" },
    { "rct2.wallwd32", "rct2.scenery_wall.wallwd32" },
    { "rct2.wallwdps", "rct2.scenery_wall.wallwdps" },
    { "rct2.wrwa", "rct2.scenery_wall.wrwa" },
    { "rct2.wch", "rct2.scenery_wall.wch" },
    { "rct2.wbw", "rct2.scenery_wall.wbw" },
    { "rct2.wc15", "rct2.scenery_wall.wc15" },
    { "rct2.wallpr35", "rct2.scenery_wall.wallpr35" },
    { "rct2.wmww", "rct2.scenery_wall.wmww" },
    { "rct2.wallsk32", "rct2.scenery_wall.wallsk32" },
    { "rct2.wc16", "rct2.scenery_wall.wc16" },
    { "rct2.wallcb32", "rct2.scenery_wall.wallcb32" },
    { "rct2.wallig16", "rct2.scenery_wall.wallig16" },
    { "rct2.wbr1", "rct2.scenery_wall.wbr1" },
    { "rct2.littersp", "rct2.footpath_item.littersp" },
    { "rct2.litterww", "rct2.footpath_item.litterww" },
    { "rct2.jumpsnw1", "rct2.footpath_item.jumpsnw1" },
    { "rct2.benchspc", "rct2.footpath_item.benchspc" },
    { "rct2.lamppir", "rct2.footpath_item.lamppir" },
    { "rct2.benchpl", "rct2.footpath_item.benchpl" },
    { "rct2.bench1", "rct2.footpath_item.bench1" },
    { "rct2.lampdsy", "rct2.footpath_item.lampdsy" },
    { "rct2.qtv1", "rct2.footpath_item.qtv1" },
    { "rct2.benchlog", "rct2.footpath_item.benchlog" },
    { "rct2.lamp3", "rct2.footpath_item.lamp3" },
    { "rct2.jumpfnt1", "rct2.footpath_item.jumpfnt1" },
    { "rct2.lamp1", "rct2.footpath_item.lamp1" },
    { "rct2.litter1", "rct2.footpath_item.litter1" },
    { "rct2.benchstn", "rct2.footpath_item.benchstn" },
    { "rct2.littermn", "rct2.footpath_item.littermn" },
    { "rct2.lamp4", "rct2.footpath_item.lamp4" },
    { "rct2.lamp2", "rct2.footpath_item.lamp2" },
    { "rct2.railings.bamboobrown", "rct2.footpath_railings.bamboo_brown" },
    { "rct2.railings.space", "rct2.footpath_railings.space" },
    { "rct2.railings.bambooblack", "rct2.footpath_railings.bamboo_black" },
    { "rct2.railings.wood", "rct2.footpath_railings.wood" },
    { "rct2.railings.concretegreen", "rct2.footpath_railings.concrete_green" },
    { "rct2.railings.concrete", "rct2.footpath_railings.concrete" },
    { "rct2.rckc", "rct2.ride.rckc" },
    { "rct2.hchoc", "rct2.ride.hchoc" },
    { "rct2.vekst", "rct2.ride.vekst" },
    { "rct2.c3d", "rct2.ride.c3d" },
    { "rct2.obs1", "rct2.ride.obs1" },
    { "rct2.truck1", "rct2.ride.truck1" },
    { "rct2.arrsw1", "rct2.ride.arrsw1" },
    { "rct2.dough", "rct2.ride.dough" },
    { "rct2.faid1", "rct2.ride.faid1" },
    { "rct2.infok", "rct2.ride.infok" },
    { "rct2.vekdv", "rct2.ride.vekdv" },
    { "rct2.revcar", "rct2.ride.revcar" },
    { "rct2.smono", "rct2.ride.smono" },
    { "rct2.gdrop1", "rct2.ride.gdrop1" },
    { "rct2.tram1", "rct2.ride.tram1" },
    { "rct2.coffs", "rct2.ride.coffs" },
    { "rct2.bmsu", "rct2.ride.bmsu" },
    { "rct2.substl", "rct2.ride.substl" },
    { "rct2.steep2", "rct2.ride.steep2" },
    { "rct2.icetst", "rct2.ride.icetst" },
    { "rct2.intst", "rct2.ride.intst" },
    { "rct2.fsauc", "rct2.ride.fsauc" },
    { "rct2.aml1", "rct2.ride.aml1" },
    { "rct2.souvs", "rct2.ride.souvs" },
    { "rct2.rapboat", "rct2.ride.rapboat" },
    { "rct2.nrl2", "rct2.ride.nrl2" },
    { "rct2.bmsd", "rct2.ride.bmsd" },
    { "rct2.icecr2", "rct2.ride.icecr2" },
    { "rct2.wonton", "rct2.ride.wonton" },
    { "rct2.bnoodles", "rct2.ride.bnoodles" },
    { "rct2.obs2", "rct2.ride.obs2" },
    { "rct2.atm1", "rct2.ride.atm1" },
    { "rct2.ctcar", "rct2.ride.ctcar" },
    { "rct2.topsp1", "rct2.ride.topsp1" },
    { "rct2.tlt1", "rct2.ride.tlt1" },
    { "rct2.sungst", "rct2.ride.sungst" },
    { "rct2.4x4", "rct2.ride.4x4" },
    { "rct2.pmt1", "rct2.ride.pmt1" },
    { "rct2.toffs", "rct2.ride.toffs" },
    { "rct2.steep1", "rct2.ride.steep1" },
    { "rct2.soybean", "rct2.ride.soybean" },
    { "rct2.zldb", "rct2.ride.zldb" },
    { "rct2.ding1", "rct2.ride.ding1" },
    { "rct2.smc1", "rct2.ride.smc1" },
    { "rct2.funcake", "rct2.ride.funcake" },
    { "rct2.skytr", "rct2.ride.skytr" },
    { "rct2.cndyf", "rct2.ride.cndyf" },
    { "rct2.bmrb", "rct2.ride.bmrb" },
    { "rct2.goltr", "rct2.ride.goltr" },
    { "rct2.simpod", "rct2.ride.simpod" },
    { "rct2.amt1", "rct2.ride.amt1" },
    { "rct2.bmfl", "rct2.ride.bmfl" },
    { "rct2.premt1", "rct2.ride.premt1" },
    { "rct2.togst", "rct2.ride.togst" },
    { "rct2.chcks", "rct2.ride.chcks" },
    { "rct2.hhbuild", "rct2.ride.hhbuild" },
    { "rct2.icecr1", "rct2.ride.icecr1" },
    { "rct2.utcar", "rct2.ride.utcar" },
    { "rct2.submar", "rct2.ride.submar" },
    { "rct2.fwh1", "rct2.ride.fwh1" },
    { "rct2.swans", "rct2.ride.swans" },
    { "rct2.bboat", "rct2.ride.bboat" },
    { "rct2.golf1", "rct2.ride.golf1" },
    { "rct2.ptct2r", "rct2.ride.ptct2r" },
    { "rct2.arrx", "rct2.ride.arrx" },
    { "rct2.slcfo", "rct2.ride.slcfo" },
    { "rct2.burgb", "rct2.ride.burgb" },
    { "rct2.hotds", "rct2.ride.hotds" },
    { "rct2.gtc", "rct2.ride.gtc" },
    { "rct2.tshrt", "rct2.ride.tshrt" },
    { "rct2.tlt2", "rct2.ride.tlt2" },
    { "rct2.scht1", "rct2.ride.scht1" },
    { "rct2.mgr1", "rct2.ride.mgr1" },
    { "rct2.swsh2", "rct2.ride.swsh2" },
    { "rct2.wmspin", "rct2.ride.wmspin" },
    { "rct2.chpsh2", "rct2.ride.chpsh2" },
    { "rct2.cstboat", "rct2.ride.cstboat" },
    { "rct2.mono1", "rct2.ride.mono1" },
    { "rct2.balln", "rct2.ride.balln" },
    { "rct2.vekvamp", "rct2.ride.vekvamp" },
    { "rct2.mft", "rct2.ride.mft" },
    { "rct2.hmaze", "rct2.ride.hmaze" },
    { "rct2.sqdst", "rct2.ride.sqdst" },
    { "rct2.intinv", "rct2.ride.intinv" },
    { "rct2.jstar1", "rct2.ride.jstar1" },
    { "rct2.mono3", "rct2.ride.mono3" },
    { "rct2.ssc1", "rct2.ride.ssc1" },
    { "rct2.kart1", "rct2.ride.kart1" },
    { "rct2.cboat", "rct2.ride.cboat" },
    { "rct2.cookst", "rct2.ride.cookst" },
    { "rct2.monbk", "rct2.ride.monbk" },
    { "rct2.swsh1", "rct2.ride.swsh1" },
    { "rct2.trike", "rct2.ride.trike" },
    { "rct2.rftboat", "rct2.ride.rftboat" },
    { "rct2.bmvd", "rct2.ride.bmvd" },
    { "rct2.sbox", "rct2.ride.sbox" },
    { "rct2.sfric1", "rct2.ride.sfric1" },
    { "rct2.arrt1", "rct2.ride.arrt1" },
    { "rct2.thcar", "rct2.ride.thcar" },
    { "rct2.wmouse", "rct2.ride.wmouse" },
    { "rct2.spboat", "rct2.ride.spboat" },
    { "rct2.clift2", "rct2.ride.clift2" },
    { "rct2.jski", "rct2.ride.jski" },
    { "rct2.mcarpet1", "rct2.ride.mcarpet1" },
    { "rct2.lfb1", "rct2.ride.lfb1" },
    { "rct2.ivmc1", "rct2.ride.ivmc1" },
    { "rct2.popcs", "rct2.ride.popcs" },
    { "rct2.vreel", "rct2.ride.vreel" },
    { "rct2.mono2", "rct2.ride.mono2" },
    { "rct2.twist1", "rct2.ride.twist1" },
    { "rct2.pretst", "rct2.ride.pretst" },
    { "rct2.lemst", "rct2.ride.lemst" },
    { "rct2.ptct2", "rct2.ride.ptct2" },
    { "rct2.arrt2", "rct2.ride.arrt2" },
    { "rct2.srings", "rct2.ride.srings" },
    { "rct2.nemt", "rct2.ride.nemt" },
    { "rct2.chknug", "rct2.ride.chknug" },
    { "rct2.nrl", "rct2.ride.nrl" },
    { "rct2.cindr", "rct2.ride.cindr" },
    { "rct2.rcr", "rct2.ride.rcr" },
    { "rct2.rsaus", "rct2.ride.rsaus" },
    { "rct2.frnood", "rct2.ride.frnood" },
    { "rct2.mbsoup", "rct2.ride.mbsoup" },
    { "rct2.chbuild", "rct2.ride.chbuild" },
    { "rct2.zlog", "rct2.ride.zlog" },
    { "rct2.chpsh", "rct2.ride.chpsh" },
    { "rct2.slct", "rct2.ride.slct" },
    { "rct2.lift1", "rct2.ride.lift1" },
    { "rct2.spcar", "rct2.ride.spcar" },
    { "rct2.twist2", "rct2.ride.twist2" },
    { "rct2.hatst", "rct2.ride.hatst" },
    { "rct2.ptct1", "rct2.ride.ptct1" },
    { "rct2.dodg1", "rct2.ride.dodg1" },
    { "rct2.starfrdr", "rct2.ride.starfrdr" },
    { "rct2.clift1", "rct2.ride.clift1" },
    { "rct2.wmmine", "rct2.ride.wmmine" },
    { "rct2.pizzs", "rct2.ride.pizzs" },
    { "rct2.rboat", "rct2.ride.rboat" },
    { "rct2.batfl", "rct2.ride.batfl" },
    { "rct2.hskelt", "rct2.ride.hskelt" },
    { "rct2.utcarr", "rct2.ride.utcarr" },
    { "rct2.vcr", "rct2.ride.vcr" },
    { "rct2.hmcar", "rct2.ride.hmcar" },
    { "rct2.arrsw2", "rct2.ride.arrsw2" },
    { "rct2.helicar", "rct2.ride.helicar" },
    { "rct2.wcatc", "rct2.ride.wcatc" },
    { "rct2.bmair", "rct2.ride.bmair" },
    { "rct2.circus1", "rct2.ride.circus1" },
    { "rct2.bob1", "rct2.ride.bob1" },
    { "rct2.spdrcr", "rct2.ride.spdrcr" },
    { "rct2.enterp", "rct2.ride.enterp" },
    { "rct2.revf1", "rct2.ride.revf1" },
    { "rct2.intbob", "rct2.ride.intbob" },
    { "rct2.smc2", "rct2.ride.smc2" },
    { "rct2.drnks", "rct2.ride.drnks" },
    { "rct2.pkesfh", "rct2.park_entrance.pkesfh" },
    { "rct2.pkent1", "rct2.park_entrance.pkent1" },
    { "rct2.pkemm", "rct2.park_entrance.pkemm" },
    { "rct2.pathsurface.queue_red", "rct2.footpath_surface.queue_red" },
    { "rct2.pathsurface.queue_yellow", "rct2.footpath_surface.queue_yellow" },
    { "rct2.pathsurface.tarmac", "rct2.footpath_surface.tarmac" },
    { "rct2.pathsurface.crazypaving", "rct2.footpath_surface.crazy_paving" },
    { "rct2.pathsurface.ash", "rct2.footpath_surface.ash" },
    { "rct2.pathsurface.queue_green", "rct2.footpath_surface.queue_green" },
    { "rct2.pathsurface.tarmac.green", "rct2.footpath_surface.tarmac_green" },
    { "rct2.pathsurface.queue_blue", "rct2.footpath_surface.queue_blue" },
    { "rct2.pathsurface.space", "rct2.footpath_surface.tarmac_red" },
    { "rct2.pathsurface.tarmac.brown", "rct2.footpath_surface.tarmac_brown" },
    { "rct2.pathsurface.dirt", "rct2.footpath_surface.dirt" },
    { "rct2.pathsurface.road", "rct2.footpath_surface.road" },
    { "rct2.surface.sandred", "rct2.terrain_surface.sand_red" },
    { "rct2.surface.grassclumps", "rct2.terrain_surface.grass_clumps" },
    { "rct2.surface.gridgreen", "rct2.terrain_surface.grid_green" },
    { "rct2.surface.sandbrown", "rct2.terrain_surface.sand_brown" },
    { "rct2.surface.gridyellow", "rct2.terrain_surface.grid_yellow" },
    { "rct2.surface.martian", "rct2.terrain_surface.martian" },
    { "rct2.surface.dirt", "rct2.terrain_surface.dirt" },
    { "rct2.surface.gridred", "rct2.terrain_surface.grid_red" },
    { "rct2.surface.ice", "rct2.terrain_surface.ice" },
    { "rct2.surface.grass", "rct2.terrain_surface.grass" },
    { "rct2.surface.sand", "rct2.terrain_surface.sand" },
    { "rct2.surface.rock", "rct2.terrain_surface.rock" },
    { "rct2.surface.chequerboard", "rct2.terrain_surface.chequerboard" },
    { "rct2.surface.gridpurple", "rct2.terrain_surface.grid_purple" },
    { "rct2.station.castlegrey", "rct2.station.castle_grey" },
    { "rct2.station.space", "rct2.station.space" },
    { "rct2.station.plain", "rct2.station.plain" },
    { "rct2.station.log", "rct2.station.log" },
    { "rct2.station.jungle", "rct2.station.jungle" },
    { "rct2.station.snow", "rct2.station.snow" },
    { "rct2.station.castlebrown", "rct2.station.castle_brown" },
    { "rct2.station.abstract", "rct2.station.abstract" },
    { "rct2.station.pagoda", "rct2.station.pagoda" },
    { "rct2.station.classical", "rct2.station.classical" },
    { "rct2.station.canvastent", "rct2.station.canvas_tent" },
    { "rct2.station.wooden", "rct2.station.wooden" },
    { "rct2.edge.rock", "rct2.terrain_edge.rock" },
    { "rct2.edge.woodred", "rct2.terrain_edge.wood_red" },
    { "rct2.edge.ice", "rct2.terrain_edge.ice" },
    { "rct2.edge.woodblack", "rct2.terrain_edge.wood_black" },
    { "rct2.tt.rdmet2x2", "rct2tt.scenery_large.rdmet2x2" },
    { "rct2.tt.travlr02", "rct2tt.scenery_large.travlr02" },
    { "rct2.tt.romne2x1", "rct2tt.scenery_large.romne2x1" },
    { "rct2.tt.jailxx17", "rct2tt.scenery_large.jailxx17" },
    { "rct2.tt.perdtk02", "rct2tt.scenery_large.perdtk02" },
    { "rct2.tt.schntent", "rct2tt.scenery_large.schntent" },
    { "rct2.tt.hrbwal07", "rct2tt.scenery_large.hrbwal07" },
    { "rct2.tt.alnstr08", "rct2tt.scenery_large.alnstr08" },
    { "rct2.tt.4x4volca", "rct2tt.scenery_large.4x4volca" },
    { "rct2.tt.futsky25", "rct2tt.scenery_large.futsky25" },
    { "rct2.tt.bigdrums", "rct2tt.scenery_large.bigdrums" },
    { "rct2.tt.ploughxx", "rct2tt.scenery_large.ploughxx" },
    { "rct2.tt.oldnyk20", "rct2tt.scenery_large.oldnyk20" },
    { "rct2.tt.hrbwal08", "rct2tt.scenery_large.hrbwal08" },
    { "rct2.tt.psntwl26", "rct2tt.scenery_large.psntwl26" },
    { "rct2.tt.romns2x2", "rct2tt.scenery_large.romns2x2" },
    { "rct2.tt.oldnyk27", "rct2tt.scenery_large.oldnyk27" },
    { "rct2.tt.ghotrod2", "rct2tt.scenery_large.ghotrod2" },
    { "rct2.tt.futsky20", "rct2tt.scenery_large.futsky20" },
    { "rct2.tt.majoroak", "rct2tt.scenery_large.majoroak" },
    { "rct2.tt.artdec25", "rct2tt.scenery_large.artdec25" },
    { "rct2.tt.jailxx18", "rct2tt.scenery_large.jailxx18" },
    { "rct2.tt.futsky26", "rct2tt.scenery_large.futsky26" },
    { "rct2.tt.futsky22", "rct2tt.scenery_large.futsky22" },
    { "rct2.tt.oldnyk32", "rct2tt.scenery_large.oldnyk32" },
    { "rct2.tt.peasthut", "rct2tt.scenery_large.peasthut" },
    { "rct2.tt.histfix2", "rct2tt.scenery_large.histfix2" },
    { "rct2.tt.artdec27", "rct2tt.scenery_large.artdec27" },
    { "rct2.tt.strgs2x2", "rct2tt.scenery_large.strgs2x2" },
    { "rct2.tt.forbidft", "rct2tt.scenery_large.forbidft" },
    { "rct2.tt.histfix1", "rct2tt.scenery_large.histfix1" },
    { "rct2.tt.mcastl13", "rct2tt.scenery_large.mcastl13" },
    { "rct2.tt.futsky31", "rct2tt.scenery_large.futsky31" },
    { "rct2.tt.footprnt", "rct2tt.scenery_large.footprnt" },
    { "rct2.tt.hrbwal09", "rct2tt.scenery_large.hrbwal09" },
    { "rct2.tt.mcastl11", "rct2tt.scenery_large.mcastl11" },
    { "rct2.tt.oldnyk30", "rct2tt.scenery_large.oldnyk30" },
    { "rct2.tt.caventra", "rct2tt.scenery_large.caventra" },
    { "rct2.tt.4x4diner", "rct2tt.scenery_large.4x4diner" },
    { "rct2.tt.futsky24", "rct2tt.scenery_large.futsky24" },
    { "rct2.tt.metoan01", "rct2tt.scenery_large.metoan01" },
    { "rct2.tt.perdtk01", "rct2tt.scenery_large.perdtk01" },
    { "rct2.tt.artdec26", "rct2tt.scenery_large.artdec26" },
    { "rct2.tt.romvc2x2", "rct2tt.scenery_large.romvc2x2" },
    { "rct2.tt.alnstr09", "rct2tt.scenery_large.alnstr09" },
    { "rct2.tt.mcastl17", "rct2tt.scenery_large.mcastl17" },
    { "rct2.tt.strvs2x2", "rct2tt.scenery_large.strvs2x2" },
    { "rct2.tt.romnm2x2", "rct2tt.scenery_large.romnm2x2" },
    { "rct2.tt.gbeetlex", "rct2tt.scenery_large.gbeetlex" },
    { "rct2.tt.gschlbus", "rct2tt.scenery_large.gschlbus" },
    { "rct2.tt.schnpits", "rct2tt.scenery_large.schnpits" },
    { "rct2.tt.shipm4x4", "rct2tt.scenery_large.shipm4x4" },
    { "rct2.tt.romve2x1", "rct2tt.scenery_large.romve2x1" },
    { "rct2.tt.rdmet4x4", "rct2tt.scenery_large.rdmet4x4" },
    { "rct2.tt.psntwl28", "rct2tt.scenery_large.psntwl28" },
    { "rct2.tt.futsky30", "rct2tt.scenery_large.futsky30" },
    { "rct2.tt.rdmeto02", "rct2tt.scenery_large.rdmeto02" },
    { "rct2.tt.jetplan1", "rct2tt.scenery_large.jetplan1" },
    { "rct2.tt.oldnyk24", "rct2tt.scenery_large.oldnyk24" },
    { "rct2.tt.futsky32", "rct2tt.scenery_large.futsky32" },
    { "rct2.tt.peramob2", "rct2tt.scenery_large.peramob2" },
    { "rct2.tt.metoan02", "rct2tt.scenery_large.metoan02" },
    { "rct2.tt.prdyacht", "rct2tt.scenery_large.prdyacht" },
    { "rct2.tt.mcastl14", "rct2tt.scenery_large.mcastl14" },
    { "rct2.tt.mcastl12", "rct2tt.scenery_large.mcastl12" },
    { "rct2.tt.corns2x2", "rct2tt.scenery_large.corns2x2" },
    { "rct2.tt.psntwl27", "rct2tt.scenery_large.psntwl27" },
    { "rct2.tt.hadesxxx", "rct2tt.scenery_large.hadesxxx" },
    { "rct2.tt.corvs2x2", "rct2tt.scenery_large.corvs2x2" },
    { "rct2.tt.mcastl16", "rct2tt.scenery_large.mcastl16" },
    { "rct2.tt.alnstr10", "rct2tt.scenery_large.alnstr10" },
    { "rct2.tt.ashnymph", "rct2tt.scenery_large.ashnymph" },
    { "rct2.tt.schnpit2", "rct2tt.scenery_large.schnpit2" },
    { "rct2.tt.futsky28", "rct2tt.scenery_large.futsky28" },
    { "rct2.tt.cyclopss", "rct2tt.scenery_large.cyclopss" },
    { "rct2.tt.4x4stnhn", "rct2tt.scenery_large.4x4stnhn" },
    { "rct2.tt.feastabl", "rct2tt.scenery_large.feastabl" },
    { "rct2.tt.oldnyk26", "rct2tt.scenery_large.oldnyk26" },
    { "rct2.tt.oldnyk22", "rct2tt.scenery_large.oldnyk22" },
    { "rct2.tt.tavernxx", "rct2tt.scenery_large.tavernxx" },
    { "rct2.tt.mcastl15", "rct2tt.scenery_large.mcastl15" },
    { "rct2.tt.mcastl18", "rct2tt.scenery_large.mcastl18" },
    { "rct2.tt.crsss2x2", "rct2tt.scenery_large.crsss2x2" },
    { "rct2.tt.oldnyk28", "rct2tt.scenery_large.oldnyk28" },
    { "rct2.tt.hodshut2", "rct2tt.scenery_large.hodshut2" },
    { "rct2.tt.romvm2x2", "rct2tt.scenery_large.romvm2x2" },
    { "rct2.tt.travlr01", "rct2tt.scenery_large.travlr01" },
    { "rct2.tt.hrbwal11", "rct2tt.scenery_large.hrbwal11" },
    { "rct2.tt.rdmeto01", "rct2tt.scenery_large.rdmeto01" },
    { "rct2.tt.oldnyk25", "rct2tt.scenery_large.oldnyk25" },
    { "rct2.tt.jetplan2", "rct2tt.scenery_large.jetplan2" },
    { "rct2.tt.alencrsh", "rct2tt.scenery_large.alencrsh" },
    { "rct2.tt.cratr2x2", "rct2tt.scenery_large.cratr2x2" },
    { "rct2.tt.romnc2x2", "rct2tt.scenery_large.romnc2x2" },
    { "rct2.tt.hodshut1", "rct2tt.scenery_large.hodshut1" },
    { "rct2.tt.romvs2x2", "rct2tt.scenery_large.romvs2x2" },
    { "rct2.tt.jetplan3", "rct2tt.scenery_large.jetplan3" },
    { "rct2.tt.1950scar", "rct2tt.scenery_large.1950scar" },
    { "rct2.tt.oldnyk29", "rct2tt.scenery_large.oldnyk29" },
    { "rct2.tt.ghotrod1", "rct2tt.scenery_large.ghotrod1" },
    { "rct2.tt.peramob1", "rct2tt.scenery_large.peramob1" },
    { "rct2.tt.mcastl01", "rct2tt.scenery_large.mcastl01" },
    { "rct2.tt.robncamp", "rct2tt.scenery_large.robncamp" },
    { "rct2.tt.shipb4x4", "rct2tt.scenery_large.shipb4x4" },
    { "rct2.tt.oldnyk23", "rct2tt.scenery_large.oldnyk23" },
    { "rct2.tt.4x4gmant", "rct2tt.scenery_large.4x4gmant" },
    { "rct2.tt.bandbusx", "rct2tt.scenery_large.bandbusx" },
    { "rct2.tt.alnstr11", "rct2tt.scenery_large.alnstr11" },
    { "rct2.tt.ships4x4", "rct2tt.scenery_large.ships4x4" },
    { "rct2.tt.oldnyk21", "rct2tt.scenery_large.oldnyk21" },
    { "rct2.tt.4x4trpit", "rct2tt.scenery_large.4x4trpit" },
    { "rct2.tt.cratr4x4", "rct2tt.scenery_large.cratr4x4" },
    { "rct2.tt.melmtree", "rct2tt.scenery_large.melmtree" },
    { "rct2.tt.crsvs2x2", "rct2tt.scenery_large.crsvs2x2" },
    { "rct2.tt.scg1960s", "rct2tt.scenery_group.scg1960s" },
    { "rct2.tt.scgfutur", "rct2tt.scenery_group.scgfutur" },
    { "rct2.tt.scgmytho", "rct2tt.scenery_group.scgmytho" },
    { "rct2.tt.scg1920s", "rct2tt.scenery_group.scg1920s" },
    { "rct2.tt.scg1920w", "rct2tt.scenery_group.scg1920w" },
    { "rct2.tt.scgmediv", "rct2tt.scenery_group.scgmediv" },
    { "rct2.tt.scgjurra", "rct2tt.scenery_group.scgjurra" },
    { "rct2.tt.waveking", "rct2tt.scenery_small.waveking" },
    { "rct2.tt.shwdfrst", "rct2tt.scenery_small.shwdfrst" },
    { "rct2.tt.psntwl13", "rct2tt.scenery_small.psntwl13" },
    { "rct2.tt.hevbth09", "rct2tt.scenery_small.hevbth09" },
    { "rct2.tt.medtools", "rct2tt.scenery_small.medtools" },
    { "rct2.tt.compeyex", "rct2tt.scenery_small.compeyex" },
    { "rct2.tt.evalien3", "rct2tt.scenery_small.evalien3" },
    { "rct2.tt.psntwl10", "rct2tt.scenery_small.psntwl10" },
    { "rct2.tt.hrbwal04", "rct2tt.scenery_small.hrbwal04" },
    { "rct2.tt.runway06", "rct2tt.scenery_small.runway06" },
    { "rct2.tt.elecfen4", "rct2tt.scenery_small.elecfen4" },
    { "rct2.tt.primst01", "rct2tt.scenery_small.primst01" },
    { "rct2.tt.hevrof02", "rct2tt.scenery_small.hevrof02" },
    { "rct2.tt.dkfight1", "rct2tt.scenery_small.dkfight1" },
    { "rct2.tt.oldnyk07", "rct2tt.scenery_small.oldnyk07" },
    { "rct2.tt.swamplt4", "rct2tt.scenery_small.swamplt4" },
    { "rct2.tt.hovrcar4", "rct2tt.scenery_small.hovrcar4" },
    { "rct2.tt.spiktail", "rct2tt.scenery_small.spiktail" },
    { "rct2.tt.laserx01", "rct2tt.scenery_small.laserx01" },
    { "rct2.tt.stgband1", "rct2tt.scenery_small.stgband1" },
    { "rct2.tt.pdflag03", "rct2tt.scenery_small.pdflag03" },
    { "rct2.tt.futsky21", "rct2tt.scenery_small.futsky21" },
    { "rct2.tt.artdec01", "rct2tt.scenery_small.artdec01" },
    { "rct2.tt.horsecrt", "rct2tt.scenery_small.horsecrt" },
    { "rct2.tt.spcshp01", "rct2tt.scenery_small.spcshp01" },
    { "rct2.tt.gldchest", "rct2tt.scenery_small.gldchest" },
    { "rct2.tt.cavemenx", "rct2tt.scenery_small.cavemenx" },
    { "rct2.tt.dkfight2", "rct2tt.scenery_small.dkfight2" },
    { "rct2.tt.stnesta4", "rct2tt.scenery_small.stnesta4" },
    { "rct2.tt.dinsign2", "rct2tt.scenery_small.dinsign2" },
    { "rct2.tt.oldnyk19", "rct2tt.scenery_small.oldnyk19" },
    { "rct2.tt.argonau2", "rct2tt.scenery_small.argonau2" },
    { "rct2.tt.bigbassx", "rct2tt.scenery_small.bigbassx" },
    { "rct2.tt.artdec10", "rct2tt.scenery_small.artdec10" },
    { "rct2.tt.runway04", "rct2tt.scenery_small.runway04" },
    { "rct2.tt.valkri01", "rct2tt.scenery_small.valkri01" },
    { "rct2.tt.futsky39", "rct2tt.scenery_small.futsky39" },
    { "rct2.tt.artdec14", "rct2tt.scenery_small.artdec14" },
    { "rct2.tt.fwrprdc1", "rct2tt.scenery_small.fwrprdc1" },
    { "rct2.tt.skeleto3", "rct2tt.scenery_small.skeleto3" },
    { "rct2.tt.artdec12", "rct2tt.scenery_small.artdec12" },
    { "rct2.tt.alnstr06", "rct2tt.scenery_small.alnstr06" },
    { "rct2.tt.jailxx20", "rct2tt.scenery_small.jailxx20" },
    { "rct2.tt.indwal12", "rct2tt.scenery_small.indwal12" },
    { "rct2.tt.speakr01", "rct2tt.scenery_small.speakr01" },
    { "rct2.tt.futsky42", "rct2tt.scenery_small.futsky42" },
    { "rct2.tt.mcastl04", "rct2tt.scenery_small.mcastl04" },
    { "rct2.tt.jailxx21", "rct2tt.scenery_small.jailxx21" },
    { "rct2.tt.futsky33", "rct2tt.scenery_small.futsky33" },
    { "rct2.tt.spcshp06", "rct2tt.scenery_small.spcshp06" },
    { "rct2.tt.artdec09", "rct2tt.scenery_small.artdec09" },
    { "rct2.tt.indwal18", "rct2tt.scenery_small.indwal18" },
    { "rct2.tt.artdec22", "rct2tt.scenery_small.artdec22" },
    { "rct2.tt.psntwl05", "rct2tt.scenery_small.psntwl05" },
    { "rct2.tt.hevbth03", "rct2tt.scenery_small.hevbth03" },
    { "rct2.tt.elecfen5", "rct2tt.scenery_small.elecfen5" },
    { "rct2.tt.indwal10", "rct2tt.scenery_small.indwal10" },
    { "rct2.tt.schnpl01", "rct2tt.scenery_small.schnpl01" },
    { "rct2.tt.hevbth15", "rct2tt.scenery_small.hevbth15" },
    { "rct2.tt.alenplt2", "rct2tt.scenery_small.alenplt2" },
    { "rct2.tt.artdec28", "rct2tt.scenery_small.artdec28" },
    { "rct2.tt.robnhood", "rct2tt.scenery_small.robnhood" },
    { "rct2.tt.alentre2", "rct2tt.scenery_small.alentre2" },
    { "rct2.tt.schnpl05", "rct2tt.scenery_small.schnpl05" },
    { "rct2.tt.futsky36", "rct2tt.scenery_small.futsky36" },
    { "rct2.tt.smoksk01", "rct2tt.scenery_small.smoksk01" },
    { "rct2.tt.primhear", "rct2tt.scenery_small.primhear" },
    { "rct2.tt.skeleto2", "rct2tt.scenery_small.skeleto2" },
    { "rct2.tt.futsky11", "rct2tt.scenery_small.futsky11" },
    { "rct2.tt.swrdstne", "rct2tt.scenery_small.swrdstne" },
    { "rct2.tt.hrbwal01", "rct2tt.scenery_small.hrbwal01" },
    { "rct2.tt.evalien2", "rct2tt.scenery_small.evalien2" },
    { "rct2.tt.indwal21", "rct2tt.scenery_small.indwal21" },
    { "rct2.tt.smallcpu", "rct2tt.scenery_small.smallcpu" },
    { "rct2.tt.rswatres", "rct2tt.scenery_small.rswatres" },
    { "rct2.tt.psntwl06", "rct2tt.scenery_small.psntwl06" },
    { "rct2.tt.primroot", "rct2tt.scenery_small.primroot" },
    { "rct2.tt.spcshp03", "rct2tt.scenery_small.spcshp03" },
    { "rct2.tt.stoolset", "rct2tt.scenery_small.stoolset" },
    { "rct2.tt.ggntocto", "rct2tt.scenery_small.ggntocto" },
    { "rct2.tt.tarpit02", "rct2tt.scenery_small.tarpit02" },
    { "rct2.tt.drgnattk", "rct2tt.scenery_small.drgnattk" },
    { "rct2.tt.futsky01", "rct2tt.scenery_small.futsky01" },
    { "rct2.tt.tarpit05", "rct2tt.scenery_small.tarpit05" },
    { "rct2.tt.jailxx13", "rct2tt.scenery_small.jailxx13" },
    { "rct2.tt.tarpit10", "rct2tt.scenery_small.tarpit10" },
    { "rct2.tt.hvrbike1", "rct2tt.scenery_small.hvrbike1" },
    { "rct2.tt.oldnyk33", "rct2tt.scenery_small.oldnyk33" },
    { "rct2.tt.metrcrs2", "rct2tt.scenery_small.metrcrs2" },
    { "rct2.tt.hevbth12", "rct2tt.scenery_small.hevbth12" },
    { "rct2.tt.swamplt5", "rct2tt.scenery_small.swamplt5" },
    { "rct2.tt.spcshp08", "rct2tt.scenery_small.spcshp08" },
    { "rct2.tt.indwal22", "rct2tt.scenery_small.indwal22" },
    { "rct2.tt.spcshp09", "rct2tt.scenery_small.spcshp09" },
    { "rct2.tt.hevbth05", "rct2tt.scenery_small.hevbth05" },
    { "rct2.tt.oldnyk09", "rct2tt.scenery_small.oldnyk09" },
    { "rct2.tt.elecfen3", "rct2tt.scenery_small.elecfen3" },
    { "rct2.tt.frbeacon", "rct2tt.scenery_small.frbeacon" },
    { "rct2.tt.strictop", "rct2tt.scenery_small.strictop" },
    { "rct2.tt.futsky40", "rct2tt.scenery_small.futsky40" },
    { "rct2.tt.swamplt6", "rct2tt.scenery_small.swamplt6" },
    { "rct2.tt.mamthw02", "rct2tt.scenery_small.mamthw02" },
    { "rct2.tt.psntwl30", "rct2tt.scenery_small.psntwl30" },
    { "rct2.tt.primroor", "rct2tt.scenery_small.primroor" },
    { "rct2.tt.spcshp07", "rct2tt.scenery_small.spcshp07" },
    { "rct2.tt.futsky44", "rct2tt.scenery_small.futsky44" },
    { "rct2.tt.jazzmbr1", "rct2tt.scenery_small.jazzmbr1" },
    { "rct2.tt.pdflag05", "rct2tt.scenery_small.pdflag05" },
    { "rct2.tt.tarpit01", "rct2tt.scenery_small.tarpit01" },
    { "rct2.tt.futsky17", "rct2tt.scenery_small.futsky17" },
    { "rct2.tt.futsky04", "rct2tt.scenery_small.futsky04" },
    { "rct2.tt.pdflag04", "rct2tt.scenery_small.pdflag04" },
    { "rct2.tt.artdec05", "rct2tt.scenery_small.artdec05" },
    { "rct2.tt.psntwl02", "rct2tt.scenery_small.psntwl02" },
    { "rct2.tt.indwal30", "rct2tt.scenery_small.indwal30" },
    { "rct2.tt.mcastl06", "rct2tt.scenery_small.mcastl06" },
    { "rct2.tt.jazzmbr2", "rct2tt.scenery_small.jazzmbr2" },
    { "rct2.tt.futsky48", "rct2tt.scenery_small.futsky48" },
    { "rct2.tt.futsky05", "rct2tt.scenery_small.futsky05" },
    { "rct2.tt.oldnyk13", "rct2tt.scenery_small.oldnyk13" },
    { "rct2.tt.psntwl31", "rct2tt.scenery_small.psntwl31" },
    { "rct2.tt.argonau3", "rct2tt.scenery_small.argonau3" },
    { "rct2.tt.psntwl07", "rct2tt.scenery_small.psntwl07" },
    { "rct2.tt.oldnyk16", "rct2tt.scenery_small.oldnyk16" },
    { "rct2.tt.haybails", "rct2tt.scenery_small.haybails" },
    { "rct2.tt.indwal29", "rct2tt.scenery_small.indwal29" },
    { "rct2.tt.clnsmen1", "rct2tt.scenery_small.clnsmen1" },
    { "rct2.tt.smoksk02", "rct2tt.scenery_small.smoksk02" },
    { "rct2.tt.oldnyk05", "rct2tt.scenery_small.oldnyk05" },
    { "rct2.tt.tarpit13", "rct2tt.scenery_small.tarpit13" },
    { "rct2.tt.mcastl05", "rct2tt.scenery_small.mcastl05" },
    { "rct2.tt.hevbth10", "rct2tt.scenery_small.hevbth10" },
    { "rct2.tt.indwal04", "rct2tt.scenery_small.indwal04" },
    { "rct2.tt.futsky08", "rct2tt.scenery_small.futsky08" },
    { "rct2.tt.futsky12", "rct2tt.scenery_small.futsky12" },
    { "rct2.tt.stnesta1", "rct2tt.scenery_small.stnesta1" },
    { "rct2.tt.mcastl19", "rct2tt.scenery_small.mcastl19" },
    { "rct2.tt.futsky46", "rct2tt.scenery_small.futsky46" },
    { "rct2.tt.chprbke2", "rct2tt.scenery_small.chprbke2" },
    { "rct2.tt.spcshp11", "rct2tt.scenery_small.spcshp11" },
    { "rct2.tt.indwal11", "rct2tt.scenery_small.indwal11" },
    { "rct2.tt.artdec21", "rct2tt.scenery_small.artdec21" },
    { "rct2.tt.alenplt1", "rct2tt.scenery_small.alenplt1" },
    { "rct2.tt.hevbth08", "rct2tt.scenery_small.hevbth08" },
    { "rct2.tt.spcshp02", "rct2tt.scenery_small.spcshp02" },
    { "rct2.tt.tarpit08", "rct2tt.scenery_small.tarpit08" },
    { "rct2.tt.jailxx11", "rct2tt.scenery_small.jailxx11" },
    { "rct2.tt.artdec15", "rct2tt.scenery_small.artdec15" },
    { "rct2.tt.psntwl08", "rct2tt.scenery_small.psntwl08" },
    { "rct2.tt.dinsign4", "rct2tt.scenery_small.dinsign4" },
    { "rct2.tt.armrswrd", "rct2tt.scenery_small.armrswrd" },
    { "rct2.tt.oldnyk08", "rct2tt.scenery_small.oldnyk08" },
    { "rct2.tt.psntwl24", "rct2tt.scenery_small.psntwl24" },
    { "rct2.tt.gdalien2", "rct2tt.scenery_small.gdalien2" },
    { "rct2.tt.hevbth13", "rct2tt.scenery_small.hevbth13" },
    { "rct2.tt.psntwl21", "rct2tt.scenery_small.psntwl21" },
    { "rct2.tt.metrcrs1", "rct2tt.scenery_small.metrcrs1" },
    { "rct2.tt.swamplt2", "rct2tt.scenery_small.swamplt2" },
    { "rct2.tt.futsky19", "rct2tt.scenery_small.futsky19" },
    { "rct2.tt.hevbth14", "rct2tt.scenery_small.hevbth14" },
    { "rct2.tt.jailxx06", "rct2tt.scenery_small.jailxx06" },
    { "rct2.tt.gdalien3", "rct2tt.scenery_small.gdalien3" },
    { "rct2.tt.artdec19", "rct2tt.scenery_small.artdec19" },
    { "rct2.tt.hurcluls", "rct2tt.scenery_small.hurcluls" },
    { "rct2.tt.pdflag06", "rct2tt.scenery_small.pdflag06" },
    { "rct2.tt.yewtreex", "rct2tt.scenery_small.yewtreex" },
    { "rct2.tt.furobot1", "rct2tt.scenery_small.furobot1" },
    { "rct2.tt.furobot2", "rct2tt.scenery_small.furobot2" },
    { "rct2.tt.mcastl09", "rct2tt.scenery_small.mcastl09" },
    { "rct2.tt.futsky35", "rct2tt.scenery_small.futsky35" },
    { "rct2.tt.psntwl17", "rct2tt.scenery_small.psntwl17" },
    { "rct2.tt.killrvin", "rct2tt.scenery_small.killrvin" },
    { "rct2.tt.alentre1", "rct2tt.scenery_small.alentre1" },
    { "rct2.tt.swamplt7", "rct2tt.scenery_small.swamplt7" },
    { "rct2.tt.hevbth04", "rct2tt.scenery_small.hevbth04" },
    { "rct2.tt.primst03", "rct2tt.scenery_small.primst03" },
    { "rct2.tt.jailxx02", "rct2tt.scenery_small.jailxx02" },
    { "rct2.tt.wepnrack", "rct2tt.scenery_small.wepnrack" },
    { "rct2.tt.oldnyk35", "rct2tt.scenery_small.oldnyk35" },
    { "rct2.tt.laserx02", "rct2tt.scenery_small.laserx02" },
    { "rct2.tt.psntwl11", "rct2tt.scenery_small.psntwl11" },
    { "rct2.tt.souptl01", "rct2tt.scenery_small.souptl01" },
    { "rct2.tt.futsky41", "rct2tt.scenery_small.futsky41" },
    { "rct2.tt.tarpit12", "rct2tt.scenery_small.tarpit12" },
    { "rct2.tt.artdec13", "rct2tt.scenery_small.artdec13" },
    { "rct2.tt.mdusasta", "rct2tt.scenery_small.mdusasta" },
    { "rct2.tt.psntwl23", "rct2tt.scenery_small.psntwl23" },
    { "rct2.tt.wiskybl2", "rct2tt.scenery_small.wiskybl2" },
    { "rct2.tt.indwal14", "rct2tt.scenery_small.indwal14" },
    { "rct2.tt.pdflag15", "rct2tt.scenery_small.pdflag15" },
    { "rct2.tt.primscrn", "rct2tt.scenery_small.primscrn" },
    { "rct2.tt.tyranrex", "rct2tt.scenery_small.tyranrex" },
    { "rct2.tt.gscorpo2", "rct2tt.scenery_small.gscorpo2" },
    { "rct2.tt.mamthw05", "rct2tt.scenery_small.mamthw05" },
    { "rct2.tt.spcshp12", "rct2tt.scenery_small.spcshp12" },
    { "rct2.tt.valkri02", "rct2tt.scenery_small.valkri02" },
    { "rct2.tt.tarpit06", "rct2tt.scenery_small.tarpit06" },
    { "rct2.tt.hrbwal02", "rct2tt.scenery_small.hrbwal02" },
    { "rct2.tt.psntwl04", "rct2tt.scenery_small.psntwl04" },
    { "rct2.tt.alnstr01", "rct2tt.scenery_small.alnstr01" },
    { "rct2.tt.indwal09", "rct2tt.scenery_small.indwal09" },
    { "rct2.tt.artdec17", "rct2tt.scenery_small.artdec17" },
    { "rct2.tt.indwal06", "rct2tt.scenery_small.indwal06" },
    { "rct2.tt.fltsign1", "rct2tt.scenery_small.fltsign1" },
    { "rct2.tt.hrbwal05", "rct2tt.scenery_small.hrbwal05" },
    { "rct2.tt.futsky03", "rct2tt.scenery_small.futsky03" },
    { "rct2.tt.stnesta2", "rct2tt.scenery_small.stnesta2" },
    { "rct2.tt.futsky18", "rct2tt.scenery_small.futsky18" },
    { "rct2.tt.indwal28", "rct2tt.scenery_small.indwal28" },
    { "rct2.tt.tarpit14", "rct2tt.scenery_small.tarpit14" },
    { "rct2.tt.swamplt8", "rct2tt.scenery_small.swamplt8" },
    { "rct2.tt.indwal17", "rct2tt.scenery_small.indwal17" },
    { "rct2.tt.indwal23", "rct2tt.scenery_small.indwal23" },
    { "rct2.tt.mcastl03", "rct2tt.scenery_small.mcastl03" },
    { "rct2.tt.armrhelm", "rct2tt.scenery_small.armrhelm" },
    { "rct2.tt.gangster", "rct2tt.scenery_small.gangster" },
    { "rct2.tt.tarpit03", "rct2tt.scenery_small.tarpit03" },
    { "rct2.tt.stgband4", "rct2tt.scenery_small.stgband4" },
    { "rct2.tt.schnpl03", "rct2tt.scenery_small.schnpl03" },
    { "rct2.tt.hrbwal06", "rct2tt.scenery_small.hrbwal06" },
    { "rct2.tt.whccolds", "rct2tt.scenery_small.whccolds" },
    { "rct2.tt.futsky52", "rct2tt.scenery_small.futsky52" },
    { "rct2.tt.futsky13", "rct2tt.scenery_small.futsky13" },
    { "rct2.tt.armrshld", "rct2tt.scenery_small.armrshld" },
    { "rct2.tt.artdec02", "rct2tt.scenery_small.artdec02" },
    { "rct2.tt.indwal08", "rct2tt.scenery_small.indwal08" },
    { "rct2.tt.mamthw03", "rct2tt.scenery_small.mamthw03" },
    { "rct2.tt.gscorpon", "rct2tt.scenery_small.gscorpon" },
    { "rct2.tt.futsky38", "rct2tt.scenery_small.futsky38" },
    { "rct2.tt.wiskybl1", "rct2tt.scenery_small.wiskybl1" },
    { "rct2.tt.largecpu", "rct2tt.scenery_small.largecpu" },
    { "rct2.tt.schnbouy", "rct2tt.scenery_small.schnbouy" },
    { "rct2.tt.indwal16", "rct2tt.scenery_small.indwal16" },
    { "rct2.tt.hevrof03", "rct2tt.scenery_small.hevrof03" },
    { "rct2.tt.psntwl25", "rct2tt.scenery_small.psntwl25" },
    { "rct2.tt.meteorst", "rct2tt.scenery_small.meteorst" },
    { "rct2.tt.futsky14", "rct2tt.scenery_small.futsky14" },
    { "rct2.tt.indwal31", "rct2tt.scenery_small.indwal31" },
    { "rct2.tt.spcshp05", "rct2tt.scenery_small.spcshp05" },
    { "rct2.tt.jailxx01", "rct2tt.scenery_small.jailxx01" },
    { "rct2.tt.indwal25", "rct2tt.scenery_small.indwal25" },
    { "rct2.tt.futsky09", "rct2tt.scenery_small.futsky09" },
    { "rct2.tt.primwal1", "rct2tt.scenery_small.primwal1" },
    { "rct2.tt.artdec11", "rct2tt.scenery_small.artdec11" },
    { "rct2.tt.jailxx15", "rct2tt.scenery_small.jailxx15" },
    { "rct2.tt.souped01", "rct2tt.scenery_small.souped01" },
    { "rct2.tt.indwal13", "rct2tt.scenery_small.indwal13" },
    { "rct2.tt.artdec18", "rct2tt.scenery_small.artdec18" },
    { "rct2.tt.minotaur", "rct2tt.scenery_small.minotaur" },
    { "rct2.tt.indwal32", "rct2tt.scenery_small.indwal32" },
    { "rct2.tt.stnesta3", "rct2tt.scenery_small.stnesta3" },
    { "rct2.tt.zeusstat", "rct2tt.scenery_small.zeusstat" },
    { "rct2.tt.indwal27", "rct2tt.scenery_small.indwal27" },
    { "rct2.tt.knfight2", "rct2tt.scenery_small.knfight2" },
    { "rct2.tt.schncrsh", "rct2tt.scenery_small.schncrsh" },
    { "rct2.tt.polwtbtn", "rct2tt.scenery_small.polwtbtn" },
    { "rct2.tt.ggntspid", "rct2tt.scenery_small.ggntspid" },
    { "rct2.tt.oldnyk14", "rct2tt.scenery_small.oldnyk14" },
    { "rct2.tt.indwal02", "rct2tt.scenery_small.indwal02" },
    { "rct2.tt.futsky47", "rct2tt.scenery_small.futsky47" },
    { "rct2.tt.soupcrn2", "rct2tt.scenery_small.soupcrn2" },
    { "rct2.tt.indwal20", "rct2tt.scenery_small.indwal20" },
    { "rct2.tt.hevbth06", "rct2tt.scenery_small.hevbth06" },
    { "rct2.tt.futsky02", "rct2tt.scenery_small.futsky02" },
    { "rct2.tt.schnbost", "rct2tt.scenery_small.schnbost" },
    { "rct2.tt.alnstr02", "rct2tt.scenery_small.alnstr02" },
    { "rct2.tt.tarpit07", "rct2tt.scenery_small.tarpit07" },
    { "rct2.tt.pdflag14", "rct2tt.scenery_small.pdflag14" },
    { "rct2.tt.1920slmp", "rct2tt.scenery_small.1920slmp" },
    { "rct2.tt.tarpit09", "rct2tt.scenery_small.tarpit09" },
    { "rct2.tt.mcastl07", "rct2tt.scenery_small.mcastl07" },
    { "rct2.tt.artdec06", "rct2tt.scenery_small.artdec06" },
    { "rct2.tt.futsky29", "rct2tt.scenery_small.futsky29" },
    { "rct2.tt.indwal05", "rct2tt.scenery_small.indwal05" },
    { "rct2.tt.pdflag08", "rct2tt.scenery_small.pdflag08" },
    { "rct2.tt.rdmeto04", "rct2tt.scenery_small.rdmeto04" },
    { "rct2.tt.wdncart2", "rct2tt.scenery_small.wdncart2" },
    { "rct2.tt.gdalien1", "rct2tt.scenery_small.gdalien1" },
    { "rct2.tt.mamthw01", "rct2tt.scenery_small.mamthw01" },
    { "rct2.tt.jailxx10", "rct2tt.scenery_small.jailxx10" },
    { "rct2.tt.jailxx16", "rct2tt.scenery_small.jailxx16" },
    { "rct2.tt.smoksk03", "rct2tt.scenery_small.smoksk03" },
    { "rct2.tt.pdflag13", "rct2tt.scenery_small.pdflag13" },
    { "rct2.tt.oldnyk01", "rct2tt.scenery_small.oldnyk01" },
    { "rct2.tt.artdec29", "rct2tt.scenery_small.artdec29" },
    { "rct2.tt.firemanx", "rct2tt.scenery_small.firemanx" },
    { "rct2.tt.cagdstat", "rct2tt.scenery_small.cagdstat" },
    { "rct2.tt.oldnyk04", "rct2tt.scenery_small.oldnyk04" },
    { "rct2.tt.jailxx05", "rct2tt.scenery_small.jailxx05" },
    { "rct2.tt.chanmaid", "rct2tt.scenery_small.chanmaid" },
    { "rct2.tt.runway02", "rct2tt.scenery_small.runway02" },
    { "rct2.tt.hevrof04", "rct2tt.scenery_small.hevrof04" },
    { "rct2.tt.indwal19", "rct2tt.scenery_small.indwal19" },
    { "rct2.tt.mamthw04", "rct2tt.scenery_small.mamthw04" },
    { "rct2.tt.stmdran1", "rct2tt.scenery_small.stmdran1" },
    { "rct2.tt.futsky37", "rct2tt.scenery_small.futsky37" },
    { "rct2.tt.psntwl01", "rct2tt.scenery_small.psntwl01" },
    { "rct2.tt.indwal01", "rct2tt.scenery_small.indwal01" },
    { "rct2.tt.oldnyk18", "rct2tt.scenery_small.oldnyk18" },
    { "rct2.tt.alnstr04", "rct2tt.scenery_small.alnstr04" },
    { "rct2.tt.schnpl04", "rct2tt.scenery_small.schnpl04" },
    { "rct2.tt.oldnyk17", "rct2tt.scenery_small.oldnyk17" },
    { "rct2.tt.hovrcar1", "rct2tt.scenery_small.hovrcar1" },
    { "rct2.tt.goldflec", "rct2tt.scenery_small.goldflec" },
    { "rct2.tt.rcknrolr", "rct2tt.scenery_small.rcknrolr" },
    { "rct2.tt.jailxx08", "rct2tt.scenery_small.jailxx08" },
    { "rct2.tt.alnstr07", "rct2tt.scenery_small.alnstr07" },
    { "rct2.tt.schnpl06", "rct2tt.scenery_small.schnpl06" },
    { "rct2.tt.artdec03", "rct2tt.scenery_small.artdec03" },
    { "rct2.tt.swamplt3", "rct2tt.scenery_small.swamplt3" },
    { "rct2.tt.jailxx14", "rct2tt.scenery_small.jailxx14" },
    { "rct2.tt.futsky53", "rct2tt.scenery_small.futsky53" },
    { "rct2.tt.hevrof01", "rct2tt.scenery_small.hevrof01" },
    { "rct2.tt.sdragfly", "rct2tt.scenery_small.sdragfly" },
    { "rct2.tt.elecfen1", "rct2tt.scenery_small.elecfen1" },
    { "rct2.tt.knfight1", "rct2tt.scenery_small.knfight1" },
    { "rct2.tt.guyqwifx", "rct2tt.scenery_small.guyqwifx" },
    { "rct2.tt.psntwl20", "rct2tt.scenery_small.psntwl20" },
    { "rct2.tt.schntnny", "rct2tt.scenery_small.schntnny" },
    { "rct2.tt.oldnyk02", "rct2tt.scenery_small.oldnyk02" },
    { "rct2.tt.futsky10", "rct2tt.scenery_small.futsky10" },
    { "rct2.tt.rdmeto03", "rct2tt.scenery_small.rdmeto03" },
    { "rct2.tt.indwal26", "rct2tt.scenery_small.indwal26" },
    { "rct2.tt.psntwl12", "rct2tt.scenery_small.psntwl12" },
    { "rct2.tt.bolpot01", "rct2tt.scenery_small.bolpot01" },
    { "rct2.tt.biggutar", "rct2tt.scenery_small.biggutar" },
    { "rct2.tt.fltsign2", "rct2tt.scenery_small.fltsign2" },
    { "rct2.tt.artdec04", "rct2tt.scenery_small.artdec04" },
    { "rct2.tt.armrbody", "rct2tt.scenery_small.armrbody" },
    { "rct2.tt.souptl02", "rct2tt.scenery_small.souptl02" },
    { "rct2.tt.spcshp10", "rct2tt.scenery_small.spcshp10" },
    { "rct2.tt.futsky15", "rct2tt.scenery_small.futsky15" },
    { "rct2.tt.futsky54", "rct2tt.scenery_small.futsky54" },
    { "rct2.tt.soupcrnr", "rct2tt.scenery_small.soupcrnr" },
    { "rct2.tt.dinsign1", "rct2tt.scenery_small.dinsign1" },
    { "rct2.tt.volcvent", "rct2tt.scenery_small.volcvent" },
    { "rct2.tt.schnpl02", "rct2tt.scenery_small.schnpl02" },
    { "rct2.tt.futsky23", "rct2tt.scenery_small.futsky23" },
    { "rct2.tt.psntwl14", "rct2tt.scenery_small.psntwl14" },
    { "rct2.tt.astrongt", "rct2tt.scenery_small.astrongt" },
    { "rct2.tt.spterdac", "rct2tt.scenery_small.spterdac" },
    { "rct2.tt.futsky34", "rct2tt.scenery_small.futsky34" },
    { "rct2.tt.hovrcar2", "rct2tt.scenery_small.hovrcar2" },
    { "rct2.tt.alnstr03", "rct2tt.scenery_small.alnstr03" },
    { "rct2.tt.primhead", "rct2tt.scenery_small.primhead" },
    { "rct2.tt.futsky16", "rct2tt.scenery_small.futsky16" },
    { "rct2.tt.spcshp04", "rct2tt.scenery_small.spcshp04" },
    { "rct2.tt.jazzmbr3", "rct2tt.scenery_small.jazzmbr3" },
    { "rct2.tt.psntwl16", "rct2tt.scenery_small.psntwl16" },
    { "rct2.tt.mamthw06", "rct2tt.scenery_small.mamthw06" },
    { "rct2.tt.oldnyk15", "rct2tt.scenery_small.oldnyk15" },
    { "rct2.tt.tarpit04", "rct2tt.scenery_small.tarpit04" },
    { "rct2.tt.chprbke1", "rct2tt.scenery_small.chprbke1" },
    { "rct2.tt.indwal07", "rct2tt.scenery_small.indwal07" },
    { "rct2.tt.allseeye", "rct2tt.scenery_small.allseeye" },
    { "rct2.tt.artdec20", "rct2tt.scenery_small.artdec20" },
    { "rct2.tt.oldnyk34", "rct2tt.scenery_small.oldnyk34" },
    { "rct2.tt.argonau1", "rct2tt.scenery_small.argonau1" },
    { "rct2.tt.oldnyk11", "rct2tt.scenery_small.oldnyk11" },
    { "rct2.tt.indwal15", "rct2tt.scenery_small.indwal15" },
    { "rct2.tt.evalien1", "rct2tt.scenery_small.evalien1" },
    { "rct2.tt.tarpit11", "rct2tt.scenery_small.tarpit11" },
    { "rct2.tt.speakr02", "rct2tt.scenery_small.speakr02" },
    { "rct2.tt.runway05", "rct2tt.scenery_small.runway05" },
    { "rct2.tt.skeleto1", "rct2tt.scenery_small.skeleto1" },
    { "rct2.tt.pdflag16", "rct2tt.scenery_small.pdflag16" },
    { "rct2.tt.mcastl10", "rct2tt.scenery_small.mcastl10" },
    { "rct2.tt.cookspit", "rct2tt.scenery_small.cookspit" },
    { "rct2.tt.primst02", "rct2tt.scenery_small.primst02" },
    { "rct2.tt.jailxx03", "rct2tt.scenery_small.jailxx03" },
    { "rct2.tt.artdec07", "rct2tt.scenery_small.artdec07" },
    { "rct2.tt.geyserxx", "rct2tt.scenery_small.geyserxx" },
    { "rct2.tt.spacrang", "rct2tt.scenery_small.spacrang" },
    { "rct2.tt.hevbth11", "rct2tt.scenery_small.hevbth11" },
    { "rct2.tt.runway01", "rct2tt.scenery_small.runway01" },
    { "rct2.tt.jailxx12", "rct2tt.scenery_small.jailxx12" },
    { "rct2.tt.runway03", "rct2tt.scenery_small.runway03" },
    { "rct2.tt.hevbth16", "rct2tt.scenery_small.hevbth16" },
    { "rct2.tt.jazzmbr4", "rct2tt.scenery_small.jazzmbr4" },
    { "rct2.tt.stgband2", "rct2tt.scenery_small.stgband2" },
    { "rct2.tt.oldnyk03", "rct2tt.scenery_small.oldnyk03" },
    { "rct2.tt.oldnyk12", "rct2tt.scenery_small.oldnyk12" },
    { "rct2.tt.meteorcr", "rct2tt.scenery_small.meteorcr" },
    { "rct2.tt.indwal24", "rct2tt.scenery_small.indwal24" },
    { "rct2.tt.dinsign3", "rct2tt.scenery_small.dinsign3" },
    { "rct2.tt.futsky07", "rct2tt.scenery_small.futsky07" },
    { "rct2.tt.swamplt1", "rct2tt.scenery_small.swamplt1" },
    { "rct2.tt.psntwl29", "rct2tt.scenery_small.psntwl29" },
    { "rct2.tt.hrbwal03", "rct2tt.scenery_small.hrbwal03" },
    { "rct2.tt.swamplt9", "rct2tt.scenery_small.swamplt9" },
    { "rct2.tt.artdec24", "rct2tt.scenery_small.artdec24" },
    { "rct2.tt.schndrum", "rct2tt.scenery_small.schndrum" },
    { "rct2.tt.indwal03", "rct2tt.scenery_small.indwal03" },
    { "rct2.tt.mdbucket", "rct2tt.scenery_small.mdbucket" },
    { "rct2.tt.jailxx04", "rct2tt.scenery_small.jailxx04" },
    { "rct2.tt.souped02", "rct2tt.scenery_small.souped02" },
    { "rct2.tt.psntwl15", "rct2tt.scenery_small.psntwl15" },
    { "rct2.tt.hvrbike3", "rct2tt.scenery_small.hvrbike3" },
    { "rct2.tt.hevbth17", "rct2tt.scenery_small.hevbth17" },
    { "rct2.tt.artdec16", "rct2tt.scenery_small.artdec16" },
    { "rct2.tt.futsky06", "rct2tt.scenery_small.futsky06" },
    { "rct2.tt.artdec23", "rct2tt.scenery_small.artdec23" },
    { "rct2.tt.titansta", "rct2tt.scenery_small.titansta" },
    { "rct2.tt.oldnyk31", "rct2tt.scenery_small.oldnyk31" },
    { "rct2.tt.hvrbike4", "rct2tt.scenery_small.hvrbike4" },
    { "rct2.tt.medtrget", "rct2tt.scenery_small.medtrget" },
    { "rct2.tt.stgband3", "rct2tt.scenery_small.stgband3" },
    { "rct2.tt.primtall", "rct2tt.scenery_small.primtall" },
    { "rct2.tt.jailxx09", "rct2tt.scenery_small.jailxx09" },
    { "rct2.tt.hovrcar3", "rct2tt.scenery_small.hovrcar3" },
    { "rct2.tt.fircanon", "rct2tt.scenery_small.fircanon" },
    { "rct2.tt.psntwl03", "rct2tt.scenery_small.psntwl03" },
    { "rct2.tt.mcastl02", "rct2tt.scenery_small.mcastl02" },
    { "rct2.tt.oldnyk10", "rct2tt.scenery_small.oldnyk10" },
    { "rct2.tt.hevbth02", "rct2tt.scenery_small.hevbth02" },
    { "rct2.tt.elecfen2", "rct2tt.scenery_small.elecfen2" },
    { "rct2.tt.jailxx07", "rct2tt.scenery_small.jailxx07" },
    { "rct2.tt.alnstr05", "rct2tt.scenery_small.alnstr05" },
    { "rct2.tt.bkrgang1", "rct2tt.scenery_small.bkrgang1" },
    { "rct2.tt.medstool", "rct2tt.scenery_small.medstool" },
    { "rct2.tt.artdec08", "rct2tt.scenery_small.artdec08" },
    { "rct2.tt.psntwl09", "rct2tt.scenery_small.psntwl09" },
    { "rct2.tt.mcastl08", "rct2tt.scenery_small.mcastl08" },
    { "rct2.tt.hvrbike2", "rct2tt.scenery_small.hvrbike2" },
    { "rct2.tt.primwal3", "rct2tt.scenery_small.primwal3" },
    { "rct2.tt.pdflag02", "rct2tt.scenery_small.pdflag02" },
    { "rct2.tt.hevbth01", "rct2tt.scenery_small.hevbth01" },
    { "rct2.tt.hevbth07", "rct2tt.scenery_small.hevbth07" },
    { "rct2.tt.oldnyk06", "rct2tt.scenery_small.oldnyk06" },
    { "rct2.tt.primwal2", "rct2tt.scenery_small.primwal2" },
    { "rct2.tt.mspkwa01", "rct2tt.scenery_wall.mspkwa01" },
    { "rct2.tt.jailxx19", "rct2tt.scenery_wall.jailxx19" },
    { "rct2.tt.firhydrt", "rct2tt.footpath_item.firhydrt" },
    { "rct2.tt.medbench", "rct2tt.footpath_item.medbench" },
    { "rct2.tt.railings.balustrade", "rct2tt.footpath_railings.balustrade" },
    { "rct2.tt.railings.medieval", "rct2tt.footpath_railings.medieval" },
    { "rct2.tt.railings.rainbow", "rct2tt.footpath_railings.rainbow" },
    { "rct2.tt.railings.circuitboard", "rct2tt.footpath_railings.circuitboard" },
    { "rct2.tt.railings.rocky", "rct2tt.footpath_railings.rocky" },
    { "rct2.tt.railings.pavement", "rct2tt.footpath_railings.pavement" },
    { "rct2.tt.pathrailings.balustrade", "rct2tt.footpath_railings.balustrade" },
    { "rct2.tt.pathrailings.medieval", "rct2tt.footpath_railings.medieval" },
    { "rct2.tt.pathrailings.rainbow", "rct2tt.footpath_railings.rainbow" },
    { "rct2.tt.pathrailings.circuitboard", "rct2tt.footpath_railings.circuitboard" },
    { "rct2.tt.pathrailings.rocky", "rct2tt.footpath_railings.rocky" },
    { "rct2.tt.pathrailings.pavement", "rct2tt.footpath_railings.pavement" },
    { "rct2.tt.timemach", "rct2tt.ride.timemach" },
    { "rct2.tt.flygboat", "rct2tt.ride.flygboat" },
    { "rct2.tt.hoverbke", "rct2tt.ride.hoverbke" },
    { "rct2.tt.mgr2", "rct2tt.ride.mgr2" },
    { "rct2.tt.halofmrs", "rct2tt.ride.halofmrs" },
    { "rct2.tt.dragnfly", "rct2tt.ride.dragnfly" },
    { "rct2.tt.moonjuce", "rct2tt.ride.moonjuce" },
    { "rct2.tt.harpiesx", "rct2tt.ride.harpiesx" },
    { "rct2.tt.polchase", "rct2tt.ride.polchase" },
    { "rct2.tt.rivrstyx", "rct2tt.ride.rivrstyx" },
    { "rct2.tt.zeplelin", "rct2tt.ride.zeplelin" },
    { "rct2.tt.schoolbs", "rct2tt.ride.schoolbs" },
    { "rct2.tt.stamphrd", "rct2tt.ride.stamphrd" },
    { "rct2.tt.valkyrie", "rct2tt.ride.valkyrie" },
    { "rct2.tt.mktstal2", "rct2tt.ride.mktstal2" },
    { "rct2.tt.dinoeggs", "rct2tt.ride.dinoeggs" },
    { "rct2.tt.jetpackx", "rct2tt.ride.jetpackx" },
    { "rct2.tt.trebucht", "rct2tt.ride.trebucht" },
    { "rct2.tt.trilobte", "rct2tt.ride.trilobte" },
    { "rct2.tt.mktstal1", "rct2tt.ride.mktstal1" },
    { "rct2.tt.microbus", "rct2tt.ride.microbus" },
    { "rct2.tt.cyclopsx", "rct2tt.ride.cyclopsx" },
    { "rct2.tt.softoyst", "rct2tt.ride.softoyst" },
    { "rct2.tt.tricatop", "rct2tt.ride.tricatop" },
    { "rct2.tt.hovercar", "rct2tt.ride.hovercar" },
    { "rct2.tt.hovrbord", "rct2tt.ride.hovrbord" },
    { "rct2.tt.tommygun", "rct2tt.ride.tommygun" },
    { "rct2.tt.neptunex", "rct2tt.ride.neptunex" },
    { "rct2.tt.ganstrcr", "rct2tt.ride.ganstrcr" },
    { "rct2.tt.hotrodxx", "rct2tt.ride.hotrodxx" },
    { "rct2.tt.raptorxx", "rct2tt.ride.raptorxx" },
    { "rct2.tt.spokprsn", "rct2tt.ride.spokprsn" },
    { "rct2.tt.1960tsrt", "rct2tt.ride.1960tsrt" },
    { "rct2.tt.blckdeth", "rct2tt.ride.blckdeth" },
    { "rct2.tt.flwrpowr", "rct2tt.ride.flwrpowr" },
    { "rct2.tt.bmvoctps", "rct2tt.ride.bmvoctps" },
    { "rct2.tt.flalmace", "rct2tt.ride.flalmace" },
    { "rct2.tt.cavmncar", "rct2tt.ride.cavmncar" },
    { "rct2.tt.policecr", "rct2tt.ride.policecr" },
    { "rct2.tt.seaplane", "rct2tt.ride.seaplane" },
    { "rct2.tt.mythosea", "rct2tt.ride.mythosea" },
    { "rct2.tt.cerberus", "rct2tt.ride.cerberus" },
    { "rct2.tt.1920sand", "rct2tt.ride.1920sand" },
    { "rct2.tt.telepter", "rct2tt.ride.telepter" },
    { "rct2.tt.jetplane", "rct2tt.ride.jetplane" },
    { "rct2.tt.gintspdr", "rct2tt.ride.gintspdr" },
    { "rct2.tt.figtknit", "rct2tt.ride.figtknit" },
    { "rct2.tt.barnstrm", "rct2tt.ride.barnstrm" },
    { "rct2.tt.oakbarel", "rct2tt.ride.oakbarel" },
    { "rct2.tt.1920racr", "rct2tt.ride.1920racr" },
    { "rct2.tt.pegasusx", "rct2tt.ride.pegasusx" },
    { "rct2.tt.funhouse", "rct2tt.ride.funhouse" },
    { "rct2.tt.medisoup", "rct2tt.ride.medisoup" },
    { "rct2.tt.pterodac", "rct2tt.ride.pterodac" },
    { "rct2.tt.battrram", "rct2tt.ride.battrram" },
    { "rct2.tt.jousting", "rct2tt.ride.jousting" },
    { "rct2.tt.mythentr", "rct2tt.park_entrance.mythentr" },
    { "rct2.tt.futurent", "rct2tt.park_entrance.futurent" },
    { "rct2.tt.jurasent", "rct2tt.park_entrance.jurasent" },
    { "rct2.tt.gldyrent", "rct2tt.park_entrance.gldyrent" },
    { "rct2.tt.1920sent", "rct2tt.park_entrance.1920sent" },
    { "rct2.tt.medientr", "rct2tt.park_entrance.medientr" },
    { "rct2.tt.pathsurface.rocky", "rct2tt.footpath_surface.rocky" },
    { "rct2.tt.pathsurface.queue.pavement", "rct2tt.footpath_surface.queue_pavement" },
    { "rct2.tt.pathsurface.queue.rainbow", "rct2tt.footpath_surface.queue_rainbow" },
    { "rct2.tt.pathsurface.rainbow", "rct2tt.footpath_surface.rainbow" },
    { "rct2.tt.pathsurface.circuitboard", "rct2tt.footpath_surface.circuitboard" },
    { "rct2.tt.pathsurface.mosaic", "rct2tt.footpath_surface.mosaic" },
    { "rct2.tt.pathsurface.pavement", "rct2tt.footpath_surface.pavement" },
    { "rct2.tt.pathsurface.medieval", "rct2tt.footpath_surface.medieval" },
    { "rct2.tt.pathsurface.queue.circuitboard", "rct2tt.footpath_surface.queue_circuitboard" },
    { "rct2.ww.hippo01", "rct2ww.scenery_large.hippo01" },
    { "rct2.ww.geisha", "rct2ww.scenery_large.geisha" },
    { "rct2.ww.atractor", "rct2ww.scenery_large.atractor" },
    { "rct2.ww.easerlnd", "rct2ww.scenery_large.easerlnd" },
    { "rct2.ww.1x4brg01", "rct2ww.scenery_large.1x4brg01" },
    { "rct2.ww.gwoctur1", "rct2ww.scenery_large.gwoctur1" },
    { "rct2.ww.rtudor06", "rct2ww.scenery_large.rtudor06" },
    { "rct2.ww.3x3altre", "rct2ww.scenery_large.3x3altre" },
    { "rct2.ww.bigben", "rct2ww.scenery_large.bigben" },
    { "rct2.ww.pogodal", "rct2ww.scenery_large.pogodal" },
    { "rct2.ww.bigdish", "rct2ww.scenery_large.bigdish" },
    { "rct2.ww.rdrab01", "rct2ww.scenery_large.rdrab01" },
    { "rct2.ww.goodsam", "rct2ww.scenery_large.goodsam" },
    { "rct2.ww.mbskyr02", "rct2ww.scenery_large.mbskyr02" },
    { "rct2.ww.icefor02", "rct2ww.scenery_large.icefor02" },
    { "rct2.ww.tajmcolm", "rct2ww.scenery_large.tajmcolm" },
    { "rct2.ww.giraffe2", "rct2ww.scenery_large.giraffe2" },
    { "rct2.ww.rkreml10", "rct2ww.scenery_large.rkreml10" },
    { "rct2.ww.sputnik", "rct2ww.scenery_large.sputnik" },
    { "rct2.ww.pagodam", "rct2ww.scenery_large.pagodam" },
    { "rct2.ww.ovrgrwnt", "rct2ww.scenery_large.ovrgrwnt" },
    { "rct2.ww.sunken", "rct2ww.scenery_large.sunken" },
    { "rct2.ww.bamborf1", "rct2ww.scenery_large.bamborf1" },
    { "rct2.ww.1x2abr03", "rct2ww.scenery_large.1x2abr03" },
    { "rct2.ww.1x2abr01", "rct2ww.scenery_large.1x2abr01" },
    { "rct2.ww.3x3eucal", "rct2ww.scenery_large.3x3eucal" },
    { "rct2.ww.goldbuda", "rct2ww.scenery_large.goldbuda" },
    { "rct2.ww.lincolns", "rct2ww.scenery_large.lincolns" },
    { "rct2.ww.windmill", "rct2ww.scenery_large.windmill" },
    { "rct2.ww.helipad", "rct2ww.scenery_large.helipad" },
    { "rct2.ww.gdstaue2", "rct2ww.scenery_large.gdstaue2" },
    { "rct2.ww.3x3atre3", "rct2ww.scenery_large.3x3atre3" },
    { "rct2.ww.damtower", "rct2ww.scenery_large.damtower" },
    { "rct2.ww.spaceorb", "rct2ww.scenery_large.spaceorb" },
    { "rct2.ww.hippo02", "rct2ww.scenery_large.hippo02" },
    { "rct2.ww.eiffel", "rct2ww.scenery_large.eiffel" },
    { "rct2.ww.rkreml09", "rct2ww.scenery_large.rkreml09" },
    { "rct2.ww.1x4brg02", "rct2ww.scenery_large.1x4brg02" },
    { "rct2.ww.afrrhino", "rct2ww.scenery_large.afrrhino" },
    { "rct2.ww.giraffe1", "rct2ww.scenery_large.giraffe1" },
    { "rct2.ww.3x3mantr", "rct2ww.scenery_large.3x3mantr" },
    { "rct2.ww.indianst", "rct2ww.scenery_large.indianst" },
    { "rct2.ww.atomium", "rct2ww.scenery_large.atomium" },
    { "rct2.ww.tajmcbse", "rct2ww.scenery_large.tajmcbse" },
    { "rct2.ww.icefor01", "rct2ww.scenery_large.icefor01" },
    { "rct2.ww.bamborf3", "rct2ww.scenery_large.bamborf3" },
    { "rct2.ww.1x2lgchm", "rct2ww.scenery_large.1x2lgchm" },
    { "rct2.ww.circus", "rct2ww.scenery_large.circus" },
    { "rct2.ww.gdstaue1", "rct2ww.scenery_large.gdstaue1" },
    { "rct2.ww.cdragon", "rct2ww.scenery_large.cdragon" },
    { "rct2.ww.cowboy02", "rct2ww.scenery_large.cowboy02" },
    { "rct2.ww.3x3atre1", "rct2ww.scenery_large.3x3atre1" },
    { "rct2.ww.gwoctur2", "rct2ww.scenery_large.gwoctur2" },
    { "rct2.ww.1x2glama", "rct2ww.scenery_large.1x2glama" },
    { "rct2.ww.pagodas", "rct2ww.scenery_large.pagodas" },
    { "rct2.ww.1x2abr02", "rct2ww.scenery_large.1x2abr02" },
    { "rct2.ww.biggeosp", "rct2ww.scenery_large.biggeosp" },
    { "rct2.ww.evilsam", "rct2ww.scenery_large.evilsam" },
    { "rct2.ww.pagodatw", "rct2ww.scenery_large.pagodatw" },
    { "rct2.ww.mbskyr01", "rct2ww.scenery_large.mbskyr01" },
    { "rct2.ww.redwood", "rct2ww.scenery_large.redwood" },
    { "rct2.ww.soflibrt", "rct2ww.scenery_large.soflibrt" },
    { "rct2.ww.1x2azt01", "rct2ww.scenery_large.1x2azt01" },
    { "rct2.ww.rdrab03", "rct2ww.scenery_large.rdrab03" },
    { "rct2.ww.rdrab02", "rct2ww.scenery_large.rdrab02" },
    { "rct2.ww.1x4brg05", "rct2ww.scenery_large.1x4brg05" },
    { "rct2.ww.1x4brg04", "rct2ww.scenery_large.1x4brg04" },
    { "rct2.ww.radar", "rct2ww.scenery_large.radar" },
    { "rct2.ww.afrclion", "rct2ww.scenery_large.afrclion" },
    { "rct2.ww.afrzebra", "rct2ww.scenery_large.afrzebra" },
    { "rct2.ww.largeju1", "rct2ww.scenery_large.largeju1" },
    { "rct2.ww.cowboy01", "rct2ww.scenery_large.cowboy01" },
    { "rct2.ww.rdrab04", "rct2ww.scenery_large.rdrab04" },
    { "rct2.ww.50rocket", "rct2ww.scenery_large.50rocket" },
    { "rct2.ww.tajmdome", "rct2ww.scenery_large.tajmdome" },
    { "rct2.ww.fatbudda", "rct2ww.scenery_large.fatbudda" },
    { "rct2.ww.1x4brg03", "rct2ww.scenery_large.1x4brg03" },
    { "rct2.ww.shiva", "rct2ww.scenery_large.shiva" },
    { "rct2.ww.3x3hmsen", "rct2ww.scenery_large.3x3hmsen" },
    { "rct2.ww.rkreml08", "rct2ww.scenery_large.rkreml08" },
    { "rct2.ww.adultele", "rct2ww.scenery_large.adultele" },
    { "rct2.ww.3x3atre2", "rct2ww.scenery_large.3x3atre2" },
    { "rct2.ww.rtudor05", "rct2ww.scenery_large.rtudor05" },
    { "rct2.ww.tajmctop", "rct2ww.scenery_large.tajmctop" },
    { "rct2.ww.1x2azt02", "rct2ww.scenery_large.1x2azt02" },
    { "rct2.ww.bamborf2", "rct2ww.scenery_large.bamborf2" },
    { "rct2.ww.scgeurop", "rct2ww.scenery_group.scgeurop" },
    { "rct2.ww.scgaustr", "rct2ww.scenery_group.scgaustr" },
    { "rct2.ww.scgsamer", "rct2ww.scenery_group.scgsamer" },
    { "rct2.ww.scgartic", "rct2ww.scenery_group.scgartic" },
    { "rct2.ww.scgnamrc", "rct2ww.scenery_group.scgnamrc" },
    { "rct2.ww.scgafric", "rct2ww.scenery_group.scgafric" },
    { "rct2.ww.scgasia", "rct2ww.scenery_group.scgasia" },
    { "rct2.ww.1x1atree", "rct2ww.scenery_small.1x1atree" },
    { "rct2.ww.sbh1wgwh", "rct2ww.scenery_small.sbh1wgwh" },
    { "rct2.ww.1x1brang", "rct2ww.scenery_small.1x1brang" },
    { "rct2.ww.oriegong", "rct2ww.scenery_small.oriegong" },
    { "rct2.ww.sbwind07", "rct2ww.scenery_small.sbwind07" },
    { "rct2.ww.rdrab07", "rct2ww.scenery_small.rdrab07" },
    { "rct2.ww.rmud01", "rct2ww.scenery_small.rmud01" },
    { "rct2.ww.sbwind12", "rct2ww.scenery_small.sbwind12" },
    { "rct2.ww.rdrab09", "rct2ww.scenery_small.rdrab09" },
    { "rct2.ww.icebarl3", "rct2ww.scenery_small.icebarl3" },
    { "rct2.ww.sbskys17", "rct2ww.scenery_small.sbskys17" },
    { "rct2.ww.rcorr01", "rct2ww.scenery_small.rcorr01" },
    { "rct2.ww.sbh3cac2", "rct2ww.scenery_small.sbh3cac2" },
    { "rct2.ww.wtudor16", "rct2ww.scenery_small.wtudor16" },
    { "rct2.ww.waztec21", "rct2ww.scenery_small.waztec21" },
    { "rct2.ww.fountarc", "rct2ww.scenery_small.fountarc" },
    { "rct2.ww.fountrow", "rct2ww.scenery_small.fountrow" },
    { "rct2.ww.trckprt6", "rct2ww.scenery_small.trckprt6" },
    { "rct2.ww.wdrab09", "rct2ww.scenery_small.wdrab09" },
    { "rct2.ww.sbwind02", "rct2ww.scenery_small.sbwind02" },
    { "rct2.ww.sbskys03", "rct2ww.scenery_small.sbskys03" },
    { "rct2.ww.rdrab13", "rct2ww.scenery_small.rdrab13" },
    { "rct2.ww.sbwplm01", "rct2ww.scenery_small.sbwplm01" },
    { "rct2.ww.sbskys07", "rct2ww.scenery_small.sbskys07" },
    { "rct2.ww.rcorr11", "rct2ww.scenery_small.rcorr11" },
    { "rct2.ww.sbwind01", "rct2ww.scenery_small.sbwind01" },
    { "rct2.ww.wmayan08", "rct2ww.scenery_small.wmayan08" },
    { "rct2.ww.sbwplm04", "rct2ww.scenery_small.sbwplm04" },
    { "rct2.ww.icebarl1", "rct2ww.scenery_small.icebarl1" },
    { "rct2.ww.sbwind16", "rct2ww.scenery_small.sbwind16" },
    { "rct2.ww.wnauti01", "rct2ww.scenery_small.wnauti01" },
    { "rct2.ww.rkreml06", "rct2ww.scenery_small.rkreml06" },
    { "rct2.ww.sbskys15", "rct2ww.scenery_small.sbskys15" },
    { "rct2.ww.wcuzco24", "rct2ww.scenery_small.wcuzco24" },
    { "rct2.ww.inflag04", "rct2ww.scenery_small.inflag04" },
    { "rct2.ww.rlog04", "rct2ww.scenery_small.rlog04" },
    { "rct2.ww.conveyr1", "rct2ww.scenery_small.conveyr1" },
    { "rct2.ww.rcorr03", "rct2ww.scenery_small.rcorr03" },
    { "rct2.ww.waztec03", "rct2ww.scenery_small.waztec03" },
    { "rct2.ww.trckprt9", "rct2ww.scenery_small.trckprt9" },
    { "rct2.ww.wgeorg09", "rct2ww.scenery_small.wgeorg09" },
    { "rct2.ww.rcorr05", "rct2ww.scenery_small.rcorr05" },
    { "rct2.ww.rgeorg04", "rct2ww.scenery_small.rgeorg04" },
    { "rct2.ww.rcorr07", "rct2ww.scenery_small.rcorr07" },
    { "rct2.ww.bamboopl", "rct2ww.scenery_small.bamboopl" },
    { "rct2.ww.sbskys18", "rct2ww.scenery_small.sbskys18" },
    { "rct2.ww.sbskys06", "rct2ww.scenery_small.sbskys06" },
    { "rct2.ww.wkreml02", "rct2ww.scenery_small.wkreml02" },
    { "rct2.ww.trckprt1", "rct2ww.scenery_small.trckprt1" },
    { "rct2.ww.wtudor13", "rct2ww.scenery_small.wtudor13" },
    { "rct2.ww.rlog03", "rct2ww.scenery_small.rlog03" },
    { "rct2.ww.rdrab11", "rct2ww.scenery_small.rdrab11" },
    { "rct2.ww.sbh3oscr", "rct2ww.scenery_small.sbh3oscr" },
    { "rct2.ww.japsnotr", "rct2ww.scenery_small.japsnotr" },
    { "rct2.ww.trckprt5", "rct2ww.scenery_small.trckprt5" },
    { "rct2.ww.roofig06", "rct2ww.scenery_small.roofig06" },
    { "rct2.ww.wtudor18", "rct2ww.scenery_small.wtudor18" },
    { "rct2.ww.waztec18", "rct2ww.scenery_small.waztec18" },
    { "rct2.ww.wmayan20", "rct2ww.scenery_small.wmayan20" },
    { "rct2.ww.sbh3cac3", "rct2ww.scenery_small.sbh3cac3" },
    { "rct2.ww.sb2palm1", "rct2ww.scenery_small.sb2palm1" },
    { "rct2.ww.rgeorg01", "rct2ww.scenery_small.rgeorg01" },
    { "rct2.ww.1x1termm", "rct2ww.scenery_small.1x1termm" },
    { "rct2.ww.wmayan17", "rct2ww.scenery_small.wmayan17" },
    { "rct2.ww.sbh3rt66", "rct2ww.scenery_small.sbh3rt66" },
    { "rct2.ww.wcuzco09", "rct2ww.scenery_small.wcuzco09" },
    { "rct2.ww.bstatue1", "rct2ww.scenery_small.bstatue1" },
    { "rct2.ww.wcuzfoun", "rct2ww.scenery_small.wcuzfoun" },
    { "rct2.ww.wropeswa", "rct2ww.scenery_small.wropeswa" },
    { "rct2.ww.sbwplm07", "rct2ww.scenery_small.sbwplm07" },
    { "rct2.ww.roofice3", "rct2ww.scenery_small.roofice3" },
    { "rct2.ww.flamngo1", "rct2ww.scenery_small.flamngo1" },
    { "rct2.ww.wdrab13", "rct2ww.scenery_small.wdrab13" },
    { "rct2.ww.sbwplm02", "rct2ww.scenery_small.sbwplm02" },
    { "rct2.ww.rtudor03", "rct2ww.scenery_small.rtudor03" },
    { "rct2.ww.pipebase", "rct2ww.scenery_small.pipebase" },
    { "rct2.ww.wgeorg08", "rct2ww.scenery_small.wgeorg08" },
    { "rct2.ww.fishhole", "rct2ww.scenery_small.fishhole" },
    { "rct2.ww.wmayan12", "rct2ww.scenery_small.wmayan12" },
    { "rct2.ww.wcuzco18", "rct2ww.scenery_small.wcuzco18" },
    { "rct2.ww.rbrick02", "rct2ww.scenery_small.rbrick02" },
    { "rct2.ww.waborg08", "rct2ww.scenery_small.waborg08" },
    { "rct2.ww.rgeorg07", "rct2ww.scenery_small.rgeorg07" },
    { "rct2.ww.rdrab14", "rct2ww.scenery_small.rdrab14" },
    { "rct2.ww.wtudor17", "rct2ww.scenery_small.wtudor17" },
    { "rct2.ww.wmayan01", "rct2ww.scenery_small.wmayan01" },
    { "rct2.ww.g1dancer", "rct2ww.scenery_small.g1dancer" },
    { "rct2.ww.japchblo", "rct2ww.scenery_small.japchblo" },
    { "rct2.ww.wnauti05", "rct2ww.scenery_small.wnauti05" },
    { "rct2.ww.waborg02", "rct2ww.scenery_small.waborg02" },
    { "rct2.ww.pcg", "rct2ww.scenery_small.pcg" },
    { "rct2.ww.antilopf", "rct2ww.scenery_small.antilopf" },
    { "rct2.ww.waztec25", "rct2ww.scenery_small.waztec25" },
    { "rct2.ww.wcuzco23", "rct2ww.scenery_small.wcuzco23" },
    { "rct2.ww.sbh1tepe", "rct2ww.scenery_small.sbh1tepe" },
    { "rct2.ww.pst", "rct2ww.scenery_small.pst" },
    { "rct2.ww.trckprt4", "rct2ww.scenery_small.trckprt4" },
    { "rct2.ww.rgeorg10", "rct2ww.scenery_small.rgeorg10" },
    { "rct2.ww.wropecor", "rct2ww.scenery_small.wropecor" },
    { "rct2.ww.tjblock2", "rct2ww.scenery_small.tjblock2" },
    { "rct2.ww.wtudor15", "rct2ww.scenery_small.wtudor15" },
    { "rct2.ww.wmayan11", "rct2ww.scenery_small.wmayan11" },
    { "rct2.ww.inuit", "rct2ww.scenery_small.inuit" },
    { "rct2.ww.wmayan26", "rct2ww.scenery_small.wmayan26" },
    { "rct2.ww.wcuzco06", "rct2ww.scenery_small.wcuzco06" },
    { "rct2.ww.tjblock3", "rct2ww.scenery_small.tjblock3" },
    { "rct2.ww.waztec27", "rct2ww.scenery_small.waztec27" },
    { "rct2.ww.sbh3plm2", "rct2ww.scenery_small.sbh3plm2" },
    { "rct2.ww.wmayan09", "rct2ww.scenery_small.wmayan09" },
    { "rct2.ww.sbh3plm1", "rct2ww.scenery_small.sbh3plm1" },
    { "rct2.ww.sbwind21", "rct2ww.scenery_small.sbwind21" },
    { "rct2.ww.waztec10", "rct2ww.scenery_small.waztec10" },
    { "rct2.ww.sbh3cac1", "rct2ww.scenery_small.sbh3cac1" },
    { "rct2.ww.1x1kanga", "rct2ww.scenery_small.1x1kanga" },
    { "rct2.ww.rbrick08", "rct2ww.scenery_small.rbrick08" },
    { "rct2.ww.rgeorg03", "rct2ww.scenery_small.rgeorg03" },
    { "rct2.ww.rdrab10", "rct2ww.scenery_small.rdrab10" },
    { "rct2.ww.jachtree", "rct2ww.scenery_small.jachtree" },
    { "rct2.ww.tnt1", "rct2ww.scenery_small.tnt1" },
    { "rct2.ww.wmayan24", "rct2ww.scenery_small.wmayan24" },
    { "rct2.ww.wmayan10", "rct2ww.scenery_small.wmayan10" },
    { "rct2.ww.roofig03", "rct2ww.scenery_small.roofig03" },
    { "rct2.ww.rkreml07", "rct2ww.scenery_small.rkreml07" },
    { "rct2.ww.oilpump", "rct2ww.scenery_small.oilpump" },
    { "rct2.ww.inflag02", "rct2ww.scenery_small.inflag02" },
    { "rct2.ww.roofig04", "rct2ww.scenery_small.roofig04" },
    { "rct2.ww.waztec22", "rct2ww.scenery_small.waztec22" },
    { "rct2.ww.wmayan05", "rct2ww.scenery_small.wmayan05" },
    { "rct2.ww.jpflag06", "rct2ww.scenery_small.jpflag06" },
    { "rct2.ww.rgeorg05", "rct2ww.scenery_small.rgeorg05" },
    { "rct2.ww.rgeorg02", "rct2ww.scenery_small.rgeorg02" },
    { "rct2.ww.ptj", "rct2ww.scenery_small.ptj" },
    { "rct2.ww.sbskys10", "rct2ww.scenery_small.sbskys10" },
    { "rct2.ww.wcuzco21", "rct2ww.scenery_small.wcuzco21" },
    { "rct2.ww.trckprt2", "rct2ww.scenery_small.trckprt2" },
    { "rct2.ww.tnt3", "rct2ww.scenery_small.tnt3" },
    { "rct2.ww.rcorr09", "rct2ww.scenery_small.rcorr09" },
    { "rct2.ww.sbh3plm3", "rct2ww.scenery_small.sbh3plm3" },
    { "rct2.ww.rdrab12", "rct2ww.scenery_small.rdrab12" },
    { "rct2.ww.sbwplm06", "rct2ww.scenery_small.sbwplm06" },
    { "rct2.ww.inuit2", "rct2ww.scenery_small.inuit2" },
    { "rct2.ww.rcorr04", "rct2ww.scenery_small.rcorr04" },
    { "rct2.ww.1x1emuxx", "rct2ww.scenery_small.1x1emuxx" },
    { "rct2.ww.sbskys08", "rct2ww.scenery_small.sbskys08" },
    { "rct2.ww.waztec14", "rct2ww.scenery_small.waztec14" },
    { "rct2.ww.sbskys16", "rct2ww.scenery_small.sbskys16" },
    { "rct2.ww.ukphone", "rct2ww.scenery_small.ukphone" },
    { "rct2.ww.rdrab06", "rct2ww.scenery_small.rdrab06" },
    { "rct2.ww.waztec09", "rct2ww.scenery_small.waztec09" },
    { "rct2.ww.wcuzco17", "rct2ww.scenery_small.wcuzco17" },
    { "rct2.ww.jpflag05", "rct2ww.scenery_small.jpflag05" },
    { "rct2.ww.wmayan13", "rct2ww.scenery_small.wmayan13" },
    { "rct2.ww.sbwind10", "rct2ww.scenery_small.sbwind10" },
    { "rct2.ww.wkreml01", "rct2ww.scenery_small.wkreml01" },
    { "rct2.ww.waztec24", "rct2ww.scenery_small.waztec24" },
    { "rct2.ww.roofice5", "rct2ww.scenery_small.roofice5" },
    { "rct2.ww.sbwplm08", "rct2ww.scenery_small.sbwplm08" },
    { "rct2.ww.wmayan25", "rct2ww.scenery_small.wmayan25" },
    { "rct2.ww.sbwind18", "rct2ww.scenery_small.sbwind18" },
    { "rct2.ww.jpflag02", "rct2ww.scenery_small.jpflag02" },
    { "rct2.ww.wmayan22", "rct2ww.scenery_small.wmayan22" },
    { "rct2.ww.wnauti02", "rct2ww.scenery_small.wnauti02" },
    { "rct2.ww.rgeorg11", "rct2ww.scenery_small.rgeorg11" },
    { "rct2.ww.sbwind15", "rct2ww.scenery_small.sbwind15" },
    { "rct2.ww.wcuzco25", "rct2ww.scenery_small.wcuzco25" },
    { "rct2.ww.waztec12", "rct2ww.scenery_small.waztec12" },
    { "rct2.ww.wmayan18", "rct2ww.scenery_small.wmayan18" },
    { "rct2.ww.jappintr", "rct2ww.scenery_small.jappintr" },
    { "rct2.ww.sbwind06", "rct2ww.scenery_small.sbwind06" },
    { "rct2.ww.rmud05", "rct2ww.scenery_small.rmud05" },
    { "rct2.ww.waztec16", "rct2ww.scenery_small.waztec16" },
    { "rct2.ww.sbwind08", "rct2ww.scenery_small.sbwind08" },
    { "rct2.ww.sbwplm05", "rct2ww.scenery_small.sbwplm05" },
    { "rct2.ww.wmayan23", "rct2ww.scenery_small.wmayan23" },
    { "rct2.ww.bamboobs", "rct2ww.scenery_small.bamboobs" },
    { "rct2.ww.wcuzco11", "rct2ww.scenery_small.wcuzco11" },
    { "rct2.ww.waborg07", "rct2ww.scenery_small.waborg07" },
    { "rct2.ww.rshogi2", "rct2ww.scenery_small.rshogi2" },
    { "rct2.ww.rkreml02", "rct2ww.scenery_small.rkreml02" },
    { "rct2.ww.talllan2", "rct2ww.scenery_small.talllan2" },
    { "rct2.ww.wdrab10", "rct2ww.scenery_small.wdrab10" },
    { "rct2.ww.wcuzco07", "rct2ww.scenery_small.wcuzco07" },
    { "rct2.ww.sbwind17", "rct2ww.scenery_small.sbwind17" },
    { "rct2.ww.wmayan06", "rct2ww.scenery_small.wmayan06" },
    { "rct2.ww.roofice4", "rct2ww.scenery_small.roofice4" },
    { "rct2.ww.rkreml11", "rct2ww.scenery_small.rkreml11" },
    { "rct2.ww.wcuzco04", "rct2ww.scenery_small.wcuzco04" },
    { "rct2.ww.1x1atre2", "rct2ww.scenery_small.1x1atre2" },
    { "rct2.ww.sb1hspb1", "rct2ww.scenery_small.sb1hspb1" },
    { "rct2.ww.waborg03", "rct2ww.scenery_small.waborg03" },
    { "rct2.ww.sbwind09", "rct2ww.scenery_small.sbwind09" },
    { "rct2.ww.waztec01", "rct2ww.scenery_small.waztec01" },
    { "rct2.ww.waztec15", "rct2ww.scenery_small.waztec15" },
    { "rct2.ww.wgeorg12", "rct2ww.scenery_small.wgeorg12" },
    { "rct2.ww.rtudor01", "rct2ww.scenery_small.rtudor01" },
    { "rct2.ww.wmayan21", "rct2ww.scenery_small.wmayan21" },
    { "rct2.ww.wmayan03", "rct2ww.scenery_small.wmayan03" },
    { "rct2.ww.wcuzco03", "rct2ww.scenery_small.wcuzco03" },
    { "rct2.ww.sb1hspb3", "rct2ww.scenery_small.sb1hspb3" },
    { "rct2.ww.sbwind04", "rct2ww.scenery_small.sbwind04" },
    { "rct2.ww.waztec17", "rct2ww.scenery_small.waztec17" },
    { "rct2.ww.rdrab05", "rct2ww.scenery_small.rdrab05" },
    { "rct2.ww.sbskys09", "rct2ww.scenery_small.sbskys09" },
    { "rct2.ww.sbskys14", "rct2ww.scenery_small.sbskys14" },
    { "rct2.ww.babyele", "rct2ww.scenery_small.babyele" },
    { "rct2.ww.rshogi1", "rct2ww.scenery_small.rshogi1" },
    { "rct2.ww.sbh2shlt", "rct2ww.scenery_small.sbh2shlt" },
    { "rct2.ww.wnauti04", "rct2ww.scenery_small.wnauti04" },
    { "rct2.ww.wtudor14", "rct2ww.scenery_small.wtudor14" },
    { "rct2.ww.sbskys04", "rct2ww.scenery_small.sbskys04" },
    { "rct2.ww.antilopm", "rct2ww.scenery_small.antilopm" },
    { "rct2.ww.wmayan04", "rct2ww.scenery_small.wmayan04" },
    { "rct2.ww.campfani", "rct2ww.scenery_small.campfani" },
    { "rct2.ww.jpflag01", "rct2ww.scenery_small.jpflag01" },
    { "rct2.ww.conveyr4", "rct2ww.scenery_small.conveyr4" },
    { "rct2.ww.wcuzco19", "rct2ww.scenery_small.wcuzco19" },
    { "rct2.ww.sbwind13", "rct2ww.scenery_small.sbwind13" },
    { "rct2.ww.smallgeo", "rct2ww.scenery_small.smallgeo" },
    { "rct2.ww.antilopp", "rct2ww.scenery_small.antilopp" },
    { "rct2.ww.wdrab12", "rct2ww.scenery_small.wdrab12" },
    { "rct2.ww.rcorr02", "rct2ww.scenery_small.rcorr02" },
    { "rct2.ww.inflag03", "rct2ww.scenery_small.inflag03" },
    { "rct2.ww.sbwind11", "rct2ww.scenery_small.sbwind11" },
    { "rct2.ww.waztec13", "rct2ww.scenery_small.waztec13" },
    { "rct2.ww.1x1jugt3", "rct2ww.scenery_small.1x1jugt3" },
    { "rct2.ww.wcuzco16", "rct2ww.scenery_small.wcuzco16" },
    { "rct2.ww.rkreml05", "rct2ww.scenery_small.rkreml05" },
    { "rct2.ww.roofice2", "rct2ww.scenery_small.roofice2" },
    { "rct2.ww.rgeorg06", "rct2ww.scenery_small.rgeorg06" },
    { "rct2.ww.sbwind20", "rct2ww.scenery_small.sbwind20" },
    { "rct2.ww.waborg04", "rct2ww.scenery_small.waborg04" },
    { "rct2.ww.wcuzco10", "rct2ww.scenery_small.wcuzco10" },
    { "rct2.ww.wmayan14", "rct2ww.scenery_small.wmayan14" },
    { "rct2.ww.adpanda", "rct2ww.scenery_small.adpanda" },
    { "rct2.ww.rwdaub01", "rct2ww.scenery_small.rwdaub01" },
    { "rct2.ww.ptk", "rct2ww.scenery_small.ptk" },
    { "rct2.ww.roofice6", "rct2ww.scenery_small.roofice6" },
    { "rct2.ww.sb2sky01", "rct2ww.scenery_small.sb2sky01" },
    { "rct2.ww.sb1hspb2", "rct2ww.scenery_small.sb1hspb2" },
    { "rct2.ww.rlog01", "rct2ww.scenery_small.rlog01" },
    { "rct2.ww.roofig02", "rct2ww.scenery_small.roofig02" },
    { "rct2.ww.rkreml03", "rct2ww.scenery_small.rkreml03" },
    { "rct2.ww.wcuzco01", "rct2ww.scenery_small.wcuzco01" },
    { "rct2.ww.sbskys05", "rct2ww.scenery_small.sbskys05" },
    { "rct2.ww.rwdaub02", "rct2ww.scenery_small.rwdaub02" },
    { "rct2.ww.wgeorg07", "rct2ww.scenery_small.wgeorg07" },
    { "rct2.ww.rbrick03", "rct2ww.scenery_small.rbrick03" },
    { "rct2.ww.conveyr5", "rct2ww.scenery_small.conveyr5" },
    { "rct2.ww.rlog02", "rct2ww.scenery_small.rlog02" },
    { "rct2.ww.sbskys02", "rct2ww.scenery_small.sbskys02" },
    { "rct2.ww.rmud03", "rct2ww.scenery_small.rmud03" },
    { "rct2.ww.waztec20", "rct2ww.scenery_small.waztec20" },
    { "rct2.ww.rlog05", "rct2ww.scenery_small.rlog05" },
    { "rct2.ww.rbrick04", "rct2ww.scenery_small.rbrick04" },
    { "rct2.ww.sbskys01", "rct2ww.scenery_small.sbskys01" },
    { "rct2.ww.waztec11", "rct2ww.scenery_small.waztec11" },
    { "rct2.ww.waztec08", "rct2ww.scenery_small.waztec08" },
    { "rct2.ww.wcuzco12", "rct2ww.scenery_small.wcuzco12" },
    { "rct2.ww.rbrick07", "rct2ww.scenery_small.rbrick07" },
    { "rct2.ww.waborg06", "rct2ww.scenery_small.waborg06" },
    { "rct2.ww.wkreml04", "rct2ww.scenery_small.wkreml04" },
    { "rct2.ww.postbox", "rct2ww.scenery_small.postbox" },
    { "rct2.ww.waztec19", "rct2ww.scenery_small.waztec19" },
    { "rct2.ww.conveyr2", "rct2ww.scenery_small.conveyr2" },
    { "rct2.ww.talllan1", "rct2ww.scenery_small.talllan1" },
    { "rct2.ww.rbrick05", "rct2ww.scenery_small.rbrick05" },
    { "rct2.ww.vertpipe", "rct2ww.scenery_small.vertpipe" },
    { "rct2.ww.waborg01", "rct2ww.scenery_small.waborg01" },
    { "rct2.ww.trckprt7", "rct2ww.scenery_small.trckprt7" },
    { "rct2.ww.wgeorg11", "rct2ww.scenery_small.wgeorg11" },
    { "rct2.ww.flamngo2", "rct2ww.scenery_small.flamngo2" },
    { "rct2.ww.tjblock1", "rct2ww.scenery_small.tjblock1" },
    { "rct2.ww.sbskys13", "rct2ww.scenery_small.sbskys13" },
    { "rct2.ww.wmayan15", "rct2ww.scenery_small.wmayan15" },
    { "rct2.ww.sbskys11", "rct2ww.scenery_small.sbskys11" },
    { "rct2.ww.pipevent", "rct2ww.scenery_small.pipevent" },
    { "rct2.ww.balllant", "rct2ww.scenery_small.balllant" },
    { "rct2.ww.waztec04", "rct2ww.scenery_small.waztec04" },
    { "rct2.ww.rshogi3", "rct2ww.scenery_small.rshogi3" },
    { "rct2.ww.waztec06", "rct2ww.scenery_small.waztec06" },
    { "rct2.ww.wcuzco05", "rct2ww.scenery_small.wcuzco05" },
    { "rct2.ww.inflag01", "rct2ww.scenery_small.inflag01" },
    { "rct2.ww.roofice1", "rct2ww.scenery_small.roofice1" },
    { "rct2.ww.rmarble1", "rct2ww.scenery_small.rmarble1" },
    { "rct2.ww.rdrab08", "rct2ww.scenery_small.rdrab08" },
    { "rct2.ww.rkreml01", "rct2ww.scenery_small.rkreml01" },
    { "rct2.ww.conveyr3", "rct2ww.scenery_small.conveyr3" },
    { "rct2.ww.wcuzco28", "rct2ww.scenery_small.wcuzco28" },
    { "rct2.ww.wmayan16", "rct2ww.scenery_small.wmayan16" },
    { "rct2.ww.rmarble4", "rct2ww.scenery_small.rmarble4" },
    { "rct2.ww.wnauti03", "rct2ww.scenery_small.wnauti03" },
    { "rct2.ww.waztec07", "rct2ww.scenery_small.waztec07" },
    { "rct2.ww.rcorr10", "rct2ww.scenery_small.rcorr10" },
    { "rct2.ww.sbskys12", "rct2ww.scenery_small.sbskys12" },
    { "rct2.ww.rmud02", "rct2ww.scenery_small.rmud02" },
    { "rct2.ww.sbwind05", "rct2ww.scenery_small.sbwind05" },
    { "rct2.ww.rwdaub03", "rct2ww.scenery_small.rwdaub03" },
    { "rct2.ww.jpflag04", "rct2ww.scenery_small.jpflag04" },
    { "rct2.ww.wkreml06", "rct2ww.scenery_small.wkreml06" },
    { "rct2.ww.rkreml04", "rct2ww.scenery_small.rkreml04" },
    { "rct2.ww.rgeorg09", "rct2ww.scenery_small.rgeorg09" },
    { "rct2.ww.wkreml05", "rct2ww.scenery_small.wkreml05" },
    { "rct2.ww.pco", "rct2ww.scenery_small.pco" },
    { "rct2.ww.sbwind03", "rct2ww.scenery_small.sbwind03" },
    { "rct2.ww.rmarble2", "rct2ww.scenery_small.rmarble2" },
    { "rct2.ww.wcuzco22", "rct2ww.scenery_small.wcuzco22" },
    { "rct2.ww.wcuzco27", "rct2ww.scenery_small.wcuzco27" },
    { "rct2.ww.trckprt8", "rct2ww.scenery_small.trckprt8" },
    { "rct2.ww.waztec02", "rct2ww.scenery_small.waztec02" },
    { "rct2.ww.terrarmy", "rct2ww.scenery_small.terrarmy" },
    { "rct2.ww.sbwind19", "rct2ww.scenery_small.sbwind19" },
    { "rct2.ww.roofig01", "rct2ww.scenery_small.roofig01" },
    { "rct2.ww.icebarl2", "rct2ww.scenery_small.icebarl2" },
    { "rct2.ww.wcuzco26", "rct2ww.scenery_small.wcuzco26" },
    { "rct2.ww.balllan2", "rct2ww.scenery_small.balllan2" },
    { "rct2.ww.tnt4", "rct2ww.scenery_small.tnt4" },
    { "rct2.ww.sballoon", "rct2ww.scenery_small.sballoon" },
    { "rct2.ww.babpanda", "rct2ww.scenery_small.babpanda" },
    { "rct2.ww.trckprt3", "rct2ww.scenery_small.trckprt3" },
    { "rct2.ww.wcuzco08", "rct2ww.scenery_small.wcuzco08" },
    { "rct2.ww.waztec26", "rct2ww.scenery_small.waztec26" },
    { "rct2.ww.rtudor02", "rct2ww.scenery_small.rtudor02" },
    { "rct2.ww.rcorr08", "rct2ww.scenery_small.rcorr08" },
    { "rct2.ww.wgeorg10", "rct2ww.scenery_small.wgeorg10" },
    { "rct2.ww.1x1didge", "rct2ww.scenery_small.1x1didge" },
    { "rct2.ww.sbwplm03", "rct2ww.scenery_small.sbwplm03" },
    { "rct2.ww.1x1jugt2", "rct2ww.scenery_small.1x1jugt2" },
    { "rct2.ww.g2dancer", "rct2ww.scenery_small.g2dancer" },
    { "rct2.ww.wcuzco15", "rct2ww.scenery_small.wcuzco15" },
    { "rct2.ww.wcuzco14", "rct2ww.scenery_small.wcuzco14" },
    { "rct2.ww.rbrick01", "rct2ww.scenery_small.rbrick01" },
    { "rct2.ww.flamngo3", "rct2ww.scenery_small.flamngo3" },
    { "rct2.ww.sbh4totm", "rct2ww.scenery_small.sbh4totm" },
    { "rct2.ww.rgeorg12", "rct2ww.scenery_small.rgeorg12" },
    { "rct2.ww.sbh3cskl", "rct2ww.scenery_small.sbh3cskl" },
    { "rct2.ww.waztec05", "rct2ww.scenery_small.waztec05" },
    { "rct2.ww.wcuzco20", "rct2ww.scenery_small.wcuzco20" },
    { "rct2.ww.wcuzco13", "rct2ww.scenery_small.wcuzco13" },
    { "rct2.ww.inflag05", "rct2ww.scenery_small.inflag05" },
    { "rct2.ww.rmarble3", "rct2ww.scenery_small.rmarble3" },
    { "rct2.ww.waborg05", "rct2ww.scenery_small.waborg05" },
    { "rct2.ww.wmayan07", "rct2ww.scenery_small.wmayan07" },
    { "rct2.ww.tnt2", "rct2ww.scenery_small.tnt2" },
    { "rct2.ww.wmayan02", "rct2ww.scenery_small.wmayan02" },
    { "rct2.ww.wdrab11", "rct2ww.scenery_small.wdrab11" },
    { "rct2.ww.rgeorg08", "rct2ww.scenery_small.rgeorg08" },
    { "rct2.ww.pva", "rct2ww.scenery_small.pva" },
    { "rct2.ww.wtudor12", "rct2ww.scenery_small.wtudor12" },
    { "rct2.ww.wkreml03", "rct2ww.scenery_small.wkreml03" },
    { "rct2.ww.jpflag03", "rct2ww.scenery_small.jpflag03" },
    { "rct2.ww.fstatue1", "rct2ww.scenery_small.fstatue1" },
    { "rct2.ww.wcuzco02", "rct2ww.scenery_small.wcuzco02" },
    { "rct2.ww.waztec23", "rct2ww.scenery_small.waztec23" },
    { "rct2.ww.sbwind14", "rct2ww.scenery_small.sbwind14" },
    { "rct2.ww.wmarble1", "rct2ww.scenery_wall.wmarble1" },
    { "rct2.ww.wbambopc", "rct2ww.scenery_wall.wbambopc" },
    { "rct2.ww.wshogi07", "rct2ww.scenery_wall.wshogi07" },
    { "rct2.ww.wskysc08", "rct2ww.scenery_wall.wskysc08" },
    { "rct2.ww.wmud05", "rct2ww.scenery_wall.wmud05" },
    { "rct2.ww.wtudor03", "rct2ww.scenery_wall.wtudor03" },
    { "rct2.ww.wdrab07", "rct2ww.scenery_wall.wdrab07" },
    { "rct2.ww.wshogi10", "rct2ww.scenery_wall.wshogi10" },
    { "rct2.ww.wkreml07", "rct2ww.scenery_wall.wkreml07" },
    { "rct2.ww.wallna07", "rct2ww.scenery_wall.wallna07" },
    { "rct2.ww.wshogi11", "rct2ww.scenery_wall.wshogi11" },
    { "rct2.ww.w2corr06", "rct2ww.scenery_wall.w2corr06" },
    { "rct2.ww.w3corr08", "rct2ww.scenery_wall.w3corr08" },
    { "rct2.ww.wskysc04", "rct2ww.scenery_wall.wskysc04" },
    { "rct2.ww.wallice5", "rct2ww.scenery_wall.wallice5" },
    { "rct2.ww.wtudor11", "rct2ww.scenery_wall.wtudor11" },
    { "rct2.ww.wmud08", "rct2ww.scenery_wall.wmud08" },
    { "rct2.ww.wtudor09", "rct2ww.scenery_wall.wtudor09" },
    { "rct2.ww.wbambo01", "rct2ww.scenery_wall.wbambo01" },
    { "rct2.ww.wallna13", "rct2ww.scenery_wall.wallna13" },
    { "rct2.ww.wmarble3", "rct2ww.scenery_wall.wmarble3" },
    { "rct2.ww.wmud01", "rct2ww.scenery_wall.wmud01" },
    { "rct2.ww.wmud03", "rct2ww.scenery_wall.wmud03" },
    { "rct2.ww.wgwoc2", "rct2ww.scenery_wall.wgwoc2" },
    { "rct2.ww.wkreml10", "rct2ww.scenery_wall.wkreml10" },
    { "rct2.ww.wcorr09", "rct2ww.scenery_wall.wcorr09" },
    { "rct2.ww.w3corr05", "rct2ww.scenery_wall.w3corr05" },
    { "rct2.ww.wallna05", "rct2ww.scenery_wall.wallna05" },
    { "rct2.ww.wskysc05", "rct2ww.scenery_wall.wskysc05" },
    { "rct2.ww.wmarbpl7", "rct2ww.scenery_wall.wmarbpl7" },
    { "rct2.ww.wallna04", "rct2ww.scenery_wall.wallna04" },
    { "rct2.ww.wgeorg03", "rct2ww.scenery_wall.wgeorg03" },
    { "rct2.ww.w2corr03", "rct2ww.scenery_wall.w2corr03" },
    { "rct2.ww.wbrick01", "rct2ww.scenery_wall.wbrick01" },
    { "rct2.ww.wshogi02", "rct2ww.scenery_wall.wshogi02" },
    { "rct2.ww.w2corr05", "rct2ww.scenery_wall.w2corr05" },
    { "rct2.ww.tmarch1", "rct2ww.scenery_wall.tmarch1" },
    { "rct2.ww.wdrab02", "rct2ww.scenery_wall.wdrab02" },
    { "rct2.ww.wgeorg01", "rct2ww.scenery_wall.wgeorg01" },
    { "rct2.ww.wskysc09", "rct2ww.scenery_wall.wskysc09" },
    { "rct2.ww.wbambo15", "rct2ww.scenery_wall.wbambo15" },
    { "rct2.ww.wcorr08", "rct2ww.scenery_wall.wcorr08" },
    { "rct2.ww.wallna03", "rct2ww.scenery_wall.wallna03" },
    { "rct2.ww.wshogi06", "rct2ww.scenery_wall.wshogi06" },
    { "rct2.ww.wmud06", "rct2ww.scenery_wall.wmud06" },
    { "rct2.ww.wwdaub05", "rct2ww.scenery_wall.wwdaub05" },
    { "rct2.ww.wtudor04", "rct2ww.scenery_wall.wtudor04" },
    { "rct2.ww.wcorr12", "rct2ww.scenery_wall.wcorr12" },
    { "rct2.ww.wskysc03", "rct2ww.scenery_wall.wskysc03" },
    { "rct2.ww.w2corr07", "rct2ww.scenery_wall.w2corr07" },
    { "rct2.ww.wwdaub02", "rct2ww.scenery_wall.wwdaub02" },
    { "rct2.ww.wcorr03", "rct2ww.scenery_wall.wcorr03" },
    { "rct2.ww.wwdaub04", "rct2ww.scenery_wall.wwdaub04" },
    { "rct2.ww.wcorr10", "rct2ww.scenery_wall.wcorr10" },
    { "rct2.ww.wallice7", "rct2ww.scenery_wall.wallice7" },
    { "rct2.ww.wwdaub03", "rct2ww.scenery_wall.wwdaub03" },
    { "rct2.ww.wdrab03", "rct2ww.scenery_wall.wdrab03" },
    { "rct2.ww.wlog01", "rct2ww.scenery_wall.wlog01" },
    { "rct2.ww.wbrick05", "rct2ww.scenery_wall.wbrick05" },
    { "rct2.ww.wwind06", "rct2ww.scenery_wall.wwind06" },
    { "rct2.ww.wshogi14", "rct2ww.scenery_wall.wshogi14" },
    { "rct2.ww.wbrick04", "rct2ww.scenery_wall.wbrick04" },
    { "rct2.ww.wgeorg02", "rct2ww.scenery_wall.wgeorg02" },
    { "rct2.ww.wbambo21", "rct2ww.scenery_wall.wbambo21" },
    { "rct2.ww.wtudor06", "rct2ww.scenery_wall.wtudor06" },
    { "rct2.ww.wgeorg04", "rct2ww.scenery_wall.wgeorg04" },
    { "rct2.ww.wshogi04", "rct2ww.scenery_wall.wshogi04" },
    { "rct2.ww.w3corr04", "rct2ww.scenery_wall.w3corr04" },
    { "rct2.ww.wtudor02", "rct2ww.scenery_wall.wtudor02" },
    { "rct2.ww.wbrick03", "rct2ww.scenery_wall.wbrick03" },
    { "rct2.ww.wallna08", "rct2ww.scenery_wall.wallna08" },
    { "rct2.ww.wmarble2", "rct2ww.scenery_wall.wmarble2" },
    { "rct2.ww.wkreml09", "rct2ww.scenery_wall.wkreml09" },
    { "rct2.ww.wgeorg06", "rct2ww.scenery_wall.wgeorg06" },
    { "rct2.ww.wallice3", "rct2ww.scenery_wall.wallice3" },
    { "rct2.ww.wigloo2", "rct2ww.scenery_wall.wigloo2" },
    { "rct2.ww.wskysc10", "rct2ww.scenery_wall.wskysc10" },
    { "rct2.ww.wallice9", "rct2ww.scenery_wall.wallice9" },
    { "rct2.ww.w3corr02", "rct2ww.scenery_wall.w3corr02" },
    { "rct2.ww.wmarbpl3", "rct2ww.scenery_wall.wmarbpl3" },
    { "rct2.ww.w3corr07", "rct2ww.scenery_wall.w3corr07" },
    { "rct2.ww.wlog05", "rct2ww.scenery_wall.wlog05" },
    { "rct2.ww.wgwoc3", "rct2ww.scenery_wall.wgwoc3" },
    { "rct2.ww.wskysc06", "rct2ww.scenery_wall.wskysc06" },
    { "rct2.ww.wcorr14", "rct2ww.scenery_wall.wcorr14" },
    { "rct2.ww.wbrick13", "rct2ww.scenery_wall.wbrick13" },
    { "rct2.ww.wmarble5", "rct2ww.scenery_wall.wmarble5" },
    { "rct2.ww.wkreml08", "rct2ww.scenery_wall.wkreml08" },
    { "rct2.ww.wmarbpl4", "rct2ww.scenery_wall.wmarbpl4" },
    { "rct2.ww.wlog06", "rct2ww.scenery_wall.wlog06" },
    { "rct2.ww.wdrab06", "rct2ww.scenery_wall.wdrab06" },
    { "rct2.ww.wbrick12", "rct2ww.scenery_wall.wbrick12" },
    { "rct2.ww.wshogi17", "rct2ww.scenery_wall.wshogi17" },
    { "rct2.ww.wtudor05", "rct2ww.scenery_wall.wtudor05" },
    { "rct2.ww.wallna01", "rct2ww.scenery_wall.wallna01" },
    { "rct2.ww.wmarbpl6", "rct2ww.scenery_wall.wmarbpl6" },
    { "rct2.ww.wcorr07", "rct2ww.scenery_wall.wcorr07" },
    { "rct2.ww.wbambo05", "rct2ww.scenery_wall.wbambo05" },
    { "rct2.ww.wmud04", "rct2ww.scenery_wall.wmud04" },
    { "rct2.ww.wmarbpl5", "rct2ww.scenery_wall.wmarbpl5" },
    { "rct2.ww.wallice4", "rct2ww.scenery_wall.wallice4" },
    { "rct2.ww.wallice2", "rct2ww.scenery_wall.wallice2" },
    { "rct2.ww.wbambo13", "rct2ww.scenery_wall.wbambo13" },
    { "rct2.ww.wbambo04", "rct2ww.scenery_wall.wbambo04" },
    { "rct2.ww.wcorr01", "rct2ww.scenery_wall.wcorr01" },
    { "rct2.ww.wpalm01", "rct2ww.scenery_wall.wpalm01" },
    { "rct2.ww.wshogi16", "rct2ww.scenery_wall.wshogi16" },
    { "rct2.ww.wallice8", "rct2ww.scenery_wall.wallice8" },
    { "rct2.ww.wlog02", "rct2ww.scenery_wall.wlog02" },
    { "rct2.ww.wtudor01", "rct2ww.scenery_wall.wtudor01" },
    { "rct2.ww.wshogi08", "rct2ww.scenery_wall.wshogi08" },
    { "rct2.ww.wcorr04", "rct2ww.scenery_wall.wcorr04" },
    { "rct2.ww.wgwoc1", "rct2ww.scenery_wall.wgwoc1" },
    { "rct2.ww.w2corr02", "rct2ww.scenery_wall.w2corr02" },
    { "rct2.ww.wallice6", "rct2ww.scenery_wall.wallice6" },
    { "rct2.ww.wallna10", "rct2ww.scenery_wall.wallna10" },
    { "rct2.ww.wbrick11", "rct2ww.scenery_wall.wbrick11" },
    { "rct2.ww.wshogi13", "rct2ww.scenery_wall.wshogi13" },
    { "rct2.ww.wskysc01", "rct2ww.scenery_wall.wskysc01" },
    { "rct2.ww.wcorr05", "rct2ww.scenery_wall.wcorr05" },
    { "rct2.ww.w2corr01", "rct2ww.scenery_wall.w2corr01" },
    { "rct2.ww.wshogi15", "rct2ww.scenery_wall.wshogi15" },
    { "rct2.ww.wdrab01", "rct2ww.scenery_wall.wdrab01" },
    { "rct2.ww.wallna09", "rct2ww.scenery_wall.wallna09" },
    { "rct2.ww.wshogi12", "rct2ww.scenery_wall.wshogi12" },
    { "rct2.ww.wmud07", "rct2ww.scenery_wall.wmud07" },
    { "rct2.ww.wpalm02", "rct2ww.scenery_wall.wpalm02" },
    { "rct2.ww.wmarbpl2", "rct2ww.scenery_wall.wmarbpl2" },
    { "rct2.ww.wwind04", "rct2ww.scenery_wall.wwind04" },
    { "rct2.ww.wbambo14", "rct2ww.scenery_wall.wbambo14" },
    { "rct2.ww.wshogi09", "rct2ww.scenery_wall.wshogi09" },
    { "rct2.ww.wwdaub07", "rct2ww.scenery_wall.wwdaub07" },
    { "rct2.ww.wallna12", "rct2ww.scenery_wall.wallna12" },
    { "rct2.ww.wmarble6", "rct2ww.scenery_wall.wmarble6" },
    { "rct2.ww.wbrick08", "rct2ww.scenery_wall.wbrick08" },
    { "rct2.ww.wpalm04", "rct2ww.scenery_wall.wpalm04" },
    { "rct2.ww.tmarch2", "rct2ww.scenery_wall.tmarch2" },
    { "rct2.ww.wallna14", "rct2ww.scenery_wall.wallna14" },
    { "rct2.ww.wmarbpl1", "rct2ww.scenery_wall.wmarbpl1" },
    { "rct2.ww.wwind05", "rct2ww.scenery_wall.wwind05" },
    { "rct2.ww.wpalm05", "rct2ww.scenery_wall.wpalm05" },
    { "rct2.ww.wallice1", "rct2ww.scenery_wall.wallice1" },
    { "rct2.ww.wbambo02", "rct2ww.scenery_wall.wbambo02" },
    { "rct2.ww.wskysc11", "rct2ww.scenery_wall.wskysc11" },
    { "rct2.ww.wtudor08", "rct2ww.scenery_wall.wtudor08" },
    { "rct2.ww.w3corr03", "rct2ww.scenery_wall.w3corr03" },
    { "rct2.ww.w2corr08", "rct2ww.scenery_wall.w2corr08" },
    { "rct2.ww.wwind03", "rct2ww.scenery_wall.wwind03" },
    { "rct2.ww.wmarble4", "rct2ww.scenery_wall.wmarble4" },
    { "rct2.ww.wwdaub06", "rct2ww.scenery_wall.wwdaub06" },
    { "rct2.ww.wmud02", "rct2ww.scenery_wall.wmud02" },
    { "rct2.ww.wtudor10", "rct2ww.scenery_wall.wtudor10" },
    { "rct2.ww.wbambo03", "rct2ww.scenery_wall.wbambo03" },
    { "rct2.ww.wskysc02", "rct2ww.scenery_wall.wskysc02" },
    { "rct2.ww.wdrab04", "rct2ww.scenery_wall.wdrab04" },
    { "rct2.ww.wcorr02", "rct2ww.scenery_wall.wcorr02" },
    { "rct2.ww.wallna02", "rct2ww.scenery_wall.wallna02" },
    { "rct2.ww.wdrab05", "rct2ww.scenery_wall.wdrab05" },
    { "rct2.ww.wbrick02", "rct2ww.scenery_wall.wbrick02" },
    { "rct2.ww.wigloo1", "rct2ww.scenery_wall.wigloo1" },
    { "rct2.ww.wcorr13", "rct2ww.scenery_wall.wcorr13" },
    { "rct2.ww.wskysc07", "rct2ww.scenery_wall.wskysc07" },
    { "rct2.ww.wcorr15", "rct2ww.scenery_wall.wcorr15" },
    { "rct2.ww.wcorr16", "rct2ww.scenery_wall.wcorr16" },
    { "rct2.ww.w2corr04", "rct2ww.scenery_wall.w2corr04" },
    { "rct2.ww.wdrab08", "rct2ww.scenery_wall.wdrab08" },
    { "rct2.ww.wlog04", "rct2ww.scenery_wall.wlog04" },
    { "rct2.ww.wskysc12", "rct2ww.scenery_wall.wskysc12" },
    { "rct2.ww.wbambo12", "rct2ww.scenery_wall.wbambo12" },
    { "rct2.ww.wbambo11", "rct2ww.scenery_wall.wbambo11" },
    { "rct2.ww.w3corr01", "rct2ww.scenery_wall.w3corr01" },
    { "rct2.ww.wshogi03", "rct2ww.scenery_wall.wshogi03" },
    { "rct2.ww.wwdaub01", "rct2ww.scenery_wall.wwdaub01" },
    { "rct2.ww.wcorr06", "rct2ww.scenery_wall.wcorr06" },
    { "rct2.ww.wtudor07", "rct2ww.scenery_wall.wtudor07" },
    { "rct2.ww.wgeorg05", "rct2ww.scenery_wall.wgeorg05" },
    { "rct2.ww.wallna11", "rct2ww.scenery_wall.wallna11" },
    { "rct2.ww.wshogi05", "rct2ww.scenery_wall.wshogi05" },
    { "rct2.ww.wlog03", "rct2ww.scenery_wall.wlog03" },
    { "rct2.ww.w3corr06", "rct2ww.scenery_wall.w3corr06" },
    { "rct2.ww.wshogi01", "rct2ww.scenery_wall.wshogi01" },
    { "rct2.ww.wallic10", "rct2ww.scenery_wall.wallic10" },
    { "rct2.ww.wcorr11", "rct2ww.scenery_wall.wcorr11" },
    { "rct2.ww.wpalm03", "rct2ww.scenery_wall.wpalm03" },
    { "rct2.ww.wallna06", "rct2ww.scenery_wall.wallna06" },
    { "rct2.ww.lionride", "rct2ww.ride.lionride" },
    { "rct2.ww.tigrtwst", "rct2ww.ride.tigrtwst" },
    { "rct2.ww.tgvtrain", "rct2ww.ride.tgvtrain" },
    { "rct2.ww.caddilac", "rct2ww.ride.caddilac" },
    { "rct2.ww.bomerang", "rct2ww.ride.bomerang" },
    { "rct2.ww.coffeecu", "rct2ww.ride.coffeecu" },
    { "rct2.ww.football", "rct2ww.ride.football" },
    { "rct2.ww.dolphinr", "rct2ww.ride.dolphinr" },
    { "rct2.ww.mandarin", "rct2ww.ride.mandarin" },
    { "rct2.ww.crnvbfly", "rct2ww.ride.crnvbfly" },
    { "rct2.ww.mantaray", "rct2ww.ride.mantaray" },
    { "rct2.ww.blackcab", "rct2ww.ride.blackcab" },
    { "rct2.ww.skidoo", "rct2ww.ride.skidoo" },
    { "rct2.ww.dhowwatr", "rct2ww.ride.dhowwatr" },
    { "rct2.ww.minelift", "rct2ww.ride.minelift" },
    { "rct2.ww.sloth", "rct2ww.ride.sloth" },
    { "rct2.ww.huskie", "rct2ww.ride.huskie" },
    { "rct2.ww.condorrd", "rct2ww.ride.condorrd" },
    { "rct2.ww.junkswng", "rct2ww.ride.junkswng" },
    { "rct2.ww.congaeel", "rct2ww.ride.congaeel" },
    { "rct2.ww.dragdodg", "rct2ww.ride.dragdodg" },
    { "rct2.ww.diamondr", "rct2ww.ride.diamondr" },
    { "rct2.ww.fightkit", "rct2ww.ride.fightkit" },
    { "rct2.ww.faberge", "rct2ww.ride.faberge" },
    { "rct2.ww.penguinb", "rct2ww.ride.penguinb" },
    { "rct2.ww.crnvlzrd", "rct2ww.ride.crnvlzrd" },
    { "rct2.ww.killwhal", "rct2ww.ride.killwhal" },
    { "rct2.ww.rocket", "rct2ww.ride.rocket" },
    { "rct2.ww.tutlboat", "rct2ww.ride.tutlboat" },
    { "rct2.ww.gratwhte", "rct2ww.ride.gratwhte" },
    { "rct2.ww.steamtrn", "rct2ww.ride.steamtrn" },
    { "rct2.ww.londonbs", "rct2ww.ride.londonbs" },
    { "rct2.ww.rhinorid", "rct2ww.ride.rhinorid" },
    { "rct2.ww.anaconda", "rct2ww.ride.anaconda" },
    { "rct2.ww.italypor", "rct2ww.ride.italypor" },
    { "rct2.ww.gorilla", "rct2ww.ride.gorilla" },
    { "rct2.ww.ostrich", "rct2ww.ride.ostrich" },
    { "rct2.ww.crnvfrog", "rct2ww.ride.crnvfrog" },
    { "rct2.ww.dragon", "rct2ww.ride.dragon" },
    { "rct2.ww.minecart", "rct2ww.ride.minecart" },
    { "rct2.ww.firecrak", "rct2ww.ride.firecrak" },
    { "rct2.ww.hipporid", "rct2ww.ride.hipporid" },
    { "rct2.ww.rssncrrd", "rct2ww.ride.rssncrrd" },
    { "rct2.ww.kolaride", "rct2ww.ride.kolaride" },
    { "rct2.ww.seals", "rct2ww.ride.seals" },
    { "rct2.ww.polarber", "rct2ww.ride.polarber" },
    { "rct2.ww.taxicstr", "rct2ww.ride.taxicstr" },
    { "rct2.ww.whicgrub", "rct2ww.ride.whicgrub" },
    { "rct2.ww.jaguarrd", "rct2ww.ride.jaguarrd" },
    { "rct2.ww.sputnikr", "rct2ww.ride.sputnikr" },
    { "rct2.ww.outriggr", "rct2ww.ride.outriggr" },
    { "rct2.ww.stgccstr", "rct2ww.ride.stgccstr" },
    { "rct2.ww.surfbrdc", "rct2ww.ride.surfbrdc" },
    { "rct2.ww.bullet", "rct2ww.ride.bullet" },
    { "rct2.ww.crocflum", "rct2ww.ride.crocflum" },
    { "rct2.ww.sanftram", "rct2ww.ride.sanftram" },
    { "rct2.ww.ozentran", "rct2ww.park_entrance.ozentran" },
    { "rct2.ww.euroent", "rct2ww.park_entrance.euroent" },
    { "rct2.ww.iceent", "rct2ww.park_entrance.iceent" },
    { "rct2.ww.japent", "rct2ww.park_entrance.japent" },
    { "rct2.ww.africent", "rct2ww.park_entrance.africent" },
    { "rct2.ww.naent", "rct2ww.park_entrance.naent" },
    { "rct2.ww.samerent", "rct2ww.park_entrance.samerent" },
    { "rct1.wooden_fence_red", "rct1.scenery_wall.wooden_fence_red" },
    { "rct1.ll.railings.bamboo", "rct1ll.footpath_railings.bamboo" },
    { "rct1.ll.railings.space", "rct1ll.footpath_railings.space" },
    { "rct1.toilets", "rct1.ride.toilets" },
    { "rct1.pathsurface.crazy", "rct1.footpath_surface.crazy_paving" },
    { "rct1.ll.pathsurface.tile.red", "rct1ll.footpath_surface.tiles_red" },
    { "rct1.pathsurface.tarmac", "rct1.footpath_surface.tarmac" },
    { "rct1.aa.pathsurface.tile.grey", "rct1aa.footpath_surface.tiles_grey" },
    { "rct1.pathsurface.dirt", "rct1.footpath_surface.dirt" },
    { "rct1.aa.pathsurface.space", "rct1aa.footpath_surface.tarmac_red" },
    { "rct1.ll.pathsurface.tile.green", "rct1ll.footpath_surface.tiles_green" },
    { "rct1.pathsurface.queue.blue", "rct1.footpath_surface.queue_blue" },
    { "rct1.aa.pathsurface.queue.yellow", "rct1aa.footpath_surface.queue_yellow" },
    { "rct1.aa.pathsurface.ash", "rct1aa.footpath_surface.ash" },
    { "rct1.aa.pathsurface.tarmac.green", "rct1aa.footpath_surface.tarmac_green" },
    { "rct1.pathsurface.tile.pink", "rct1.footpath_surface.tiles_brown" },
    { "rct1.aa.pathsurface.tarmac.brown", "rct1aa.footpath_surface.tarmac_brown" },
    { "rct1.aa.pathsurface.queue.red", "rct1aa.footpath_surface.queue_red" },
    { "rct1.aa.pathsurface.queue.green", "rct1aa.footpath_surface.queue_green" },
    { "rct1.ll.surface.roofgrey", "rct1ll.terrain_surface.roof_grey" },
    { "rct1.ll.surface.wood", "rct1ll.terrain_surface.wood" },
    { "rct1.aa.surface.roofred", "rct1aa.terrain_surface.roof_red" },
    { "rct1.ll.surface.rust", "rct1ll.terrain_surface.rust" },
    { "rct1.ll.edge.green", "rct1ll.terrain_edge.green" },
    { "rct1.aa.edge.yellow", "rct1aa.terrain_edge.yellow" },
    { "rct1.ll.edge.stonegrey", "rct1ll.terrain_edge.stone_grey" },
    { "rct1.aa.edge.red", "rct1aa.terrain_edge.red" },
    { "rct1.ll.edge.skyscraperb", "rct1ll.terrain_edge.skyscraper_b" },
    { "rct1.ll.edge.stonebrown", "rct1ll.terrain_edge.stone_brown" },
    { "rct1.edge.iron", "rct1.terrain_edge.iron" },
    { "rct1.ll.edge.skyscrapera", "rct1ll.terrain_edge.skyscraper_a" },
    { "rct1.aa.edge.grey", "rct1aa.terrain_edge.grey" },
    { "rct1.ll.edge.purple", "rct1ll.terrain_edge.purple" },
    { "rct1.edge.brick", "rct1.terrain_edge.brick" },
    { "rct2.pathsurface.queue.red", "rct2.footpath_surface.queue_red" },
    { "rct2.pathsurface.queue.yellow", "rct2.footpath_surface.queue_yellow" },
    { "rct2.pathsurface.crazy", "rct2.footpath_surface.crazy_paving" },
    { "rct2.pathsurface.queue.green", "rct2.footpath_surface.queue_green" },
    { "rct2.pathsurface.queue.blue", "rct2.footpath_surface.queue_blue" },
    { "rct1.pathsurface.tile.brown", "rct1.footpath_surface.tiles_brown" },
};

static std::string_view MapToNewObjectIdentifier(std::string_view s)
{
    auto it = oldObjectIds.find(s);
    if (it != oldObjectIds.end())
    {
        return it->second;
    }
    return s;
}

static std::map<std::string_view, std::string_view> DATPathNames = {
    { "rct2.pathash", "PATHASH " },  { "rct2.pathcrzy", "PATHCRZY" }, { "rct2.pathdirt", "PATHDIRT" },
    { "rct2.pathspce", "PATHSPCE" }, { "rct2.road", "ROAD    " },     { "rct2.tarmacb", "TARMACB " },
    { "rct2.tarmacg", "TARMACG " },  { "rct2.tarmac", "TARMAC  " },   { "rct2.1920path", "1920PATH" },
    { "rct2.futrpath", "FUTRPATH" }, { "rct2.futrpat2", "FUTRPAT2" }, { "rct2.jurrpath", "JURRPATH" },
    { "rct2.medipath", "MEDIPATH" }, { "rct2.mythpath", "MYTHPATH" }, { "rct2.ranbpath", "RANBPATH" },
};

static std::optional<std::string_view> GetDATPathName(std::string_view newPathName)
{
    auto it = DATPathNames.find(newPathName);
    if (it != DATPathNames.end())
    {
        return it->second;
    }
    return std::nullopt;
}

static FootpathMapping _extendedFootpathMappings[] = {
    { "rct1.path.tarmac", "rct1.footpath_surface.tarmac", "rct1.footpath_surface.queue_blue", "rct2.footpath_railings.wood" },
};

static const FootpathMapping* GetFootpathMapping(const ObjectEntryDescriptor& desc)
{
    for (const auto& mapping : _extendedFootpathMappings)
    {
        if (mapping.Original == desc.GetName())
        {
            return &mapping;
        }
    }

    // GetFootpathSurfaceId expects an old-style DAT identifier. In early versions of the NSF,
    // we used JSON ids for legacy paths, so we have to map those to old DAT identifiers first.
    if (desc.Generation == ObjectGeneration::JSON)
    {
        auto datPathName = GetDATPathName(desc.Identifier);
        if (datPathName.has_value())
        {
            rct_object_entry objectEntry = {};
            objectEntry.SetName(datPathName.value());
            return GetFootpathSurfaceId(ObjectEntryDescriptor(objectEntry));
        }

        return nullptr;
    }

    // Even old .park saves with DAT identifiers somehow exist.
    return GetFootpathSurfaceId(desc);
}

static void UpdateFootpathsFromMapping(
    ObjectEntryIndex* pathToSurfaceMap, ObjectEntryIndex* pathToQueueSurfaceMap, ObjectEntryIndex* pathToRailingsMap,
    ObjectList& requiredObjects, ObjectEntryIndex& surfaceCount, ObjectEntryIndex& railingCount, ObjectEntryIndex entryIndex,
    const FootpathMapping* footpathMapping)
{
    auto surfaceIndex = requiredObjects.Find(ObjectType::FootpathSurface, footpathMapping->NormalSurface);
    if (surfaceIndex == OBJECT_ENTRY_INDEX_NULL)
    {
        requiredObjects.SetObject(ObjectType::FootpathSurface, surfaceCount, footpathMapping->NormalSurface);
        surfaceIndex = surfaceCount++;
    }
    pathToSurfaceMap[entryIndex] = surfaceIndex;

    surfaceIndex = requiredObjects.Find(ObjectType::FootpathSurface, footpathMapping->QueueSurface);
    if (surfaceIndex == OBJECT_ENTRY_INDEX_NULL)
    {
        requiredObjects.SetObject(ObjectType::FootpathSurface, surfaceCount, footpathMapping->QueueSurface);
        surfaceIndex = surfaceCount++;
    }
    pathToQueueSurfaceMap[entryIndex] = surfaceIndex;

    auto railingIndex = requiredObjects.Find(ObjectType::FootpathRailings, footpathMapping->Railing);
    if (railingIndex == OBJECT_ENTRY_INDEX_NULL)
    {
        requiredObjects.SetObject(ObjectType::FootpathRailings, railingCount, footpathMapping->Railing);
        railingIndex = railingCount++;
    }
    pathToRailingsMap[entryIndex] = railingIndex;
}