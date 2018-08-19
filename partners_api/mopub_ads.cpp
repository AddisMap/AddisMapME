#include "partners_api/mopub_ads.hpp"
#include "partners_api/partners.hpp"

#include "private.h"

namespace
{
#if defined(OMIM_OS_IPHONE)
  auto const kTourismPlacementId = "e1f5937c038241eeaaa4d62322564c3c";
  auto const kNavigationPlacementId = "0f50c592742a4fb5a19ede2876c4c446";
  auto const kNonTourismPlacementId = "a144c93ba6ca4339b10a7df5a887aca5";
  auto const kSponsoredBannerPlacementId = "3f121b2b9e6544158c4976d9afac22b1";
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
