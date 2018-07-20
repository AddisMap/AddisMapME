#include "partners_api/mopub_ads.hpp"
#include "partners_api/partners.hpp"

#include "private.h"

namespace
{
#if defined(OMIM_OS_IPHONE)
  auto const kTourismPlacementId = "29c1bc85b46442b5a370552916aa6822";
  auto const kNavigationPlacementId = "00af522ea7f94b77b6c671c7e1b13c3f";
  auto const kNonTourismPlacementId = "67ebcbd0af8345f18cccfb230ca08a17";
  auto const kSponsoredBannerPlacementId = "e0c1d4d9c88c4670b9541116bcb75c5f";
#else
  auto const kTourismPlacementId = "3fce55a072e244d9af4c7324828599e2";
  auto const kNavigationPlacementId = "fb6d568c5e1c44da9708bc8f74ec3543";
  auto const kNonTourismPlacementId = "70cbf346ce7c4498896e69df52ec76a6";
  auto const kSponsoredBannerPlacementId = "a3d1a98651614e708e04d3c1f4b56e8d";
#endif
}  // namespace

namespace ads
{
Mopub::Mopub()
{
  AppendEntry({{"amenity", "cafe"},       // food
               {"amenity", "fast_food"},
               {"amenity", "restaurant"},
               {"amenity", "bar"},
               {"amenity", "pub"},
               {"shop"},                  // shops
               {"amenity", "marketplace"},
               {"tourism", "zoo"},        // sights
               {"tourism", "artwork"},
               {"tourism", "information"},
               {"tourism", "attraction"},
               {"tourism", "viewpoint"},
               {"tourism", "museum"},
               {"amenity", "fountain"},
               {"amenity", "townhall"},
               {"historic"},
               {"amenity", "cinema"},     // entertainment
               {"amenity", "brothel"},
               {"amenity", "casino"},
               {"amenity", "nightclub"},
               {"amenity", "theatre"},
               {"boundary", "national_park"},
               {"leisure"}},
              kTourismPlacementId);

  AppendEntry({{"building"},              // building
               {"place"},                 // large toponyms
               {"aerialway"},             // city transport
               {"highway", "bus_stop"},
               {"highway", "speed_camera"},
               {"public_transport"},
               {"aeroway"},               // global transport
               {"railway"},
               {"man_made", "pier"}},
              kNavigationPlacementId);

  AppendEntry({{"amenity", "dentist"},    // health
               {"amenity", "doctors"},
               {"amenity", "clinic"},
               {"amenity", "hospital"},
               {"amenity", "pharmacy"},
               {"amenity", "veterinary"},
               {"amenity", "bank"},       // financial
               {"amenity", "atm"},
               {"amenity", "bureau_de_change"}},
              kNonTourismPlacementId);

  AppendEntry({{"sponsored", "banner"}}, kSponsoredBannerPlacementId);

  for (auto const & p : GetPartners())
  {
    auto const & placementId = p.GetBannerPlacementId();
    if (!placementId.empty())
      AppendEntry({{"sponsored", p.m_type.c_str()}}, placementId);
  }
}

std::string Mopub::GetBannerIdForOtherTypes() const
{
  return kNonTourismPlacementId;
}

// static
std::string Mopub::InitializationBannerId()
{
  return kSponsoredBannerPlacementId;
}
}  // namespace ads
