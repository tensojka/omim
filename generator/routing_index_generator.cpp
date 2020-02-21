#include "generator/routing_index_generator.hpp"

#include "generator/borders.hpp"
#include "generator/cross_mwm_osm_ways_collector.hpp"
#include "generator/routing_helpers.hpp"

#include "routing/base/astar_algorithm.hpp"
#include "routing/base/astar_graph.hpp"

#include "routing/cross_mwm_connector.hpp"
#include "routing/cross_mwm_connector_serialization.hpp"
#include "routing/cross_mwm_ids.hpp"
#include "routing/fake_feature_ids.hpp"
#include "routing/index_graph.hpp"
#include "routing/index_graph_loader.hpp"
#include "routing/index_graph_serialization.hpp"
#include "routing/index_graph_starter_joints.hpp"
#include "routing/joint_segment.hpp"
#include "routing/vehicle_mask.hpp"

#include "routing_common/bicycle_model.hpp"
#include "routing_common/car_model.hpp"
#include "routing_common/pedestrian_model.hpp"

#include "transit/transit_graph_data.hpp"
#include "transit/transit_serdes.hpp"

#include "indexer/data_header.hpp"
#include "indexer/feature.hpp"
#include "indexer/feature_processor.hpp"

#include "coding/files_container.hpp"
#include "coding/geometry_coding.hpp"
#include "coding/point_coding.hpp"
#include "coding/reader.hpp"

#include "geometry/point2d.hpp"

#include "base/checked_cast.hpp"
#include "base/file_name_utils.hpp"
#include "base/geo_object_id.hpp"
#include "base/logging.hpp"
#include "base/timer.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace feature;
using namespace platform;
using namespace routing;
using namespace std;
using namespace std::placeholders;

namespace
{
class VehicleMaskBuilder final
{
public:
  VehicleMaskBuilder(string const & country,
                              CountryParentNameGetterFn const & countryParentNameGetterFn)
    : m_pedestrianModel(
          PedestrianModelFactory(countryParentNameGetterFn).GetVehicleModelForCountry(country))
    , m_bicycleModel(
          BicycleModelFactory(countryParentNameGetterFn).GetVehicleModelForCountry(country))
    , m_carModel(CarModelFactory(countryParentNameGetterFn).GetVehicleModelForCountry(country))
  {
    CHECK(m_pedestrianModel, ());
    CHECK(m_bicycleModel, ());
    CHECK(m_carModel, ());
  }

  VehicleMask CalcRoadMask(FeatureType & f) const
  {
    return CalcMask(
        f, [&](VehicleModelInterface const & model, FeatureType & f) { return model.IsRoad(f); });
  }

  VehicleMask CalcOneWayMask(FeatureType & f) const
  {
    return CalcMask(
        f, [&](VehicleModelInterface const & model, FeatureType & f) { return model.IsOneWay(f); });
  }

private:
  template <class Fn>
  VehicleMask CalcMask(FeatureType & f, Fn && fn) const
  {
    VehicleMask mask = 0;
    if (fn(*m_pedestrianModel, f))
      mask |= kPedestrianMask;
    if (fn(*m_bicycleModel, f))
      mask |= kBicycleMask;
    if (fn(*m_carModel, f))
      mask |= kCarMask;

    return mask;
  }

  shared_ptr<VehicleModelInterface> const m_pedestrianModel;
  shared_ptr<VehicleModelInterface> const m_bicycleModel;
  shared_ptr<VehicleModelInterface> const m_carModel;
};

class Processor final
{
public:
  Processor(string const & country, CountryParentNameGetterFn const & countryParentNameGetterFn)
    : m_maskBuilder(country, countryParentNameGetterFn)
  {
  }

  void ProcessAllFeatures(string const & filename)
  {
    feature::ForEachFromDat(filename, bind(&Processor::ProcessFeature, this, _1, _2));
  }

  void BuildGraph(IndexGraph & graph) const
  {
    vector<Joint> joints;
    for (auto const & it : m_posToJoint)
    {
      // Need only connected points (2 or more roads)
      if (it.second.GetSize() >= 2)
        joints.emplace_back(it.second);
    }

    graph.Import(joints);
  }

  unordered_map<uint32_t, VehicleMask> const & GetMasks() const { return m_masks; }

private:
  void ProcessFeature(FeatureType & f, uint32_t id)
  {
    VehicleMask const mask = m_maskBuilder.CalcRoadMask(f);
    if (mask == 0)
      return;

    m_masks[id] = mask;
    f.ParseGeometry(FeatureType::BEST_GEOMETRY);

    for (size_t i = 0; i < f.GetPointsCount(); ++i)
    {
      uint64_t const locationKey = PointToInt64Obsolete(f.GetPoint(i), kPointCoordBits);
      m_posToJoint[locationKey].AddPoint(RoadPoint(id, base::checked_cast<uint32_t>(i)));
    }
  }

  VehicleMaskBuilder const m_maskBuilder;
  unordered_map<uint64_t, Joint> m_posToJoint;
  unordered_map<uint32_t, VehicleMask> m_masks;
};

class IndexGraphWrapper final
{
public:
  IndexGraphWrapper(IndexGraph & graph, Segment const & start)
    : m_graph(graph), m_start(start) {}

  // Just for compatibility with IndexGraphStarterJoints
  // @{
  Segment GetStartSegment() const { return m_start; }
  Segment GetFinishSegment() const { return {}; }
  bool ConvertToReal(Segment const & /* segment */) const { return false; }
  RouteWeight HeuristicCostEstimate(Segment const & /* from */, ms::LatLon const & /* to */)
  {
    CHECK(false, ("This method exists only for compatibility with IndexGraphStarterJoints"));
    return GetAStarWeightZero<RouteWeight>();
  }

  bool AreWavesConnectible(IndexGraph::Parents<JointSegment> const & /* forwardParents */,
                           JointSegment const & /* commonVertex */,
                           IndexGraph::Parents<JointSegment> const & /* backwardParents */,
                           function<uint32_t(JointSegment const &)> && /* fakeFeatureConverter */)
  {
    return true;
  }

  void SetAStarParents(bool /* forward */, IndexGraph::Parents<JointSegment> & parents)
  {
    m_AStarParents = &parents;
  }

  void DropAStarParents()
  {
    m_AStarParents = nullptr;
  }

  RouteWeight GetAStarWeightEpsilon() { return RouteWeight(0.0); }
  // @}

  ms::LatLon const & GetPoint(Segment const & s, bool forward)
  {
    return m_graph.GetPoint(s, forward);
  }

  void GetEdgesList(Segment const & child, bool isOutgoing, vector<SegmentEdge> & edges)
  {
    m_graph.GetEdgeList(child, isOutgoing, true /* useRoutingOptions */, edges);
  }

  void GetEdgeList(astar::VertexData<JointSegment, RouteWeight> const & vertexData,
                   Segment const & parent, bool isOutgoing, vector<JointEdge> & edges,
                   vector<RouteWeight> & parentWeights) const
  {
    CHECK(m_AStarParents, ());
    return m_graph.GetEdgeList(vertexData.m_vertex, parent, isOutgoing, edges, parentWeights,
                               *m_AStarParents);
  }

  bool IsJoint(Segment const & segment, bool fromStart) const
  {
    return IsJointOrEnd(segment, fromStart);
  }

  bool IsJointOrEnd(Segment const & segment, bool fromStart) const
  {
    return m_graph.IsJointOrEnd(segment, fromStart);
  }

  template <typename Vertex>
  RouteWeight HeuristicCostEstimate(Vertex const & /* from */, m2::PointD const & /* to */)
  {
    CHECK(false, ("This method should not be use, it is just for compatibility with "
                  "IndexGraphStarterJoints."));

    return GetAStarWeightZero<RouteWeight>();
  }

private:
  IndexGraph::Parents<JointSegment> * m_AStarParents = nullptr;
  IndexGraph & m_graph;
  Segment m_start;
};

class DijkstraWrapperJoints : public IndexGraphStarterJoints<IndexGraphWrapper>
{
public:

  DijkstraWrapperJoints(IndexGraphWrapper & graph, Segment const & start)
    : IndexGraphStarterJoints<IndexGraphWrapper>(graph, start) {}

  Weight HeuristicCostEstimate(Vertex const & /* from */, Vertex const & /* to */) override
  {
    return GetAStarWeightZero<Weight>();
  }
};

/// \brief Fills |transitions| for osm id case. That means |Transition::m_roadMask| for items in
/// |transitions| will be combination of |VehicleType::Pedestrian|, |VehicleType::Bicycle|
/// and |VehicleType::Car|.
void CalcCrossMwmTransitions(
    string const & mwmFile, string const & intermediateDir,
    string const & mappingFile, vector<m2::RegionD> const & borders,
    string const & country, CountryParentNameGetterFn const & countryParentNameGetterFn,
    vector<CrossMwmConnectorSerializer::Transition<base::GeoObjectId>> & transitions)
{
  VehicleMaskBuilder const maskMaker(country, countryParentNameGetterFn);
  map<uint32_t, base::GeoObjectId> featureIdToOsmId;
  CHECK(ParseRoadsFeatureIdToOsmIdMapping(mappingFile, featureIdToOsmId),
        ("Can't parse feature id to osm id mapping. File:", mappingFile));

  auto const & path = base::JoinPath(intermediateDir, CROSS_MWM_OSM_WAYS_DIR, country);
  auto const crossMwmOsmIdWays =
      generator::CrossMwmOsmWaysCollector::CrossMwmInfo::LoadFromFileToSet(path);

  ForEachFromDat(mwmFile, [&](FeatureType & f, uint32_t featureId) {
    VehicleMask const roadMask = maskMaker.CalcRoadMask(f);
    if (roadMask == 0)
      return;

    auto const it = featureIdToOsmId.find(featureId);
    CHECK(it != featureIdToOsmId.end(), ("Can't find osm id for feature id", featureId));
    auto const osmId = it->second;
    CHECK(osmId.GetType() == base::GeoObjectId::Type::ObsoleteOsmWay, ());

    auto const crossMwmWayInfoIt =
        crossMwmOsmIdWays.find(generator::CrossMwmOsmWaysCollector::CrossMwmInfo(osmId.GetEncodedId()));

    if (crossMwmWayInfoIt != crossMwmOsmIdWays.cend())
    {
      f.ParseGeometry(FeatureType::BEST_GEOMETRY);

      VehicleMask const oneWayMask = maskMaker.CalcOneWayMask(f);

      auto const & crossMwmWayInfo = *crossMwmWayInfoIt;
      for (auto const & segmentInfo : crossMwmWayInfo.m_crossMwmSegments)
      {
        uint32_t const segmentId = segmentInfo.m_segmentId;
        bool const forwardIsEnter = segmentInfo.m_forwardIsEnter;

        transitions.emplace_back(osmId, featureId, segmentId, roadMask, oneWayMask, forwardIsEnter);
      }
    }
  });
}

/// \brief Fills |transitions| for transit case. That means Transition::m_roadMask for items in
/// |transitions| will be equal to VehicleType::Transit after call of this method.
void CalcCrossMwmTransitions(string const & mwmFile, string const & intermediateDir,
                             string const & mappingFile,
                             vector<m2::RegionD> const & borders, string const & country,
                             CountryParentNameGetterFn const & /* countryParentNameGetterFn */,
                             vector<CrossMwmConnectorSerializer::Transition<connector::TransitId>> & transitions)
{
  CHECK(mappingFile.empty(), ());
  CHECK(intermediateDir.empty(), ());
  try
  {
    FilesContainerR cont(mwmFile);
    if (!cont.IsExist(TRANSIT_FILE_TAG))
    {
      LOG(LINFO, ("Transit cross mwm section is not generated because no transit section in mwm:",
                  mwmFile));
      return;
    }
    auto reader = cont.GetReader(TRANSIT_FILE_TAG);

    transit::GraphData graphData;
    graphData.DeserializeForCrossMwm(*reader.GetPtr());
    auto const & stops = graphData.GetStops();
    auto const & edges = graphData.GetEdges();

    auto const getStopIdPoint = [&stops](transit::StopId stopId) -> m2::PointD const & {
      auto const it = equal_range(stops.cbegin(), stops.cend(), transit::Stop(stopId));
      CHECK_EQUAL(
          distance(it.first, it.second), 1,
          ("A stop with id:", stopId, "is not unique or there's no such item in stops:", stops));
      return it.first->GetPoint();
    };

    // Index |i| is a zero based edge index. This zero based index should be increased with
    // |FakeFeatureIds::kTransitGraphFeaturesStart| by setting it as |featureNumerationOffset| for
    // CrossMwmConnector (see CrossMwmIndexGraph::Deserialize()) and then used in Segment class as
    // feature id in transit case.
    for (size_t i = 0; i < edges.size(); ++i)
    {
      auto const & e = edges[i];
      m2::PointD const & stop1Point = getStopIdPoint(e.GetStop1Id());
      m2::PointD const & stop2Point = getStopIdPoint(e.GetStop2Id());
      bool const stop2In = m2::RegionsContain(borders, stop2Point);
      if (m2::RegionsContain(borders, stop1Point) == stop2In)
        continue;

      // Note. One way mask is set to kTransitMask because all transit edges are one way edges.
      transitions.emplace_back(connector::TransitId(e.GetStop1Id(), e.GetStop2Id(), e.GetLineId()),
                               i /* feature id */, 0 /* segment index */, kTransitMask,
                               kTransitMask /* one way mask */, stop2In /* forward is enter */);
    }
  }
  catch (Reader::OpenException const & e)
  {
    CHECK(false, ("Error while reading", TRANSIT_FILE_TAG, "section.", e.Msg()));
  }
}

/// \brief Fills |transitions| and |connectors| params.
/// \note This method fills only |connections| which are applicable for |CrossMwmId|.
/// For example |VehicleType::Pedestrian|, |VehicleType::Bicycle| and |VehicleType::Car|
/// are applicable for |connector::OsmId|.
/// And |VehicleType::Transit| is applicable for |connector::TransitId|.
template <typename CrossMwmId>
void CalcCrossMwmConnectors(
    string const & path, string const & mwmFile, string const & intermediateDir, string const & country,
    CountryParentNameGetterFn const & countryParentNameGetterFn, string const & mappingFile,
    vector<CrossMwmConnectorSerializer::Transition<CrossMwmId>> & transitions,
    CrossMwmConnectorPerVehicleType<CrossMwmId> & connectors)
{
  base::Timer timer;
  string const polyFile = base::JoinPath(path, BORDERS_DIR, country + BORDERS_EXTENSION);
  vector<m2::RegionD> borders;
  borders::LoadBorders(polyFile, borders);

  // Note 1. CalcCrossMwmTransitions() method fills vector |transitions|.
  // There are two implementation of the method for |connector::OsmId| and for |connector::TransitId|.
  // For all items in |transitions| |Transition::m_roadMask| may be set to any combination of masks:
  // GetVehicleMask(VehicleType::Pedestrian), GetVehicleMask(VehicleType::Bicycle) and
  // GetVehicleMask(VehicleType::Car) for |connector::OsmId| implementation.
  // For all items in |transitions| |Transition::m_roadMask| is set to
  // GetVehicleMask(VehicleType::Transit) for |connector::TransitId| implementation.
  // Note 2. Taking into account note 1 it's clear that field |Transition<TransitId>::m_roadMask|
  // is always set to |VehicleType::Transit| and field |Transition<OsmId>::m_roadMask| can't have
  // |VehicleType::Transit| value.
  CalcCrossMwmTransitions(mwmFile, intermediateDir, mappingFile, borders, country, countryParentNameGetterFn,
                          transitions);
  LOG(LINFO, ("Transitions finished, transitions:", transitions.size(),
      ", elapsed:", timer.ElapsedSeconds(), "seconds"));

  timer.Reset();

  for (auto const & transition : transitions)
  {
    for (size_t i = 0; i < connectors.size(); ++i)
    {
      VehicleMask const mask = GetVehicleMask(static_cast<VehicleType>(i));
      CrossMwmConnectorSerializer::AddTransition(transition, mask, connectors[i]);
    }
  }

  for (size_t i = 0; i < connectors.size(); ++i)
  {
    auto const vehicleType = static_cast<VehicleType>(i);
    auto const & connector = connectors[i];
    LOG(LINFO, (vehicleType, "model. Number of enters:", connector.GetEnters().size(),
                "Number of exits:", connector.GetExits().size()));
  }
}

template <typename CrossMwmId>
void FillWeights(string const & path, string const & mwmFile, string const & country,
                 CountryParentNameGetterFn const & countryParentNameGetterFn,
                 bool disableCrossMwmProgress, CrossMwmConnector<CrossMwmId> & connector)
{
  base::Timer timer;

  shared_ptr<VehicleModelInterface> vehicleModel =
      CarModelFactory(countryParentNameGetterFn).GetVehicleModelForCountry(country);
  IndexGraph graph(
      make_shared<Geometry>(GeometryLoader::CreateFromFile(mwmFile, vehicleModel)),
      EdgeEstimator::Create(VehicleType::Car, *vehicleModel, nullptr /* trafficStash */));

  MwmValue mwmValue(LocalCountryFile(path, platform::CountryFile(country), 0 /* version */));
  DeserializeIndexGraph(mwmValue, VehicleType::Car, graph);

  map<Segment, map<Segment, RouteWeight>> weights;
  auto const numEnters = connector.GetEnters().size();
  size_t foundCount = 0;
  size_t notFoundCount = 0;
  for (size_t i = 0; i < numEnters; ++i)
  {
    if (i % 10 == 0)
      LOG(LINFO, ("Building leaps:", i, "/", numEnters, "waves passed"));

    Segment const & enter = connector.GetEnter(i);

    using Algorithm = AStarAlgorithm<JointSegment, JointEdge, RouteWeight>;

    Algorithm astar;
    IndexGraphWrapper indexGraphWrapper(graph, enter);
    DijkstraWrapperJoints wrapper(indexGraphWrapper, enter);
    AStarAlgorithm<JointSegment, JointEdge, RouteWeight>::Context context(wrapper);
    unordered_map<uint32_t, vector<JointSegment>> visitedVertexes;
    astar.PropagateWave(wrapper, wrapper.GetStartJoint(),
                        [&](JointSegment const & vertex)
                        {
                          if (vertex.IsFake())
                          {
                            Segment start = wrapper.GetSegmentOfFakeJoint(vertex, true /* start */);
                            Segment end = wrapper.GetSegmentOfFakeJoint(vertex, false /* start */);
                            if (start.IsForward() != end.IsForward())
                              return true;

                            visitedVertexes[end.GetFeatureId()].emplace_back(start, end);
                          }
                          else
                          {
                            visitedVertexes[vertex.GetFeatureId()].emplace_back(vertex);
                          }

                            return true;
                          } /* visitVertex */,
                          context);

    for (Segment const & exit : connector.GetExits())
    {
      auto const it = visitedVertexes.find(exit.GetFeatureId());
      if (it == visitedVertexes.cend())
      {
        ++notFoundCount;
        continue;
      }

      uint32_t const id = exit.GetSegmentIdx();
      bool const forward = exit.IsForward();
      for (auto const & jointSegment : it->second)
      {
        if (jointSegment.IsForward() != forward)
          continue;

        if ((jointSegment.GetStartSegmentId() <= id && id <= jointSegment.GetEndSegmentId()) ||
            (jointSegment.GetEndSegmentId() <= id && id <= jointSegment.GetStartSegmentId()))
        {
          RouteWeight weight;
          Segment parentSegment;
          if (context.HasParent(jointSegment))
          {
            JointSegment const & parent = context.GetParent(jointSegment);
            parentSegment = parent.IsFake() ? wrapper.GetSegmentOfFakeJoint(parent, false /* start */)
                                            : parent.GetSegment(false /* start */);

            weight = context.GetDistance(parent);
          }
          else
          {
            parentSegment = enter;
          }

          Segment const & firstChild = jointSegment.GetSegment(true /* start */);
          uint32_t const lastPoint = exit.GetPointId(true /* front */);

          static map<JointSegment, JointSegment> kEmptyParents;
          auto optionalEdge =  graph.GetJointEdgeByLastPoint(parentSegment, firstChild,
                                                             true /* isOutgoing */, lastPoint);

          if (!optionalEdge)
            continue;

          weight += (*optionalEdge).GetWeight();
          weights[enter][exit] = weight;

          ++foundCount;
          break;
        }
      }
    }
  }

  connector.FillWeights([&](Segment const & enter, Segment const & exit) {
    auto it0 = weights.find(enter);
    if (it0 == weights.end())
      return connector::kNoRoute;

    auto it1 = it0->second.find(exit);
    if (it1 == it0->second.end())
      return connector::kNoRoute;

    return it1->second.ToCrossMwmWeight();
  });

  LOG(LINFO, ("Leaps finished, elapsed:", timer.ElapsedSeconds(), "seconds, routes found:",
              foundCount, ", not found:", notFoundCount));
}
}  // namespace

namespace routing
{
bool BuildRoutingIndex(string const & filename, string const & country,
                       CountryParentNameGetterFn const & countryParentNameGetterFn)
{
  LOG(LINFO, ("Building routing index for", filename));
  try
  {
    Processor processor(country, countryParentNameGetterFn);
    processor.ProcessAllFeatures(filename);

    IndexGraph graph;
    processor.BuildGraph(graph);

    FilesContainerW cont(filename, FileWriter::OP_WRITE_EXISTING);
    auto writer = cont.GetWriter(ROUTING_FILE_TAG);

    auto const startPos = writer->Pos();
    IndexGraphSerializer::Serialize(graph, processor.GetMasks(), *writer);
    auto const sectionSize = writer->Pos() - startPos;

    LOG(LINFO, ("Routing section created:", sectionSize, "bytes,", graph.GetNumRoads(), "roads,",
                graph.GetNumJoints(), "joints,", graph.GetNumPoints(), "points"));
    return true;
  }
  catch (RootException const & e)
  {
    LOG(LERROR, ("An exception happened while creating", ROUTING_FILE_TAG, "section:", e.what()));
    return false;
  }
}

/// \brief Serializes all the cross mwm information to |sectionName| of |mwmFile| including:
/// * header
/// * transitions
/// * weight buffers if any
template <typename CrossMwmId>
void SerializeCrossMwm(string const & mwmFile, string const & sectionName,
                       CrossMwmConnectorPerVehicleType<CrossMwmId> const & connectors,
                       vector<CrossMwmConnectorSerializer::Transition<CrossMwmId>> const & transitions)
{
  FilesContainerW cont(mwmFile, FileWriter::OP_WRITE_EXISTING);
  auto writer = cont.GetWriter(sectionName);
  auto const startPos = writer->Pos();
  CrossMwmConnectorSerializer::Serialize(transitions, connectors, *writer);
  auto const sectionSize = writer->Pos() - startPos;

  LOG(LINFO, ("Cross mwm section generated, size:", sectionSize, "bytes"));
}

void BuildRoutingCrossMwmSection(string const & path, string const & mwmFile,
                                 string const & country, string const & intermediateDir,
                                 CountryParentNameGetterFn const & countryParentNameGetterFn,
                                 string const & osmToFeatureFile, bool disableCrossMwmProgress)
{
  LOG(LINFO, ("Building cross mwm section for", country));
  using CrossMwmId = base::GeoObjectId;
  CrossMwmConnectorPerVehicleType<CrossMwmId> connectors;
  vector<CrossMwmConnectorSerializer::Transition<CrossMwmId>> transitions;

  CalcCrossMwmConnectors(path, mwmFile, intermediateDir, country, countryParentNameGetterFn, osmToFeatureFile,
                         transitions, connectors);

  // We use leaps for cars only. To use leaps for other vehicle types add weights generation
  // here and change WorldGraph mode selection rule in IndexRouter::CalculateSubroute.
  FillWeights(path, mwmFile, country, countryParentNameGetterFn, disableCrossMwmProgress,
              connectors[static_cast<size_t>(VehicleType::Car)]);

  CHECK(connectors[static_cast<size_t>(VehicleType::Transit)].IsEmpty(), ());
  SerializeCrossMwm(mwmFile, CROSS_MWM_FILE_TAG, connectors, transitions);
}

void BuildTransitCrossMwmSection(string const & path, string const & mwmFile,
                                 string const & country,
                                 CountryParentNameGetterFn const & countryParentNameGetterFn)
{
  LOG(LINFO, ("Building transit cross mwm section for", country));
  using CrossMwmId = connector::TransitId;
  CrossMwmConnectorPerVehicleType<CrossMwmId> connectors;
  vector<CrossMwmConnectorSerializer::Transition<CrossMwmId>> transitions;

  CalcCrossMwmConnectors(path, mwmFile, "" /* intermediateDir */, country,
                         countryParentNameGetterFn, "" /* mapping file */,
                         transitions, connectors);

  CHECK(connectors[static_cast<size_t>(VehicleType::Pedestrian)].IsEmpty(), ());
  CHECK(connectors[static_cast<size_t>(VehicleType::Bicycle)].IsEmpty(), ());
  CHECK(connectors[static_cast<size_t>(VehicleType::Car)].IsEmpty(), ());
  SerializeCrossMwm(mwmFile, TRANSIT_CROSS_MWM_FILE_TAG, connectors, transitions);
}
}  // namespace routing
