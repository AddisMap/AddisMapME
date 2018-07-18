#include "editor/osm_editor.hpp"

#include "editor/changeset_wrapper.hpp"
#include "editor/edits_migration.hpp"
#include "editor/osm_auth.hpp"
#include "editor/server_api.hpp"
#include "editor/xml_feature.hpp"

#include "indexer/categories_holder.hpp"
#include "indexer/classificator.hpp"
#include "indexer/data_source_helpers.hpp"
#include "indexer/fake_feature_ids.hpp"
#include "indexer/feature_algo.hpp"
#include "indexer/feature_decl.hpp"
#include "indexer/feature_impl.hpp"
#include "indexer/feature_meta.hpp"
#include "indexer/feature_source.hpp"
#include "indexer/ftypes_matcher.hpp"
#include "indexer/mwm_set.hpp"

#include "platform/local_country_file_utils.hpp"
#include "platform/platform.hpp"
#include "platform/preferred_languages.hpp"

#include "coding/internal/file_data.hpp"

#include "geometry/algorithm.hpp"

#include "base/exception.hpp"
#include "base/logging.hpp"
#include "base/stl_helpers.hpp"
#include "base/string_utils.hpp"
#include "base/thread_checker.hpp"
#include "base/timer.hpp"

#include "std/algorithm.hpp"
#include "std/array.hpp"
#include "std/chrono.hpp"
#include "std/sstream.hpp"
#include "std/target_os.hpp"
#include "std/tuple.hpp"
#include "std/unordered_map.hpp"
#include "std/unordered_set.hpp"

#include "3party/Alohalytics/src/alohalytics.h"
#include "3party/opening_hours/opening_hours.hpp"
#include "3party/pugixml/src/pugixml.hpp"

using namespace pugi;
using feature::EGeomType;
using feature::Metadata;
using editor::XMLFeature;

namespace
{
constexpr char const * kXmlRootNode = "mapsme";
constexpr char const * kXmlMwmNode = "mwm";
constexpr char const * kDeleteSection = "delete";
constexpr char const * kModifySection = "modify";
constexpr char const * kCreateSection = "create";
constexpr char const * kObsoleteSection = "obsolete";
/// We store edited streets in OSM-compatible way.
constexpr char const * kAddrStreetTag = "addr:street";

constexpr char const * kUploaded = "Uploaded";
constexpr char const * kDeletedFromOSMServer = "Deleted from OSM by someone";
constexpr char const * kNeedsRetry = "Needs Retry";
constexpr char const * kMatchedFeatureIsEmpty = "Matched feature has no tags";

struct XmlSection
{
  XmlSection(FeatureStatus status, std::string const & sectionName)
    : m_status(status), m_sectionName(sectionName)
  {
  }

  FeatureStatus m_status = FeatureStatus::Untouched;
  std::string m_sectionName;
};

array<XmlSection, 4> const kXmlSections = {{{FeatureStatus::Deleted, kDeleteSection},
                                            {FeatureStatus::Modified, kModifySection},
                                            {FeatureStatus::Obsolete, kObsoleteSection},
                                            {FeatureStatus::Created, kCreateSection}}};

struct LogHelper
{
  LogHelper(MwmSet::MwmId const & mwmId) : m_mwmId(mwmId) {}

  ~LogHelper()
  {
    LOG(LINFO, ("For", m_mwmId, ". Was loaded", m_modified, "modified,", m_created, "created,",
                m_deleted, "deleted and", m_obsolete, "obsolete features."));
  }

  void OnStatus(FeatureStatus status)
  {
    switch (status)
    {
    case FeatureStatus::Deleted: ++m_deleted; break;
    case FeatureStatus::Modified: ++m_modified; break;
    case FeatureStatus::Obsolete: ++m_obsolete; break;
    case FeatureStatus::Created: ++m_created; break;
    case FeatureStatus::Untouched: ASSERT(false, ());
    }
  }

  uint32_t m_deleted = 0;
  uint32_t m_obsolete = 0;
  uint32_t m_modified = 0;
  uint32_t m_created = 0;
  MwmSet::MwmId const & m_mwmId;
};

bool NeedsUpload(string const & uploadStatus)
{
  return uploadStatus != kUploaded && uploadStatus != kDeletedFromOSMServer &&
         uploadStatus != kMatchedFeatureIsEmpty;
}

/// Compares editable fields connected with feature ignoring street.
bool AreFeaturesEqualButStreet(FeatureType & a, FeatureType & b)
{
  feature::TypesHolder const aTypes(a);
  feature::TypesHolder const bTypes(b);

  if (!aTypes.Equals(bTypes))
    return false;

  if (a.GetHouseNumber() != b.GetHouseNumber())
    return false;

  if (!a.GetMetadata().Equals(b.GetMetadata()))
    return false;

  if (a.GetNames() != b.GetNames())
    return false;

  return true;
}

XMLFeature GetMatchingFeatureFromOSM(osm::ChangesetWrapper & cw, FeatureType & ft)
{
  ASSERT_NOT_EQUAL(ft.GetFeatureType(), feature::GEOM_LINE,
                   ("Line features are not supported yet."));
  if (ft.GetFeatureType() == feature::GEOM_POINT)
    return cw.GetMatchingNodeFeatureFromOSM(ft.GetCenter());

  // Warning: geometry is cached in FeatureType. So if it wasn't BEST_GEOMETRY,
  // we can never have it. Features here came from editors loader and should
  // have BEST_GEOMETRY geometry.
  auto geometry = ft.GetTriangesAsPoints(FeatureType::BEST_GEOMETRY);

  ASSERT_GREATER_OR_EQUAL(geometry.size(), 3, ("Is it an area feature?"));

  return cw.GetMatchingAreaFeatureFromOSM(geometry);
}

uint64_t GetMwmCreationTimeByMwmId(MwmSet::MwmId const & mwmId)
{
  return mwmId.GetInfo()->m_version.GetSecondsSinceEpoch();
}

bool IsObsolete(editor::XMLFeature const & xml, FeatureID const & fid)
{
  // TODO(mgsergio): If xml and feature are identical return true
  auto const uploadTime = xml.GetUploadTime();
  return uploadTime != base::INVALID_TIME_STAMP &&
         base::TimeTToSecondsSinceEpoch(uploadTime) < GetMwmCreationTimeByMwmId(fid.m_mwmId);
}
}  // namespace

namespace osm
{
// TODO(AlexZ): Normalize osm multivalue strings for correct merging
// (e.g. insert/remove spaces after ';' delimeter);

Editor::Editor()
  : m_configLoader(m_config)
  , m_notes(editor::Notes::MakeNotes())
  , m_isUploadingNow(false)
{
  SetDefaultStorage();
}

Editor & Editor::Instance()
{
  static Editor instance;
  return instance;
}

void Editor::SetDefaultStorage() { m_storage = make_unique<editor::LocalStorage>(); }

void Editor::LoadEdits()
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));
  if (!m_delegate)
  {
    LOG(LERROR, ("Can't load any map edits, delegate has not been set."));
    return;
  }

  xml_document doc;
  bool needRewriteEdits = false;

  if (!m_storage->Load(doc))
    return;

  m_features.Set(make_shared<FeaturesContainer>());
  auto loadedFeatures = make_shared<FeaturesContainer>();

  for (auto const & mwm : doc.child(kXmlRootNode).children(kXmlMwmNode))
  {
    string const mapName = mwm.attribute("name").as_string("");
    int64_t const mapVersion = mwm.attribute("version").as_llong(0);
    auto const mwmId = GetMwmIdByMapName(mapName);

    // TODO(mgsergio, milchakov): |mapName| may change between launches.
    // The right thing to do here is to try to migrate all changes anyway.
    if (!mwmId.IsAlive())
    {
      LOG(LINFO, ("Mwm", mapName, "was deleted"));
      needRewriteEdits = true;
      continue;
    }

    auto const needMigrateEdits = mapVersion != mwmId.GetInfo()->GetVersion();
    needRewriteEdits = needRewriteEdits || needMigrateEdits;

    LoadMwmEdits(*loadedFeatures, mwm, mwmId, needMigrateEdits);
  }
  // Save edits with new indexes and mwm version to avoid another migration on next startup.
  if (needRewriteEdits)
    SaveTransaction(loadedFeatures);
  else
    m_features.Set(loadedFeatures);
}

bool Editor::Save(FeaturesContainer const & features) const
{
  if (features.empty())
    return m_storage->Reset();

  xml_document doc;
  xml_node root = doc.append_child(kXmlRootNode);
  // Use format_version for possible future format changes.
  root.append_attribute("format_version") = 1;
  for (auto const & mwm : features)
  {
    xml_node mwmNode = root.append_child(kXmlMwmNode);
    mwmNode.append_attribute("name") = mwm.first.GetInfo()->GetCountryName().c_str();
    mwmNode.append_attribute("version") = static_cast<long long>(mwm.first.GetInfo()->GetVersion());
    xml_node deleted = mwmNode.append_child(kDeleteSection);
    xml_node modified = mwmNode.append_child(kModifySection);
    xml_node created = mwmNode.append_child(kCreateSection);
    xml_node obsolete = mwmNode.append_child(kObsoleteSection);
    for (auto & index : mwm.second)
    {
      FeatureTypeInfo const & fti = index.second;
      // Temporary solution because of FeatureType does not have constant getters.
      // TODO(a): Use constant reference instead of copy.
      auto feature = fti.m_feature;
      // TODO: Do we really need to serialize deleted features in full details? Looks like mwm ID
      // and meta fields are enough.
      XMLFeature xf =
          editor::ToXML(feature, true /*type serializing helps during migration*/);
      xf.SetMWMFeatureIndex(index.first);
      if (!fti.m_street.empty())
        xf.SetTagValue(kAddrStreetTag, fti.m_street);
      ASSERT_NOT_EQUAL(0, fti.m_modificationTimestamp, ());
      xf.SetModificationTime(fti.m_modificationTimestamp);
      if (fti.m_uploadAttemptTimestamp != base::INVALID_TIME_STAMP)
      {
        xf.SetUploadTime(fti.m_uploadAttemptTimestamp);
        ASSERT(!fti.m_uploadStatus.empty(), ("Upload status updates with upload timestamp."));
        xf.SetUploadStatus(fti.m_uploadStatus);
        if (!fti.m_uploadError.empty())
          xf.SetUploadError(fti.m_uploadError);
      }
      switch (fti.m_status)
      {
      case FeatureStatus::Deleted: VERIFY(xf.AttachToParentNode(deleted), ()); break;
      case FeatureStatus::Modified: VERIFY(xf.AttachToParentNode(modified), ()); break;
      case FeatureStatus::Created: VERIFY(xf.AttachToParentNode(created), ()); break;
      case FeatureStatus::Obsolete: VERIFY(xf.AttachToParentNode(obsolete), ()); break;
      case FeatureStatus::Untouched: CHECK(false, ("Not edited features shouldn't be here."));
      }
    }
  }

  return m_storage->Save(doc);
}

bool Editor::SaveTransaction(shared_ptr<FeaturesContainer> const & features)
{
  if (!Save(*features))
    return false;

  m_features.Set(features);
  return true;
}

void Editor::ClearAllLocalEdits()
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  SaveTransaction(make_shared<FeaturesContainer>());
  Invalidate();
}

void Editor::OnMapDeregistered(platform::LocalCountryFile const & localFile)
{
  // Can be called on non-main-thread in case of simultaneous uploading process.
  GetPlatform().RunTask(Platform::Thread::Gui, [this, localFile]
  {
    using FeaturePair = FeaturesContainer::value_type;

    auto const findMwm = [](FeaturesContainer const & src, platform::LocalCountryFile const & localFile)
    {
      // Cannot search by MwmId because country already removed. So, search by country name.
      return find_if(cbegin(src), cend(src), [&localFile](FeaturePair const & item) {
        return item.first.GetInfo()->GetCountryName() == localFile.GetCountryName();
      });
    };

    auto const features = m_features.Get();
    auto const matchedMwm = findMwm(*features, localFile);

    if (features->cend() != matchedMwm)
    {
      auto editableFeatures = make_shared<FeaturesContainer>(*features);
      editableFeatures->erase(findMwm(*editableFeatures, localFile));

      SaveTransaction(editableFeatures);
    }
  });
}

FeatureStatus Editor::GetFeatureStatus(MwmSet::MwmId const & mwmId, uint32_t index) const
{
  auto const features = m_features.Get();
  return GetFeatureStatusImpl(*features, mwmId, index);
}

FeatureStatus Editor::GetFeatureStatus(FeatureID const & fid) const
{
  auto const features = m_features.Get();
  return GetFeatureStatusImpl(*features, fid.m_mwmId, fid.m_index);
}

bool Editor::IsFeatureUploaded(MwmSet::MwmId const & mwmId, uint32_t index) const
{
  auto const features = m_features.Get();
  return IsFeatureUploadedImpl(*features, mwmId, index);
}

void Editor::DeleteFeature(FeatureID const & fid)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  auto const features = m_features.Get();
  auto editableFeatures = make_shared<FeaturesContainer>(*features);

  auto const mwm = editableFeatures->find(fid.m_mwmId);

  if (mwm != editableFeatures->end())
  {
    auto const f = mwm->second.find(fid.m_index);
    // Created feature is deleted by removing all traces of it.
    if (f != mwm->second.end() && f->second.m_status == FeatureStatus::Created)
    {
      mwm->second.erase(f);
      SaveTransaction(editableFeatures);
      return;
    }
  }

  MarkFeatureWithStatus(*editableFeatures, fid, FeatureStatus::Deleted);
  SaveTransaction(editableFeatures);
  Invalidate();
}

bool Editor::IsCreatedFeature(FeatureID const & fid)
{
  return feature::FakeFeatureIds::IsEditorCreatedFeature(fid.m_index);
}

/// Several cases should be handled while saving changes:
/// 1) a feature is not in editor's cache
///   I. a feature was created
///      save it and mark as `Created'
///   II. a feature was modified
///      save it and mark as `Modified'
/// 2) a feature is in editor's cache
///   I. a feature was created
///      save it and mark as `Created'
///   II. a feature was modified and equals to the one in cache
///      ignore it
///   III. a feature was modified and equals to the one in mwm
///      either delete it or save and mark as `Modified' depending on upload status
Editor::SaveResult Editor::SaveEditedFeature(EditableMapObject const & emo)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  FeatureID const & fid = emo.GetID();
  FeatureTypeInfo fti;

  auto const features = m_features.Get();

  auto const featureStatus = GetFeatureStatusImpl(*features, fid.m_mwmId, fid.m_index);
  ASSERT_NOT_EQUAL(featureStatus, FeatureStatus::Obsolete, ("Obsolete feature cannot be modified."));
  ASSERT_NOT_EQUAL(featureStatus, FeatureStatus::Deleted, ("Unexpected feature status."));

  bool const wasCreatedByUser = IsCreatedFeature(fid);
  if (wasCreatedByUser)
  {
    fti.m_status = FeatureStatus::Created;
    fti.m_feature.ReplaceBy(emo);

    if (featureStatus == FeatureStatus::Created)
    {
      auto const & editedFeatureInfo = features->at(fid.m_mwmId).at(fid.m_index);
      // Temporary solution because of FeatureType does not have constant getters.
      // TODO(a): Use constant reference instead of copy.
      auto feature = editedFeatureInfo.m_feature;
      if (AreFeaturesEqualButStreet(fti.m_feature, feature) &&
          emo.GetStreet().m_defaultName == editedFeatureInfo.m_street)
      {
        return SaveResult::NothingWasChanged;
      }
    }
  }
  else
  {
    auto const originalFeaturePtr = GetOriginalFeature(fid);
    if (!originalFeaturePtr)
    {
      LOG(LERROR, ("A feature with id", fid, "cannot be loaded."));
      alohalytics::LogEvent("Editor_MissingFeature_Error");
      return SaveResult::SavingError;
    }

    fti.m_feature = featureStatus == FeatureStatus::Untouched
                        ? *originalFeaturePtr
                        : features->at(fid.m_mwmId).at(fid.m_index).m_feature;
    fti.m_feature.ReplaceBy(emo);
    bool const sameAsInMWM =
        AreFeaturesEqualButStreet(fti.m_feature, *originalFeaturePtr) &&
        emo.GetStreet().m_defaultName == GetOriginalFeatureStreet(fti.m_feature);

    if (featureStatus != FeatureStatus::Untouched)
    {
      // A feature was modified and equals to the one in editor.
      auto const & editedFeatureInfo = features->at(fid.m_mwmId).at(fid.m_index);
      // Temporary solution because of FeatureType does not have constant getters.
      // TODO(a): Use constant reference instead of copy.
      auto feature = editedFeatureInfo.m_feature;
      if (AreFeaturesEqualButStreet(fti.m_feature, feature) &&
          emo.GetStreet().m_defaultName == editedFeatureInfo.m_street)
      {
        return SaveResult::NothingWasChanged;
      }

      // A feature was modified and equals to the one in mwm (changes are rolled back).
      if (sameAsInMWM)
      {
        // Feature was not yet uploaded. Since it's equal to one mwm we can remove changes.
        if (editedFeatureInfo.m_uploadStatus != kUploaded)
        {
          if (!RemoveFeature(fid))
            return SaveResult::SavingError;

          return SaveResult::SavedSuccessfully;
        }
      }

      // If a feature is not the same as in mwm or it was uploaded
      // we must save it and mark for upload.
    }
    // A feature was NOT edited before and current changes are useless.
    else if (sameAsInMWM)
    {
      return SaveResult::NothingWasChanged;
    }

    fti.m_status = FeatureStatus::Modified;
  }

  // TODO: What if local client time is absolutely wrong?
  fti.m_modificationTimestamp = time(nullptr);
  fti.m_street = emo.GetStreet().m_defaultName;

  // Reset upload status so already uploaded features can be uploaded again after modification.
  fti.m_uploadStatus = {};

  auto editableFeatures = make_shared<FeaturesContainer>(*features);
  (*editableFeatures)[fid.m_mwmId][fid.m_index] = move(fti);

  bool const savedSuccessfully = SaveTransaction(editableFeatures);

  Invalidate();
  return savedSuccessfully ? SaveResult::SavedSuccessfully : SaveResult::NoFreeSpaceError;
}

bool Editor::RollBackChanges(FeatureID const & fid)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  if (IsFeatureUploaded(fid.m_mwmId, fid.m_index))
    return false;

  return RemoveFeature(fid);
}

void Editor::ForEachCreatedFeature(MwmSet::MwmId const & id, FeatureIndexFunctor const & f,
                                   m2::RectD const & rect, int /*scale*/) const
{
  auto const features = m_features.Get();

  auto const mwmFound = features->find(id);
  if (mwmFound == features->cend())
    return;

  // Process only new (created) features.
  for (auto const & index : mwmFound->second)
  {
    FeatureTypeInfo const & ftInfo = index.second;
    if (ftInfo.m_status == FeatureStatus::Created)
    {
      // Temporary solution because of FeatureType does not have constant getters.
      // TODO(a): Use constant reference instead of copy.
      auto feature = ftInfo.m_feature;
      if (rect.IsPointInside(feature.GetCenter()))
        f(index.first);
    }
  }
}

bool Editor::GetEditedFeature(MwmSet::MwmId const & mwmId, uint32_t index,
                              FeatureType & outFeature) const
{
  auto const features = m_features.Get();
  auto const * featureInfo = GetFeatureTypeInfo(*features, mwmId, index);
  if (featureInfo == nullptr)
    return false;

  outFeature = featureInfo->m_feature;
  return true;
}

bool Editor::GetEditedFeature(FeatureID const & fid, FeatureType & outFeature) const
{
  return GetEditedFeature(fid.m_mwmId, fid.m_index, outFeature);
}

bool Editor::GetEditedFeatureStreet(FeatureID const & fid, string & outFeatureStreet) const
{
  auto const features = m_features.Get();
  auto const * featureInfo = GetFeatureTypeInfo(*features, fid.m_mwmId, fid.m_index);
  if (featureInfo == nullptr)
    return false;

  outFeatureStreet = featureInfo->m_street;
  return true;
}

vector<uint32_t> Editor::GetFeaturesByStatus(MwmSet::MwmId const & mwmId,
                                             FeatureStatus status) const
{
  auto const features = m_features.Get();

  vector<uint32_t> result;
  auto const matchedMwm = features->find(mwmId);
  if (matchedMwm == features->cend())
    return result;

  for (auto const & index : matchedMwm->second)
  {
    if (index.second.m_status == status)
      result.push_back(index.first);
  }
  sort(result.begin(), result.end());
  return result;
}

EditableProperties Editor::GetEditableProperties(FeatureType & feature) const
{
  auto const features = m_features.Get();

  auto const & fid = feature.GetID();
  auto const featureStatus = GetFeatureStatusImpl(*features, fid.m_mwmId, fid.m_index);

  ASSERT(version::IsSingleMwm(feature.GetID().m_mwmId.GetInfo()->m_version.GetVersion()),
         ("Edit mode should be available only on new data"));
  ASSERT(featureStatus != FeatureStatus::Obsolete,
         ("Edit mode should not be available on obsolete features"));

  // TODO(mgsergio): Check if feature is in the area where editing is disabled in the config.
  auto editableProperties = GetEditablePropertiesForTypes(feature::TypesHolder(feature));

  // Disable opening hours editing if opening hours cannot be parsed.
  if (featureStatus != FeatureStatus::Created)
  {
    auto const originalFeaturePtr = GetOriginalFeature(fid);
    if (!originalFeaturePtr)
    {
      LOG(LERROR, ("A feature with id", fid, "cannot be loaded."));
      alohalytics::LogEvent("Editor_MissingFeature_Error");
      return {};
    }

    auto const & metadata = originalFeaturePtr->GetMetadata();
    auto const & featureOpeningHours = metadata.Get(feature::Metadata::FMD_OPEN_HOURS);
    // Note: empty string is parsed as a valid opening hours rule.
    if (!osmoh::OpeningHours(featureOpeningHours).IsValid())
    {
      auto & meta = editableProperties.m_metadata;
      auto const toBeRemoved = remove(begin(meta), end(meta), feature::Metadata::FMD_OPEN_HOURS);
      if (toBeRemoved != end(meta))
        meta.erase(toBeRemoved);
    }
  }

  return editableProperties;
}

EditableProperties Editor::GetEditablePropertiesForTypes(feature::TypesHolder const & types) const
{
  editor::TypeAggregatedDescription desc;
  if (m_config.Get()->GetTypeDescription(types.ToObjectNames(), desc))
    return {desc.GetEditableFields(), desc.IsNameEditable(), desc.IsAddressEditable()};
  return {};
}

bool Editor::HaveMapEditsOrNotesToUpload() const
{
  if (m_notes->NotUploadedNotesCount() != 0)
    return true;

  auto const features = m_features.Get();
  return HaveMapEditsToUpload(*features);
}

bool Editor::HaveMapEditsToUpload(MwmSet::MwmId const & mwmId) const
{
  auto const features = m_features.Get();

  auto const found = features->find(mwmId);
  if (found != features->cend())
  {
    for (auto const & index : found->second)
    {
      if (NeedsUpload(index.second.m_uploadStatus))
        return true;
    }
  }
  return false;
}

void Editor::UploadChanges(string const & key, string const & secret, ChangesetTags tags,
                           FinishUploadCallback callback)
{
  if (m_notes->NotUploadedNotesCount())
  {
    alohalytics::LogEvent("Editor_UploadNotes", strings::to_string(m_notes->NotUploadedNotesCount()));
    m_notes->Upload(OsmOAuth::ServerAuth({key, secret}));
  }

  auto const features = m_features.Get();

  if (!HaveMapEditsToUpload(*features))
  {
    LOG(LDEBUG, ("There are no local edits to upload."));
    return;
  }

  alohalytics::LogEvent("Editor_DataSync_started");

  auto upload = [this](string key, string secret, ChangesetTags tags, FinishUploadCallback callback)
  {
    int uploadedFeaturesCount = 0, errorsCount = 0;
    ChangesetWrapper changeset({key, secret}, tags);
    auto const features = m_features.Get();

    for (auto const & id : *features)
    {
      for (auto const & index : id.second)
      {
        FeatureTypeInfo const & fti = index.second;
        // Do not process already uploaded features or those failed permanently.
        if (!NeedsUpload(fti.m_uploadStatus))
          continue;

        // TODO(a): Use UploadInfo as part of FeatureTypeInfo.
        UploadInfo uploadInfo = {fti.m_uploadAttemptTimestamp, fti.m_uploadStatus, fti.m_uploadError};
        // Temporary solution because of FeatureType does not have constant getters.
        // TODO(a): Use constant reference instead of copy.
        auto featureData = fti.m_feature;
        string ourDebugFeatureString;

        try
        {
          switch (fti.m_status)
          {
          case FeatureStatus::Untouched: CHECK(false, ("It's impossible.")); continue;
          case FeatureStatus::Obsolete: continue;  // Obsolete features will be deleted by OSMers.
          case FeatureStatus::Created:
          {
            XMLFeature feature = editor::ToXML(featureData, true);
            if (!fti.m_street.empty())
              feature.SetTagValue(kAddrStreetTag, fti.m_street);
            ourDebugFeatureString = DebugPrint(feature);

            ASSERT_EQUAL(feature.GetType(), XMLFeature::Type::Node,
                         ("Linear and area features creation is not supported yet."));
            try
            {
              auto const center = featureData.GetCenter();

              XMLFeature osmFeature = changeset.GetMatchingNodeFeatureFromOSM(center);
              // If we are here, it means that object already exists at the given point.
              // To avoid nodes duplication, merge and apply changes to it instead of creating an new one.
              XMLFeature const osmFeatureCopy = osmFeature;
              osmFeature.ApplyPatch(feature);
              // Check to avoid uploading duplicates into OSM.
              if (osmFeature == osmFeatureCopy)
              {
                LOG(LWARNING, ("Local changes are equal to OSM, feature has not been uploaded.", osmFeatureCopy));
                // Don't delete this local change right now for user to see it in profile.
                // It will be automatically deleted by migration code on the next maps update.
              }
              else
              {
                LOG(LDEBUG, ("Create case: uploading patched feature", osmFeature));
                changeset.Modify(osmFeature);
              }
            }
            catch (ChangesetWrapper::OsmObjectWasDeletedException const &)
            {
              // Object was never created by anyone else - it's safe to create it.
              changeset.Create(feature);
            }
            catch (ChangesetWrapper::EmptyFeatureException const &)
            {
              // There is another node nearby, but it should be safe to create a new one.
              changeset.Create(feature);
            }
            catch (...)
            {
              // Pass network or other errors to outside exception handler.
              throw;
            }
          }
          break;

          case FeatureStatus::Modified:
          {
            // Do not serialize feature's type to avoid breaking OSM data.
            // TODO: Implement correct types matching when we support modifying existing feature types.
            XMLFeature feature = editor::ToXML(featureData, false);
            if (!fti.m_street.empty())
              feature.SetTagValue(kAddrStreetTag, fti.m_street);
            ourDebugFeatureString = DebugPrint(feature);

            auto const originalFeaturePtr = GetOriginalFeature(fti.m_feature.GetID());
            if (!originalFeaturePtr)
            {
              LOG(LERROR, ("A feature with id", fti.m_feature.GetID(), "cannot be loaded."));
              alohalytics::LogEvent("Editor_MissingFeature_Error");
              GetPlatform().RunTask(Platform::Thread::Gui, [this, fid = fti.m_feature.GetID()]()
              {
                RemoveFeatureIfExists(fid);
              });
              continue;
            }

            XMLFeature osmFeature = GetMatchingFeatureFromOSM(changeset, *originalFeaturePtr);
            XMLFeature const osmFeatureCopy = osmFeature;
            osmFeature.ApplyPatch(feature);
            // Check to avoid uploading duplicates into OSM.
            if (osmFeature == osmFeatureCopy)
            {
              LOG(LWARNING, ("Local changes are equal to OSM, feature has not been uploaded.", osmFeatureCopy));
              // Don't delete this local change right now for user to see it in profile.
              // It will be automatically deleted by migration code on the next maps update.
            }
            else
            {
              LOG(LDEBUG, ("Uploading patched feature", osmFeature));
              changeset.Modify(osmFeature);
            }
          }
          break;

          case FeatureStatus::Deleted:
            auto const originalFeaturePtr = GetOriginalFeature(fti.m_feature.GetID());
            if (!originalFeaturePtr)
            {
              LOG(LERROR, ("A feature with id", fti.m_feature.GetID(), "cannot be loaded."));
              alohalytics::LogEvent("Editor_MissingFeature_Error");
              GetPlatform().RunTask(Platform::Thread::Gui, [this, fid = fti.m_feature.GetID()]()
              {
                RemoveFeatureIfExists(fid);
              });
              continue;
            }
            changeset.Delete(GetMatchingFeatureFromOSM(changeset, *originalFeaturePtr));
            break;
          }
          uploadInfo.m_uploadStatus = kUploaded;
          uploadInfo.m_uploadError.clear();
          ++uploadedFeaturesCount;
        }
        catch (ChangesetWrapper::OsmObjectWasDeletedException const & ex)
        {
          uploadInfo.m_uploadStatus = kDeletedFromOSMServer;
          uploadInfo.m_uploadError = ex.Msg();
          ++errorsCount;
          LOG(LWARNING, (ex.what()));
        }
        catch (ChangesetWrapper::EmptyFeatureException const & ex)
        {
          uploadInfo.m_uploadStatus = kMatchedFeatureIsEmpty;
          uploadInfo.m_uploadError = ex.Msg();
          ++errorsCount;
          LOG(LWARNING, (ex.what()));
        }
        catch (RootException const & ex)
        {
          uploadInfo.m_uploadStatus = kNeedsRetry;
          uploadInfo.m_uploadError = ex.Msg();
          ++errorsCount;
          LOG(LWARNING, (ex.what()));
        }
        // TODO(AlexZ): Use timestamp from the server.
        uploadInfo.m_uploadAttemptTimestamp = time(nullptr);

        if (uploadInfo.m_uploadStatus != kUploaded)
        {
          ms::LatLon const ll = MercatorBounds::ToLatLon(feature::GetCenter(featureData));
          alohalytics::LogEvent(
              "Editor_DataSync_error",
              {{"type", fti.m_uploadStatus},
               {"details", fti.m_uploadError},
               {"our", ourDebugFeatureString},
               {"mwm", fti.m_feature.GetID().GetMwmName()},
               {"mwm_version", strings::to_string(fti.m_feature.GetID().GetMwmVersion())}},
              alohalytics::Location::FromLatLon(ll.lat, ll.lon));
        }

        GetPlatform().RunTask(Platform::Thread::Gui, [this, id = fti.m_feature.GetID(), uploadInfo]()
        {
          // Call Save every time we modify each feature's information.
          SaveUploadedInformation(id, uploadInfo);
        });
      }
    }

    alohalytics::LogEvent("Editor_DataSync_finished",
                          {{"errors", strings::to_string(errorsCount)},
                           {"uploaded", strings::to_string(uploadedFeaturesCount)},
                           {"changeset", strings::to_string(changeset.GetChangesetId())}});
    if (callback)
    {
      UploadResult result = UploadResult::NothingToUpload;
      if (uploadedFeaturesCount)
        result = UploadResult::Success;
      else if (errorsCount)
        result = UploadResult::Error;
      callback(result);
    }

    m_isUploadingNow = false;
  };

  // Do not run more than one uploading task at a time.
  if (!m_isUploadingNow)
  {
    m_isUploadingNow = true;
    GetPlatform().RunTask(Platform::Thread::Network, [upload = std::move(upload), key, secret,
                                                      tags = std::move(tags), callback = std::move(callback)]()
    {
      upload(std::move(key), std::move(secret), std::move(tags), std::move(callback));
    });
  }
}

void Editor::SaveUploadedInformation(FeatureID const & fid, UploadInfo const & uploadInfo)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  auto const features = m_features.Get();
  auto editableFeatures = make_shared<FeaturesContainer>(*features);

  auto id = editableFeatures->find(fid.m_mwmId);
  // Rare case: feature was deleted at the time of changes uploading.
  if (id == editableFeatures->end())
    return;

  auto index = id->second.find(fid.m_index);
  // Rare case: feature was deleted at the time of changes uploading.
  if (index == id->second.end())
    return;

  auto & fti = index->second;
  fti.m_uploadAttemptTimestamp = uploadInfo.m_uploadAttemptTimestamp;
  fti.m_uploadStatus = uploadInfo.m_uploadStatus;
  fti.m_uploadError = uploadInfo.m_uploadError;

  SaveTransaction(editableFeatures);
}

bool Editor::FillFeatureInfo(FeatureStatus status, XMLFeature const & xml, FeatureID const & fid,
                             FeatureTypeInfo & fti) const
{
  if (status == FeatureStatus::Created)
  {
    editor::FromXML(xml, fti.m_feature);
  }
  else
  {
    auto const originalFeaturePtr = GetOriginalFeature(fid);
    if (!originalFeaturePtr)
    {
      LOG(LERROR, ("A feature with id", fid, "cannot be loaded."));
      alohalytics::LogEvent("Editor_MissingFeature_Error");
      return false;
    }

    fti.m_feature = *originalFeaturePtr;
    editor::ApplyPatch(xml, fti.m_feature);
  }

  fti.m_feature.SetID(fid);
  fti.m_street = xml.GetTagValue(kAddrStreetTag);

  fti.m_modificationTimestamp = xml.GetModificationTime();
  ASSERT_NOT_EQUAL(base::INVALID_TIME_STAMP, fti.m_modificationTimestamp, ());
  fti.m_uploadAttemptTimestamp = xml.GetUploadTime();
  fti.m_uploadStatus = xml.GetUploadStatus();
  fti.m_uploadError = xml.GetUploadError();
  fti.m_status = status;

  return true;
}

Editor::FeatureTypeInfo const * Editor::GetFeatureTypeInfo(FeaturesContainer const & features,
                                                           MwmSet::MwmId const & mwmId,
                                                           uint32_t index) const
{
  auto const matchedMwm = features.find(mwmId);
  if (matchedMwm == features.cend())
    return nullptr;

  auto const matchedIndex = matchedMwm->second.find(index);
  if (matchedIndex == matchedMwm->second.cend())
    return nullptr;

  /* TODO(AlexZ): Should we process deleted/created features as well?*/
  return &matchedIndex->second;
}

bool Editor::RemoveFeatureIfExists(FeatureID const & fid)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  auto const features = m_features.Get();
  auto editableFeatures = make_shared<FeaturesContainer>(*features);

  auto matchedMwm = editableFeatures->find(fid.m_mwmId);
  if (matchedMwm == editableFeatures->end())
    return true;

  auto matchedIndex = matchedMwm->second.find(fid.m_index);
  if (matchedIndex != matchedMwm->second.end())
    matchedMwm->second.erase(matchedIndex);

  if (matchedMwm->second.empty())
    editableFeatures->erase(matchedMwm);

  return SaveTransaction(editableFeatures);
}

void Editor::Invalidate()
{
  if (m_invalidateFn)
    m_invalidateFn();
}

bool Editor::MarkFeatureAsObsolete(FeatureID const & fid)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  auto const features = m_features.Get();
  auto editableFeatures = make_shared<FeaturesContainer>(*features);

  auto const featureStatus = GetFeatureStatusImpl(*editableFeatures, fid.m_mwmId, fid.m_index);
  if (featureStatus != FeatureStatus::Untouched && featureStatus != FeatureStatus::Modified)
  {
    ASSERT(false, ("Only untouched and modified features can be made obsolete"));
    return false;
  }

  MarkFeatureWithStatus(*editableFeatures, fid, FeatureStatus::Obsolete);
  auto const result = SaveTransaction(editableFeatures);
  Invalidate();

  return result;
}

bool Editor::RemoveFeature(FeatureID const & fid)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  auto const result = RemoveFeatureIfExists(fid);

  if (result)
    Invalidate();

  return result;
}

Editor::Stats Editor::GetStats() const
{
  Stats stats;
  LOG(LDEBUG, ("Edited features status:"));

  auto const features = m_features.Get();
  for (auto const & id : *features)
  {
    for (auto & index : id.second)
    {
      auto const & fti = index.second;
      // Temporary solution because of FeatureType does not have constant getters.
      // TODO(a): Use constant reference instead of copy.
      auto feature = fti.m_feature;
      stats.m_edits.push_back(make_pair(FeatureID(id.first, index.first),
                                        fti.m_uploadStatus + " " + fti.m_uploadError));
      LOG(LDEBUG, (fti.m_uploadAttemptTimestamp == base::INVALID_TIME_STAMP
                       ? "NOT_UPLOADED_YET"
                       : base::TimestampToString(fti.m_uploadAttemptTimestamp),
                   fti.m_uploadStatus, fti.m_uploadError, feature.GetFeatureType(),
                   feature::GetCenter(feature)));
      if (fti.m_uploadStatus == kUploaded)
      {
        ++stats.m_uploadedCount;
        if (stats.m_lastUploadTimestamp < fti.m_uploadAttemptTimestamp)
          stats.m_lastUploadTimestamp = fti.m_uploadAttemptTimestamp;
      }
    }
  }
  return stats;
}

NewFeatureCategories Editor::GetNewFeatureCategories() const
{
  return NewFeatureCategories(*(m_config.Get()));
}

FeatureID Editor::GenerateNewFeatureId(FeaturesContainer const & features,
                                       MwmSet::MwmId const & id) const
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  uint32_t featureIndex = feature::FakeFeatureIds::kEditorCreatedFeaturesStart;

  auto const found = features.find(id);
  if (found != features.cend())
  {
    // Scan all already created features and choose next available ID.
    for (auto const & feature : found->second)
    {
      if (feature.second.m_status == FeatureStatus::Created && featureIndex <= feature.first)
        featureIndex = feature.first + 1;
    }
  }
  CHECK(feature::FakeFeatureIds::IsEditorCreatedFeature(featureIndex), ());
  return FeatureID(id, featureIndex);
}

bool Editor::CreatePoint(uint32_t type, m2::PointD const & mercator, MwmSet::MwmId const & id,
                         EditableMapObject & outFeature) const
{
  ASSERT(id.IsAlive(),
         ("Please check that feature is created in valid MWM file before calling this method."));
  if (!id.GetInfo()->m_bordersRect.IsPointInside(mercator))
  {
    LOG(LERROR, ("Attempt to create a feature outside of the MWM's bounding box."));
    return false;
  }

  outFeature.SetMercator(mercator);
  outFeature.SetID(GenerateNewFeatureId(*(m_features.Get()), id));
  outFeature.SetType(type);
  outFeature.SetEditableProperties(GetEditablePropertiesForTypes(outFeature.GetTypes()));
  // Only point type features can be created at the moment.
  outFeature.SetPointType();
  return true;
}

void Editor::CreateNote(ms::LatLon const & latLon, FeatureID const & fid,
                        feature::TypesHolder const & holder, string const & defaultName,
                        NoteProblemType const type, string const & note)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  auto const version = GetMwmCreationTimeByMwmId(fid.m_mwmId);
  auto const stringVersion = base::TimestampToString(base::SecondsSinceEpochToTimeT(version));
  ostringstream sstr;
  auto canCreate = true;

  if (!note.empty())
    sstr << "\"" << note << "\"" << endl;

  switch (type)
  {
  case NoteProblemType::PlaceDoesNotExist:
  {
    sstr << kPlaceDoesNotExistMessage << endl;
    auto const features = m_features.Get();
    auto const isCreated =
        GetFeatureStatusImpl(*features, fid.m_mwmId, fid.m_index) == FeatureStatus::Created;
    auto const createdAndUploaded =
        (isCreated && IsFeatureUploadedImpl(*features, fid.m_mwmId, fid.m_index));
    CHECK(!isCreated || createdAndUploaded, ());

    if (createdAndUploaded)
      canCreate = RemoveFeature(fid);
    else
      canCreate = MarkFeatureAsObsolete(fid);

    break;
  }
  case NoteProblemType::General: break;
  }

  if (!canCreate)
    return;

  if (defaultName.empty())
    sstr << "POI has no name" << endl;
  else
    sstr << "POI name: " << defaultName << endl;

  sstr << "POI types:";
  for (auto const & type : holder.ToObjectNames())
  {
    sstr << ' ' << type;
  }
  sstr << endl;

  sstr << "OSM data version: " << stringVersion << endl;

  m_notes->CreateNote(latLon, sstr.str());
}

void Editor::MarkFeatureWithStatus(FeaturesContainer & editableFeatures, FeatureID const & fid,
                                   FeatureStatus status)
{
  CHECK_THREAD_CHECKER(MainThreadChecker, (""));

  auto & fti = editableFeatures[fid.m_mwmId][fid.m_index];

  auto const originalFeaturePtr = GetOriginalFeature(fid);

  if (!originalFeaturePtr)
  {
    LOG(LERROR, ("A feature with id", fid, "cannot be loaded."));
    alohalytics::LogEvent("Editor_MissingFeature_Error");
    return;
  }

  fti.m_feature = *originalFeaturePtr;
  fti.m_status = status;
  fti.m_modificationTimestamp = time(nullptr);
}

MwmSet::MwmId Editor::GetMwmIdByMapName(string const & name)
{
  if (!m_delegate)
  {
    LOG(LERROR, ("Can't get mwm id by map name:", name, ", delegate is not set."));
    return {};
  }
  return m_delegate->GetMwmIdByMapName(name);
}

unique_ptr<FeatureType> Editor::GetOriginalFeature(FeatureID const & fid) const
{
  if (!m_delegate)
  {
    LOG(LERROR, ("Can't get original feature by id:", fid, ", delegate is not set."));
    return {};
  }
  return m_delegate->GetOriginalFeature(fid);
}

string Editor::GetOriginalFeatureStreet(FeatureType & ft) const
{
  if (!m_delegate)
  {
    LOG(LERROR, ("Can't get feature street, delegate is not set."));
    return {};
  }
  return m_delegate->GetOriginalFeatureStreet(ft);
}

void Editor::ForEachFeatureAtPoint(FeatureTypeFn && fn, m2::PointD const & point) const
{
  if (!m_delegate)
  {
    LOG(LERROR, ("Can't load features in point's vicinity, delegate is not set."));
    return;
  }
  m_delegate->ForEachFeatureAtPoint(move(fn), point);
}

FeatureID Editor::GetFeatureIdByXmlFeature(FeaturesContainer const & features,
                                           XMLFeature const & xml, MwmSet::MwmId const & mwmId,
                                           FeatureStatus status, bool needMigrate) const
{
  ForEachFeaturesNearByFn forEach = [this](FeatureTypeFn && fn, m2::PointD const & point) {
    return ForEachFeatureAtPoint(move(fn), point);
  };

  // TODO(mgsergio): Deleted features are not properly handled yet.
  if (needMigrate)
  {
    return editor::MigrateFeatureIndex(forEach, xml, status, [this, &mwmId, &features] {
      return GenerateNewFeatureId(features, mwmId);
    });
  }

  return FeatureID(mwmId, xml.GetMWMFeatureIndex());
}

void Editor::LoadMwmEdits(FeaturesContainer & loadedFeatures, xml_node const & mwm,
                          MwmSet::MwmId const & mwmId, bool needMigrate)
{
  LogHelper logHelper(mwmId);

  for (auto const & section : kXmlSections)
  {
    for (auto const & nodeOrWay : mwm.child(section.m_sectionName.c_str()).select_nodes("node|way"))
    {
      try
      {
        XMLFeature const xml(nodeOrWay.node());

        auto const fid =
            GetFeatureIdByXmlFeature(loadedFeatures, xml, mwmId, section.m_status, needMigrate);

        // Remove obsolete changes during migration.
        if (needMigrate && IsObsolete(xml, fid))
          continue;

        FeatureTypeInfo fti;
        if (!FillFeatureInfo(section.m_status, xml, fid, fti))
          continue;

        logHelper.OnStatus(section.m_status);

        loadedFeatures[fid.m_mwmId].emplace(fid.m_index, move(fti));
      }
      catch (editor::XMLFeatureError const & ex)
      {
        ostringstream s;
        nodeOrWay.node().print(s, "  ");
        LOG(LERROR, (ex.what(), "mwmId =", mwmId, "in section", section.m_sectionName, s.str()));
      }
      catch (editor::MigrationError const & ex)
      {
        LOG(LWARNING, (ex.what(), "mwmId =", mwmId, XMLFeature(nodeOrWay.node())));
      }
    }
  }
}

bool Editor::HaveMapEditsToUpload(FeaturesContainer const & features) const
{
  for (auto const & id : features)
  {
    for (auto const & index : id.second)
    {
      if (NeedsUpload(index.second.m_uploadStatus))
        return true;
    }
  }
  return false;
}

FeatureStatus Editor::GetFeatureStatusImpl(FeaturesContainer const & features,
                                           MwmSet::MwmId const & mwmId, uint32_t index) const
{
  // Most popular case optimization.
  if (features.empty())
    return FeatureStatus::Untouched;

  auto const * featureInfo = GetFeatureTypeInfo(features, mwmId, index);
  if (featureInfo == nullptr)
    return FeatureStatus::Untouched;

  return featureInfo->m_status;
}

bool Editor::IsFeatureUploadedImpl(FeaturesContainer const & features, MwmSet::MwmId const & mwmId,
                                   uint32_t index) const
{
  auto const * info = GetFeatureTypeInfo(features, mwmId, index);
  return info && info->m_uploadStatus == kUploaded;
}

const char * const Editor::kPlaceDoesNotExistMessage =
    "The place has gone or never existed. This is an auto-generated note from MAPS.ME (AddisMapME) application: "
    "a user reports a POI that is visible on a map (which can be outdated), but cannot be found on "
    "the ground.";

string DebugPrint(Editor::SaveResult const saveResult)
{
  switch (saveResult)
  {
    case Editor::SaveResult::NothingWasChanged: return "NothingWasChanged";
    case Editor::SaveResult::SavedSuccessfully: return "SavedSuccessfully";
    case Editor::SaveResult::NoFreeSpaceError: return "NoFreeSpaceError";
    case Editor::SaveResult::NoUnderlyingMapError: return "NoUnderlyingMapError";
    case Editor::SaveResult::SavingError: return "SavingError";
  }
}
}  // namespace osm
