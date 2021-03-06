project(generator_tests)

set(
  SRC
  altitude_test.cpp
  check_mwms.cpp
  coasts_test.cpp
  feature_builder_test.cpp
  feature_merger_test.cpp
  metadata_parser_test.cpp
  node_mixer_test.cpp
  osm2meta_test.cpp
  osm_id_test.cpp
  osm_o5m_source_test.cpp
  osm_type_test.cpp
  road_access_test.cpp
  restriction_collector_test.cpp
  restriction_test.cpp
  source_data.cpp
  source_data.hpp
  source_to_element_test.cpp
  sponsored_storage_tests.cpp
  srtm_parser_test.cpp
  tag_admixer_test.cpp
  tesselator_test.cpp
  triangles_tree_coding_test.cpp
  types_helper.hpp
  ugc_test.cpp
)

omim_add_test(${PROJECT_NAME} ${SRC})

omim_link_libraries(
  ${PROJECT_NAME}
  generator_tests_support
  platform_tests_support
  generator
  drape_frontend
  shaders
  map
  routing
  search
  storage
  drape
  traffic
  routing_common
  transit
  kml
  editor
  indexer
  platform
  geometry
  coding
  base
  freetype
  expat
  icu
  agg
  jansson
  protobuf
  stats_client
  minizip
  succinct
  pugixml
  tess2
  gflags
  oauthcpp
  opening_hours
  stb_image
  sdf_image
  # TODO(syershov): Use FindPackage.
  sqlite3
  ${LIBZ}
)

link_qt5_core(${PROJECT_NAME})
