#include "partners_api/google_ads.hpp"

namespace
{
#if defined(OMIM_OS_IPHONE)
  auto const kSearchbannerId = "ca-app-pub-2128181646657514/6481830252";
#elif defined(OMIM_OS_ANDROID)
  auto const kSearchbannerId = "ca-app-pub-2128181646657514/6481830252";
#else
  auto const kSearchbannerId = "dummy";
#endif
}  // namespace

namespace ads
{
bool Google::HasSearchBanner() const
{
  return true;
}

std::string Google::GetSearchBannerId() const
{
  return kSearchbannerId;
}
}  // namespace ads
